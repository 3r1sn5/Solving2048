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
class Solver;

enum class RZoneMode {
    Disabled,
    Generate,
    Lookup,
};

enum class RZoneStepKind {
    Terminal,
    FixedSlide,
    UniversalPlacement,
    FullBoardPlacement,
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
    // FullBoardPlacement uses this as an inclusive proof-prefix ID bound.
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
    std::uint64_t trim_attempts = 0;
    std::uint64_t trim_cells_removed = 0;
    std::uint64_t trimmed_certificates = 0;
    std::uint64_t placement_lift_attempts = 0;
    std::uint64_t placement_lift_successes = 0;
    std::uint64_t placement_lift_blocked = 0;
    std::uint64_t full_board_placement_certificates = 0;
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
               bool use_symmetry = false,
               bool trimming = true,
               bool full_board_placement_fallback = true);

    // The ordered index borrows bucket addresses. Map moves preserve them,
    // but a shallow copy would leave the index pointing into the source table.
    RZoneTable(const RZoneTable&) = delete;
    RZoneTable& operator=(const RZoneTable&) = delete;
    RZoneTable(RZoneTable&&) = default;

    std::optional<RZoneCertificateId> createTerminal(Board board, Phase phase);
    std::optional<RZoneCertificateId> liftFixedSlide(
        Board parent,
        Direction slide,
        RZoneCertificateId child);
    std::optional<RZoneCertificateId> liftUniversalPlacement(
        Board parent,
        RZoneCertificateId child);
    std::optional<RZoneCertificateId> liftFullBoardPlacement(
        Board parent,
        const std::vector<RZoneMatch>& children);

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
    friend class Solver;

    using PatternBucket =
        std::unordered_map<std::uint64_t, std::vector<RZoneCertificateId>>;

    struct MaskIndexEntry {
        std::uint64_t nibble_mask = 0;
        const PatternBucket* bucket = nullptr;
    };

    static std::size_t phaseIndex(Phase phase);
    std::vector<RZoneCertificateId>& patternIds(const RZoneCertificate& certificate);
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
    bool trimming_ = true;
    bool full_board_placement_fallback_ = true;
    // IDs and returned references remain stable while later certificates are
    // appended; certificates are immutable after publication.
    std::deque<RZoneCertificate> certificates_;
    // A mask is counted once globally, but each phase owns its pattern bucket
    // and ordered mask list. unordered_map rehash preserves bucket addresses;
    // no buckets are erased while their index entries remain in use.
    std::unordered_map<std::uint16_t, std::array<PatternBucket, 2>> patterns_;
    std::array<std::array<std::vector<MaskIndexEntry>, 17>, 2> masks_by_phase_;
    RZoneStatistics statistics_;
};

const char* rzoneModeName(RZoneMode mode);
const char* rzoneStepKindName(RZoneStepKind kind);
const char* rzoneLookupPolicyName(RZoneLookupPolicy policy);

} // namespace solver2048
