#pragma once

#include "game_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace solver2048 {

enum class Phase;
class RzFile;

enum class RZoneMode {
    Disabled,
    Generate,
    Lookup,
};

enum class RZoneStepKind {
    Terminal,
    FixedSlide,
    UniversalPlacement,
};

enum class RZoneLookupPolicy {
    FullScan,
    SmallestFirst,
};

using RZoneCertificateId = std::uint32_t;
inline constexpr RZoneCertificateId kNoRZoneCertificate = 0;

struct RZoneMatch {
    RZoneCertificateId id = kNoRZoneCertificate;
    // Current candidate orientation -> stored certificate orientation.
    std::uint8_t transform = 0;
};

struct RZoneCertificate {
    RZoneCertificateId id = kNoRZoneCertificate;
    RZoneStepKind kind = RZoneStepKind::Terminal;
    Phase phase;
    unsigned winning_tile = 0;
    std::uint16_t mask = 0;
    std::uint64_t masked_board = 0;
    std::optional<Direction> slide;
    int terminal_position = -1;
    RZoneCertificateId child = kNoRZoneCertificate;
    unsigned depth = 0;
    bool validated = false;
};

struct RZoneStatistics {
    std::uint64_t loaded_certificates = 0;
    std::uint64_t terminal_certificates = 0;
    std::uint64_t slide_lift_attempts = 0;
    std::uint64_t slide_lift_successes = 0;
    std::uint64_t slide_lift_failures = 0;
    std::uint64_t placement_lift_attempts = 0;
    std::uint64_t placement_lift_successes = 0;
    std::uint64_t placement_lift_blocked = 0;
    std::uint64_t patterns_stored = 0;
    std::uint64_t duplicate_patterns = 0;
    std::uint64_t lookups = 0;
    std::uint64_t masks_indexed = 0;
    std::uint64_t lookup_masks_tested = 0;
    std::uint64_t lookup_candidates = 0;
    std::uint64_t pattern_matches = 0;
    std::uint64_t lookup_early_exits = 0;
    std::uint64_t player_hits = 0;
    std::uint64_t environment_hits = 0;
    std::uint64_t exact_orientation_hits = 0;
    std::uint64_t symmetry_hits = 0;
    std::uint64_t lookup_transforms_tested = 0;
    std::uint64_t partial_hits = 0;
    std::uint64_t full_board_hits = 0;
    std::uint64_t active_pruning_and_nodes = 0;
    std::uint64_t active_pruning_certificates_tested = 0;
    std::uint64_t active_pruning_transform_tests = 0;
    std::uint64_t active_pruning_children_matched = 0;
    std::uint64_t active_pruning_full_lookups_avoided = 0;
    std::uint64_t active_pruning_and_nodes_closed = 0;
    std::uint64_t active_pruning_universal_lift_attempts = 0;
    std::uint64_t active_pruning_universal_lift_successes = 0;

    std::uint64_t hits() const { return player_hits + environment_hits; }
};

class RZoneTable {
public:
    RZoneTable(const Game& game,
               unsigned winning_tile,
               unsigned max_cells = 16,
               RZoneLookupPolicy lookup_policy = RZoneLookupPolicy::FullScan,
               bool use_symmetry = false);

    std::optional<RZoneCertificateId> createTerminal(Board board, Phase phase);
    std::optional<RZoneCertificateId> liftFixedSlide(
        Board parent,
        Direction slide,
        RZoneCertificateId child);
    std::optional<RZoneCertificateId> liftUniversalPlacement(
        Board parent,
        RZoneCertificateId child);

    std::optional<RZoneMatch> lookup(Board board, Phase phase);
    std::optional<RZoneMatch> matchCertificate(
        Board board,
        Phase phase,
        RZoneCertificateId id,
        std::optional<std::uint8_t> preferred_transform = std::nullopt);
    const RZoneCertificate* find(RZoneCertificateId id) const;

    void recordActivePruningAndNode() { ++statistics_.active_pruning_and_nodes; }
    void recordActivePruningHit(Phase phase, const RZoneMatch& match);
    void recordActivePruningAndNodeClosed() {
        ++statistics_.active_pruning_and_nodes_closed;
    }
    void recordActivePruningUniversalLift(bool success) {
        ++statistics_.active_pruning_universal_lift_attempts;
        if (success) ++statistics_.active_pruning_universal_lift_successes;
    }
    const RZoneStatistics& statistics() const { return statistics_; }
    std::size_t size() const { return certificates_.size(); }
    unsigned winningTile() const { return winning_tile_; }
    bool usesSymmetry() const { return use_symmetry_; }

    static std::uint64_t maskedBoard(Board board, std::uint16_t mask);
    static unsigned countCells(std::uint16_t mask);
    std::uint16_t deriveSlidePreimageMask(
        Board parent,
        Direction slide,
        std::uint16_t child_mask) const;

private:
    friend class RzFile;

    using PatternBucket =
        std::unordered_map<std::uint64_t, std::vector<RZoneCertificateId>>;

    struct MaskIndexEntry {
        std::uint16_t mask = 0;
        std::uint64_t nibble_mask = 0;
    };

    std::optional<RZoneCertificateId> publish(RZoneCertificate certificate);
    bool maskContainsOnlyNonEmpty(Board board, std::uint16_t mask) const;
    bool sameCertificate(const RZoneCertificate& lhs, const RZoneCertificate& rhs) const;
    const SlideResult* findSlide(
        const std::vector<SlideResult>& slides,
        Direction direction) const;
    std::optional<RZoneMatch> lookupFullScan(
        const std::array<Board, 8>& candidates,
        unsigned transform_count,
        Phase phase);
    std::optional<RZoneMatch> lookupSmallestFirst(
        const std::array<Board, 8>& candidates,
        unsigned transform_count,
        Phase phase);
    std::optional<RZoneMatch> recordLookupHit(
        const RZoneCertificate* certificate,
        Phase phase,
        std::uint8_t transform);
    std::optional<RZoneMatch> matchCertificateImpl(
        Board board,
        Phase phase,
        RZoneCertificateId id,
        std::optional<std::uint8_t> preferred_transform);
    static std::uint64_t expandNibbleMask(std::uint16_t mask);

    const Game& game_;
    unsigned winning_tile_ = 0;
    unsigned max_cells_ = 16;
    RZoneLookupPolicy lookup_policy_ = RZoneLookupPolicy::FullScan;
    bool use_symmetry_ = false;
    // IDs and returned references remain stable while later certificates are
    // appended; certificates are immutable after publication.
    std::deque<RZoneCertificate> certificates_;
    std::unordered_map<std::uint16_t, PatternBucket> patterns_;
    std::array<std::vector<MaskIndexEntry>, 17> masks_by_cell_count_;
    RZoneStatistics statistics_;
};

const char* rzoneModeName(RZoneMode mode);
const char* rzoneStepKindName(RZoneStepKind kind);
const char* rzoneLookupPolicyName(RZoneLookupPolicy policy);

} // namespace solver2048
