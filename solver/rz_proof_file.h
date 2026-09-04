#pragma once

#include "rz_file.h"
#include "search.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace solver2048 {

struct RzProofEntry {
    RZoneCertificateId id = kNoRZoneCertificate;
    RZoneStepKind kind = RZoneStepKind::Terminal;
    Phase phase = Phase::Player;
    std::uint16_t mask = 0;
    std::uint64_t masked_board = 0;
    std::optional<Direction> slide;
    int terminal_position = -1;
    RZoneCertificateId child = kNoRZoneCertificate;
    std::uint32_t depth = 0;
};

class RzProofTable {
public:
    static RzProofTable load(const std::filesystem::path& path);

    unsigned winningTile() const { return winning_tile_; }
    const RzFileSummary& summary() const { return summary_; }
    const RzProofEntry* find(RZoneCertificateId id) const;
    const RzProofEntry* findMatching(
        Board board,
        Phase phase,
        RZoneCertificateId maximum_id,
        bool include_symmetry = false) const;
    std::uint64_t prefixChecksum(std::uint64_t record_count) const;

    static bool matches(
        const RzProofEntry& entry,
        Board board,
        Phase phase,
        std::uint8_t transform = 0);

private:
    using PatternBucket =
        std::unordered_map<std::uint64_t, std::vector<RZoneCertificateId>>;

    struct MaskIndexEntry {
        std::uint16_t mask = 0;
        std::uint64_t nibble_mask = 0;
    };

    unsigned winning_tile_ = 0;
    RzFileSummary summary_;
    std::vector<RzProofEntry> entries_;
    std::vector<std::uint64_t> prefix_checksums_;
    std::unordered_map<std::uint16_t, PatternBucket> patterns_;
    std::vector<MaskIndexEntry> masks_;
};

} // namespace solver2048
