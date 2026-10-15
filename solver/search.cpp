#include "search.h"
#include "compact_trace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace solver2048 {

SearchAction SearchAction::slide(Direction value)
{
    SearchAction action;
    action.kind = Kind::Slide;
    action.direction = value;
    return action;
}

SearchAction SearchAction::placement(int value_position, unsigned value_tile)
{
    if (value_position < 0 || value_position >= 16 || (value_tile != 2 && value_tile != 4)) {
        throw std::invalid_argument("invalid Environment placement action");
    }
    SearchAction action;
    action.kind = Kind::Placement;
    action.position = value_position;
    action.tile = value_tile;
    return action;
}

int SearchAction::sgfId() const
{
    if (kind == Kind::Slide) return directionOpcode(direction);
    return (tile == 2 ? 4 : 20) + position;
}

char SearchAction::sgfPlayer() const
{
    return kind == Kind::Slide ? 'B' : 'W';
}

std::string SearchAction::name() const
{
    if (kind == Kind::Slide) return directionName(direction);
    return std::to_string(tile) + "@" + std::to_string(position);
}

std::string SearchAction::summaryToken() const
{
    return name() + "(" + std::to_string(sgfId()) + ")";
}

Solver::Solver(const Game& game, SolverOptions options, ProgressCallback progress)
    : game_(game),
      options_(options),
      progress_(std::move(progress)),
      rzone_table_(game,
                   options.winning_tile,
                   options.rzone_max_cells,
                   options.rzone_lookup_policy,
                   options.use_rzone_symmetry,
                   options.rzone_trimming,
                   options.rzone_full_board_placement_fallback)
{
    if (options_.winning_tile < 2 || (options_.winning_tile & (options_.winning_tile - 1)) != 0) {
        throw std::invalid_argument("winning tile must be a power of two and at least 2");
    }
    if (options_.use_tt_symmetry && !options_.use_transposition_table) {
        throw std::invalid_argument("TT symmetry requires the transposition table to be enabled");
    }
    if (options_.rzone_active_pruning && options_.rzone_mode != RZoneMode::Lookup) {
        throw std::invalid_argument("RZone active pruning requires lookup mode");
    }
}

void Solver::adoptPersistentProofs(Solver& completed)
{
    if (solve_started_ || !player_tt_.empty() || !environment_tt_.empty() ||
        !rzone_table_.certificates_.empty()) {
        throw std::logic_error("proofs can only be adopted by a fresh Solver");
    }
    if (!completed.solve_finished_) {
        throw std::logic_error("proofs can only be adopted from a completed Solver");
    }
    if (options_.winning_tile != completed.options_.winning_tile ||
        options_.use_transposition_table !=
            completed.options_.use_transposition_table ||
        options_.use_tt_symmetry != completed.options_.use_tt_symmetry ||
        options_.rzone_mode != completed.options_.rzone_mode ||
        options_.rzone_lookup_policy != completed.options_.rzone_lookup_policy ||
        options_.use_rzone_symmetry != completed.options_.use_rzone_symmetry ||
        options_.rzone_max_cells != completed.options_.rzone_max_cells) {
        throw std::invalid_argument(
            "cannot adopt proof tables across incompatible solver settings");
    }

    player_tt_ = std::move(completed.player_tt_);
    environment_tt_ = std::move(completed.environment_tt_);
    auto makePersistent = [](auto& table) {
        for (auto& [board, entry] : table) {
            (void)board;
            entry.source_trace_id = 0;
            entry.selected_action_value.reset();
            entry.source_transform = 0;
            entry.persistent = true;
        }
    };
    makePersistent(player_tt_);
    makePersistent(environment_tt_);

    rzone_table_.certificates_ =
        std::move(completed.rzone_table_.certificates_);
    // Moving the owning map with its default allocator preserves addresses of
    // its phase buckets, including those borrowed by the ordered mask index.
    rzone_table_.patterns_ = std::move(completed.rzone_table_.patterns_);
    rzone_table_.masks_by_phase_ =
        std::move(completed.rzone_table_.masks_by_phase_);
    rzone_table_.radix_nodes_ = std::move(completed.rzone_table_.radix_nodes_);
    rzone_table_.radix_roots_ = completed.rzone_table_.radix_roots_;
    completed.rzone_table_.radix_roots_ = {};
    rzone_table_.statistics_ = {};
    rzone_table_.statistics_.loaded_certificates =
        rzone_table_.certificates_.size();
    rzone_table_.statistics_.masks_indexed = rzone_table_.patterns_.size();
    rzone_table_.statistics_.radix_nodes = rzone_table_.radix_nodes_.empty()
        ? 0 : rzone_table_.radix_nodes_.size() - 1;
}

SearchResult Solver::solve()
{
    if (solve_started_) throw std::logic_error("a Solver instance can only solve once");
    solve_started_ = true;
    statistics_ = {};
    statistics_.player_tt_loaded_entries = player_tt_.size();
    statistics_.environment_tt_loaded_entries = environment_tt_.size();
    next_trace_id_ = 0;
    compact_trace_ = std::make_shared<CompactTrace>();
    auto root = std::make_unique<TraceNode>();
    root->phase = Phase::Player;
    root->dfs_order = 0;

    SearchResult result;
    result.outcome = solvePlayer(game_.fixedOpening(), *root);
    statistics_.rzone = rzone_table_.statistics();
    if (root->selected_action && root->selected_action->kind == SearchAction::Kind::Slide) {
        result.root_action = root->selected_action->direction;
        result.root_action_value = root->selected_action_value;
    }
    compact_trace_->seal(*root);
    // Restore the root's skipped-action summary, without expanding descendants.
    *root = compact_trace_->decode(root->trace_id);
    statistics_.trace_nodes = compact_trace_->nodes();
    statistics_.trace_storage_bytes = compact_trace_->capacityBytes();
    result.statistics = statistics_;
    result.compact_trace = std::move(compact_trace_);
    result.root = std::move(root);
    solve_finished_ = true;
    return result;
}

void Solver::annotateRZone(
    TraceNode& trace,
    RZoneCertificateId id,
    bool hit,
    std::uint8_t transform)
{
    const RZoneCertificate* certificate = rzone_table_.find(id);
    if (!certificate || !certificate->validated) {
        throw std::logic_error("attempted to attach an invalid RZone certificate");
    }
    trace.rzone_hit = hit;
    trace.rzone_symmetry_hit = transform != 0;
    trace.rzone_transform = transform;
    trace.rzone_certificate_id = id;
    trace.rzone_kind = certificate->kind;
    trace.rzone_mask = certificate->mask;
    trace.rzone_masked_board = certificate->masked_board;
    trace.rzone_depth = certificate->depth;
}

void Solver::createTerminalRZone(Board board, TraceNode& trace)
{
    if (options_.rzone_mode == RZoneMode::Disabled) return;
    const std::optional<RZoneCertificateId> id = rzone_table_.createTerminal(board, trace.phase);
    if (id) annotateRZone(trace, *id, false);
}

bool Solver::lookupRZone(Board board, Phase phase, TraceNode& trace)
{
    if (options_.rzone_mode != RZoneMode::Lookup) return false;
    const std::optional<RZoneMatch> match = rzone_table_.lookup(board, phase);
    if (!match) return false;
    resolveRZoneMatch(phase, *match, trace);
    return true;
}

void Solver::resolveRZoneMatch(
    Phase phase, const RZoneMatch& match, TraceNode& trace)
{
    trace.outcome = Outcome::Win;
    trace.terminal_reason = TerminalReason::None;
    trace.solved_by = SolvedBy::RZoneTable;
    annotateRZone(trace, match.id, true, match.transform);
    const RZoneCertificate* certificate = rzone_table_.find(match.id);
    if (phase == Phase::Player && certificate && certificate->slide) {
        trace.selected_action = SearchAction::slide(transformDirection(
            *certificate->slide,
            inverseTransform(match.transform)));
    }
}

void Solver::liftPlayerRZone(
    Board board, Direction direction, const TraceNode& child, TraceNode& trace)
{
    if (options_.rzone_mode == RZoneMode::Disabled || !child.rzone_certificate_id) return;
    const std::uint8_t transform = child.rzone_transform.value_or(0);
    const Board stored_parent = transform == 0 ? board : transformBoard(board, transform);
    const Direction stored_direction = transformDirection(direction, transform);
    const std::optional<RZoneCertificateId> id =
        rzone_table_.liftFixedSlide(stored_parent, stored_direction, *child.rzone_certificate_id);
    if (id) annotateRZone(trace, *id, false, transform);
}

void Solver::liftEnvironmentRZone(Board board, TraceNode& trace)
{
    if (options_.rzone_mode == RZoneMode::Disabled) return;
    std::optional<RZoneCertificateId> best_id;
    std::uint8_t best_transform = 0;
    unsigned best_cells = 17;
    unsigned best_depth = 0;
    std::unordered_set<std::uint64_t> attempted;
    for (const auto& child : trace.children) {
        if (!child->rzone_certificate_id) continue;
        const std::uint8_t transform = child->rzone_transform.value_or(0);
        const std::uint64_t attempt_key =
            (static_cast<std::uint64_t>(transform) << 32) | *child->rzone_certificate_id;
        if (!attempted.insert(attempt_key).second) continue;
        const Board stored_parent = transform == 0 ? board : transformBoard(board, transform);
        const std::optional<RZoneCertificateId> id =
            rzone_table_.liftUniversalPlacement(stored_parent, *child->rzone_certificate_id);
        if (!id) continue;
        const RZoneCertificate* candidate = rzone_table_.find(*id);
        if (!candidate) continue;
        const unsigned candidate_cells = RZoneTable::countCells(candidate->mask);
        if (!best_id || candidate_cells < best_cells ||
            (candidate_cells == best_cells && candidate->depth < best_depth) ||
            (candidate_cells == best_cells && candidate->depth == best_depth &&
             candidate->id < *best_id) ||
            (candidate_cells == best_cells && candidate->depth == best_depth &&
             candidate->id == *best_id && transform < best_transform)) {
            best_id = candidate->id;
            best_transform = transform;
            best_cells = candidate_cells;
            best_depth = candidate->depth;
        }
    }
    if (best_id) annotateRZone(trace, *best_id, false, best_transform);
}

void Solver::assignTraceId(TraceNode& trace)
{
    if (trace.trace_id != 0) return;
    if (next_trace_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("trace ID space exhausted");
    }
    trace.trace_id = ++next_trace_id_;
    compact_trace_->allocate(trace.trace_id);
}

void Solver::visitedNode(Phase phase)
{
    if (phase == Phase::Player) {
        ++statistics_.player_nodes;
    } else {
        ++statistics_.environment_nodes;
    }
    if (progress_ && options_.progress_interval != 0 &&
        statistics_.combinedNodes() % options_.progress_interval == 0) {
        statistics_.rzone = rzone_table_.statistics();
        statistics_.trace_nodes = compact_trace_->nodes();
        statistics_.trace_storage_bytes = compact_trace_->capacityBytes();
        progress_(statistics_);
    }
}

CanonicalBoard Solver::transpositionKey(Board board) const
{
    return options_.use_tt_symmetry ? canonicalize(board) : CanonicalBoard{board, 0};
}

SearchAction Solver::transformAction(const SearchAction& action, std::uint8_t transform)
{
    if (action.kind == SearchAction::Kind::Slide) {
        return SearchAction::slide(transformDirection(action.direction, transform));
    }
    return SearchAction::placement(transformPosition(action.position, transform), action.tile);
}

double Solver::selectedSlideActionValue(Board board, Direction direction) const
{
    for (const SlideResult& slide : game_.legalSlides(board)) {
        if (slide.direction != direction) continue;
        const double afterstate_value = game_.evaluateAfterstate(slide.afterstate);
        if (!std::isfinite(afterstate_value)) {
            throw std::runtime_error("N-tuple evaluator returned a non-finite value");
        }
        return static_cast<double>(slide.reward) + afterstate_value;
    }
    throw std::logic_error("symmetry TT restored an illegal Player slide witness");
}

bool Solver::lookupPlayerTransposition(Board board, TraceNode& trace)
{
    if (!options_.use_transposition_table) return false;
    const CanonicalBoard key = transpositionKey(board);
    const auto found = player_tt_.find(key.board.raw);
    if (found == player_tt_.end()) return false;

    const TranspositionEntry& entry = found->second;
    if (entry.outcome != Outcome::Win) {
        throw std::logic_error("Player TT contained a non-Win entry");
    }
    ++statistics_.player_tt_hits;
    if (entry.persistent) ++statistics_.player_persistent_tt_hits;
    const bool symmetry_hit = options_.use_tt_symmetry &&
        (entry.persistent ? key.transform != 0 : entry.source_transform != key.transform);
    if (symmetry_hit) ++statistics_.player_tt_symmetry_hits;
    trace.outcome = entry.outcome;
    trace.terminal_reason = TerminalReason::TranspositionTable;
    trace.solved_by = SolvedBy::TranspositionTable;
    trace.tt_hit = true;
    trace.tt_persistent_hit = entry.persistent;
    trace.tt_key = key.board.raw;
    if (!entry.persistent) trace.tt_source_trace_id = entry.source_trace_id;
    trace.tt_symmetry_hit = symmetry_hit;
    if (options_.use_tt_symmetry) trace.tt_transform = key.transform;
    trace.children_total = entry.children_total;
    if (entry.selected_action) {
        trace.selected_action = transformAction(*entry.selected_action, inverseTransform(key.transform));
    }
    trace.selected_action_value = entry.selected_action_value;
    if ((entry.persistent || symmetry_hit) && trace.selected_action &&
        trace.selected_action->kind == SearchAction::Kind::Slide) {
        trace.selected_action_value =
            selectedSlideActionValue(board, trace.selected_action->direction);
    }
    return true;
}

bool Solver::lookupEnvironmentTransposition(Board afterstate, TraceNode& trace)
{
    if (!options_.use_transposition_table) return false;
    const CanonicalBoard key = transpositionKey(afterstate);
    const auto found = environment_tt_.find(key.board.raw);
    if (found == environment_tt_.end()) return false;

    const TranspositionEntry& entry = found->second;
    if (entry.outcome != Outcome::Win) {
        throw std::logic_error("Environment TT contained a non-Win entry");
    }
    ++statistics_.environment_tt_hits;
    if (entry.persistent) ++statistics_.environment_persistent_tt_hits;
    const bool symmetry_hit = options_.use_tt_symmetry &&
        (entry.persistent ? key.transform != 0 : entry.source_transform != key.transform);
    if (symmetry_hit) ++statistics_.environment_tt_symmetry_hits;
    trace.outcome = entry.outcome;
    trace.terminal_reason = TerminalReason::TranspositionTable;
    trace.solved_by = SolvedBy::TranspositionTable;
    trace.tt_hit = true;
    trace.tt_persistent_hit = entry.persistent;
    trace.tt_key = key.board.raw;
    if (!entry.persistent) trace.tt_source_trace_id = entry.source_trace_id;
    trace.tt_symmetry_hit = symmetry_hit;
    if (options_.use_tt_symmetry) trace.tt_transform = key.transform;
    trace.children_total = entry.children_total;
    if (entry.selected_action) {
        trace.selected_action = transformAction(*entry.selected_action, inverseTransform(key.transform));
    }
    trace.selected_action_value = entry.selected_action_value;
    return true;
}

void Solver::storePlayerTransposition(Board board, const TraceNode& trace)
{
    if (!options_.use_transposition_table || trace.outcome != Outcome::Win) return;
    const CanonicalBoard key = transpositionKey(board);
    TranspositionEntry entry;
    entry.outcome = trace.outcome;
    entry.source_trace_id = trace.trace_id;
    entry.children_total = trace.children_total;
    if (trace.selected_action) entry.selected_action = transformAction(*trace.selected_action, key.transform);
    entry.selected_action_value = trace.selected_action_value;
    entry.source_transform = key.transform;
    const auto [found, inserted] = player_tt_.emplace(key.board.raw, std::move(entry));
    if (!inserted && found->second.outcome != Outcome::Win) {
        throw std::logic_error("Player TT contains a non-Win entry");
    }
    if (inserted) ++statistics_.player_tt_stores;
}

void Solver::storeEnvironmentTransposition(Board afterstate, const TraceNode& trace)
{
    if (!options_.use_transposition_table || trace.outcome != Outcome::Win) return;
    const CanonicalBoard key = transpositionKey(afterstate);
    TranspositionEntry entry;
    entry.outcome = trace.outcome;
    entry.source_trace_id = trace.trace_id;
    entry.children_total = trace.children_total;
    if (trace.selected_action) entry.selected_action = transformAction(*trace.selected_action, key.transform);
    entry.selected_action_value = trace.selected_action_value;
    entry.source_transform = key.transform;
    const auto [found, inserted] = environment_tt_.emplace(key.board.raw, std::move(entry));
    if (!inserted && found->second.outcome != Outcome::Win) {
        throw std::logic_error("Environment TT contains a non-Win entry");
    }
    if (inserted) ++statistics_.environment_tt_stores;
}

bool Solver::atNodeLimit() const
{
    return options_.node_limit != 0 &&
           statistics_.combinedNodes() > options_.node_limit;
}

Outcome Solver::solvePlayer(Board board, TraceNode& trace)
{
    trace.phase = Phase::Player;
    assignTraceId(trace);
    visitedNode(Phase::Player);
    if (game_.hasWinningTile(board, options_.winning_tile)) {
        trace.outcome = Outcome::Win;
        trace.terminal_reason = TerminalReason::WinningTile;
        trace.solved_by = SolvedBy::Terminal;
        createTerminalRZone(board, trace);
        return trace.outcome;
    }
    if (lookupRZone(board, Phase::Player, trace)) return trace.outcome;
    if (lookupPlayerTransposition(board, trace)) return trace.outcome;
    if (atNodeLimit()) {
        ++statistics_.node_limit_cutoffs;
        trace.outcome = Outcome::Unknown;
        trace.terminal_reason = TerminalReason::NodeLimit;
        trace.solved_by = SolvedBy::NodeLimit;
        return trace.outcome;
    }

    trace.expanded = true;
    trace.solved_by = SolvedBy::Backpropagation;

    std::vector<OrderedSlide> actions;
    for (const SlideResult& slide : game_.legalSlides(board)) {
        const double afterstate_value = game_.evaluateAfterstate(slide.afterstate);
        if (!std::isfinite(afterstate_value)) {
            throw std::runtime_error("N-tuple evaluator returned a non-finite value");
        }
        const double action_value = static_cast<double>(slide.reward) + afterstate_value;
        actions.push_back({slide, afterstate_value, action_value});
    }
    std::sort(actions.begin(), actions.end(), [](const OrderedSlide& lhs, const OrderedSlide& rhs) {
        if (lhs.action_value != rhs.action_value) return lhs.action_value > rhs.action_value;
        return directionOpcode(lhs.slide.direction) < directionOpcode(rhs.slide.direction);
    });

    trace.children_total = actions.size();
    statistics_.player_actions += actions.size();
    if (actions.empty()) {
        trace.outcome = Outcome::Loss;
        trace.terminal_reason = TerminalReason::NoLegalMove;
        trace.solved_by = SolvedBy::Terminal;
        storePlayerTransposition(board, trace);
        return trace.outcome;
    }

    bool has_unknown = false;
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const OrderedSlide& action = actions[index];
        auto child = std::make_unique<TraceNode>();
        child->phase = Phase::Environment;
        child->dfs_order = index;
        child->incoming_action = SearchAction::slide(action.slide.direction);
        child->reward = action.slide.reward;
        child->afterstate_value = action.afterstate_value;
        child->action_value = action.action_value;
        assignTraceId(*child);

        Outcome child_outcome;
        if (game_.hasWinningTile(action.slide.afterstate, options_.winning_tile)) {
            // This terminal child is checked inline instead of entering
            // solveEnvironment(), but it is still a visited search node.
            visitedNode(Phase::Environment);
            child->outcome = Outcome::Win;
            child->terminal_reason = TerminalReason::WinningTile;
            child->solved_by = SolvedBy::Terminal;
            createTerminalRZone(action.slide.afterstate, *child);
            child_outcome = Outcome::Win;
        } else {
            child_outcome = solveEnvironment(action.slide.afterstate, *child);
        }
        compact_trace_->seal(*child);
        trace.children.push_back(std::move(child));

        if (child_outcome == Outcome::Win) {
            trace.outcome = Outcome::Win;
            trace.selected_action = SearchAction::slide(action.slide.direction);
            trace.selected_action_value = action.action_value;
            for (std::size_t skipped = index + 1; skipped < actions.size(); ++skipped) {
                trace.skipped_actions.push_back(SearchAction::slide(actions[skipped].slide.direction));
            }
            liftPlayerRZone(board, action.slide.direction, *trace.children.back(), trace);
            storePlayerTransposition(board, trace);
            return trace.outcome;
        }
        if (child_outcome == Outcome::Unknown) has_unknown = true;
    }

    trace.outcome = has_unknown ? Outcome::Unknown : Outcome::Loss;
    storePlayerTransposition(board, trace);
    return trace.outcome;
}

Outcome Solver::solveEnvironment(Board afterstate, TraceNode& trace)
{
    trace.phase = Phase::Environment;
    assignTraceId(trace);
    visitedNode(Phase::Environment);
    if (game_.hasWinningTile(afterstate, options_.winning_tile)) {
        trace.outcome = Outcome::Win;
        trace.terminal_reason = TerminalReason::WinningTile;
        trace.solved_by = SolvedBy::Terminal;
        createTerminalRZone(afterstate, trace);
        return trace.outcome;
    }
    if (lookupRZone(afterstate, Phase::Environment, trace)) return trace.outcome;
    if (lookupEnvironmentTransposition(afterstate, trace)) return trace.outcome;
    if (atNodeLimit()) {
        ++statistics_.node_limit_cutoffs;
        trace.outcome = Outcome::Unknown;
        trace.terminal_reason = TerminalReason::NodeLimit;
        trace.solved_by = SolvedBy::NodeLimit;
        return trace.outcome;
    }

    trace.expanded = true;
    trace.solved_by = SolvedBy::Backpropagation;

    const std::vector<PlacementResult> placements = game_.legalPlacements(afterstate);
    if (placements.empty()) {
        throw std::logic_error("a legal Player afterstate has no empty cell for Environment placement");
    }
    trace.children_total = placements.size();
    statistics_.placement_actions += placements.size();

    bool has_unknown = false;
    std::vector<std::optional<RZoneMatch>> active_matches(placements.size());
    std::unordered_set<std::uint64_t> observed_certificates;
    bool active_pruning_started = false;
    bool active_pruning_hit_used = false;
    std::optional<RZoneCertificateId> active_parent_certificate;
    std::uint8_t active_parent_transform = 0;
    unsigned active_parent_cells = 17;
    unsigned active_parent_depth = 0;
    auto considerActiveParent = [&](RZoneCertificateId id, std::uint8_t transform) {
        const RZoneCertificate* candidate = rzone_table_.find(id);
        if (!candidate) return;
        const unsigned candidate_cells = RZoneTable::countCells(candidate->mask);
        if (!active_parent_certificate || candidate_cells < active_parent_cells ||
            (candidate_cells == active_parent_cells &&
             candidate->depth < active_parent_depth) ||
            (candidate_cells == active_parent_cells &&
             candidate->depth == active_parent_depth && id < *active_parent_certificate) ||
            (candidate_cells == active_parent_cells &&
             candidate->depth == active_parent_depth && id == *active_parent_certificate &&
             transform < active_parent_transform)) {
            active_parent_certificate = id;
            active_parent_transform = transform;
            active_parent_cells = candidate_cells;
            active_parent_depth = candidate->depth;
        }
    };

    for (std::size_t index = 0; index < placements.size(); ++index) {
        const PlacementResult& placement = placements[index];
        auto child = std::make_unique<TraceNode>();
        child->phase = Phase::Player;
        child->dfs_order = index;
        child->incoming_action = SearchAction::placement(placement.position, placement.tile);
        Outcome child_outcome = Outcome::Unknown;
        if (options_.rzone_active_pruning && active_matches[index]) {
            assignTraceId(*child);
            resolveRZoneMatch(Phase::Player, *active_matches[index], *child);
            rzone_table_.recordActivePruningHit(Phase::Player, *active_matches[index]);
            active_pruning_hit_used = true;
            child_outcome = child->outcome;
        } else {
            child_outcome = solvePlayer(placement.board, *child);
        }
        compact_trace_->seal(*child);
        trace.children.push_back(std::move(child));
        const TraceNode& resolved_child = *trace.children.back();

        if (child_outcome == Outcome::Loss) {
            trace.outcome = Outcome::Loss;
            trace.selected_action = SearchAction::placement(placement.position, placement.tile);
            for (std::size_t skipped = index + 1; skipped < placements.size(); ++skipped) {
                trace.skipped_actions.push_back(
                    SearchAction::placement(placements[skipped].position, placements[skipped].tile));
            }
            storeEnvironmentTransposition(afterstate, trace);
            return trace.outcome;
        }
        if (child_outcome == Outcome::Unknown) {
            has_unknown = true;
            continue;
        }

        if (!options_.rzone_active_pruning || !resolved_child.rzone_certificate_id) {
            continue;
        }
        const std::uint8_t source_transform = resolved_child.rzone_transform.value_or(0);
        const std::uint64_t attempt_key =
            (static_cast<std::uint64_t>(source_transform) << 32) |
            *resolved_child.rzone_certificate_id;
        if (!observed_certificates.insert(attempt_key).second) continue;

        if (!active_pruning_started) {
            rzone_table_.recordActivePruningAndNode();
            active_pruning_started = true;
        }

        const Board stored_parent = source_transform == 0
            ? afterstate
            : transformBoard(afterstate, source_transform);
        const std::optional<RZoneCertificateId> lifted =
            rzone_table_.liftUniversalPlacement(
                stored_parent, *resolved_child.rzone_certificate_id);
        rzone_table_.recordActivePruningUniversalLift(lifted.has_value());
        if (lifted) considerActiveParent(*lifted, source_transform);

        for (std::size_t sibling = index + 1; sibling < placements.size(); ++sibling) {
            if (active_matches[sibling] ||
                game_.hasWinningTile(placements[sibling].board, options_.winning_tile)) {
                continue;
            }
            const std::optional<RZoneMatch> match = rzone_table_.matchCertificate(
                placements[sibling].board,
                Phase::Player,
                *resolved_child.rzone_certificate_id,
                source_transform);
            if (match) active_matches[sibling] = *match;
        }
    }

    trace.outcome = has_unknown ? Outcome::Unknown : Outcome::Win;
    if (trace.outcome == Outcome::Win) {
        if (options_.rzone_active_pruning) {
            if (active_parent_certificate) {
                annotateRZone(
                    trace, *active_parent_certificate, false, active_parent_transform);
            }
            if (active_pruning_hit_used) rzone_table_.recordActivePruningAndNodeClosed();
        } else {
            liftEnvironmentRZone(afterstate, trace);
        }
        if (options_.rzone_mode != RZoneMode::Disabled &&
            options_.rzone_full_board_placement_fallback && !trace.rzone_certificate_id) {
            std::vector<RZoneMatch> witnesses;
            for (const auto& child : trace.children) {
                if (!child->rzone_certificate_id) break;
                witnesses.push_back({*child->rzone_certificate_id,
                                     child->rzone_transform.value_or(0)});
            }
            if (witnesses.size() == placements.size()) {
                const auto id = rzone_table_.liftFullBoardPlacement(afterstate, witnesses);
                if (id) annotateRZone(trace, *id, false);
            }
        }
    }
    storeEnvironmentTransposition(afterstate, trace);
    return trace.outcome;
}

const char* outcomeName(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Win: return "WIN";
        case Outcome::Loss: return "LOSS";
        case Outcome::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* phaseName(Phase phase)
{
    return phase == Phase::Player ? "PLAYER" : "ENVIRONMENT";
}

const char* terminalReasonName(TerminalReason reason)
{
    switch (reason) {
        case TerminalReason::None: return "NONE";
        case TerminalReason::WinningTile: return "WINNING_TILE";
        case TerminalReason::NoLegalMove: return "NO_LEGAL_MOVE";
        case TerminalReason::NodeLimit: return "NODE_LIMIT";
        case TerminalReason::TranspositionTable: return "TRANSPOSITION_TABLE";
    }
    return "NONE";
}

const char* solvedByName(SolvedBy solved_by)
{
    switch (solved_by) {
        case SolvedBy::None: return "NONE";
        case SolvedBy::Terminal: return "TERMINAL";
        case SolvedBy::RZoneTable: return "RZONE_TT";
        case SolvedBy::TranspositionTable: return "TT";
        case SolvedBy::Backpropagation: return "BACKPROPAGATION";
        case SolvedBy::NodeLimit: return "NODE_LIMIT";
    }
    return "NONE";
}

} // namespace solver2048
