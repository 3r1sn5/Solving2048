#include "rz_proof_file.h"

#include "game_api.h"
#include "full_board_proof.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace solver2048 {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'S', '2', '0', '4', '8', 'R', 'Z', 0};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint32_t kRulesVersion = 1;
constexpr std::uint32_t kBoardEncodingVersion = 1;
constexpr std::uint32_t kChecksumAlgorithmCrc64Ecma = 1;
constexpr std::uint8_t kTerminalKind = 1;
constexpr std::uint8_t kFixedSlideKind = 2;
constexpr std::uint8_t kUniversalPlacementKind = 3;
constexpr std::uint8_t kFullBoardPlacementKind = 4;
constexpr std::uint8_t kPlayerPhase = 1;
constexpr std::uint8_t kEnvironmentPhase = 2;
constexpr std::uint8_t kWinOutcome = 1;
constexpr std::uint8_t kNoArgument = 255;
constexpr std::uint64_t kCrc64Polynomial = 0x42f0e1eba9ea3693ull;
constexpr int kBoardSize = 4;
constexpr int kBoardCells = 16;

struct DiskRecord {
    std::uint64_t masked_board = 0;
    std::uint32_t child_id = 0;
    std::uint32_t depth = 0;
    std::uint16_t mask = 0;
    std::uint8_t kind = 0;
    std::uint8_t phase = 0;
    std::uint8_t outcome = 0;
    std::uint8_t argument = 0;
    std::uint16_t flags = 0;
    std::uint64_t reserved = 0;
};

struct RecordIdentity {
    std::uint64_t masked_board = 0;
    std::uint32_t child_id = 0;
    std::uint16_t mask = 0;
    std::uint8_t kind = 0;
    std::uint8_t phase = 0;
    std::uint8_t argument = 0;
    std::uint32_t depth = 0;

    bool operator==(const RecordIdentity& other) const
    {
        return masked_board == other.masked_board && child_id == other.child_id &&
            mask == other.mask && kind == other.kind && phase == other.phase &&
            argument == other.argument && depth == other.depth;
    }
};

struct RecordIdentityHash {
    std::size_t operator()(const RecordIdentity& value) const
    {
        std::uint64_t hash = value.masked_board;
        hash ^= static_cast<std::uint64_t>(value.child_id) * 0x9e3779b97f4a7c15ull;
        hash ^= static_cast<std::uint64_t>(value.mask) << 32;
        hash ^= static_cast<std::uint64_t>(value.kind) << 16;
        hash ^= static_cast<std::uint64_t>(value.phase) << 8;
        hash ^= value.argument;
        hash ^= static_cast<std::uint64_t>(value.depth) * 0x517cc1b727220a95ull;
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdull;
        hash ^= hash >> 33;
        return static_cast<std::size_t>(hash);
    }
};

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& message)
{
    throw std::runtime_error(path.string() + ": " + message);
}

bool isPowerOfTwo(unsigned value)
{
    return value >= 2 && (value & (value - 1)) == 0;
}

unsigned tileRank(unsigned tile)
{
    unsigned rank = 0;
    while (tile > 1) {
        tile >>= 1;
        ++rank;
    }
    return rank;
}

unsigned cellRank(std::uint64_t board, int position)
{
    return static_cast<unsigned>((board >> (position * 4)) & 0xfu);
}

std::uint64_t expandNibbleMask(std::uint16_t mask)
{
    std::uint64_t result = 0;
    for (int position = 0; position < kBoardCells; ++position) {
        if ((mask & (std::uint16_t{1} << position)) != 0) {
            result |= std::uint64_t{0xf} << (position * 4);
        }
    }
    return result;
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

void readExact(std::istream& input,
               std::uint8_t* output,
               std::size_t size,
               const std::filesystem::path& path)
{
    input.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        fail(path, "unexpected end of file");
    }
}

DiskRecord decodeRecord(const std::array<std::uint8_t, RzFile::kRecordSize>& bytes)
{
    DiskRecord record;
    record.masked_board = get64(bytes.data());
    record.child_id = get32(bytes.data() + 8);
    record.depth = get32(bytes.data() + 12);
    record.mask = get16(bytes.data() + 16);
    record.kind = bytes[18];
    record.phase = bytes[19];
    record.outcome = bytes[20];
    record.argument = bytes[21];
    record.flags = get16(bytes.data() + 22);
    record.reserved = get64(bytes.data() + 24);
    return record;
}

RZoneStepKind decodeKind(std::uint8_t kind)
{
    if (kind == kTerminalKind) return RZoneStepKind::Terminal;
    if (kind == kFixedSlideKind) return RZoneStepKind::FixedSlide;
    if (kind == kUniversalPlacementKind) return RZoneStepKind::UniversalPlacement;
    if (kind == kFullBoardPlacementKind) return RZoneStepKind::FullBoardPlacement;
    throw std::invalid_argument("kind is invalid");
}

Phase decodePhase(std::uint8_t phase)
{
    if (phase == kPlayerPhase) return Phase::Player;
    if (phase == kEnvironmentPhase) return Phase::Environment;
    throw std::invalid_argument("phase is invalid");
}

std::array<int, kBoardSize> linePositions(Direction direction, int line)
{
    std::array<int, kBoardSize> positions{};
    for (int offset = 0; offset < kBoardSize; ++offset) {
        switch (direction) {
            case Direction::Up:
                positions[offset] = offset * kBoardSize + line;
                break;
            case Direction::Right:
                positions[offset] = line * kBoardSize + (kBoardSize - 1 - offset);
                break;
            case Direction::Down:
                positions[offset] = (kBoardSize - 1 - offset) * kBoardSize + line;
                break;
            case Direction::Left:
                positions[offset] = line * kBoardSize + offset;
                break;
        }
    }
    return positions;
}

std::array<unsigned, kBoardSize> slideLine(
    const std::array<unsigned, kBoardSize>& input)
{
    std::array<unsigned, kBoardSize> compacted{};
    unsigned compacted_size = 0;
    for (const unsigned rank : input) {
        if (rank != 0) compacted[compacted_size++] = rank;
    }

    std::array<unsigned, kBoardSize> output{};
    unsigned output_size = 0;
    for (unsigned index = 0; index < compacted_size; ++index) {
        if (index + 1 < compacted_size && compacted[index] == compacted[index + 1]) {
            output[output_size++] = compacted[index] + 1;
            ++index;
        } else {
            output[output_size++] = compacted[index];
        }
    }
    return output;
}

bool validateFixedSlideLine(const RzProofEntry& parent,
                            const RzProofEntry& child,
                            Direction direction,
                            int line)
{
    const std::array<int, kBoardSize> positions = linePositions(direction, line);
    std::array<unsigned, kBoardSize> input{};
    std::array<bool, kBoardSize> known{};
    std::array<unsigned, kBoardSize> required_output{};
    std::array<bool, kBoardSize> required{};
    bool has_required_output = false;
    for (int offset = 0; offset < kBoardSize; ++offset) {
        const int position = positions[offset];
        known[offset] = (parent.mask & (std::uint16_t{1} << position)) != 0;
        if (known[offset]) input[offset] = cellRank(parent.masked_board, position);
        required[offset] = (child.mask & (std::uint16_t{1} << position)) != 0;
        if (required[offset]) {
            required_output[offset] = cellRank(child.masked_board, position);
            has_required_output = true;
        }
    }

    bool valid_outputs = true;
    bool has_unchanged_completion = false;
    std::function<void(int)> enumerate = [&](int offset) {
        if (!valid_outputs || (!has_required_output && has_unchanged_completion)) return;
        if (offset == kBoardSize) {
            const auto output = slideLine(input);
            for (int output_offset = 0; output_offset < kBoardSize; ++output_offset) {
                if (required[output_offset] &&
                    output[output_offset] != required_output[output_offset]) {
                    valid_outputs = false;
                    return;
                }
            }
            if (output == input) has_unchanged_completion = true;
            return;
        }
        if (known[offset]) {
            enumerate(offset + 1);
            return;
        }
        for (unsigned rank = 0; rank <= 15; ++rank) {
            input[offset] = rank;
            enumerate(offset + 1);
            if (!valid_outputs || (!has_required_output && has_unchanged_completion)) return;
        }
    };
    enumerate(0);
    if (!valid_outputs) {
        throw std::invalid_argument(
            "FixedSlide pattern does not imply the child masked board");
    }
    return !has_unchanged_completion;
}

void validateFixedSlide(const RzProofEntry& parent, const RzProofEntry& child)
{
    bool action_always_legal = false;
    for (int line = 0; line < kBoardSize; ++line) {
        action_always_legal =
            validateFixedSlideLine(parent, child, *parent.slide, line) ||
            action_always_legal;
    }
    if (!action_always_legal) {
        throw std::invalid_argument(
            "FixedSlide pattern does not guarantee that the action is legal");
    }
}

std::vector<RzProofEntry> validateRecords(
    const std::vector<DiskRecord>& records,
    unsigned winning_tile, unsigned semantics_version)
{
    FullBoardProofIndex full_board_proofs;
    std::vector<RzProofEntry> entries;
    entries.reserve(records.size());
    std::unordered_set<RecordIdentity, RecordIdentityHash> identities;
    identities.reserve(records.size());
    const unsigned winning_rank = tileRank(winning_tile);

    for (std::size_t index = 0; index < records.size(); ++index) {
        const DiskRecord& record = records[index];
        const RZoneCertificateId id = static_cast<RZoneCertificateId>(index + 1);
        try {
            if (record.flags != 0 || record.reserved != 0) {
                throw std::invalid_argument("flags and reserved fields must be zero");
            }
            if (record.outcome != kWinOutcome) {
                throw std::invalid_argument("v1 outcome must be Win");
            }
            if (record.mask == 0) throw std::invalid_argument("mask must not be empty");
            if ((record.masked_board & expandNibbleMask(record.mask)) !=
                record.masked_board) {
                throw std::invalid_argument("masked_board contains data outside mask");
            }
            if (record.depth == 0) throw std::invalid_argument("depth must be positive");

            const RecordIdentity identity{
                record.masked_board,
                record.child_id,
                record.mask,
                record.kind,
                record.phase,
                record.argument,
                record.depth,
            };
            if (!identities.insert(identity).second) {
                throw std::invalid_argument("duplicate certificate record");
            }

            RzProofEntry entry;
            entry.id = id;
            entry.kind = decodeKind(record.kind);
            entry.phase = decodePhase(record.phase);
            entry.mask = record.mask;
            entry.masked_board = record.masked_board;
            entry.child = record.child_id;
            entry.depth = record.depth;

            if (entry.kind == RZoneStepKind::Terminal) {
                if (record.child_id != kNoRZoneCertificate || record.depth != 1 ||
                    record.argument >= kBoardCells) {
                    throw std::invalid_argument(
                        "Terminal child, depth, or position is invalid");
                }
                entry.terminal_position = record.argument;
                if (record.mask !=
                    static_cast<std::uint16_t>(std::uint16_t{1} << record.argument)) {
                    throw std::invalid_argument(
                        "Terminal mask must contain only the terminal position");
                }
                if (cellRank(record.masked_board, record.argument) < winning_rank) {
                    throw std::invalid_argument("Terminal tile does not reach winning_tile");
                }
            } else if (entry.kind == RZoneStepKind::FullBoardPlacement) {
                if (semantics_version < 2 || record.argument != kNoArgument) {
                    throw std::invalid_argument("FullBoardPlacement semantics or argument is invalid");
                }
                full_board_proofs.validate(entry);
            } else {
                if (record.child_id == kNoRZoneCertificate || record.child_id >= id) {
                    throw std::invalid_argument(
                        "non-terminal child_id must refer to an earlier certificate");
                }
                const RzProofEntry& child =
                    entries[static_cast<std::size_t>(record.child_id - 1)];
                if (child.depth == std::numeric_limits<std::uint32_t>::max() ||
                    record.depth != child.depth + 1) {
                    throw std::invalid_argument("depth does not equal child.depth + 1");
                }

                if (entry.kind == RZoneStepKind::FixedSlide) {
                    if (entry.phase != Phase::Player ||
                        child.phase != Phase::Environment || record.argument > 3) {
                        throw std::invalid_argument(
                            "FixedSlide phase, child phase, or direction is invalid");
                    }
                    entry.slide = static_cast<Direction>(record.argument);
                    validateFixedSlide(entry, child);
                } else {
                    if (entry.phase != Phase::Environment ||
                        child.phase != Phase::Player || record.argument != kNoArgument) {
                        throw std::invalid_argument(
                            "UniversalPlacement phase, child phase, or argument is invalid");
                    }
                    if (entry.mask != child.mask ||
                        entry.masked_board != child.masked_board) {
                        throw std::invalid_argument(
                            "UniversalPlacement parent and child patterns differ");
                    }
                    for (int position = 0; position < kBoardCells; ++position) {
                        if ((entry.mask & (std::uint16_t{1} << position)) != 0 &&
                            cellRank(entry.masked_board, position) == 0) {
                            throw std::invalid_argument(
                                "UniversalPlacement mask contains an empty cell");
                        }
                    }
                }
            }
            entries.push_back(entry);
            full_board_proofs.insert(entry);
        } catch (const std::exception& error) {
            throw std::invalid_argument(
                "certificate " + std::to_string(id) + ": " + error.what());
        }
    }
    return entries;
}

} // namespace

RzProofTable RzProofTable::load(const std::filesystem::path& path)
{
    std::error_code size_error;
    const std::uintmax_t raw_file_size = std::filesystem::file_size(path, size_error);
    if (size_error) fail(path, "cannot determine file size: " + size_error.message());
    if (raw_file_size > std::numeric_limits<std::uint64_t>::max()) {
        fail(path, "file is too large");
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < RzFile::kHeaderSize) {
        fail(path, "file is smaller than the RZone header");
    }

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail(path, "cannot open RZone file");
    std::array<std::uint8_t, RzFile::kHeaderSize> header{};
    readExact(input, header.data(), header.size(), path);

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) fail(path, "invalid magic");
    if (get16(header.data() + 8) != RzFile::kFormatVersion) {
        fail(path, "unsupported format version");
    }
    if (get16(header.data() + 10) != RzFile::kHeaderSize) {
        fail(path, "unexpected header size");
    }
    if (get16(header.data() + 12) != RzFile::kRecordSize) {
        fail(path, "unexpected record size");
    }
    if (get16(header.data() + 14) != 0) fail(path, "header flags must be zero");
    if (get32(header.data() + 16) != kEndianMarker) fail(path, "invalid byte-order marker");
    if (get32(header.data() + 20) != kRulesVersion) {
        fail(path, "incompatible rules version");
    }
    if (get32(header.data() + 24) != kBoardEncodingVersion) {
        fail(path, "incompatible board encoding version");
    }
    if (get32(header.data() + 28) != 1 &&
        get32(header.data() + 28) != RzFile::kSemanticsVersion) {
        fail(path, "incompatible RZone semantics version");
    }
    const unsigned winning_tile = get32(header.data() + 32);
    if (!isPowerOfTwo(winning_tile) || winning_tile > 32768) {
        fail(path, "invalid winning tile in header");
    }
    if (get32(header.data() + 36) != 0) fail(path, "header reserved field must be zero");

    const std::uint64_t records_offset = get64(header.data() + 40);
    const std::uint64_t record_count = get64(header.data() + 48);
    const std::uint64_t payload_size = get64(header.data() + 56);
    const std::uint64_t expected_checksum = get64(header.data() + 64);
    if (get32(header.data() + 72) != kChecksumAlgorithmCrc64Ecma) {
        fail(path, "unsupported checksum algorithm");
    }
    for (std::size_t index = 76; index < header.size(); ++index) {
        if (header[index] != 0) fail(path, "header reserved bytes must be zero");
    }
    if (record_count > std::numeric_limits<RZoneCertificateId>::max() ||
        record_count > std::numeric_limits<std::size_t>::max() ||
        record_count > (std::numeric_limits<std::uint64_t>::max() -
                        RzFile::kHeaderSize) / RzFile::kRecordSize) {
        fail(path, "record count exceeds the v1 format or this process");
    }
    const std::uint64_t calculated_payload_size = record_count * RzFile::kRecordSize;
    if (records_offset != RzFile::kHeaderSize ||
        payload_size != calculated_payload_size ||
        file_size != RzFile::kHeaderSize + payload_size) {
        fail(path, "record offset, payload size, count, or file size is inconsistent");
    }

    std::vector<DiskRecord> records;
    records.reserve(static_cast<std::size_t>(record_count));
    std::vector<std::uint64_t> prefix_checksums;
    prefix_checksums.reserve(static_cast<std::size_t>(record_count) + 1);
    prefix_checksums.push_back(0);
    std::uint64_t checksum = 0;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        std::array<std::uint8_t, RzFile::kRecordSize> bytes{};
        readExact(input, bytes.data(), bytes.size(), path);
        updateCrc64(checksum, bytes.data(), bytes.size());
        prefix_checksums.push_back(checksum);
        records.push_back(decodeRecord(bytes));
    }
    if (checksum != expected_checksum) fail(path, "payload checksum mismatch");
    if (input.peek() != std::char_traits<char>::eof()) fail(path, "unexpected trailing data");

    std::vector<RzProofEntry> entries;
    try {
        entries = validateRecords(records, winning_tile, get32(header.data() + 28));
    } catch (const std::exception& error) {
        fail(path, error.what());
    }

    RzProofTable table;
    table.winning_tile_ = winning_tile;
    table.summary_ = {record_count, checksum, file_size};
    table.entries_ = std::move(entries);
    table.prefix_checksums_ = std::move(prefix_checksums);
    for (const RzProofEntry& entry : table.entries_) {
        const auto [mask_entry, new_mask] = table.patterns_.try_emplace(entry.mask);
        if (new_mask) {
            table.masks_.push_back({entry.mask, expandNibbleMask(entry.mask)});
        }
        mask_entry->second[entry.masked_board].push_back(entry.id);
    }
    return table;
}

const RzProofEntry* RzProofTable::find(RZoneCertificateId id) const
{
    if (id == kNoRZoneCertificate || id > entries_.size()) return nullptr;
    return &entries_[static_cast<std::size_t>(id - 1)];
}

bool RzProofTable::matches(
    const RzProofEntry& entry,
    Board board,
    Phase phase,
    std::uint8_t transform)
{
    if (transform >= 8) return false;
    const Board candidate = transform == 0 ? board : transformBoard(board, transform);
    return entry.phase == phase &&
           (candidate.raw & expandNibbleMask(entry.mask)) == entry.masked_board;
}

const RzProofEntry* RzProofTable::findMatching(
    Board board,
    Phase phase,
    RZoneCertificateId maximum_id,
    bool include_symmetry) const
{
    if (maximum_id > entries_.size()) {
        throw std::invalid_argument("RZone prefix exceeds the loaded table");
    }
    const std::uint8_t transform_count = include_symmetry ? 8 : 1;
    for (std::uint8_t transform = 0; transform < transform_count; ++transform) {
        const Board candidate = transform == 0 ? board : transformBoard(board, transform);
        for (const MaskIndexEntry& indexed : masks_) {
            const auto mask_entry = patterns_.find(indexed.mask);
            if (mask_entry == patterns_.end()) {
                throw std::logic_error("RZone proof index is inconsistent");
            }
            const auto found = mask_entry->second.find(candidate.raw & indexed.nibble_mask);
            if (found == mask_entry->second.end()) continue;
            for (const RZoneCertificateId id : found->second) {
                if (id > maximum_id) break;
                const RzProofEntry* entry = find(id);
                if (entry && entry->phase == phase) return entry;
            }
        }
    }
    return nullptr;
}

std::uint64_t RzProofTable::prefixChecksum(std::uint64_t record_count) const
{
    if (record_count >= prefix_checksums_.size()) {
        throw std::out_of_range("RZone prefix record count exceeds the loaded table");
    }
    return prefix_checksums_[static_cast<std::size_t>(record_count)];
}

} // namespace solver2048
