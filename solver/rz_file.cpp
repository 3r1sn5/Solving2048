#include "full_board_proof.h"
#include "rz_file.h"

#include "rzone.h"
#include "rzone_slide.h"
#include "search.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
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

void readExact(std::istream& input,
               std::uint8_t* bytes,
               std::size_t size,
               const std::filesystem::path& path)
{
    input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        fail(path, "unexpected end of file");
    }
}

void writeExact(std::ostream& output,
                const std::uint8_t* bytes,
                std::size_t size,
                const std::filesystem::path& path)
{
    output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!output) fail(path, "failed while writing RZone snapshot");
}

std::array<std::uint8_t, RzFile::kRecordSize> encodeRecord(const DiskRecord& record)
{
    std::array<std::uint8_t, RzFile::kRecordSize> bytes{};
    put64(bytes.data(), record.masked_board);
    put32(bytes.data() + 8, record.child_id);
    put32(bytes.data() + 12, record.depth);
    put16(bytes.data() + 16, record.mask);
    bytes[18] = record.kind;
    bytes[19] = record.phase;
    bytes[20] = record.outcome;
    bytes[21] = record.argument;
    put16(bytes.data() + 22, record.flags);
    put64(bytes.data() + 24, record.reserved);
    return bytes;
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

std::array<std::uint8_t, RzFile::kHeaderSize> makeHeader(
    unsigned winning_tile,
    std::uint64_t record_count,
    std::uint64_t checksum)
{
    if (record_count > std::numeric_limits<RZoneCertificateId>::max() ||
        record_count > (std::numeric_limits<std::uint64_t>::max() - RzFile::kHeaderSize) /
                           RzFile::kRecordSize) {
        throw std::overflow_error("RZone snapshot size exceeds the v1 format");
    }
    const std::uint64_t payload_size = record_count * RzFile::kRecordSize;
    std::array<std::uint8_t, RzFile::kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    put16(header.data() + 8, RzFile::kFormatVersion);
    put16(header.data() + 10, RzFile::kHeaderSize);
    put16(header.data() + 12, RzFile::kRecordSize);
    put16(header.data() + 14, 0);
    put32(header.data() + 16, kEndianMarker);
    put32(header.data() + 20, kRulesVersion);
    put32(header.data() + 24, kBoardEncodingVersion);
    put32(header.data() + 28, RzFile::kSemanticsVersion);
    put32(header.data() + 32, winning_tile);
    put32(header.data() + 36, 0);
    put64(header.data() + 40, RzFile::kHeaderSize);
    put64(header.data() + 48, record_count);
    put64(header.data() + 56, payload_size);
    put64(header.data() + 64, checksum);
    put32(header.data() + 72, kChecksumAlgorithmCrc64Ecma);
    return header;
}

std::uint8_t encodeKind(RZoneStepKind kind)
{
    switch (kind) {
        case RZoneStepKind::Terminal: return kTerminalKind;
        case RZoneStepKind::FixedSlide: return kFixedSlideKind;
        case RZoneStepKind::UniversalPlacement: return kUniversalPlacementKind;
        case RZoneStepKind::FullBoardPlacement: return kFullBoardPlacementKind;
    }
    throw std::logic_error("RZone certificate has an invalid kind");
}

RZoneStepKind decodeKind(std::uint8_t kind)
{
    if (kind == kTerminalKind) return RZoneStepKind::Terminal;
    if (kind == kFixedSlideKind) return RZoneStepKind::FixedSlide;
    if (kind == kUniversalPlacementKind) return RZoneStepKind::UniversalPlacement;
    if (kind == kFullBoardPlacementKind) return RZoneStepKind::FullBoardPlacement;
    throw std::invalid_argument("kind is invalid");
}

std::uint8_t encodePhase(Phase phase)
{
    if (phase == Phase::Player) return kPlayerPhase;
    if (phase == Phase::Environment) return kEnvironmentPhase;
    throw std::logic_error("RZone certificate has an invalid phase");
}

Phase decodePhase(std::uint8_t phase)
{
    if (phase == kPlayerPhase) return Phase::Player;
    if (phase == kEnvironmentPhase) return Phase::Environment;
    throw std::invalid_argument("phase is invalid");
}

std::uint8_t encodeArgument(const RZoneCertificate& certificate)
{
    if (certificate.kind == RZoneStepKind::Terminal) {
        if (certificate.terminal_position < 0 || certificate.terminal_position >= kBoardCells) {
            throw std::logic_error("terminal position is invalid");
        }
        return static_cast<std::uint8_t>(certificate.terminal_position);
    }
    if (certificate.kind == RZoneStepKind::FixedSlide) {
        if (!certificate.slide) throw std::logic_error("FixedSlide is missing its direction");
        return static_cast<std::uint8_t>(*certificate.slide);
    }
    return kNoArgument;
}

DiskRecord encodeCertificate(const RZoneCertificate& certificate)
{
    if (certificate.depth > std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("RZone certificate depth does not fit the v1 format");
    }
    DiskRecord record;
    record.masked_board = certificate.masked_board;
    record.child_id = certificate.child;
    record.depth = static_cast<std::uint32_t>(certificate.depth);
    record.mask = certificate.mask;
    record.kind = encodeKind(certificate.kind);
    record.phase = encodePhase(certificate.phase);
    record.outcome = kWinOutcome;
    record.argument = encodeArgument(certificate);
    return record;
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

bool validateFixedSlideLine(const RZoneCertificate& parent,
                            const RZoneCertificate& child,
                            Direction direction,
                            int line)
{
    // A slide is line-local and each line has only four 4-bit ranks. Exhausting
    // every completion here proves the pattern implication without replaying a
    // certificate suffix or introducing a validation budget.
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

void validateFixedSlide(const RZoneCertificate& parent,
                        const RZoneCertificate& child)
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

std::vector<RZoneCertificate> validateRecords(
    const std::vector<DiskRecord>& records,
    unsigned winning_tile, unsigned semantics_version)
{
    FullBoardProofIndex full_board_proofs;
    std::vector<RZoneCertificate> certificates;
    certificates.reserve(records.size());
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
            if (RZoneTable::maskedBoard(Board{record.masked_board}, record.mask) !=
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

            RZoneCertificate certificate;
            certificate.id = id;
            certificate.kind = decodeKind(record.kind);
            certificate.phase = decodePhase(record.phase);
            certificate.winning_tile = winning_tile;
            certificate.mask = record.mask;
            certificate.masked_board = record.masked_board;
            certificate.child = record.child_id;
            certificate.depth = record.depth;
            certificate.validated = true;

            if (certificate.kind == RZoneStepKind::Terminal) {
                if (record.child_id != kNoRZoneCertificate || record.depth != 1 ||
                    record.argument >= kBoardCells) {
                    throw std::invalid_argument(
                        "Terminal child, depth, or position is invalid");
                }
                certificate.terminal_position = record.argument;
                if (record.mask !=
                    static_cast<std::uint16_t>(std::uint16_t{1} << record.argument)) {
                    throw std::invalid_argument(
                        "Terminal mask must contain only the terminal position");
                }
                if (cellRank(record.masked_board, record.argument) < winning_rank) {
                    throw std::invalid_argument("Terminal tile does not reach winning_tile");
                }
            } else if (certificate.kind == RZoneStepKind::FullBoardPlacement) {
                if (semantics_version < 2 || record.argument != kNoArgument) {
                    throw std::invalid_argument("FullBoardPlacement semantics or argument is invalid");
                }
                full_board_proofs.validate(certificate);
            } else {
                if (record.child_id == kNoRZoneCertificate || record.child_id >= id) {
                    throw std::invalid_argument(
                        "non-terminal child_id must refer to an earlier certificate");
                }
                const RZoneCertificate& child =
                    certificates[static_cast<std::size_t>(record.child_id - 1)];
                if (child.depth == std::numeric_limits<std::uint32_t>::max() ||
                    record.depth != child.depth + 1) {
                    throw std::invalid_argument("depth does not equal child.depth + 1");
                }

                if (certificate.kind == RZoneStepKind::FixedSlide) {
                    if (certificate.phase != Phase::Player ||
                        child.phase != Phase::Environment || record.argument > 3) {
                        throw std::invalid_argument(
                            "FixedSlide phase, child phase, or direction is invalid");
                    }
                    certificate.slide = static_cast<Direction>(record.argument);
                    if (semantics_version >= 3) {
                        if (!improvedSlideImplies(certificate.masked_board, certificate.mask,
                                                 *certificate.slide, winning_tile,
                                                 child.masked_board, child.mask)) {
                            throw std::invalid_argument("FixedSlide fails terminal-or-child implication");
                        }
                    } else validateFixedSlide(certificate, child);
                } else {
                    if (certificate.phase != Phase::Environment ||
                        child.phase != Phase::Player || record.argument != kNoArgument) {
                        throw std::invalid_argument(
                            "UniversalPlacement phase, child phase, or argument is invalid");
                    }
                    if (certificate.mask != child.mask ||
                        certificate.masked_board != child.masked_board) {
                        throw std::invalid_argument(
                            "UniversalPlacement parent and child patterns differ");
                    }
                    for (int position = 0; position < kBoardCells; ++position) {
                        if ((certificate.mask & (std::uint16_t{1} << position)) != 0 &&
                            cellRank(certificate.masked_board, position) == 0) {
                            throw std::invalid_argument(
                                "UniversalPlacement mask contains an empty cell");
                        }
                    }
                }
            }
            certificates.push_back(certificate);
            full_board_proofs.insert(certificate);
        } catch (const std::exception& error) {
            throw std::invalid_argument(
                "certificate " + std::to_string(id) + ": " + error.what());
        }
    }
    return certificates;
}

DiskRecord recordFromTable(const RZoneTable& table, std::size_t index)
{
    const RZoneCertificate* found = table.find(
        static_cast<RZoneCertificateId>(index + 1));
    if (!found) throw std::logic_error("RZone table certificate IDs are not contiguous");
    const RZoneCertificate& certificate = *found;
    if (!certificate.validated ||
        certificate.id != static_cast<RZoneCertificateId>(index + 1) ||
        certificate.winning_tile != table.winningTile()) {
        throw std::logic_error(
            "RZone table contains an unpublished or incompatible certificate");
    }
    return encodeCertificate(certificate);
}

} // namespace

RzFileSummary RzFile::load(const std::filesystem::path& path, RZoneTable& table)
{
    if (!table.certificates_.empty()) {
        throw std::logic_error("persistent RZone can only be loaded into an empty table");
    }

    std::error_code size_error;
    const std::uintmax_t raw_file_size = std::filesystem::file_size(path, size_error);
    if (size_error) fail(path, "cannot determine file size: " + size_error.message());
    if (raw_file_size > std::numeric_limits<std::uint64_t>::max()) {
        fail(path, "file is too large");
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < kHeaderSize) fail(path, "file is smaller than the RZone header");

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail(path, "cannot open RZone file");
    std::array<std::uint8_t, kHeaderSize> header{};
    readExact(input, header.data(), header.size(), path);

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) fail(path, "invalid magic");
    if (get16(header.data() + 8) != kFormatVersion) {
        fail(path, "unsupported format version");
    }
    if (get16(header.data() + 10) != kHeaderSize) fail(path, "unexpected header size");
    if (get16(header.data() + 12) != kRecordSize) fail(path, "unexpected record size");
    if (get16(header.data() + 14) != 0) fail(path, "header flags must be zero");
    if (get32(header.data() + 16) != kEndianMarker) fail(path, "invalid byte-order marker");
    if (get32(header.data() + 20) != kRulesVersion) {
        fail(path, "incompatible rules version");
    }
    if (get32(header.data() + 24) != kBoardEncodingVersion) {
        fail(path, "incompatible board encoding version");
    }
    if (get32(header.data() + 28) < 1 ||
        get32(header.data() + 28) > kSemanticsVersion) {
        fail(path, "incompatible RZone semantics version");
    }
    const unsigned winning_tile = get32(header.data() + 32);
    if (!isPowerOfTwo(winning_tile) || winning_tile > 32768) {
        fail(path, "invalid winning tile in header");
    }
    if (winning_tile != table.winning_tile_) fail(path, "winning tile is incompatible");
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
        record_count > (std::numeric_limits<std::uint64_t>::max() - kHeaderSize) /
                           kRecordSize) {
        fail(path, "record count exceeds the v1 format or this process");
    }
    const std::uint64_t calculated_payload_size = record_count * kRecordSize;
    if (records_offset != kHeaderSize || payload_size != calculated_payload_size ||
        file_size != kHeaderSize + payload_size) {
        fail(path, "record offset, payload size, count, or file size is inconsistent");
    }

    std::vector<DiskRecord> records;
    records.reserve(static_cast<std::size_t>(record_count));
    std::uint64_t checksum = 0;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        std::array<std::uint8_t, kRecordSize> bytes{};
        readExact(input, bytes.data(), bytes.size(), path);
        updateCrc64(checksum, bytes.data(), bytes.size());
        records.push_back(decodeRecord(bytes));
    }
    if (checksum != expected_checksum) fail(path, "payload checksum mismatch");
    if (input.peek() != std::char_traits<char>::eof()) fail(path, "unexpected trailing data");

    std::vector<RZoneCertificate> certificates;
    try {
        certificates = validateRecords(records, winning_tile, get32(header.data() + 28));
    } catch (const std::exception& error) {
        fail(path, error.what());
    }

    for (auto& phase_masks : table.masks_by_phase_) {
        for (auto& masks : phase_masks) masks.clear();
    }
    table.patterns_.clear();
    table.radix_nodes_.clear();
    table.radix_roots_ = {};
    table.certificates_.clear();
    table.statistics_ = {};
    for (const RZoneCertificate& certificate : certificates) {
        table.certificates_.push_back(certificate);
        const unsigned cells = RZoneTable::countCells(certificate.mask);
        if (cells > table.max_cells_) continue;
        table.patternIds(certificate).push_back(certificate.id);
        table.indexRadixCertificate(certificate);
    }
    table.statistics_.loaded_certificates = certificates.size();
    return {record_count, checksum, file_size};
}

RzFileSummary RzFile::save(const std::filesystem::path& path, const RZoneTable& table)
{
    if (path.empty()) throw std::invalid_argument("persistent RZone output path cannot be empty");
    const std::uint64_t record_count = table.size();

    if (!path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            fail(path, "cannot create output directory: " + directory_error.message());
        }
    }

    std::filesystem::path temporary_path = path;
    temporary_path += ".tmp";
    std::uint64_t checksum = 0;
    try {
        std::ofstream output(temporary_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output) fail(temporary_path, "cannot open temporary RZone file");
        const auto provisional = makeHeader(table.winning_tile_, record_count, 0);
        writeExact(output, provisional.data(), provisional.size(), temporary_path);

        for (std::size_t index = 0; index < table.size(); ++index) {
            const DiskRecord record = recordFromTable(table, index);
            const auto bytes = encodeRecord(record);
            updateCrc64(checksum, bytes.data(), bytes.size());
            writeExact(output, bytes.data(), bytes.size(), temporary_path);
        }

        const auto final_header = makeHeader(table.winning_tile_, record_count, checksum);
        output.seekp(0);
        if (!output) fail(temporary_path, "cannot seek to rewrite RZone header");
        writeExact(output, final_header.data(), final_header.size(), temporary_path);
        output.flush();
        if (!output) fail(temporary_path, "failed while flushing RZone snapshot");
        output.close();
        if (!output) fail(temporary_path, "failed while closing RZone snapshot");

        std::error_code rename_error;
        std::filesystem::rename(temporary_path, path, rename_error);
        if (rename_error) {
            fail(path, "cannot publish RZone snapshot: " + rename_error.message());
        }
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }

    const std::uint64_t payload_size = record_count * kRecordSize;
    return {record_count, checksum, kHeaderSize + payload_size};
}

RzFileSummary RzFile::summarize(
    const RZoneTable& table,
    const RzFileSummary* verified_prefix)
{
    std::uint64_t first_record = 0;
    std::uint64_t checksum = 0;
    if (verified_prefix) {
        if (verified_prefix->record_count > table.size() ||
            verified_prefix->file_size !=
                kHeaderSize + verified_prefix->record_count * kRecordSize) {
            throw std::invalid_argument(
                "RZone summary prefix is incompatible with the current table");
        }
        first_record = verified_prefix->record_count;
        checksum = verified_prefix->payload_checksum;
    }
    for (std::size_t index = static_cast<std::size_t>(first_record);
         index < table.size();
         ++index) {
        const DiskRecord record = recordFromTable(table, index);
        const auto bytes = encodeRecord(record);
        updateCrc64(checksum, bytes.data(), bytes.size());
    }
    const std::uint64_t record_count = table.size();
    return {
        record_count,
        checksum,
        kHeaderSize + record_count * kRecordSize,
    };
}

RzFileSummary RzFile::load(const std::filesystem::path& path, Solver& solver)
{
    if (solver.solve_started_) {
        throw std::logic_error("persistent RZone must be loaded before solve()");
    }
    if (solver.options_.rzone_mode == RZoneMode::Disabled) {
        throw std::logic_error("persistent RZone loading requires RZone generation or lookup");
    }
    return load(path, solver.rzone_table_);
}

RzFileSummary RzFile::save(const std::filesystem::path& path, const Solver& solver)
{
    if (!solver.solve_finished_) {
        throw std::logic_error("persistent RZone can only be saved after solve()");
    }
    if (solver.options_.rzone_mode == RZoneMode::Disabled) {
        throw std::logic_error("persistent RZone saving requires RZone generation or lookup");
    }
    return save(path, solver.rzone_table_);
}

RzFileSummary RzFile::summarize(
    const Solver& solver,
    const RzFileSummary* verified_prefix)
{
    if (!solver.solve_finished_) {
        throw std::logic_error(
            "persistent RZone can only be summarized after solve()");
    }
    if (solver.options_.rzone_mode == RZoneMode::Disabled) {
        throw std::logic_error(
            "persistent RZone summarization requires RZone generation or lookup");
    }
    return summarize(solver.rzone_table_, verified_prefix);
}

} // namespace solver2048
