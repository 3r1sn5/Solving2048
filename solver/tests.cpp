#include "game_api.h"
#include "search.h"
#include "sgf.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using solver2048::Board;
using solver2048::Direction;
using solver2048::Game;
using solver2048::GameApi;
using solver2048::Outcome;
using solver2048::Phase;
using solver2048::PlacementResult;
using solver2048::SearchAction;
using solver2048::SearchResult;
using solver2048::SgfMetadata;
using solver2048::SgfWriter;
using solver2048::SlideResult;
using solver2048::Solver;
using solver2048::SolverOptions;
using solver2048::TerminalReason;
using solver2048::TraceNode;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void check(bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

void checkNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        std::ostringstream detail;
        detail << message << ": expected " << expected << ", got " << actual;
        fail(detail.str());
    }
}

template <typename Function>
void checkThrows(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    fail(message);
}

Board board(std::uint64_t id)
{
    return Board{id};
}

SlideResult slide(Direction direction, std::uint64_t afterstate, int reward = 0)
{
    return SlideResult{direction, board(afterstate), reward};
}

PlacementResult placement(int position, unsigned tile, std::uint64_t child)
{
    return PlacementResult{position, tile, board(child)};
}

class FakeGame final : public Game {
public:
    explicit FakeGame(std::uint64_t opening = 1) : opening_(board(opening)) {}

    Board fixedOpening() const override { return opening_; }

    bool hasWinningTile(Board value, unsigned) const override
    {
        return winning_states.count(value.raw) != 0;
    }

    std::vector<SlideResult> legalSlides(Board value) const override
    {
        ++slide_calls[value.raw];
        const auto found = slides.find(value.raw);
        return found == slides.end() ? std::vector<SlideResult>{} : found->second;
    }

    std::vector<PlacementResult> legalPlacements(Board value) const override
    {
        ++placement_calls[value.raw];
        const auto found = placements.find(value.raw);
        return found == placements.end() ? std::vector<PlacementResult>{} : found->second;
    }

    double evaluateAfterstate(Board value) const override
    {
        evaluated_afterstates.push_back(value.raw);
        const auto found = values.find(value.raw);
        if (found == values.end()) fail("FakeGame has no value for afterstate " + std::to_string(value.raw));
        return found->second;
    }

    std::array<unsigned, 16> tileValues(Board) const override { return {}; }

    Board opening_;
    std::unordered_map<std::uint64_t, std::vector<SlideResult>> slides;
    std::unordered_map<std::uint64_t, std::vector<PlacementResult>> placements;
    std::unordered_map<std::uint64_t, double> values;
    std::unordered_set<std::uint64_t> winning_states;
    mutable std::unordered_map<std::uint64_t, std::size_t> slide_calls;
    mutable std::unordered_map<std::uint64_t, std::size_t> placement_calls;
    mutable std::vector<std::uint64_t> evaluated_afterstates;
};

Direction incomingDirection(const TraceNode& node)
{
    check(node.incoming_action.has_value(), "trace child is missing its incoming action");
    check(node.incoming_action->kind == SearchAction::Kind::Slide,
          "trace child incoming action is not a slide");
    return node.incoming_action->direction;
}

void checkTraceIdsAreDfsDiscoveryOrder(const TraceNode& root)
{
    std::uint64_t expected_id = 1;
    std::unordered_set<std::uint64_t> discovered;
    std::function<void(const TraceNode&)> visit = [&](const TraceNode& node) {
        check(node.trace_id != 0, "every emitted TraceNode must have a nonzero trace ID");
        check(discovered.insert(node.trace_id).second,
              "TraceNode IDs must be unique; duplicate " + std::to_string(node.trace_id));
        check(node.trace_id == expected_id,
              "TraceNode ID must equal DFS discovery order; expected " +
                  std::to_string(expected_id) + ", got " + std::to_string(node.trace_id));

        if (node.tt_hit) {
            check(node.tt_source_trace_id.has_value(),
                  "every TT hit must reference its source TraceNode");
            check(*node.tt_source_trace_id < node.trace_id,
                  "TT source ID must precede its hit in DFS discovery order");
            check(discovered.count(*node.tt_source_trace_id) == 1,
                  "TT source must already be discoverable earlier in the emitted tree");
        } else {
            check(!node.tt_source_trace_id,
                  "a non-TT TraceNode must not carry a TT source reference");
        }

        ++expected_id;
        for (const auto& child : node.children) visit(*child);
    };
    visit(root);
    check(discovered.size() == expected_id - 1,
          "DFS trace-ID validation did not visit every emitted TraceNode exactly once");
}

void testPlayerOrUnknownWinAndSkipped()
{
    FakeGame game;
    game.slides[1] = {
        slide(Direction::Up, 10, 1000),
        slide(Direction::Right, 11, 0),
        slide(Direction::Down, 12, 999999),
    };
    game.values = {{10, 30.0}, {11, 20.0}, {12, 10.0}};
    game.winning_states.insert(11);

    SearchResult result = Solver(game, SolverOptions{8, 1, 0}).solve();

    check(result.outcome == Outcome::Win, "Player OR must find a Win after an Unknown child");
    check(result.root_action == Direction::Right, "Player OR selected the wrong winning action");
    checkNear(*result.root_value, 20.0, 0.0, "selected value must be V(afterstate)");
    check(result.statistics.player_nodes == 1, "node-limit OR test player node count mismatch");
    check(result.statistics.environment_nodes == 0, "node-limit OR test environment node count mismatch");
    check(result.statistics.player_actions == 3, "all Player actions must be generated before DFS");
    check(result.statistics.node_limit_cutoffs == 1, "first Environment child must be one cutoff");

    const TraceNode& root = *result.root;
    check(root.children_total == 3, "Player trace must record all legal actions");
    check(root.children.size() == 2, "Player trace must retain Unknown and decisive Win children");
    check(root.children[0]->outcome == Outcome::Unknown,
          "first visited Player child must be Unknown, not skipped");
    check(root.children[0]->terminal_reason == TerminalReason::NodeLimit,
          "Unknown child must record NODE_LIMIT");
    check(root.children[1]->outcome == Outcome::Win,
          "second visited Player child must be the immediate Win");
    check(root.skipped_actions.size() == 1,
          "only the action after the decisive Win should be skipped");
    check(root.skipped_actions[0].direction == Direction::Down,
          "Player trace recorded the wrong skipped action");
}

void testEnvironmentAndAllWin()
{
    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 1.0;
    game.placements[10] = {
        placement(0, 2, 20),
        placement(0, 4, 21),
        placement(1, 2, 22),
    };
    game.winning_states = {20, 21, 22};

    SearchResult result = Solver(game, SolverOptions{}).solve();

    check(result.outcome == Outcome::Win, "Environment AND must be Win when every placement is Win");
    check(result.statistics.player_nodes == 1, "terminal winning children must not count as Player nodes");
    check(result.statistics.environment_nodes == 1, "Environment AND node count mismatch");
    check(result.statistics.placement_actions == 3, "Environment must generate every placement action");
    const TraceNode& environment = *result.root->children[0];
    check(environment.children.size() == 3, "Environment Win requires visiting every placement child");
    check(environment.skipped_actions.empty(), "Environment Win cannot skip a placement child");
    for (const auto& child : environment.children) {
        check(child->outcome == Outcome::Win, "all Environment AND children should be Win");
    }
}

void testEnvironmentAndLossShortCircuit()
{
    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 1.0;
    game.placements[10] = {
        placement(0, 2, 20),
        placement(0, 4, 21),
        placement(1, 2, 22),
    };
    game.winning_states.insert(20);

    SearchResult result = Solver(game, SolverOptions{}).solve();

    check(result.outcome == Outcome::Loss,
          "one losing placement must make Environment AND a Player Loss");
    check(result.statistics.player_nodes == 2, "dead placement child should be expanded exactly once");
    check(result.statistics.environment_nodes == 1, "Environment loss node count mismatch");
    check(result.statistics.placement_actions == 3,
          "Environment action count includes children skipped after short-circuit");
    const TraceNode& environment = *result.root->children[0];
    check(environment.children.size() == 2,
          "Environment must stop recursively visiting children after the first Loss");
    check(environment.children[0]->outcome == Outcome::Win,
          "Environment must continue after a Player Win child");
    check(environment.children[1]->outcome == Outcome::Loss,
          "second Environment child should be the decisive Loss");
    check(environment.children[1]->terminal_reason == TerminalReason::NoLegalMove,
          "dead Player child should record NO_LEGAL_MOVE");
    check(environment.skipped_actions.size() == 1,
          "Environment must record the unvisited placement as skipped");
    check(environment.skipped_actions[0].position == 1 && environment.skipped_actions[0].tile == 2,
          "Environment recorded the wrong skipped placement");
}

void testEnvironmentUnknownIsNotShortCircuit()
{
    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 1.0;
    game.placements[10] = {
        placement(0, 2, 20),
        placement(0, 4, 21),
        placement(1, 2, 22),
    };
    game.winning_states = {20, 22};

    SearchResult result = Solver(game, SolverOptions{8, 2, 0}).solve();

    check(result.outcome == Outcome::Unknown,
          "Environment with Win, Unknown, Win children must be Unknown");
    check(result.statistics.player_nodes == 1 && result.statistics.environment_nodes == 1,
          "combined node limit must count Player and Environment nodes");
    check(result.statistics.node_limit_cutoffs == 1,
          "only the non-winning placement at the limit should be a cutoff");
    const TraceNode& environment = *result.root->children[0];
    check(environment.children.size() == 3,
          "Environment must continue traversing after an Unknown placement child");
    check(environment.children[1]->outcome == Outcome::Unknown,
          "middle Environment child should be Unknown");
    check(environment.children[2]->outcome == Outcome::Win,
          "winning-tile check must still identify the later Win child");
    check(environment.skipped_actions.empty(), "Unknown must not be recorded as skipped");
}

void testValueOrderingAndDirectionTieBreak()
{
    FakeGame game;
    // Deliberately return directions out of opcode order. Equal values must still
    // use Up, Right, Down, Left as the deterministic tie-break.
    game.slides[1] = {
        slide(Direction::Left, 10, 1000000),
        slide(Direction::Up, 11, 999999),
        slide(Direction::Right, 12, -1000000),
        slide(Direction::Down, 13, 5000000),
    };
    game.values = {{10, 50.0}, {11, 10.0}, {12, 50.0}, {13, 30.0}};
    game.placements[10] = {placement(0, 2, 20)};
    game.placements[11] = {placement(0, 2, 21)};
    game.placements[12] = {placement(0, 2, 22)};
    game.placements[13] = {placement(0, 2, 23)};

    SearchResult result = Solver(game, SolverOptions{}).solve();

    check(result.outcome == Outcome::Loss, "ordering graph should visit all four losing actions");
    check(result.root->children.size() == 4, "ordering graph did not visit every Player action");
    const std::array<Direction, 4> expected = {
        Direction::Right,
        Direction::Left,
        Direction::Down,
        Direction::Up,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        check(incomingDirection(*result.root->children[index]) == expected[index],
              "Player DFS order must be descending V(afterstate), then direction opcode");
    }
    checkNear(*result.root->children[0]->value, 50.0, 0.0, "first action value mismatch");
    checkNear(*result.root->children[2]->value, 30.0, 0.0, "third action value mismatch");
}

void testNoCacheExpandsRepeatedStates()
{
    FakeGame game;
    game.slides[1] = {
        slide(Direction::Right, 10),
        slide(Direction::Left, 10),
    };
    game.values[10] = 7.0;
    game.placements[10] = {placement(0, 2, 20)};

    SearchResult result = Solver(game, SolverOptions{8, 0, 0, false}).solve();

    check(result.outcome == Outcome::Loss, "repeated-state graph should be Loss");
    check(result.statistics.player_nodes == 3,
          "root and repeated dead Player state must be expanded twice");
    check(result.statistics.environment_nodes == 2,
          "same Environment afterstate must be expanded twice without cache");
    check(result.statistics.player_actions == 2, "repeated-state Player action count mismatch");
    check(result.statistics.placement_actions == 2, "repeated-state placement action count mismatch");
    check(game.placement_calls[10] == 2,
          "Environment state was not re-expanded on its second occurrence");
    check(game.slide_calls[20] == 2,
          "Player state was not re-expanded on its second occurrence");
    check(result.root->children[0].get() != result.root->children[1].get(),
          "repeated states must have separate trace nodes");
    check(result.statistics.ttHits() == 0 && result.statistics.ttStores() == 0,
          "TT-disabled baseline must not report TT hits or stores");
    for (const auto& environment : result.root->children) {
        check(!environment->tt_hit && !environment->tt_source_trace_id,
              "TT-disabled Environment trace must not contain a TT annotation");
        check(environment->children.size() == 1 && !environment->children[0]->tt_hit &&
                  !environment->children[0]->tt_source_trace_id,
              "TT-disabled Player trace must not contain a TT annotation");
    }
}

void testPlayerTranspositionHit()
{
    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 1.0;
    game.placements[10] = {
        placement(0, 2, 20),
        placement(0, 4, 20),
    };
    game.slides[20] = {slide(Direction::Right, 30)};
    game.values[30] = 5.0;
    game.winning_states.insert(30);

    // The duplicate is reached exactly when the combined budget is exhausted.
    // A TT lookup must happen before the node-limit check, so the cached exact
    // result remains usable without consuming another expanded node.
    SearchResult result = Solver(game, SolverOptions{8, 3, 0, true}).solve();

    check(result.outcome == Outcome::Win,
          "two identical winning Player states should make Environment AND a Win");
    check(result.statistics.player_nodes == 2,
          "TT must expand the repeated Player state only once");
    check(result.statistics.environment_nodes == 1,
          "Player TT graph should expand one Environment node");
    check(result.statistics.player_tt_hits == 1 && result.statistics.environment_tt_hits == 0,
          "repeated Player state must produce exactly one Player TT hit");
    check(result.statistics.node_limit_cutoffs == 0,
          "an exact TT hit at the node-limit boundary must not become Unknown");
    check(result.statistics.player_tt_stores == 2 &&
              result.statistics.environment_tt_stores == 1,
          "exact Player TT graph store counts are wrong");
    check(game.slide_calls[20] == 1,
          "Player TT hit must avoid regenerating the repeated state's slides");

    const TraceNode& environment = *result.root->children[0];
    check(environment.children.size() == 2,
          "Environment AND must retain both repeated Player variations");
    const TraceNode& source = *environment.children[0];
    const TraceNode& hit = *environment.children[1];
    check(source.expanded && !source.tt_hit,
          "first Player occurrence must be the expanded TT source");
    check(hit.outcome == Outcome::Win && hit.tt_hit && !hit.expanded,
          "second Player occurrence must be an unexpanded winning TT hit");
    check(hit.terminal_reason == TerminalReason::TranspositionTable,
          "Player TT hit must record TRANSPOSITION_TABLE as its terminal reason");
    check(hit.tt_source_trace_id && *hit.tt_source_trace_id == source.trace_id,
          "Player TT hit must point to the first occurrence's trace ID");
    check(hit.trace_id != 0 && hit.trace_id != source.trace_id,
          "TT hit must remain a distinct, addressable trace node");
    check(hit.selected_action && hit.selected_action->kind == SearchAction::Kind::Slide &&
              hit.selected_action->direction == Direction::Right,
          "Player TT hit must restore the cached winning action");
    check(hit.selected_value && *hit.selected_value == 5.0,
          "Player TT hit must restore the cached action value");
    check(hit.children.empty(), "TT hit trace must not duplicate the cached subtree");
    check(source.trace_id < hit.trace_id,
          "Player TT source must be discovered before its reference node");
    checkTraceIdsAreDfsDiscoveryOrder(*result.root);
}

FakeGame environmentTranspositionGame()
{
    FakeGame game;
    game.slides[1] = {
        slide(Direction::Up, 10),
        slide(Direction::Right, 10),
        slide(Direction::Down, 11),
    };
    game.values = {{10, 30.0}, {11, 10.0}};
    game.placements[10] = {placement(0, 2, 20)};
    game.winning_states.insert(11);
    return game;
}

void testEnvironmentTranspositionHitAndOutcomeConsistency()
{
    FakeGame baseline_game = environmentTranspositionGame();
    SearchResult baseline = Solver(baseline_game, SolverOptions{8, 0, 0, false}).solve();

    FakeGame tt_game = environmentTranspositionGame();
    SearchResult cached = Solver(tt_game, SolverOptions{8, 0, 0, true}).solve();

    check(baseline.outcome == Outcome::Win && cached.outcome == baseline.outcome,
          "TT on/off must produce the same root outcome");
    check(baseline.root_action == Direction::Down && cached.root_action == baseline.root_action,
          "TT on/off must select the same proven winning root action");
    check(baseline.root_value && cached.root_value && *baseline.root_value == *cached.root_value,
          "TT on/off must preserve the selected root action value");
    check(baseline.statistics.environment_nodes == 2 &&
              baseline_game.placement_calls[10] == 2,
          "TT-off baseline must re-expand the repeated Environment state");
    check(baseline.statistics.player_nodes == 3,
          "TT-off baseline must re-expand the repeated dead Player child");
    check(baseline.statistics.ttHits() == 0 && baseline.statistics.ttStores() == 0,
          "TT-off outcome comparison must not use either table");

    check(cached.statistics.environment_nodes == 1 && tt_game.placement_calls[10] == 1,
          "Environment TT hit must avoid regenerating placements");
    check(cached.statistics.player_nodes == 2,
          "Environment TT hit must avoid revisiting the cached subtree");
    check(cached.statistics.environment_tt_hits == 1 &&
              cached.statistics.player_tt_hits == 0,
          "repeated Environment afterstate must produce exactly one Environment TT hit");
    check(cached.statistics.environment_tt_stores == 1 &&
              cached.statistics.player_tt_stores == 2,
          "exact Environment TT graph store counts are wrong");

    check(cached.root->children.size() == 3,
          "Player must visit two losing actions before the final winning action");
    const TraceNode& source = *cached.root->children[0];
    const TraceNode& hit = *cached.root->children[1];
    check(source.expanded && source.outcome == Outcome::Loss && !source.tt_hit,
          "first Environment occurrence must be the expanded losing TT source");
    check(hit.outcome == Outcome::Loss && hit.tt_hit && !hit.expanded && hit.children.empty(),
          "second Environment occurrence must be a compact losing TT hit");
    check(hit.terminal_reason == TerminalReason::TranspositionTable,
          "Environment TT hit must record TRANSPOSITION_TABLE as its terminal reason");
    check(hit.tt_source_trace_id && *hit.tt_source_trace_id == source.trace_id,
          "Environment TT hit must point to the first occurrence's trace ID");
    check(hit.selected_action && hit.selected_action->kind == SearchAction::Kind::Placement &&
              hit.selected_action->position == 0 && hit.selected_action->tile == 2,
          "Environment TT hit must restore its decisive placement");
    check(source.trace_id < hit.trace_id,
          "Environment TT source must be discovered before its reference node");
    checkTraceIdsAreDfsDiscoveryOrder(*cached.root);
}

void testUnknownOutcomesAreNotCached()
{
    FakeGame game;
    game.slides[1] = {
        slide(Direction::Right, 10),
        slide(Direction::Left, 10),
    };
    game.values[10] = 1.0;
    game.placements[10] = {placement(0, 2, 20)};

    SearchResult result = Solver(game, SolverOptions{8, 2, 0, true}).solve();

    check(result.outcome == Outcome::Unknown,
          "node-limited repeated state must remain Unknown with TT enabled");
    check(result.statistics.player_nodes == 1 && result.statistics.environment_nodes == 1,
          "Unknown TT graph must stop at the combined-node limit");
    check(result.statistics.node_limit_cutoffs == 2,
          "Unknown TT graph must record both attempted cutoff children");
    check(result.statistics.ttHits() == 0,
          "an Unknown first occurrence must not create a later TT hit");
    check(result.statistics.ttStores() == 0,
          "Unknown outcomes must never be stored in either transposition table");
    check(game.placement_calls[10] == 1,
          "first Environment occurrence should expand before returning Unknown");
    check(result.root->children.size() == 2,
          "Player must retain both Unknown repeated Environment variations");

    const TraceNode& first = *result.root->children[0];
    const TraceNode& second = *result.root->children[1];
    check(first.expanded && first.outcome == Outcome::Unknown && !first.tt_hit,
          "first Unknown Environment occurrence must be expanded, not cached");
    check(first.children.size() == 1 &&
              first.children[0]->terminal_reason == TerminalReason::NodeLimit,
          "first Unknown Environment occurrence must contain its cutoff Player child");
    check(!second.expanded && second.outcome == Outcome::Unknown && !second.tt_hit,
          "second Unknown Environment occurrence must be a fresh node-limit cutoff, not a TT hit");
    check(second.terminal_reason == TerminalReason::NodeLimit &&
              !second.tt_source_trace_id,
          "uncached Unknown trace must not reference a TT source");
}

void testNodeLimitReturnsUnknownForEveryAttemptedChild()
{
    FakeGame game;
    game.slides[1] = {
        slide(Direction::Right, 10),
        slide(Direction::Left, 11),
    };
    game.values = {{10, 2.0}, {11, 1.0}};

    SearchResult result = Solver(game, SolverOptions{8, 1, 0}).solve();

    check(result.outcome == Outcome::Unknown, "node limit must return Unknown, not Loss");
    check(result.statistics.player_nodes == 1 && result.statistics.environment_nodes == 0,
          "node limit must stop expanding at the combined-node boundary");
    check(result.statistics.player_actions == 2,
          "expanded Player node must count all generated actions before child cutoffs");
    check(result.statistics.node_limit_cutoffs == 2,
          "each attempted child beyond the limit must be recorded as a cutoff");
    check(result.root->children.size() == 2 && result.root->skipped_actions.empty(),
          "Unknown children are visited children, never skipped actions");
    for (const auto& child : result.root->children) {
        check(child->outcome == Outcome::Unknown &&
                  child->terminal_reason == TerminalReason::NodeLimit && !child->expanded,
              "node-limit trace child is not a terminal Unknown");
    }
}

void checkTiles(const std::array<unsigned, 16>& actual,
                const std::array<unsigned, 16>& expected,
                const std::string& message)
{
    if (actual != expected) fail(message);
}

void testRealGameApiAndThreeNodeSearch()
{
    const std::filesystem::path model_path = "models/4x6patt.w";
    check(std::filesystem::is_regular_file(model_path),
          "real GameApi test requires models/4x6patt.w");

    GameApi game("4x6patt", model_path.string());
    const Board opening = game.fixedOpening();
    std::array<unsigned, 16> expected_opening{};
    expected_opening[0] = 2;
    expected_opening[1] = 2;
    checkTiles(game.tileValues(opening), expected_opening,
               "fixed opening must contain 2 at positions 0 and 1");

    const std::vector<SlideResult> slides = game.legalSlides(opening);
    check(slides.size() == 3, "fixed opening should have exactly three legal slides");
    check(slides[0].direction == Direction::Right &&
              slides[1].direction == Direction::Down &&
              slides[2].direction == Direction::Left,
          "GameApi legal slides must retain direction/opcode order");

    std::array<unsigned, 16> right_tiles{};
    right_tiles[3] = 4;
    std::array<unsigned, 16> down_tiles{};
    down_tiles[12] = 2;
    down_tiles[13] = 2;
    std::array<unsigned, 16> left_tiles{};
    left_tiles[0] = 4;
    checkTiles(game.tileValues(slides[0].afterstate), right_tiles,
               "Right afterstate from the fixed opening is wrong");
    checkTiles(game.tileValues(slides[1].afterstate), down_tiles,
               "Down afterstate from the fixed opening is wrong");
    checkTiles(game.tileValues(slides[2].afterstate), left_tiles,
               "Left afterstate from the fixed opening is wrong");
    check(slides[0].reward == 4 && slides[1].reward == 0 && slides[2].reward == 4,
          "GameApi slide rewards are inconsistent with the shared 2048 rules");

    const double right_value = game.evaluateAfterstate(slides[0].afterstate);
    const double down_value = game.evaluateAfterstate(slides[1].afterstate);
    const double left_value = game.evaluateAfterstate(slides[2].afterstate);
    checkNear(right_value, 281380.09375, 0.01, "fixed-opening Right value mismatch");
    checkNear(down_value, 282950.34375, 0.01, "fixed-opening Down value mismatch");
    checkNear(left_value, 281380.09375, 0.01, "fixed-opening Left value mismatch");
    check(down_value > right_value && right_value == left_value,
          "fixed-opening values must order Down before tied Right and Left");

    const std::vector<PlacementResult> placements = game.legalPlacements(slides[1].afterstate);
    check(placements.size() == 28, "Down afterstate must have 14 x 2 placement actions");
    check(placements[0].position == 0 && placements[0].tile == 2 &&
              placements[1].position == 0 && placements[1].tile == 4 &&
              placements[2].position == 1 && placements[2].tile == 2 &&
              placements[3].position == 1 && placements[3].tile == 4,
          "Environment placements must be row-major with 2 before 4");
    check(placements[26].position == 15 && placements[26].tile == 2 &&
              placements[27].position == 15 && placements[27].tile == 4,
          "Environment placement order must skip occupied positions 12 and 13");

    {
        SearchResult result = Solver(game, SolverOptions{8, 3, 0, false}).solve();
        check(result.outcome == Outcome::Unknown, "three-node real search must be Unknown");
        check(result.statistics.player_nodes == 2, "three-node search player_nodes must be 2");
        check(result.statistics.environment_nodes == 1,
              "three-node search environment_nodes must be 1");
        check(result.statistics.player_actions == 7, "three-node search player_actions must be 7");
        check(result.statistics.placement_actions == 28,
              "three-node search placement_actions must be 28");
        check(result.statistics.node_limit_cutoffs == 33,
              "three-node search node_limit_cutoffs must be 33");
        check(result.statistics.combinedNodes() == 3,
              "three-node search combined node count must be 3");
        check(!result.root_action.has_value(), "Unknown root must not select a proven winning action");
        check(result.root->children.size() == 3, "Unknown root must try all three slide actions");
        check(incomingDirection(*result.root->children[0]) == Direction::Down &&
                  incomingDirection(*result.root->children[1]) == Direction::Right &&
                  incomingDirection(*result.root->children[2]) == Direction::Left,
              "real DFS root order must be Down, Right, Left by V then opcode");
    }

    // Keep this integration solve in the one GameApi test: the legacy N-tuple
    // network is initialized process-wide, and the preceding limited trace has
    // left scope before this complete target-8 proof is retained.
    SearchResult tt_result = Solver(game, SolverOptions{8, 0, 0, true}).solve();
    check(tt_result.outcome == Outcome::Win,
          "complete target-8 TT search must prove a guaranteed Win");
    check(tt_result.root_action == Direction::Down,
          "complete target-8 TT search must select Down at the fixed opening");
    check(tt_result.statistics.player_nodes == 159766,
          "complete target-8 TT search player node count mismatch");
    check(tt_result.statistics.environment_nodes == 13775,
          "complete target-8 TT search Environment node count mismatch");
    check(tt_result.statistics.player_tt_stores == 159766,
          "complete target-8 TT search Player entry count mismatch");
    check(tt_result.statistics.environment_tt_stores == 13775,
          "complete target-8 TT search Environment entry count mismatch");
    check(tt_result.statistics.player_tt_hits == 26741,
          "complete target-8 TT search Player hit count mismatch");
    check(tt_result.statistics.environment_tt_hits == 33200,
          "complete target-8 TT search Environment hit count mismatch");
    check(tt_result.statistics.node_limit_cutoffs == 0,
          "unlimited target-8 TT search must not report a node-limit cutoff");
    checkTraceIdsAreDfsDiscoveryOrder(*tt_result.root);
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail("cannot read test SGF file: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<std::uint64_t> parseUnsignedCommentFields(const std::string& sgf,
                                                      const std::string& field)
{
    // Requiring the line boundary prevents `node_id` from also matching the
    // suffix of `tt_source_node_id`.
    const std::string prefix = "\n" + field + " = ";
    std::vector<std::uint64_t> values;
    std::size_t position = 0;
    while ((position = sgf.find(prefix, position)) != std::string::npos) {
        const std::size_t value_begin = position + prefix.size();
        std::size_t value_end = value_begin;
        while (value_end < sgf.size() && sgf[value_end] >= '0' && sgf[value_end] <= '9') {
            ++value_end;
        }
        check(value_end != value_begin, "SGF comment field " + field + " is not an unsigned integer");
        values.push_back(std::stoull(sgf.substr(value_begin, value_end - value_begin)));
        position = value_end;
    }
    return values;
}

struct TemporarySgf {
    TemporarySgf()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("solving2048-solver-tests-" + std::to_string(nonce) + ".sgf");
    }

    ~TemporarySgf()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".tmp", ignored);
    }

    std::filesystem::path path;
};

void testSgfActionEncodingEscapingAndOrder()
{
    const std::array<Direction, 4> directions = {
        Direction::Up,
        Direction::Right,
        Direction::Down,
        Direction::Left,
    };
    for (std::size_t opcode = 0; opcode < directions.size(); ++opcode) {
        const SearchAction action = SearchAction::slide(directions[opcode]);
        check(action.sgfPlayer() == 'B' && action.sgfId() == static_cast<int>(opcode),
              "slide SGF action ID/player mismatch");
    }
    check(SearchAction::placement(0, 2).sgfId() == 4,
          "2@0 must use SGF action ID 4");
    check(SearchAction::placement(15, 2).sgfId() == 19,
          "2@15 must use SGF action ID 19");
    check(SearchAction::placement(0, 4).sgfId() == 20,
          "4@0 must use SGF action ID 20");
    check(SearchAction::placement(15, 4).sgfId() == 35,
          "4@15 must use SGF action ID 35");
    check(SearchAction::placement(3, 4).sgfPlayer() == 'W',
          "placement SGF player must be W");
    checkThrows([] { (void)SearchAction::placement(-1, 2); },
                "negative placement position should be rejected");
    checkThrows([] { (void)SearchAction::placement(16, 2); },
                "placement position 16 should be rejected");
    checkThrows([] { (void)SearchAction::placement(0, 8); },
                "placement tile other than 2 or 4 should be rejected");
    check(SgfWriter::escapeValue("line]x\\y") == "line\\]x\\\\y",
          "SGF escaping must quote closing brackets and backslashes");

    TraceNode root;
    root.trace_id = 1;
    root.phase = Phase::Player;
    root.outcome = Outcome::Unknown;
    root.expanded = true;
    root.children_total = 3;
    root.skipped_actions = {
        SearchAction::slide(Direction::Left),
        SearchAction::slide(Direction::Up),
    };

    auto down = std::make_unique<TraceNode>();
    down->trace_id = 2;
    down->phase = Phase::Environment;
    down->outcome = Outcome::Loss;
    down->expanded = true;
    down->dfs_order = 0;
    down->incoming_action = SearchAction::slide(Direction::Down);
    down->value = 42.5;
    down->children_total = 1;

    auto spawned = std::make_unique<TraceNode>();
    spawned->trace_id = 3;
    spawned->phase = Phase::Player;
    spawned->outcome = Outcome::Loss;
    spawned->expanded = true;
    spawned->dfs_order = 0;
    spawned->incoming_action = SearchAction::placement(0, 2);
    spawned->terminal_reason = TerminalReason::NoLegalMove;
    down->children.push_back(std::move(spawned));

    auto right = std::make_unique<TraceNode>();
    right->trace_id = 4;
    right->phase = Phase::Environment;
    right->outcome = Outcome::Loss;
    right->terminal_reason = TerminalReason::TranspositionTable;
    right->tt_hit = true;
    right->tt_source_trace_id = 2;
    right->dfs_order = 1;
    right->incoming_action = SearchAction::slide(Direction::Right);
    right->value = 10.0;

    root.children.push_back(std::move(down));
    root.children.push_back(std::move(right));

    SgfMetadata metadata;
    metadata.winning_tile = 8;
    metadata.node_limit = 3;
    metadata.use_transposition_table = true;
    metadata.network = "fake]network";
    metadata.model_path = "path\\model";
    metadata.elapsed_seconds = 1.25;
    metadata.statistics.player_nodes = 2;
    metadata.statistics.environment_nodes = 1;

    TemporarySgf temporary;
    SgfWriter::write(temporary.path, root, metadata);
    const std::string sgf = readFile(temporary.path);

    const std::string prefix = "(;FF[4]CA[UTF-8]SZ[4]AP[Solving2048:1];W[4];W[5]C[";
    check(sgf.rfind(prefix, 0) == 0,
          "SGF must start with metadata followed by the two fixed-opening placements");
    check(sgf.find("network = fake\\]network") != std::string::npos,
          "SGF root metadata did not escape a closing bracket");
    check(sgf.find("model = path\\\\model") != std::string::npos,
          "SGF root metadata did not escape a backslash");
    check(sgf.find("children_skipped_actions = left(3) up(0)") != std::string::npos,
          "SGF must preserve skipped action order");

    const std::vector<std::uint64_t> visit_orders =
        parseUnsignedCommentFields(sgf, "dfs_visit_order");
    check(visit_orders == std::vector<std::uint64_t>({1, 2, 3, 4}),
          "every SGF node must expose its unique, continuous DFS visit order");
    const std::vector<std::uint64_t> node_ids = parseUnsignedCommentFields(sgf, "node_id");
    check(node_ids == visit_orders,
          "SGF node_id and dfs_visit_order must identify the same discovery sequence");
    const std::vector<std::uint64_t> tt_sources =
        parseUnsignedCommentFields(sgf, "tt_source_node_id");
    check(tt_sources == std::vector<std::uint64_t>({2}),
          "SGF TT hit must contain one parseable source-node reference");

    const std::size_t source_visit_position = sgf.find("dfs_visit_order = 2");
    const std::size_t hit_visit_position = sgf.find("dfs_visit_order = 4");
    const std::size_t reference_position = sgf.find("tt_source_node_id = 2", hit_visit_position);
    check(source_visit_position != std::string::npos && hit_visit_position != std::string::npos &&
              reference_position != std::string::npos && source_visit_position < hit_visit_position &&
              hit_visit_position < reference_position && tt_sources[0] < visit_orders.back(),
          "SGF TT source must be emitted before the later hit that references it");

    const std::size_t down_position = sgf.find("(;B[2]");
    const std::size_t nested_placement_position = sgf.find("(;W[4]", down_position);
    const std::size_t right_position = sgf.find("(;B[1]");
    check(down_position != std::string::npos && nested_placement_position != std::string::npos &&
              right_position != std::string::npos,
          "SGF is missing an expected action node");
    check(down_position < nested_placement_position && nested_placement_position < right_position,
          "SGF writer must preserve DFS child and nested placement order");
    check(sgf.size() >= 2 && sgf.substr(sgf.size() - 2) == ")\n",
          "SGF output must close the game tree and end with a newline");
    check(!std::filesystem::exists(temporary.path.string() + ".tmp"),
          "successful SGF publication must not leave a temporary file");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"Player OR: Unknown, Win, and skipped", testPlayerOrUnknownWinAndSkipped},
        {"Environment AND: all Win", testEnvironmentAndAllWin},
        {"Environment AND: Loss short-circuit", testEnvironmentAndLossShortCircuit},
        {"Environment AND: Unknown is not short-circuit", testEnvironmentUnknownIsNotShortCircuit},
        {"value ordering and direction tie-break", testValueOrderingAndDirectionTieBreak},
        {"no-cache repeated state expansion", testNoCacheExpandsRepeatedStates},
        {"Player transposition-table hit", testPlayerTranspositionHit},
        {"Environment TT hit and outcome consistency",
         testEnvironmentTranspositionHitAndOutcomeConsistency},
        {"Unknown outcomes are not cached", testUnknownOutcomesAreNotCached},
        {"combined node-limit Unknown semantics", testNodeLimitReturnsUnknownForEveryAttemptedChild},
        {"real GameApi limited baseline and complete target-8 TT solve",
         testRealGameApiAndThreeNodeSearch},
        {"SGF action encoding, escaping, and order", testSgfActionEncodingEscapingAndOrder},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " / " << tests.size() << " tests failed\n";
        return 1;
    }
    std::cout << "All " << tests.size() << " solver tests passed\n";
    return 0;
}
