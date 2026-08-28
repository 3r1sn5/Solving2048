#include "search.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
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
    : game_(game), options_(options), progress_(std::move(progress))
{
    if (options_.winning_tile < 2 || (options_.winning_tile & (options_.winning_tile - 1)) != 0) {
        throw std::invalid_argument("winning tile must be a power of two and at least 2");
    }
    if (options_.use_tt_symmetry && !options_.use_transposition_table) {
        throw std::invalid_argument("TT symmetry requires the transposition table to be enabled");
    }
}

SearchResult Solver::solve()
{
    statistics_ = {};
    next_trace_id_ = 0;
    player_tt_.clear();
    environment_tt_.clear();
    auto root = std::make_unique<TraceNode>();
    root->phase = Phase::Player;
    root->dfs_order = 0;

    SearchResult result;
    result.outcome = solvePlayer(game_.fixedOpening(), *root);
    result.statistics = statistics_;
    if (root->selected_action && root->selected_action->kind == SearchAction::Kind::Slide) {
        result.root_action = root->selected_action->direction;
        result.root_value = root->selected_value;
    }
    result.root = std::move(root);
    return result;
}

void Solver::assignTraceId(TraceNode& trace)
{
    if (trace.trace_id == 0) trace.trace_id = ++next_trace_id_;
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

double Solver::selectedSlideValue(Board board, Direction direction) const
{
    for (const SlideResult& slide : game_.legalSlides(board)) {
        if (slide.direction != direction) continue;
        const double value = game_.evaluateAfterstate(slide.afterstate);
        if (!std::isfinite(value)) {
            throw std::runtime_error("N-tuple evaluator returned a non-finite value");
        }
        return value;
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
    if (entry.outcome == Outcome::Unknown) {
        throw std::logic_error("Player TT contained an inexact Unknown entry");
    }
    ++statistics_.player_tt_hits;
    const bool symmetry_hit = options_.use_tt_symmetry && entry.source_transform != key.transform;
    if (symmetry_hit) ++statistics_.player_tt_symmetry_hits;
    trace.outcome = entry.outcome;
    trace.terminal_reason = TerminalReason::TranspositionTable;
    trace.solved_by = SolvedBy::TranspositionTable;
    trace.tt_hit = true;
    trace.tt_source_trace_id = entry.source_trace_id;
    trace.tt_symmetry_hit = symmetry_hit;
    if (options_.use_tt_symmetry) trace.tt_transform = key.transform;
    trace.children_total = entry.children_total;
    if (entry.selected_action) {
        trace.selected_action = transformAction(*entry.selected_action, inverseTransform(key.transform));
    }
    trace.selected_value = entry.selected_value;
    if (symmetry_hit && trace.selected_action &&
        trace.selected_action->kind == SearchAction::Kind::Slide) {
        trace.selected_value = selectedSlideValue(board, trace.selected_action->direction);
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
    if (entry.outcome == Outcome::Unknown) {
        throw std::logic_error("Environment TT contained an inexact Unknown entry");
    }
    ++statistics_.environment_tt_hits;
    const bool symmetry_hit = options_.use_tt_symmetry && entry.source_transform != key.transform;
    if (symmetry_hit) ++statistics_.environment_tt_symmetry_hits;
    trace.outcome = entry.outcome;
    trace.terminal_reason = TerminalReason::TranspositionTable;
    trace.solved_by = SolvedBy::TranspositionTable;
    trace.tt_hit = true;
    trace.tt_source_trace_id = entry.source_trace_id;
    trace.tt_symmetry_hit = symmetry_hit;
    if (options_.use_tt_symmetry) trace.tt_transform = key.transform;
    trace.children_total = entry.children_total;
    if (entry.selected_action) {
        trace.selected_action = transformAction(*entry.selected_action, inverseTransform(key.transform));
    }
    trace.selected_value = entry.selected_value;
    return true;
}

void Solver::storePlayerTransposition(Board board, const TraceNode& trace)
{
    if (!options_.use_transposition_table || trace.outcome == Outcome::Unknown) return;
    const CanonicalBoard key = transpositionKey(board);
    TranspositionEntry entry;
    entry.outcome = trace.outcome;
    entry.source_trace_id = trace.trace_id;
    entry.children_total = trace.children_total;
    if (trace.selected_action) entry.selected_action = transformAction(*trace.selected_action, key.transform);
    entry.selected_value = trace.selected_value;
    entry.source_transform = key.transform;
    const auto [found, inserted] = player_tt_.emplace(key.board.raw, std::move(entry));
    if (!inserted && found->second.outcome != trace.outcome) {
        throw std::logic_error("conflicting exact outcomes for one Player TT key");
    }
    if (inserted) ++statistics_.player_tt_stores;
}

void Solver::storeEnvironmentTransposition(Board afterstate, const TraceNode& trace)
{
    if (!options_.use_transposition_table || trace.outcome == Outcome::Unknown) return;
    const CanonicalBoard key = transpositionKey(afterstate);
    TranspositionEntry entry;
    entry.outcome = trace.outcome;
    entry.source_trace_id = trace.trace_id;
    entry.children_total = trace.children_total;
    if (trace.selected_action) entry.selected_action = transformAction(*trace.selected_action, key.transform);
    entry.selected_value = trace.selected_value;
    entry.source_transform = key.transform;
    const auto [found, inserted] = environment_tt_.emplace(key.board.raw, std::move(entry));
    if (!inserted && found->second.outcome != trace.outcome) {
        throw std::logic_error("conflicting exact outcomes for one Environment TT key");
    }
    if (inserted) ++statistics_.environment_tt_stores;
}

bool Solver::atNodeLimit() const
{
    return options_.node_limit != 0 && statistics_.combinedNodes() >= options_.node_limit;
}

void Solver::expandedNode()
{
    if (progress_ && options_.progress_interval != 0 &&
        statistics_.combinedNodes() % options_.progress_interval == 0) {
        progress_(statistics_);
    }
}

Outcome Solver::solvePlayer(Board board, TraceNode& trace)
{
    assignTraceId(trace);
    trace.phase = Phase::Player;
    if (game_.hasWinningTile(board, options_.winning_tile)) {
        trace.outcome = Outcome::Win;
        trace.terminal_reason = TerminalReason::WinningTile;
        trace.solved_by = SolvedBy::Terminal;
        return trace.outcome;
    }
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
    ++statistics_.player_nodes;
    expandedNode();

    std::vector<OrderedSlide> actions;
    for (const SlideResult& slide : game_.legalSlides(board)) {
        const double value = game_.evaluateAfterstate(slide.afterstate);
        if (!std::isfinite(value)) throw std::runtime_error("N-tuple evaluator returned a non-finite value");
        actions.push_back({slide, value});
    }
    std::sort(actions.begin(), actions.end(), [](const OrderedSlide& lhs, const OrderedSlide& rhs) {
        if (lhs.value != rhs.value) return lhs.value > rhs.value;
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
        child->value = action.value;
        assignTraceId(*child);

        Outcome child_outcome;
        if (game_.hasWinningTile(action.slide.afterstate, options_.winning_tile)) {
            child->outcome = Outcome::Win;
            child->terminal_reason = TerminalReason::WinningTile;
            child->solved_by = SolvedBy::Terminal;
            child_outcome = Outcome::Win;
        } else {
            child_outcome = solveEnvironment(action.slide.afterstate, *child);
        }
        trace.children.push_back(std::move(child));

        if (child_outcome == Outcome::Win) {
            trace.outcome = Outcome::Win;
            trace.selected_action = SearchAction::slide(action.slide.direction);
            trace.selected_value = action.value;
            for (std::size_t skipped = index + 1; skipped < actions.size(); ++skipped) {
                trace.skipped_actions.push_back(SearchAction::slide(actions[skipped].slide.direction));
            }
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
    assignTraceId(trace);
    trace.phase = Phase::Environment;
    if (game_.hasWinningTile(afterstate, options_.winning_tile)) {
        trace.outcome = Outcome::Win;
        trace.terminal_reason = TerminalReason::WinningTile;
        trace.solved_by = SolvedBy::Terminal;
        return trace.outcome;
    }
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
    ++statistics_.environment_nodes;
    expandedNode();

    const std::vector<PlacementResult> placements = game_.legalPlacements(afterstate);
    if (placements.empty()) {
        throw std::logic_error("a legal Player afterstate has no empty cell for Environment placement");
    }
    trace.children_total = placements.size();
    statistics_.placement_actions += placements.size();

    bool has_unknown = false;
    for (std::size_t index = 0; index < placements.size(); ++index) {
        const PlacementResult& placement = placements[index];
        auto child = std::make_unique<TraceNode>();
        child->phase = Phase::Player;
        child->dfs_order = index;
        child->incoming_action = SearchAction::placement(placement.position, placement.tile);
        const Outcome child_outcome = solvePlayer(placement.board, *child);
        trace.children.push_back(std::move(child));

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
        if (child_outcome == Outcome::Unknown) has_unknown = true;
    }

    trace.outcome = has_unknown ? Outcome::Unknown : Outcome::Win;
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
        case SolvedBy::TranspositionTable: return "TT";
        case SolvedBy::Backpropagation: return "BACKPROPAGATION";
        case SolvedBy::NodeLimit: return "NODE_LIMIT";
    }
    return "NONE";
}

} // namespace solver2048
