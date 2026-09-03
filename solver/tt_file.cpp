#include "tt_file.h"

#include "search.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace solver2048 {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'S', '2', '0', '4', '8', 'T', 'T', 0};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint32_t kRulesVersion = 1;
constexpr std::uint32_t kBoardEncodingVersion = 1;
constexpr std::uint32_t kChecksumAlgorithmCrc64Ecma = 1;
constexpr std::uint16_t kSymmetryFlag = 1u << 0;
constexpr std::uint8_t kWinCode = 1;
constexpr std::uint8_t kLossCode = 2;
constexpr std::uint8_t kNoWitness = 255;
constexpr std::uint64_t kCrc64Polynomial = 0x42f0e1eba9ea3693ull;

struct DiskRecord {
    std::uint64_t board_key = 0;
    std::uint8_t outcome = 0;
    std::uint8_t witness = kNoWitness;
    std::uint8_t children_total = 0;
    std::uint8_t flags = 0;
    std::uint32_t reserved = 0;
};

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& message)
{
    throw std::runtime_error(path.string() + ": " + message);
}

bool isPowerOfTwo(unsigned value)
{
    return value >= 2 && (value & (value - 1)) == 0;
}

void put16(std::uint8_t* output, std::uint16_t value)
{
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t* output, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void put64(std::uint8_t* output, std::uint64_t value)
{
    for (unsigned index = 0; index < 8; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint16_t get16(const std::uint8_t* input)
{
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(input[1]) << 8;
}

std::uint32_t get32(const std::uint8_t* input)
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(input[index]) << (index * 8);
    }
    return value;
}

std::uint64_t get64(const std::uint8_t* input)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8);
    }
    return value;
}

const std::array<std::uint64_t, 256>& crc64Table()
{
    static const std::array<std::uint64_t, 256> table = [] {
        std::array<std::uint64_t, 256> result{};
        for (std::uint64_t byte = 0; byte < result.size(); ++byte) {
            std::uint64_t value = byte << 56;
            for (unsigned bit = 0; bit < 8; ++bit) {
                value = (value & (std::uint64_t{1} << 63)) != 0
                    ? (value << 1) ^ kCrc64Polynomial
                    : value << 1;
            }
            result[byte] = value;
        }
        return result;
    }();
    return table;
}

void updateCrc64(std::uint64_t& crc, const std::uint8_t* data, std::size_t size)
{
    const auto& table = crc64Table();
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t table_index = static_cast<std::uint8_t>((crc >> 56) ^ data[index]);
        crc = table[table_index] ^ (crc << 8);
    }
}

std::uint8_t encodeOutcome(Outcome outcome)
{
    if (outcome == Outcome::Win) return kWinCode;
    if (outcome == Outcome::Loss) return kLossCode;
    throw std::logic_error("persistent TT cannot encode an Unknown outcome");
}

Outcome decodeOutcome(std::uint8_t outcome)
{
    if (outcome == kWinCode) return Outcome::Win;
    if (outcome == kLossCode) return Outcome::Loss;
    throw std::invalid_argument("outcome must be exact Win or Loss");
}

std::uint8_t encodeWitness(const std::optional<SearchAction>& witness)
{
    if (!witness) return kNoWitness;
    const int id = witness->sgfId();
    if (id < 0 || id > 35) throw std::logic_error("TT witness action ID is out of range");
    return static_cast<std::uint8_t>(id);
}

std::optional<SearchAction> decodeWitness(std::uint8_t witness)
{
    if (witness == kNoWitness) return std::nullopt;
    if (witness <= 3) return SearchAction::slide(static_cast<Direction>(witness));
    if (witness <= 19) return SearchAction::placement(witness - 4, 2);
    if (witness <= 35) return SearchAction::placement(witness - 20, 4);
    throw std::invalid_argument("witness action ID is invalid");
}

void validateRecord(const DiskRecord& record, Phase phase, bool symmetry)
{
    const Outcome outcome = decodeOutcome(record.outcome);
    const std::optional<SearchAction> witness = decodeWitness(record.witness);
    if (record.flags != 0 || record.reserved != 0) {
        throw std::invalid_argument("record flags and reserved fields must be zero");
    }
    if (phase == Phase::Player) {
        if (record.children_total > 4) {
            throw std::invalid_argument("Player children_total exceeds four slides");
        }
        if (outcome == Outcome::Win) {
            if (!witness || witness->kind != SearchAction::Kind::Slide) {
                throw std::invalid_argument("Player Win entry requires a slide witness");
            }
        } else if (witness) {
            throw std::invalid_argument("Player Loss entry must not contain a witness");
        }
    } else {
        if (record.children_total == 0 || record.children_total > 32) {
            throw std::invalid_argument("Environment children_total must be in [1, 32]");
        }
        if (outcome == Outcome::Loss) {
            if (!witness || witness->kind != SearchAction::Kind::Placement) {
                throw std::invalid_argument("Environment Loss entry requires a placement witness");
            }
        } else if (witness) {
            throw std::invalid_argument("Environment Win entry must not contain a witness");
        }
    }
    if (symmetry && canonicalize(Board{record.board_key}).board.raw != record.board_key) {
        throw std::invalid_argument("record board key is not D4 canonical");
    }
}

std::array<std::uint8_t, TtFile::kRecordSize> encodeRecord(const DiskRecord& record)
{
    std::array<std::uint8_t, TtFile::kRecordSize> bytes{};
    put64(bytes.data(), record.board_key);
    bytes[8] = record.outcome;
    bytes[9] = record.witness;
    bytes[10] = record.children_total;
    bytes[11] = record.flags;
    put32(bytes.data() + 12, record.reserved);
    return bytes;
}

DiskRecord decodeRecord(const std::array<std::uint8_t, TtFile::kRecordSize>& bytes)
{
    DiskRecord record;
    record.board_key = get64(bytes.data());
    record.outcome = bytes[8];
    record.witness = bytes[9];
    record.children_total = bytes[10];
    record.flags = bytes[11];
    record.reserved = get32(bytes.data() + 12);
    return record;
}

void readExact(std::istream& input,
               std::uint8_t* bytes,
               std::size_t size,
               const std::filesystem::path& path)
{
    input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) fail(path, "unexpected end of file");
}

void writeExact(std::ostream& output,
                const std::uint8_t* bytes,
                std::size_t size,
                const std::filesystem::path& path)
{
    output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!output) fail(path, "failed while writing TT snapshot");
}

std::array<std::uint8_t, TtFile::kHeaderSize> makeHeader(
    unsigned winning_tile,
    bool symmetry,
    std::uint64_t player_count,
    std::uint64_t environment_count,
    std::uint64_t checksum)
{
    if (player_count > (std::numeric_limits<std::uint64_t>::max() - TtFile::kHeaderSize) /
                           TtFile::kRecordSize ||
        environment_count > std::numeric_limits<std::uint64_t>::max() /
                                TtFile::kRecordSize) {
        throw std::overflow_error("TT snapshot size overflow");
    }
    const std::uint64_t player_bytes = player_count * TtFile::kRecordSize;
    const std::uint64_t environment_bytes = environment_count * TtFile::kRecordSize;
    if (player_bytes > std::numeric_limits<std::uint64_t>::max() - environment_bytes) {
        throw std::overflow_error("TT snapshot payload size overflow");
    }
    const std::uint64_t payload_size = player_bytes + environment_bytes;
    const std::uint64_t environment_offset = TtFile::kHeaderSize + player_bytes;

    std::array<std::uint8_t, TtFile::kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    put16(header.data() + 8, TtFile::kFormatVersion);
    put16(header.data() + 10, TtFile::kHeaderSize);
    put16(header.data() + 12, TtFile::kRecordSize);
    put16(header.data() + 14, symmetry ? kSymmetryFlag : 0);
    put32(header.data() + 16, kEndianMarker);
    put32(header.data() + 20, kRulesVersion);
    put32(header.data() + 24, kBoardEncodingVersion);
    put32(header.data() + 28, winning_tile);
    put64(header.data() + 32, TtFile::kHeaderSize);
    put64(header.data() + 40, player_count);
    put64(header.data() + 48, environment_offset);
    put64(header.data() + 56, environment_count);
    put64(header.data() + 64, payload_size);
    put64(header.data() + 72, checksum);
    put32(header.data() + 80, kChecksumAlgorithmCrc64Ecma);
    return header;
}

} // namespace

TtFileSummary TtFile::load(const std::filesystem::path& path, Solver& solver)
{
    if (solver.solve_started_) throw std::logic_error("persistent TT must be loaded before solve()");
    if (!solver.options_.use_transposition_table) {
        throw std::logic_error("persistent TT loading requires the transposition table");
    }
    if (!solver.player_tt_.empty() || !solver.environment_tt_.empty()) {
        throw std::logic_error("persistent TT can only be loaded into empty tables");
    }

    std::error_code size_error;
    const std::uintmax_t raw_file_size = std::filesystem::file_size(path, size_error);
    if (size_error) fail(path, "cannot determine file size: " + size_error.message());
    if (raw_file_size > std::numeric_limits<std::uint64_t>::max()) fail(path, "file is too large");
    const std::uint64_t file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < kHeaderSize) fail(path, "file is smaller than the TT header");

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail(path, "cannot open TT file");
    std::array<std::uint8_t, kHeaderSize> header{};
    readExact(input, header.data(), header.size(), path);

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) fail(path, "invalid magic");
    if (get16(header.data() + 8) != kFormatVersion) fail(path, "unsupported format version");
    if (get16(header.data() + 10) != kHeaderSize) fail(path, "unexpected header size");
    if (get16(header.data() + 12) != kRecordSize) fail(path, "unexpected record size");
    const std::uint16_t flags = get16(header.data() + 14);
    if ((flags & ~kSymmetryFlag) != 0) fail(path, "header contains unknown flags");
    const bool symmetry = (flags & kSymmetryFlag) != 0;
    if (get32(header.data() + 16) != kEndianMarker) fail(path, "invalid byte-order marker");
    if (get32(header.data() + 20) != kRulesVersion) fail(path, "incompatible rules version");
    if (get32(header.data() + 24) != kBoardEncodingVersion) {
        fail(path, "incompatible board encoding version");
    }
    const unsigned winning_tile = get32(header.data() + 28);
    if (!isPowerOfTwo(winning_tile) || winning_tile > 32768) {
        fail(path, "invalid winning tile in header");
    }
    if (winning_tile != solver.options_.winning_tile) fail(path, "winning tile is incompatible");
    if (symmetry != solver.options_.use_tt_symmetry) fail(path, "symmetry mode is incompatible");

    const std::uint64_t player_offset = get64(header.data() + 32);
    const std::uint64_t player_count = get64(header.data() + 40);
    const std::uint64_t environment_offset = get64(header.data() + 48);
    const std::uint64_t environment_count = get64(header.data() + 56);
    const std::uint64_t payload_size = get64(header.data() + 64);
    const std::uint64_t expected_checksum = get64(header.data() + 72);
    if (get32(header.data() + 80) != kChecksumAlgorithmCrc64Ecma) {
        fail(path, "unsupported checksum algorithm");
    }
    for (std::size_t index = 84; index < header.size(); ++index) {
        if (header[index] != 0) fail(path, "header reserved bytes must be zero");
    }

    if (player_count > (std::numeric_limits<std::uint64_t>::max() - kHeaderSize) / kRecordSize ||
        environment_count > std::numeric_limits<std::uint64_t>::max() / kRecordSize) {
        fail(path, "record count overflows file offsets");
    }
    const std::uint64_t player_bytes = player_count * kRecordSize;
    const std::uint64_t environment_bytes = environment_count * kRecordSize;
    if (player_bytes > std::numeric_limits<std::uint64_t>::max() - environment_bytes) {
        fail(path, "payload size overflow");
    }
    const std::uint64_t calculated_payload_size = player_bytes + environment_bytes;
    if (player_offset != kHeaderSize ||
        environment_offset != kHeaderSize + player_bytes ||
        payload_size != calculated_payload_size ||
        payload_size > std::numeric_limits<std::uint64_t>::max() - kHeaderSize ||
        file_size != kHeaderSize + payload_size) {
        fail(path, "section offsets, counts, or file size are inconsistent");
    }
    if (player_count > std::numeric_limits<std::size_t>::max() ||
        environment_count > std::numeric_limits<std::size_t>::max()) {
        fail(path, "entry count exceeds this process address space");
    }

    decltype(solver.player_tt_) player_entries;
    decltype(solver.environment_tt_) environment_entries;
    player_entries.reserve(static_cast<std::size_t>(player_count));
    environment_entries.reserve(static_cast<std::size_t>(environment_count));
    std::uint64_t checksum = 0;

    auto readSection = [&](Phase phase, std::uint64_t count, auto& destination) {
        for (std::uint64_t index = 0; index < count; ++index) {
            std::array<std::uint8_t, kRecordSize> bytes{};
            readExact(input, bytes.data(), bytes.size(), path);
            updateCrc64(checksum, bytes.data(), bytes.size());
            const DiskRecord record = decodeRecord(bytes);
            try {
                validateRecord(record, phase, symmetry);
            } catch (const std::exception& error) {
                fail(path, std::string(phaseName(phase)) + " record " +
                               std::to_string(index) + ": " + error.what());
            }

            Solver::TranspositionEntry entry;
            entry.outcome = decodeOutcome(record.outcome);
            entry.children_total = record.children_total;
            entry.selected_action = decodeWitness(record.witness);
            entry.source_transform = 0;
            entry.persistent = true;
            const auto [found, inserted] = destination.emplace(record.board_key, std::move(entry));
            if (!inserted) {
                if (found->second.outcome != decodeOutcome(record.outcome)) {
                    fail(path, std::string(phaseName(phase)) + " contains a conflicting duplicate key");
                }
                fail(path, std::string(phaseName(phase)) + " contains a duplicate key");
            }
        }
    };

    readSection(Phase::Player, player_count, player_entries);
    readSection(Phase::Environment, environment_count, environment_entries);
    if (checksum != expected_checksum) fail(path, "payload checksum mismatch");
    if (input.peek() != std::char_traits<char>::eof()) fail(path, "unexpected trailing data");

    solver.player_tt_ = std::move(player_entries);
    solver.environment_tt_ = std::move(environment_entries);
    return {player_count, environment_count, checksum, file_size};
}

TtFileSummary TtFile::save(const std::filesystem::path& path, const Solver& solver)
{
    if (!solver.solve_finished_) throw std::logic_error("persistent TT can only be saved after solve()");
    if (!solver.options_.use_transposition_table) {
        throw std::logic_error("persistent TT saving requires the transposition table");
    }
    if (path.empty()) throw std::invalid_argument("persistent TT output path cannot be empty");

    if (!path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) fail(path, "cannot create output directory: " + directory_error.message());
    }

    const std::uint64_t player_count = solver.player_tt_.size();
    const std::uint64_t environment_count = solver.environment_tt_.size();
    std::filesystem::path temporary_path = path;
    temporary_path += ".tmp";

    std::uint64_t checksum = 0;
    try {
        std::ofstream output(temporary_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output) fail(temporary_path, "cannot open temporary TT file");
        const auto provisional = makeHeader(
            solver.options_.winning_tile,
            solver.options_.use_tt_symmetry,
            player_count,
            environment_count,
            0);
        writeExact(output, provisional.data(), provisional.size(), temporary_path);

        auto writeSection = [&](Phase phase, const auto& source) {
            for (const auto& [board_key, entry] : source) {
                if (entry.children_total > std::numeric_limits<std::uint8_t>::max()) {
                    throw std::logic_error("TT children_total does not fit the file format");
                }
                DiskRecord record;
                record.board_key = board_key;
                record.outcome = encodeOutcome(entry.outcome);
                record.witness = encodeWitness(entry.selected_action);
                record.children_total = static_cast<std::uint8_t>(entry.children_total);
                try {
                    validateRecord(record, phase, solver.options_.use_tt_symmetry);
                } catch (const std::exception& error) {
                    throw std::logic_error(
                        std::string("cannot serialize ") + phaseName(phase) + " TT entry: " +
                        error.what());
                }
                const auto bytes = encodeRecord(record);
                updateCrc64(checksum, bytes.data(), bytes.size());
                writeExact(output, bytes.data(), bytes.size(), temporary_path);
            }
        };
        writeSection(Phase::Player, solver.player_tt_);
        writeSection(Phase::Environment, solver.environment_tt_);

        const auto final_header = makeHeader(
            solver.options_.winning_tile,
            solver.options_.use_tt_symmetry,
            player_count,
            environment_count,
            checksum);
        output.seekp(0);
        if (!output) fail(temporary_path, "cannot seek to rewrite TT header");
        writeExact(output, final_header.data(), final_header.size(), temporary_path);
        output.flush();
        if (!output) fail(temporary_path, "failed while flushing TT snapshot");
        output.close();
        if (!output) fail(temporary_path, "failed while closing TT snapshot");

        std::error_code rename_error;
        std::filesystem::rename(temporary_path, path, rename_error);
        if (rename_error) fail(path, "cannot publish TT snapshot: " + rename_error.message());
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }

    const std::uint64_t payload_size = (player_count + environment_count) * kRecordSize;
    return {player_count, environment_count, checksum, kHeaderSize + payload_size};
}

} // namespace solver2048
