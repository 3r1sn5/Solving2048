#pragma once

#include "game_api.h"
#include "rzone.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace solver2048 {

class TtFile;

enum class Outcome {
    Win,
    Loss,
    Unknown,
};

enum class Phase {
    Player,
    Environment,
};

enum class TerminalReason {
    None,
    WinningTile,
    NoLegalMove,
    NodeLimit,
    TranspositionTable,
};

enum class SolvedBy {
    None,
    Terminal,
    RZoneTable,
    TranspositionTable,
    Backpropagation,
    NodeLimit,
};

struct SearchAction {
    enum class Kind {
        Slide,
        Placement,
    };

    Kind kind = Kind::Slide;
    Direction direction = Direction::Up;
    int position = -1;
    unsigned tile = 0;

    static SearchAction slide(Direction direction);
    static SearchAction placement(int position, unsigned tile);

    int sgfId() const;
    char sgfPlayer() const;
    std::string name() const;
    std::string summaryToken() const;
};

struct TraceNode {
    std::uint64_t trace_id = 0;
    Phase phase = Phase::Player;
    Outcome outcome = Outcome::Unknown;
    TerminalReason terminal_reason = TerminalReason::None;
    SolvedBy solved_by = SolvedBy::None;
    bool expanded = false;
    bool tt_hit = false;
    std::size_t dfs_order = 0;
    std::size_t children_total = 0;
    std::optional<std::uint64_t> tt_source_trace_id;
    bool tt_persistent_hit = false;
    std::optional<std::uint64_t> tt_key;
    bool tt_symmetry_hit = false;
    std::optional<std::uint8_t> tt_transform;
    bool rzone_hit = false;
    bool rzone_symmetry_hit = false;
    std::optional<std::uint8_t> rzone_transform;
    std::optional<RZoneCertificateId> rzone_certificate_id;
    std::optional<RZoneStepKind> rzone_kind;
    std::optional<std::uint16_t> rzone_mask;
    std::optional<std::uint64_t> rzone_masked_board;
    std::optional<unsigned> rzone_depth;
    std::optional<SearchAction> incoming_action;
    std::optional<int> reward;
    std::optional<double> afterstate_value;
    std::optional<double> action_value;
    std::optional<SearchAction> selected_action;
    std::optional<double> selected_action_value;
    std::vector<std::unique_ptr<TraceNode>> children;
    std::vector<SearchAction> skipped_actions;
};

struct SearchStatistics {
    // Nodes entered by the DFS control flow, including terminal, TT, RZone,
    // and node-limit leaves. Parent-level active-pruned proof leaves are not
    // visited search nodes even though they remain in the SGF trace.
    std::uint64_t player_nodes = 0;
    std::uint64_t environment_nodes = 0;
    std::uint64_t player_actions = 0;
    std::uint64_t placement_actions = 0;
    std::uint64_t node_limit_cutoffs = 0;
    std::uint64_t player_tt_hits = 0;
    std::uint64_t environment_tt_hits = 0;
    std::uint64_t player_tt_stores = 0;
    std::uint64_t environment_tt_stores = 0;
    std::uint64_t player_tt_symmetry_hits = 0;
    std::uint64_t environment_tt_symmetry_hits = 0;
    std::uint64_t player_persistent_tt_hits = 0;
    std::uint64_t environment_persistent_tt_hits = 0;
    std::uint64_t player_tt_loaded_entries = 0;
    std::uint64_t environment_tt_loaded_entries = 0;
    RZoneStatistics rzone;

    std::uint64_t combinedNodes() const { return player_nodes + environment_nodes; }
    std::uint64_t ttHits() const { return player_tt_hits + environment_tt_hits; }
    std::uint64_t ttStores() const { return player_tt_stores + environment_tt_stores; }
    std::uint64_t ttSymmetryHits() const {
        return player_tt_symmetry_hits + environment_tt_symmetry_hits;
    }
    std::uint64_t persistentTtHits() const {
        return player_persistent_tt_hits + environment_persistent_tt_hits;
    }
    std::uint64_t playerTtEntries() const {
        return player_tt_loaded_entries + player_tt_stores;
    }
    std::uint64_t environmentTtEntries() const {
        return environment_tt_loaded_entries + environment_tt_stores;
    }
    std::uint64_t playerTtExactHits() const { return player_tt_hits - player_tt_symmetry_hits; }
    std::uint64_t environmentTtExactHits() const {
        return environment_tt_hits - environment_tt_symmetry_hits;
    }
};

struct SolverOptions {
    unsigned winning_tile = 8;
    std::uint64_t node_limit = 0;
    std::uint64_t progress_interval = 0;
    bool use_transposition_table = false;
    bool use_tt_symmetry = false;
    RZoneMode rzone_mode = RZoneMode::Disabled;
    RZoneLookupPolicy rzone_lookup_policy = RZoneLookupPolicy::FullScan;
    bool use_rzone_symmetry = false;
    unsigned rzone_max_cells = 16;
    bool rzone_active_pruning = false;
    bool rzone_trimming = true;
    bool rzone_full_board_placement_fallback = true;
};

struct SearchResult {
    Outcome outcome = Outcome::Unknown;
    SearchStatistics statistics;
    std::unique_ptr<TraceNode> root;
    std::optional<Direction> root_action;
    std::optional<double> root_action_value;
};

class Solver {
public:
    using ProgressCallback = std::function<void(const SearchStatistics&)>;

    Solver(const Game& game, SolverOptions options, ProgressCallback progress = {});
    // Moves a completed opening's proof tables into this fresh Solver. Entries
    // become persistent sources because their trace-local IDs belong to the
    // earlier SGF.
    void adoptPersistentProofs(Solver& completed);
    SearchResult solve();

private:
    friend class RzFile;
    friend class TtFile;

    struct OrderedSlide {
        SlideResult slide;
        double afterstate_value = 0;
        double action_value = 0;
    };

    struct TranspositionEntry {
        Outcome outcome = Outcome::Unknown;
        std::uint64_t source_trace_id = 0;
        std::size_t children_total = 0;
        std::optional<SearchAction> selected_action;
        std::optional<double> selected_action_value;
        std::uint8_t source_transform = 0;
        bool persistent = false;
    };

    Outcome solvePlayer(Board board, TraceNode& trace);
    Outcome solveEnvironment(Board afterstate, TraceNode& trace);
    void assignTraceId(TraceNode& trace);
    void visitedNode(Phase phase);
    bool lookupPlayerTransposition(Board board, TraceNode& trace);
    bool lookupEnvironmentTransposition(Board afterstate, TraceNode& trace);
    bool lookupRZone(Board board, Phase phase, TraceNode& trace);
    void resolveRZoneMatch(
        Phase phase, const RZoneMatch& match, TraceNode& trace);
    void annotateRZone(
        TraceNode& trace,
        RZoneCertificateId id,
        bool hit,
        std::uint8_t transform = 0);
    void createTerminalRZone(Board board, TraceNode& trace);
    void liftPlayerRZone(
        Board board, Direction direction, const TraceNode& child, TraceNode& trace);
    void liftEnvironmentRZone(Board board, TraceNode& trace);
    void storePlayerTransposition(Board board, const TraceNode& trace);
    void storeEnvironmentTransposition(Board afterstate, const TraceNode& trace);
    CanonicalBoard transpositionKey(Board board) const;
    static SearchAction transformAction(const SearchAction& action, std::uint8_t transform);
    double selectedSlideActionValue(Board board, Direction direction) const;
    bool atNodeLimit() const;

    const Game& game_;
    SolverOptions options_;
    ProgressCallback progress_;
    RZoneTable rzone_table_;
    SearchStatistics statistics_;
    std::uint64_t next_trace_id_ = 0;
    bool solve_started_ = false;
    bool solve_finished_ = false;
    std::unordered_map<std::uint64_t, TranspositionEntry> player_tt_;
    std::unordered_map<std::uint64_t, TranspositionEntry> environment_tt_;
};

const char* outcomeName(Outcome outcome);
const char* phaseName(Phase phase);
const char* terminalReasonName(TerminalReason reason);
const char* solvedByName(SolvedBy solved_by);

} // namespace solver2048
