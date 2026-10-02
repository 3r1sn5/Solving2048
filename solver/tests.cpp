#include "config.h"
#include "game_api.h"
#include "proof_checker.h"
#include "proof_sequence.h"
#include "rz_file.h"
#include "rzone_slide.h"
#include "search.h"
#include "sgf.h"
#include "tt_file.h"
#include "../2048_api.h"

#include <algorithm>
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
#include <tuple>
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
using solver2048::ProofManifest;
using solver2048::ProofTable;
using solver2048::ProofVerificationResult;
using solver2048::RZoneCertificate;
using solver2048::RZoneCertificateId;
using solver2048::RZoneMode;
using solver2048::RZoneLookupPolicy;
using solver2048::RZoneMatch;
using solver2048::RZoneStepKind;
using solver2048::RZoneTable;
using solver2048::RzFile;
using solver2048::RzFileSummary;
using solver2048::RzProofTable;
using solver2048::SearchAction;
using solver2048::SearchResult;
using solver2048::SolverConfig;
using solver2048::SgfMetadata;
using solver2048::SgfWriter;
using solver2048::SlideResult;
using solver2048::SolvedBy;
using solver2048::Solver;
using solver2048::SolverOptions;
using solver2048::TerminalReason;
using solver2048::TraceNode;
using solver2048::TtFile;
using solver2048::TtFileSummary;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void check(bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

std::string readFile(const std::filesystem::path& path);

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

    std::array<unsigned, 16> tileValues(Board value) const override
    {
        const auto found = tile_values.find(value.raw);
        return found == tile_values.end() ? std::array<unsigned, 16>{} : found->second;
    }

    Board opening_;
    std::unordered_map<std::uint64_t, std::vector<SlideResult>> slides;
    std::unordered_map<std::uint64_t, std::vector<PlacementResult>> placements;
    std::unordered_map<std::uint64_t, double> values;
    std::unordered_map<std::uint64_t, std::array<unsigned, 16>> tile_values;
    std::unordered_set<std::uint64_t> winning_states;
    mutable std::unordered_map<std::uint64_t, std::size_t> slide_calls;
    mutable std::unordered_map<std::uint64_t, std::size_t> placement_calls;
    mutable std::vector<std::uint64_t> evaluated_afterstates;
};

class MechanicsGame final : public Game {
public:
    explicit MechanicsGame(Board opening = {}) : opening_(opening) {}
    Board fixedOpening() const override { return opening_; }
    bool hasWinningTile(Board value, unsigned winning_tile) const override
    {
        ++winning_calls;
        return solver2048::boardHasWinningTile(value, winning_tile);
    }
    std::vector<SlideResult> legalSlides(Board value) const override
    {
        ++slide_calls;
        return solver2048::generateLegalSlides(value);
    }
    std::vector<PlacementResult> legalPlacements(Board value) const override
    {
        ++placement_calls;
        return solver2048::generateLegalPlacements(value);
    }
    double evaluateAfterstate(Board) const override { return 0; }
    std::array<unsigned, 16> tileValues(Board value) const override
    {
        ++tile_value_calls;
        return solver2048::boardTileValues(value);
    }

    void resetCalls() const
    {
        winning_calls = 0;
        slide_calls = 0;
        placement_calls = 0;
        tile_value_calls = 0;
    }

    std::uint64_t calls() const
    {
        return winning_calls + slide_calls + placement_calls + tile_value_calls;
    }

    mutable std::uint64_t winning_calls = 0;
    mutable std::uint64_t slide_calls = 0;
    mutable std::uint64_t placement_calls = 0;
    mutable std::uint64_t tile_value_calls = 0;

private:
    Board opening_;
};

Board tiles(std::initializer_list<std::pair<int, unsigned>> values)
{
    std::array<unsigned, 16> result{};
    for (const auto& [position, tile] : values) {
        if (position < 0 || position >= 16) fail("test tile position is out of range");
        result[static_cast<std::size_t>(position)] = tile;
    }
    return solver2048::boardFromTiles(result);
}

void testRZoneCertificateLiftsAndLookup()
{
    MechanicsGame game;
    RZoneTable table(game, 16, 16);

    const Board terminal = tiles({{0, 16}, {1, 2}});
    const std::optional<RZoneCertificateId> terminal_id =
        table.createTerminal(terminal, Phase::Environment);
    check(terminal_id.has_value(), "winning board must create an RZone terminal certificate");
    const RZoneCertificate* terminal_certificate = table.find(*terminal_id);
    check(terminal_certificate && terminal_certificate->kind == RZoneStepKind::Terminal &&
              terminal_certificate->phase == Phase::Environment &&
              terminal_certificate->mask == 0x0001 && terminal_certificate->depth == 1,
          "terminal RZone certificate fields mismatch");

    const Board fixed_slide_parent = tiles({{0, 8}, {2, 8}});
    const std::optional<RZoneCertificateId> fixed_id =
        table.liftFixedSlide(fixed_slide_parent, Direction::Left, *terminal_id);
    check(fixed_id.has_value(), "8,empty,8 must lift a left-slide terminal proof");
    const RZoneCertificate* fixed = table.find(*fixed_id);
    check(fixed && fixed->kind == RZoneStepKind::FixedSlide &&
              fixed->phase == Phase::Player && fixed->slide == Direction::Left &&
              fixed->mask == 0x0007 && fixed->depth == 2,
          "fixed-slide RZone preimage must include exactly the consumed input prefix");

    game.resetCalls();
    const std::optional<RZoneMatch> outside_match =
        table.lookup(tiles({{0, 8}, {2, 8}, {3, 4}}), Phase::Player);
    check(outside_match && outside_match->id == *fixed_id && outside_match->transform == 0,
          "a change outside the fixed-slide RZone must preserve the proof lookup");
    check(game.calls() == 0, "RZone lookup must not replay game mechanics");
    check(!table.lookup(tiles({{0, 8}, {1, 2}, {2, 8}}), Phase::Player),
          "a change inside the RZone must not match the certificate");
    const std::optional<RZoneMatch> direct_match = table.matchCertificate(
        tiles({{0, 8}, {2, 8}, {3, 4}}), Phase::Player, *fixed_id, 0);
    check(direct_match && direct_match->id == *fixed_id && direct_match->transform == 0,
          "direct RZone certificate matching must accept an outside-pattern change");
    check(!table.matchCertificate(
              tiles({{0, 8}, {2, 8}, {3, 4}}), Phase::Environment, *fixed_id, 0),
          "direct RZone certificate matching must reject the wrong phase");

    const Board rotated_parent = solver2048::transformBoard(fixed_slide_parent, 1);
    check(!table.lookup(rotated_parent, Phase::Player),
          "RZone lookup without symmetry must not match a rotated pattern");

    RZoneTable symmetry_table(
        game, 16, 16, RZoneLookupPolicy::SmallestFirst, true);
    const auto symmetry_terminal = symmetry_table.createTerminal(terminal, Phase::Environment);
    check(symmetry_terminal.has_value(),
          "symmetry RZone fixture terminal certificate is missing");
    const auto symmetry_fixed = symmetry_table.liftFixedSlide(
        fixed_slide_parent, Direction::Left, *symmetry_terminal);
    check(symmetry_fixed.has_value(),
          "symmetry RZone fixture fixed-slide certificate is missing");
    const RZoneCertificate* stored_fixed = symmetry_table.find(*symmetry_fixed);
    const RZoneCertificate* stored_terminal = symmetry_table.find(*symmetry_terminal);
    check(stored_fixed && stored_terminal && stored_fixed->slide,
          "symmetry RZone fixture certificates are invalid");

    game.resetCalls();
    for (std::uint8_t current_transform = 0; current_transform < 8; ++current_transform) {
        const Board current = solver2048::transformBoard(
            fixed_slide_parent, current_transform);
        const std::optional<RZoneMatch> match = symmetry_table.lookup(current, Phase::Player);
        check(match && match->id == *symmetry_fixed,
              "all eight board orientations must reuse one stored RZone certificate");
        const Board stored_orientation = solver2048::transformBoard(current, match->transform);
        check(RZoneTable::maskedBoard(stored_orientation, stored_fixed->mask) ==
                  stored_fixed->masked_board,
              "RZone match transform must map the current board to certificate orientation");

        const Direction current_slide = solver2048::transformDirection(
            *stored_fixed->slide,
            solver2048::inverseTransform(match->transform));
        const std::vector<SlideResult> current_slides =
            solver2048::generateLegalSlides(current);
        const auto selected = std::find_if(
            current_slides.begin(), current_slides.end(), [&](const SlideResult& slide) {
                return slide.direction == current_slide;
            });
        check(selected != current_slides.end() &&
                  RZoneTable::maskedBoard(
                      solver2048::transformBoard(selected->afterstate, match->transform),
                      stored_terminal->mask) == stored_terminal->masked_board,
              "a symmetry RZone hit must map its fixed slide back to the current board");
    }
    check(game.calls() == 0,
          "symmetry RZone lookup must only compare transformed bitboards, not replay mechanics");
    check(symmetry_table.statistics().exact_orientation_hits != 0 &&
              symmetry_table.statistics().symmetry_hits != 0 &&
              symmetry_table.statistics().lookup_transforms_tested == 64,
          "RZone symmetry lookup counters mismatch");
    const Board direct_rotated = solver2048::transformBoard(fixed_slide_parent, 3);
    const auto direct_symmetry = symmetry_table.matchCertificate(
        direct_rotated, Phase::Player, *symmetry_fixed, 0);
    check(direct_symmetry && direct_symmetry->id == *symmetry_fixed &&
              direct_symmetry->transform != 0 &&
              symmetry_table.statistics().active_pruning_transform_tests <= 8,
          "direct certificate matching must find a deterministic symmetry transform");

    // Sample arbitrary completions outside the pattern. Every matching board
    // must keep the fixed action legal and reproduce the child root pattern.
    std::uint64_t random_state = 0x9e3779b97f4a7c15ull;
    for (int sample = 0; sample < 256; ++sample) {
        random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
        std::array<unsigned, 16> completion{};
        for (int position = 0; position < 16; ++position) {
            const unsigned choice = static_cast<unsigned>((random_state >> (position * 3 % 61)) % 5);
            completion[static_cast<std::size_t>(position)] = choice == 0 ? 0 : (1u << choice);
        }
        completion[0] = 8;
        completion[1] = 0;
        completion[2] = 8;
        const Board candidate = solver2048::boardFromTiles(completion);
        const std::vector<SlideResult> slides = solver2048::generateLegalSlides(candidate);
        const auto left = std::find_if(slides.begin(), slides.end(), [](const SlideResult& value) {
            return value.direction == Direction::Left;
        });
        check(left != slides.end(), "every completion of a fixed-slide RZone must keep its action legal");
        if (RZoneTable::maskedBoard(left->afterstate, terminal_certificate->mask) !=
            terminal_certificate->masked_board) {
            std::ostringstream detail;
            detail << "a fixed-slide RZone completion changed its certified child pattern: board=0x"
                   << std::hex << candidate.raw << " child=0x" << left->afterstate.raw
                   << " expected=0x" << terminal_certificate->masked_board;
            fail(detail.str());
        }
    }

    const Board placed_child = tiles({{0, 8}, {1, 8}, {2, 2}});
    const Board placed_terminal = tiles({{0, 16}, {1, 2}});
    const std::optional<RZoneCertificateId> placed_terminal_id =
        table.createTerminal(placed_terminal, Phase::Environment);
    check(placed_terminal_id.has_value(), "placement fixture terminal certificate missing");
    const std::optional<RZoneCertificateId> placed_child_id =
        table.liftFixedSlide(placed_child, Direction::Left, *placed_terminal_id);
    check(placed_child_id.has_value(), "placement child must lift through its winning slide");
    const RZoneCertificate* placed_child_certificate = table.find(*placed_child_id);
    check(placed_child_certificate && placed_child_certificate->mask == 0x0003,
          "placement-child RZone should exclude the irrelevant spawned tile");

    const Board universal_parent = tiles({{0, 8}, {1, 8}});
    const std::optional<RZoneCertificateId> universal_id =
        table.liftUniversalPlacement(universal_parent, *placed_child_id);
    check(universal_id.has_value(),
          "Environment proof must lift when every placement is outside the non-empty RZone");
    const RZoneCertificate* universal = table.find(*universal_id);
    check(universal && universal->kind == RZoneStepKind::UniversalPlacement &&
              universal->phase == Phase::Environment && universal->mask == 0x0003 &&
              universal->depth == 3,
          "universal-placement RZone certificate fields mismatch");

    const Board blocked_child = tiles({{0, 8}, {2, 8}, {3, 2}});
    const std::optional<RZoneCertificateId> blocked_child_id =
        table.liftFixedSlide(blocked_child, Direction::Left, *placed_terminal_id);
    check(blocked_child_id.has_value(), "blocked universal fixture must have a child proof");
    const Board blocked_parent = tiles({{0, 8}, {2, 8}});
    check(!table.liftUniversalPlacement(blocked_parent, *blocked_child_id),
          "universal lift must fail when Environment may place inside the child RZone");

    const auto& stats = table.statistics();
    check(stats.slide_lift_successes == 3 && stats.placement_lift_successes == 1 &&
              stats.placement_lift_blocked == 1 && stats.player_hits == 1 &&
              stats.active_pruning_certificates_tested == 2 &&
              stats.active_pruning_children_matched == 1,
          "RZone generation/lookup counters mismatch");

    RZoneTable ordered_table(
        game, 16, 16, RZoneLookupPolicy::SmallestFirst);
    const auto large_terminal = ordered_table.createTerminal(
        tiles({{4, 16}}), Phase::Environment);
    check(large_terminal.has_value(), "ordered lookup large terminal fixture missing");
    const auto large_certificate = ordered_table.liftFixedSlide(
        tiles({{4, 8}, {6, 8}}), Direction::Left, *large_terminal);
    check(large_certificate.has_value() &&
              RZoneTable::countCells(ordered_table.find(*large_certificate)->mask) == 3,
          "ordered lookup large Player certificate fixture mismatch");
    const auto small_terminal = ordered_table.createTerminal(
        tiles({{0, 16}}), Phase::Environment);
    check(small_terminal.has_value(), "ordered lookup small terminal fixture missing");
    const auto small_certificate = ordered_table.liftFixedSlide(
        tiles({{0, 8}, {1, 8}}), Direction::Left, *small_terminal);
    check(small_certificate.has_value() &&
              RZoneTable::countCells(ordered_table.find(*small_certificate)->mask) == 2,
          "ordered lookup small Player certificate fixture mismatch");
    const auto ordered_hit = ordered_table.lookup(
        tiles({{0, 8}, {1, 8}, {4, 8}, {6, 8}}), Phase::Player);
    check(ordered_hit && ordered_hit->id == *small_certificate && ordered_hit->transform == 0,
          "smallest-first lookup must return a minimum-cell certificate even when stored later");
    check(ordered_table.statistics().lookup_early_exits == 1 &&
              ordered_table.statistics().lookup_masks_tested <
                  ordered_table.statistics().masks_indexed,
          "smallest-first lookup must stop before testing larger masks after a hit");

    // Broader preimage property test: generate legal slides from varied source
    // boards, then perturb every cell outside the derived mask. Relevant child
    // outputs and action legality must remain invariant without lookup replay.
    random_state = 0xd1b54a32d192ed03ull;
    for (int source_sample = 0; source_sample < 48; ++source_sample) {
        std::array<unsigned, 16> source_values{};
        for (unsigned& tile : source_values) {
            random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
            const unsigned choice = static_cast<unsigned>((random_state >> 32) % 5);
            tile = choice == 0 ? 0 : (1u << choice);
        }
        const Board source = solver2048::boardFromTiles(source_values);
        for (const SlideResult& source_slide : solver2048::generateLegalSlides(source)) {
            for (int output_position = 0; output_position < 16; ++output_position) {
                const std::uint16_t output_mask =
                    static_cast<std::uint16_t>(std::uint16_t{1} << output_position);
                const std::uint16_t preimage =
                    table.deriveSlidePreimageMask(source, source_slide.direction, output_mask);
                check(preimage != 0, "a legal slide must produce a non-empty RZone preimage");
                const std::uint64_t expected_output =
                    RZoneTable::maskedBoard(source_slide.afterstate, output_mask);
                for (int completion_sample = 0; completion_sample < 8; ++completion_sample) {
                    std::array<unsigned, 16> completion{};
                    for (int position = 0; position < 16; ++position) {
                        random_state = random_state * 6364136223846793005ull +
                                       1442695040888963407ull;
                        const unsigned choice = static_cast<unsigned>((random_state >> 32) % 5);
                        completion[static_cast<std::size_t>(position)] =
                            choice == 0 ? 0 : (1u << choice);
                        if ((preimage & (std::uint16_t{1} << position)) != 0) {
                            completion[static_cast<std::size_t>(position)] =
                                source_values[static_cast<std::size_t>(position)];
                        }
                    }
                    const Board candidate = solver2048::boardFromTiles(completion);
                    const std::vector<SlideResult> candidate_slides =
                        solver2048::generateLegalSlides(candidate);
                    const auto selected = std::find_if(
                        candidate_slides.begin(), candidate_slides.end(),
                        [&](const SlideResult& value) {
                            return value.direction == source_slide.direction;
                        });
                    check(selected != candidate_slides.end(),
                          "derived RZone preimage did not preserve slide legality");
                    check(RZoneTable::maskedBoard(selected->afterstate, output_mask) ==
                              expected_output,
                          "derived RZone preimage did not preserve a relevant output cell");
                }
            }
        }
    }
}

void checkTraceIdsAreDfsDiscoveryOrder(const TraceNode& root);

void testRZoneActivePruningCoversEnvironmentSiblings()
{
    const Board root = tiles({{4, 2}, {5, 2}});
    const Board afterstate = tiles({{0, 4}, {1, 4}});
    const Board first_child = tiles({{0, 4}, {1, 4}, {2, 2}});
    const Board covered_child = tiles({{0, 4}, {1, 4}, {3, 2}});
    const Board terminal = tiles({{0, 8}, {1, 2}});

    SolverOptions invalid_options;
    invalid_options.rzone_active_pruning = true;
    FakeGame invalid_game(root.raw);
    checkThrows([&] { (void)Solver(invalid_game, invalid_options); },
                "RZone active pruning without lookup mode must be rejected");

    auto make_game = [&]() {
        FakeGame game(root.raw);
        game.slides[root.raw] = {slide(Direction::Down, afterstate.raw)};
        // The RZone preimage validator replays the selected slide on the
        // masked witness board, which is this afterstate in the fixture.
        game.slides[afterstate.raw] = {slide(Direction::Left, terminal.raw)};
        game.values[afterstate.raw] = 0.0;
        game.placements[afterstate.raw] = {
            placement(2, 2, first_child.raw),
            placement(3, 2, covered_child.raw),
        };
        game.slides[first_child.raw] = {slide(Direction::Left, terminal.raw)};
        game.values[terminal.raw] = 0.0;
        game.winning_states.insert(terminal.raw);
        game.tile_values[terminal.raw] = solver2048::boardTileValues(terminal);
        return game;
    };

    FakeGame baseline_game = make_game();
    SolverOptions baseline_options;
    baseline_options.winning_tile = 8;
    baseline_options.rzone_mode = RZoneMode::Lookup;
    SearchResult baseline = Solver(baseline_game, baseline_options).solve();

    FakeGame active_game = make_game();
    SolverOptions active_options = baseline_options;
    active_options.rzone_active_pruning = true;
    SearchResult active = Solver(active_game, active_options).solve();

    if (baseline.outcome != Outcome::Win || active.outcome != baseline.outcome ||
        active.root_action != baseline.root_action) {
        std::ostringstream detail;
        detail << "RZone active pruning changed the root proof: baseline="
               << solver2048::outcomeName(baseline.outcome) << ", active="
               << solver2048::outcomeName(active.outcome) << ", baseline_action="
               << (baseline.root_action ? solver2048::directionName(*baseline.root_action) : "none")
               << ", active_action="
               << (active.root_action ? solver2048::directionName(*active.root_action) : "none");
        fail(detail.str());
    }
    check(active.statistics.rzone.lookups + 1 == baseline.statistics.rzone.lookups &&
              active.statistics.rzone.active_pruning_full_lookups_avoided == 1 &&
              active.statistics.rzone.active_pruning_children_matched == 1,
          "RZone active pruning must replace the covered sibling's full lookup");
    check(active.statistics.rzone.active_pruning_and_nodes == 1 &&
              active.statistics.rzone.active_pruning_and_nodes_closed == 1 &&
              active.statistics.rzone.active_pruning_universal_lift_attempts == 1 &&
              active.statistics.rzone.active_pruning_universal_lift_successes == 1,
          "RZone active pruning AND-node and universal-lift counters mismatch");
    check(active.statistics.player_nodes + 1 == baseline.statistics.player_nodes &&
              active.statistics.environment_nodes == baseline.statistics.environment_nodes &&
              active.statistics.combinedNodes() + 1 == baseline.statistics.combinedNodes(),
          "an active-pruned sibling must not count as a visited DFS node");
    check(active.root->children.size() == 1,
          "active-pruning fixture root must have one Environment child");
    const TraceNode& environment = *active.root->children[0];
    check(environment.outcome == Outcome::Win &&
              environment.solved_by == SolvedBy::Backpropagation &&
              environment.rzone_certificate_id && environment.children.size() == 2,
          "active-pruned Environment node must retain a complete proof-compatible child list");
    const TraceNode& covered = *environment.children[1];
    check(covered.outcome == Outcome::Win && covered.solved_by == SolvedBy::RZoneTable &&
              covered.rzone_hit && covered.rzone_certificate_id &&
              covered.selected_action &&
              covered.selected_action->kind == SearchAction::Kind::Slide &&
              covered.selected_action->direction == Direction::Left && !covered.expanded,
          "covered sibling must be emitted as a lightweight RZone Win leaf");
    checkTraceIdsAreDfsDiscoveryOrder(*active.root);
}

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
        check(node.solved_by != SolvedBy::None,
              "every emitted TraceNode must record how it was solved");

        if (node.tt_hit) {
            check(node.solved_by == SolvedBy::TranspositionTable,
                  "a TT hit TraceNode must be marked as solved by TT");
            check(node.tt_key.has_value(), "every TT hit must expose its table key");
            if (node.tt_persistent_hit) {
                check(!node.tt_source_trace_id,
                      "a persistent TT hit must not reference a prior-run trace node");
            } else {
                check(node.tt_source_trace_id.has_value(),
                      "a trace-local TT hit must reference its source TraceNode");
                check(*node.tt_source_trace_id < node.trace_id,
                      "TT source ID must precede its hit in DFS discovery order");
                check(discovered.count(*node.tt_source_trace_id) == 1,
                      "TT source must already be discoverable earlier in the emitted tree");
            }
        } else {
            check(!node.tt_source_trace_id,
                  "a non-TT TraceNode must not carry a TT source reference");
            check(!node.tt_persistent_hit && !node.tt_key,
                  "a non-TT TraceNode must not carry persistent TT metadata");
        }
        if (node.terminal_reason == TerminalReason::WinningTile ||
            node.terminal_reason == TerminalReason::NoLegalMove) {
            check(node.solved_by == SolvedBy::Terminal,
                  "a game-terminal TraceNode must be marked as solved by TERMINAL");
        }
        if (node.terminal_reason == TerminalReason::NodeLimit) {
            check(node.solved_by == SolvedBy::NodeLimit,
                  "a node-limit TraceNode must be marked as solved by NODE_LIMIT");
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
        slide(Direction::Up, 10),
        slide(Direction::Right, 11),
        slide(Direction::Down, 12),
    };
    game.values = {{10, 30.0}, {11, 20.0}, {12, 10.0}};
    game.winning_states.insert(11);

    SearchResult result = Solver(game, SolverOptions{8, 1, 0}).solve();

    check(result.outcome == Outcome::Win, "Player OR must find a Win after an Unknown child");
    check(result.root_action == Direction::Right, "Player OR selected the wrong winning action");
    check(result.root->solved_by == SolvedBy::Backpropagation,
          "Player OR result must be marked as solved by BACKPROPAGATION");
    checkNear(*result.root_action_value, 20.0, 0.0,
              "selected action value must be reward + V(afterstate)");
    check(result.statistics.player_nodes == 1 && result.statistics.environment_nodes == 2,
          "visited-node counters must include node-limit and terminal leaves");
    check(result.statistics.player_actions == 3, "all Player actions must be generated before DFS");
    check(result.statistics.node_limit_cutoffs == 1, "first Environment child must be one cutoff");

    const TraceNode& root = *result.root;
    check(root.children_total == 3, "Player trace must record all legal actions");
    check(root.children.size() == 2, "Player trace must retain Unknown and decisive Win children");
    check(root.children[0]->outcome == Outcome::Unknown,
          "first visited Player child must be Unknown, not skipped");
    check(root.children[0]->terminal_reason == TerminalReason::NodeLimit,
          "Unknown child must record NODE_LIMIT");
    check(root.children[0]->solved_by == SolvedBy::NodeLimit,
          "node-limit child must record solved_by NODE_LIMIT");
    check(root.children[1]->outcome == Outcome::Win,
          "second visited Player child must be the immediate Win");
    check(root.children[1]->solved_by == SolvedBy::Terminal,
          "immediate winning child must record solved_by TERMINAL");
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

    SolverOptions options;
    options.progress_interval = 2;
    std::vector<std::uint64_t> progress_counts;
    SearchResult result = Solver(
        game,
        options,
        [&](const solver2048::SearchStatistics& statistics) {
            progress_counts.push_back(statistics.combinedNodes());
        }).solve();

    check(result.outcome == Outcome::Win, "Environment AND must be Win when every placement is Win");
    check(result.statistics.player_nodes == 4 && result.statistics.environment_nodes == 1 &&
              result.statistics.combinedNodes() == 5,
          "visited-node counters must include all three terminal placement leaves");
    check(progress_counts == std::vector<std::uint64_t>({2, 4}),
          "progress interval must be driven by visited nodes");
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
    check(result.statistics.player_nodes == 4 &&
              result.statistics.environment_nodes == 1,
          "visited-node counters must include terminal and node-limit leaves");
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

void testActionValueOrderingAndDirectionTieBreak()
{
    FakeGame game;
    // Deliberately return directions out of opcode order. Right wins because its
    // immediate reward raises Q above V(afterstate); equal Q values then use
    // Up, Right, Down, Left as the deterministic tie-break.
    game.slides[1] = {
        slide(Direction::Left, 10, 0),
        slide(Direction::Up, 11, 40),
        slide(Direction::Right, 12, 4),
        slide(Direction::Down, 13, 20),
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
        Direction::Up,
        Direction::Down,
        Direction::Left,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        check(incomingDirection(*result.root->children[index]) == expected[index],
              "Player DFS order must be descending reward + V(afterstate), then direction opcode");
    }
    const TraceNode& first = *result.root->children[0];
    check(first.reward && *first.reward == 4, "first action reward mismatch");
    checkNear(*first.afterstate_value, 50.0, 0.0, "first afterstate value mismatch");
    checkNear(*first.action_value, 54.0, 0.0, "first reward + V action value mismatch");
    checkNear(*result.root->children[1]->action_value, 50.0, 0.0,
              "second tied action value mismatch");
}

struct TemporaryConfigFile {
    explicit TemporaryConfigFile(const std::string& contents)
    {
        static std::uint64_t sequence = 0;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("solving2048-config-test-" + std::to_string(nonce) + "-" +
                std::to_string(++sequence) + ".cfg");
        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output) fail("cannot create temporary solver cfg");
        output << contents;
        if (!output) fail("cannot write temporary solver cfg");
    }

    ~TemporaryConfigFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

std::string validConfigText()
{
    return
        "# search\n"
        "winning_tile = 16\n"
        "node_limit=123456\n"
        "progress_interval=789\n"
        "use_transposition_table=TRUE\n"
        "use_tt_symmetry=1\n"
        "load_tt=true\n"
        "save_tt=false\n"
        "tt_path=solver/tt/test-target16.tt\n"
        "rzone_mode=lookup\n"
        "rzone_lookup_policy=smallest_first\n"
        "use_rzone_symmetry=true\n"
        "rzone_max_cells=12\n"
        "rzone_shadow_exact_check=false\n"
        "rzone_active_pruning=true\n"
        "load_rz=true\n"
        "save_rz=false\n"
        "rz_path=solver/rz/test-target16.rz\n"
        "opening=4@5, 2@0 # inline comment\n"
        "network=4x6patt\n"
        "model_path=models/4x6patt.w\n"
        "sgf_path=solver/custom.sgf\n";
}

void testSolverConfigParsingAndValidation()
{
    // The workspace configuration is an editable experiment configuration.
    // Check that it parses; assert exact values using the isolated fixture below.
    (void)solver2048::loadSolverConfig("solver/solver.cfg");

    TemporaryConfigFile valid(validConfigText());
    const SolverConfig parsed = solver2048::loadSolverConfig(valid.path);
    check(parsed.winning_tile == 16 && parsed.node_limit == 123456 &&
              parsed.progress_interval == 789,
          "typed cfg unsigned parsing mismatch");
    check(parsed.use_transposition_table && parsed.use_tt_symmetry,
          "typed cfg bool parsing mismatch");
    check(parsed.load_tt && !parsed.save_tt &&
              parsed.tt_path == "solver/tt/test-target16.tt",
          "persistent TT cfg parsing mismatch");
    check(parsed.rzone_mode == solver2048::RZoneMode::Lookup &&
              parsed.rzone_lookup_policy == RZoneLookupPolicy::SmallestFirst &&
              parsed.use_rzone_symmetry &&
              parsed.rzone_max_cells == 12 &&
              !parsed.rzone_shadow_exact_check && parsed.rzone_active_pruning &&
              parsed.load_rz &&
              !parsed.save_rz && parsed.rz_path == "solver/rz/test-target16.rz",
          "RZone cfg parsing mismatch");
    check(parsed.opening[0] == 2 && parsed.opening[5] == 4 &&
              solver2048::formatOpening(parsed.opening) == "2@0,4@5",
          "cfg opening parsing or deterministic formatting mismatch");
    std::string batch_config_text = validConfigText();
    const std::string opening_line = "opening=4@5, 2@0 # inline comment\n";
    const std::string sgf_line = "sgf_path=solver/custom.sgf\n";
    batch_config_text.erase(
        batch_config_text.find(opening_line), opening_line.size());
    batch_config_text.erase(batch_config_text.find(sgf_line), sgf_line.size());
    TemporaryConfigFile batch_config(batch_config_text);
    const SolverConfig parsed_batch =
        solver2048::loadSolverConfig(batch_config.path, false);
    check(parsed_batch.opening == std::array<unsigned, 16>{} &&
              parsed_batch.sgf_path.empty(),
          "batch config must allow omitted single-opening fields");
    checkThrows(
        [&] { (void)solver2048::loadSolverConfig(batch_config.path); },
        "single-opening config must still require opening and sgf_path");
    const auto inline_opening = solver2048::parseOpeningSpec("4@15, 2@3");
    check(inline_opening[3] == 2 && inline_opening[15] == 4 &&
              solver2048::formatOpening(inline_opening) == "2@3,4@15",
          "batch CLI opening parsing mismatch");
    TemporaryConfigFile openings(
        "# ordered batch\n2@0,2@1\n\n4@5, 2@0 # comment\n");
    const auto parsed_openings = solver2048::loadOpeningsFile(openings.path);
    check(parsed_openings.size() == 2 &&
              solver2048::formatOpening(parsed_openings[0]) == "2@0,2@1" &&
              solver2048::formatOpening(parsed_openings[1]) == "2@0,4@5",
          "ordered openings-file parsing mismatch");
    TemporaryConfigFile duplicate_openings("2@0,2@1\n2@1,2@0\n");
    checkThrows(
        [&] { (void)solver2048::loadOpeningsFile(duplicate_openings.path); },
        "duplicate batch openings must be rejected");

    std::string legacy_text = validConfigText();
    const std::string active_setting = "rzone_active_pruning=true\n";
    legacy_text.erase(legacy_text.find(active_setting), active_setting.size());
    TemporaryConfigFile legacy(legacy_text);
    const SolverConfig legacy_config = solver2048::loadSolverConfig(legacy.path);
    check(legacy_config.rzone_trimming && legacy_config.rzone_full_board_placement_fallback,
          "legacy cfg files must keep trimming and full-board fallback enabled");
    for (bool trimming : {false, true}) {
        for (bool fallback : {false, true}) {
            TemporaryConfigFile toggles(validConfigText() +
                "rzone_trimming=" + (trimming ? "true" : "false") + "\n" +
                "rzone_full_board_placement_fallback=" + (fallback ? "true" : "false") + "\n");
            const auto config = solver2048::loadSolverConfig(toggles.path);
            check(config.rzone_trimming == trimming &&
                      config.rzone_full_board_placement_fallback == fallback,
                  "RZone generation switches must parse independently");
        }
    }
    for (const auto* key : {"rzone_trimming", "rzone_full_board_placement_fallback"}) {
        TemporaryConfigFile invalid(validConfigText() + key + "=maybe\n");
        checkThrows([&] { (void)solver2048::loadSolverConfig(invalid.path); },
                    "RZone generation switches must reject invalid boolean values");
        TemporaryConfigFile duplicate(validConfigText() + key + "=true\n" + key + "=false\n");
        checkThrows([&] { (void)solver2048::loadSolverConfig(duplicate.path); },
                    "RZone generation switches must reject duplicate keys");
    }
    check(!legacy_config.rzone_active_pruning,
          "legacy cfg files must default active pruning to disabled");

    TemporaryConfigFile unknown(validConfigText() + "unknown_option=1\n");
    checkThrows([&] { (void)solver2048::loadSolverConfig(unknown.path); },
                "unknown cfg keys must be rejected");

    std::string invalid_rzone_policy = validConfigText();
    const std::string valid_policy = "rzone_lookup_policy=smallest_first";
    std::string radix_config_text = validConfigText();
    radix_config_text.replace(radix_config_text.find(valid_policy), valid_policy.size(),
                              "rzone_lookup_policy=radix_tree");
    TemporaryConfigFile radix_config(radix_config_text);
    check(solver2048::loadSolverConfig(radix_config.path).rzone_lookup_policy ==
              RZoneLookupPolicy::RadixTree,
          "radix_tree lookup policy must parse from cfg");
    invalid_rzone_policy.replace(
        invalid_rzone_policy.find(valid_policy), valid_policy.size(),
        "rzone_lookup_policy=all_matches");
    TemporaryConfigFile invalid_policy(invalid_rzone_policy);
    checkThrows([&] { (void)solver2048::loadSolverConfig(invalid_policy.path); },
                "unknown RZone lookup policy must be rejected");

    std::string invalid_active_mode = validConfigText();
    const std::string lookup_mode = "rzone_mode=lookup";
    invalid_active_mode.replace(
        invalid_active_mode.find(lookup_mode), lookup_mode.size(),
        "rzone_mode=generate");
    TemporaryConfigFile active_mode_dependency(invalid_active_mode);
    checkThrows([&] { (void)solver2048::loadSolverConfig(active_mode_dependency.path); },
                "RZone active pruning outside lookup mode must be rejected in cfg");

    TemporaryConfigFile duplicate(validConfigText() + "winning_tile=8\n");
    checkThrows([&] { (void)solver2048::loadSolverConfig(duplicate.path); },
                "duplicate cfg keys must be rejected");

    std::string invalid_dependency = validConfigText();
    const std::string enabled = "use_transposition_table=TRUE";
    invalid_dependency.replace(invalid_dependency.find(enabled), enabled.size(),
                               "use_transposition_table=false");
    TemporaryConfigFile dependency(invalid_dependency);
    checkThrows([&] { (void)solver2048::loadSolverConfig(dependency.path); },
                "TT symmetry without exact TT must be rejected in cfg");

    std::string invalid_persistence = validConfigText();
    invalid_persistence.replace(invalid_persistence.find(enabled), enabled.size(),
                                "use_transposition_table=false");
    const std::string symmetry_enabled = "use_tt_symmetry=1";
    invalid_persistence.replace(
        invalid_persistence.find(symmetry_enabled), symmetry_enabled.size(),
        "use_tt_symmetry=false");
    TemporaryConfigFile persistence_dependency(invalid_persistence);
    checkThrows([&] { (void)solver2048::loadSolverConfig(persistence_dependency.path); },
                "persistent TT load/save without exact TT must be rejected in cfg");

    std::string invalid_rz_persistence = validConfigText();
    invalid_rz_persistence.replace(
        invalid_rz_persistence.find(lookup_mode), lookup_mode.size(),
        "rzone_mode=disabled");
    TemporaryConfigFile rz_persistence_dependency(invalid_rz_persistence);
    checkThrows([&] { (void)solver2048::loadSolverConfig(rz_persistence_dependency.path); },
                "persistent RZone load/save while RZone is disabled must be rejected in cfg");

    std::string duplicate_opening = validConfigText();
    const std::string opening = "opening=4@5, 2@0 # inline comment";
    duplicate_opening.replace(duplicate_opening.find(opening), opening.size(),
                              "opening=2@0,4@0");
    TemporaryConfigFile duplicate_position(duplicate_opening);
    checkThrows([&] { (void)solver2048::loadSolverConfig(duplicate_position.path); },
                "duplicate opening positions must be rejected");
}

Board asymmetricBoard()
{
    // Nibbles are tile exponents. This board deliberately has no D4 self-symmetry.
    return board(0x1213030030200211ull);
}

void testD4CanonicalizationAndActionTransforms()
{
    const Board original = asymmetricBoard();
    const solver2048::CanonicalBoard canonical = solver2048::canonicalize(original);
    std::unordered_set<std::uint64_t> orbit;

    for (std::uint8_t transform = 0; transform < 8; ++transform) {
        const Board transformed = solver2048::transformBoard(original, transform);
        orbit.insert(transformed.raw);
        check(solver2048::canonicalize(transformed).board == canonical.board,
              "every D4 version must have the same canonical minimum");
        check(solver2048::transformBoard(
                  transformed, solver2048::inverseTransform(transform)) == original,
              "a D4 transform followed by its inverse must restore the board");

        for (int position = 0; position < 16; ++position) {
            const Board marker = board(std::uint64_t{1} << (position * 4));
            const int mapped = solver2048::transformPosition(position, transform);
            check(solver2048::transformBoard(marker, transform) ==
                      board(std::uint64_t{1} << (mapped * 4)),
                  "position transform must match board::isom64");
        }

        for (unsigned opcode = 0; opcode < 4; ++opcode) {
            moporgic::solver_api::RawBoard original_afterstate = 0;
            moporgic::solver_api::RawBoard transformed_afterstate = 0;
            const int original_reward = moporgic::solver_api::slide(
                original.raw, opcode, original_afterstate);
            const Direction mapped_direction = solver2048::transformDirection(
                static_cast<Direction>(opcode), transform);
            const int transformed_reward = moporgic::solver_api::slide(
                transformed.raw,
                static_cast<unsigned>(solver2048::directionOpcode(mapped_direction)),
                transformed_afterstate);
            check(original_reward == transformed_reward,
                  "a transformed slide must preserve reward and legality");
            check(solver2048::transformBoard(board(original_afterstate), transform) ==
                      board(transformed_afterstate),
                  "board transform and mapped slide must commute");
        }
    }
    check(orbit.size() == 8, "asymmetric transform test board must have eight distinct versions");
    check(canonical.board.raw == *std::min_element(orbit.begin(), orbit.end()),
          "canonical board must be the minimum raw value in its D4 orbit");
    FakeGame game;
    checkThrows([&] { (void)Solver(game, SolverOptions{8, 0, 0, false, true}); },
                "TT symmetry without TT must be rejected");
}

void testPlayerSymmetryTranspositionHitAndWitnessMapping()
{
    const Board source = asymmetricBoard();
    const Board rotated = solver2048::transformBoard(source, 1);
    const Board source_win = board(0x0000000000000022ull);
    const Board rotated_win = solver2048::transformBoard(source_win, 1);

    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 1.0;
    game.placements[10] = {
        placement(0, 2, source.raw),
        placement(0, 4, rotated.raw),
    };
    game.slides[source.raw] = {slide(Direction::Up, source_win.raw, 4)};
    game.slides[rotated.raw] = {
        slide(solver2048::transformDirection(Direction::Up, 1), rotated_win.raw, 4),
    };
    game.values[source_win.raw] = 5.0;
    game.values[rotated_win.raw] = 7.0;
    game.winning_states = {source_win.raw, rotated_win.raw};

    SearchResult result = Solver(game, SolverOptions{8, 0, 0, true, true}).solve();
    check(result.outcome == Outcome::Win,
          "symmetry-equivalent winning Player states must keep Environment AND winning");
    check(result.statistics.player_tt_hits == 1 &&
              result.statistics.player_tt_symmetry_hits == 1,
          "rotated Player state must be counted as a symmetry-derived TT hit");
    check(result.statistics.playerTtExactHits() == 0,
          "rotated Player hit must not be counted as exact-orientation");
    const TraceNode& environment = *result.root->children[0];
    const TraceNode& source_trace = *environment.children[0];
    const TraceNode& hit = *environment.children[1];
    check(source_trace.expanded && !source_trace.tt_hit,
          "first Player orientation must remain the expanded TT source");
    check(hit.tt_hit && hit.tt_symmetry_hit && hit.tt_transform.has_value(),
          "rotated Player occurrence must carry symmetry TT metadata");
    check(hit.selected_action && hit.selected_action->kind == SearchAction::Kind::Slide &&
              hit.selected_action->direction ==
                  solver2048::transformDirection(Direction::Up, 1),
          "canonical Player witness must map back to the rotated direction");
    check(hit.selected_action_value && *hit.selected_action_value == 11.0,
          "symmetry Player hit must recompute reward + V for its mapped witness");
    check(hit.tt_source_trace_id && *hit.tt_source_trace_id == source_trace.trace_id,
          "symmetry Player hit must reference the original proof trace");
}

void testEnvironmentSymmetryWinTranspositionHit()
{
    const Board source = asymmetricBoard();
    const Board rotated = solver2048::transformBoard(source, 1);
    constexpr int source_position = 1;

    FakeGame game;
    game.slides[1] = {slide(Direction::Up, 10)};
    game.values[10] = 30.0;
    game.placements[10] = {
        placement(0, 2, 20),
        placement(0, 4, 21),
    };
    game.slides[20] = {slide(Direction::Up, source.raw)};
    game.slides[21] = {slide(Direction::Right, rotated.raw)};
    game.values[source.raw] = 20.0;
    game.values[rotated.raw] = 10.0;
    game.placements[source.raw] = {placement(source_position, 2, 30)};
    game.winning_states.insert(30);

    SearchResult result = Solver(game, SolverOptions{8, 0, 0, true, true}).solve();
    check(result.outcome == Outcome::Win && result.root_action == Direction::Up,
          "symmetry-equivalent winning Environment states must preserve root Win");
    check(result.statistics.environment_tt_hits == 1 &&
              result.statistics.environment_tt_symmetry_hits == 1,
          "rotated winning Environment afterstate must produce one symmetry TT hit");
    check(result.statistics.environmentTtExactHits() == 0,
          "rotated Environment hit must not be counted as exact-orientation");
    check(game.placement_calls[source.raw] == 1 && game.placement_calls[rotated.raw] == 0,
          "Environment symmetry hit must not regenerate rotated placements");

    const TraceNode& root_environment = *result.root->children[0];
    const TraceNode& first_player = *root_environment.children[0];
    const TraceNode& second_player = *root_environment.children[1];
    const TraceNode& source_trace = *first_player.children[0];
    const TraceNode& hit = *second_player.children[0];
    check(source_trace.expanded && source_trace.outcome == Outcome::Win && !source_trace.tt_hit,
          "first winning Environment orientation must be the expanded TT source");
    check(hit.outcome == Outcome::Win && hit.tt_hit && hit.tt_symmetry_hit &&
              hit.tt_transform.has_value() && !hit.selected_action,
          "rotated Environment Win hit must carry symmetry metadata without a witness");
    check(hit.tt_source_trace_id && *hit.tt_source_trace_id == source_trace.trace_id,
          "symmetry Environment Win hit must reference the original proof trace");
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
    check(result.statistics.player_nodes == 3 &&
              result.statistics.environment_nodes == 2,
          "visited-node counters must include both repeated-state paths");
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
    // result remains usable before the visited-node limit check.
    SearchResult result = Solver(game, SolverOptions{8, 3, 0, true}).solve();

    check(result.outcome == Outcome::Win,
          "two identical winning Player states should make Environment AND a Win");
    check(result.statistics.player_nodes == 3 &&
              result.statistics.environment_nodes == 2,
          "visited-node counters must include the TT source, hit, and terminal leaf");
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
    check(source.solved_by == SolvedBy::Backpropagation &&
              hit.solved_by == SolvedBy::TranspositionTable,
          "Player TT source/hit solved_by values are wrong");
    check(hit.terminal_reason == TerminalReason::TranspositionTable,
          "Player TT hit must record TRANSPOSITION_TABLE as its terminal reason");
    check(hit.tt_source_trace_id && *hit.tt_source_trace_id == source.trace_id,
          "Player TT hit must point to the first occurrence's trace ID");
    check(hit.trace_id != 0 && hit.trace_id != source.trace_id,
          "TT hit must remain a distinct, addressable trace node");
    check(hit.selected_action && hit.selected_action->kind == SearchAction::Kind::Slide &&
              hit.selected_action->direction == Direction::Right,
          "Player TT hit must restore the cached winning action");
    check(hit.selected_action_value && *hit.selected_action_value == 5.0,
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

void testLossOutcomesAreNotCached()
{
    FakeGame baseline_game = environmentTranspositionGame();
    SearchResult baseline = Solver(baseline_game, SolverOptions{8, 0, 0, false}).solve();

    FakeGame tt_game = environmentTranspositionGame();
    SearchResult cached = Solver(tt_game, SolverOptions{8, 0, 0, true}).solve();

    check(baseline.outcome == Outcome::Win && cached.outcome == baseline.outcome,
          "TT on/off must produce the same root outcome");
    check(baseline.root_action == Direction::Down && cached.root_action == baseline.root_action,
          "TT on/off must select the same proven winning root action");
    check(baseline.root_action_value && cached.root_action_value &&
              *baseline.root_action_value == *cached.root_action_value,
          "TT on/off must preserve the selected root action value");
    check(baseline_game.placement_calls[10] == 2,
          "TT-off baseline must re-expand the repeated Environment state");
    check(baseline.statistics.ttHits() == 0 && baseline.statistics.ttStores() == 0,
          "TT-off outcome comparison must not use either table");

    check(tt_game.placement_calls[10] == 2,
          "a repeated losing Environment state must be expanded again");
    check(cached.statistics.ttHits() == 0,
          "Loss outcomes must never produce a TT hit");
    check(cached.statistics.player_tt_stores == 1 &&
              cached.statistics.environment_tt_stores == 0,
          "only the final Player Win may be stored in the Win-only TT");

    check(cached.root->children.size() == 3,
          "Player must visit two losing actions before the final winning action");
    const TraceNode& source = *cached.root->children[0];
    const TraceNode& repeated = *cached.root->children[1];
    check(source.expanded && source.outcome == Outcome::Loss && !source.tt_hit,
          "first losing Environment occurrence must remain an expanded DFS node");
    check(repeated.expanded && repeated.outcome == Outcome::Loss && !repeated.tt_hit &&
              !repeated.tt_source_trace_id,
          "second losing Environment occurrence must not become a TT leaf");
    check(source.solved_by == SolvedBy::Backpropagation &&
              repeated.solved_by == SolvedBy::Backpropagation,
          "both losing Environment occurrences must be solved by backpropagation");
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
    check(result.statistics.combinedNodes() == 4,
          "Unknown TT graph visited-node count mismatch");
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
    check(result.statistics.combinedNodes() == 3,
          "node-limit graph must count both attempted cutoff children");
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

struct TemporaryTtFile {
    TemporaryTtFile()
    {
        static std::uint64_t sequence = 0;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("solving2048-tt-test-" + std::to_string(nonce) + "-" +
                std::to_string(++sequence) + ".tt");
    }

    ~TemporaryTtFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".tmp", ignored);
    }

    std::filesystem::path path;
};

struct TemporaryRzFile {
    TemporaryRzFile()
    {
        static std::uint64_t sequence = 0;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("solving2048-rz-test-" + std::to_string(nonce) + "-" +
                std::to_string(++sequence) + ".rz");
    }

    ~TemporaryRzFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".tmp", ignored);
    }

    std::filesystem::path path;
};

void testProofSequenceManifestRoundTrip()
{
    TemporaryTtFile manifest_file;
    const std::filesystem::path base = manifest_file.path.parent_path();
    solver2048::ProofSequenceManifest expected;
    expected.tt_path = base / "batch-proof.tt";
    expected.rz_path = base / "batch-proof.rz";
    expected.sgf_paths = {
        base / "batch-sgf-000001.sgf",
        base / "batch-sgf-000002.sgf",
    };
    solver2048::writeProofSequenceManifest(manifest_file.path, expected);
    const solver2048::ProofSequenceManifest parsed =
        solver2048::readProofSequenceManifest(manifest_file.path);
    check(parsed.tt_path == expected.tt_path && parsed.rz_path == expected.rz_path &&
              parsed.sgf_paths == expected.sgf_paths,
          "proof sequence manifest paths or order changed during round trip");
}

std::uint8_t readByte(const std::filesystem::path& path, std::uint64_t offset)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail("cannot open temporary TT file for byte read");
    input.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    input.read(&value, 1);
    if (!input) fail("cannot read byte from temporary TT file");
    return static_cast<std::uint8_t>(value);
}

void writeByte(const std::filesystem::path& path, std::uint64_t offset, std::uint8_t value)
{
    std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!output) fail("cannot open temporary TT file for byte mutation");
    output.seekp(static_cast<std::streamoff>(offset));
    output.put(static_cast<char>(value));
    if (!output) fail("cannot mutate byte in temporary TT file");
}

void refreshRzChecksum(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail("cannot open temporary RZone file for checksum update");
    input.seekg(static_cast<std::streamoff>(RzFile::kHeaderSize));
    if (!input) fail("cannot seek to the temporary RZone payload");

    constexpr std::uint64_t polynomial = 0x42f0e1eba9ea3693ull;
    std::uint64_t checksum = 0;
    char character = 0;
    while (input.get(character)) {
        checksum ^= static_cast<std::uint64_t>(
                        static_cast<std::uint8_t>(character))
                    << 56;
        for (unsigned bit = 0; bit < 8; ++bit) {
            checksum = (checksum & (std::uint64_t{1} << 63)) != 0
                ? (checksum << 1) ^ polynomial
                : checksum << 1;
        }
    }
    if (!input.eof()) fail("cannot read the temporary RZone payload");
    for (unsigned byte = 0; byte < 8; ++byte) {
        writeByte(
            path,
            64 + byte,
            static_cast<std::uint8_t>(checksum >> (byte * 8)));
    }
}

void testPersistentRzRoundTripAndMaxCellIndexing()
{
    MechanicsGame game;

    TemporaryRzFile empty_file;
    RZoneTable empty_source(game, 16, 16);
    const RzFileSummary empty_saved = RzFile::save(empty_file.path, empty_source);
    check(empty_saved.record_count == 0 &&
              empty_saved.file_size == RzFile::kHeaderSize,
          "empty RZone snapshot has an invalid summary");
    RZoneTable empty_loaded(game, 16, 16);
    const RzFileSummary empty_summary = RzFile::load(empty_file.path, empty_loaded);
    check(empty_summary.record_count == 0 && empty_loaded.size() == 0,
          "empty RZone snapshot did not round trip");

    RZoneTable source(game, 16, 16);
    const auto terminal = source.createTerminal(tiles({{0, 16}}), Phase::Environment);
    check(terminal.has_value(), "persistent RZone terminal fixture is missing");
    const RzFileSummary terminal_prefix = RzFile::summarize(source);
    const auto fixed = source.liftFixedSlide(
        tiles({{0, 8}, {1, 8}}), Direction::Left, *terminal);
    check(fixed.has_value(), "persistent RZone FixedSlide fixture is missing");
    const auto universal = source.liftUniversalPlacement(
        tiles({{0, 8}, {1, 8}}), *fixed);
    check(universal.has_value(), "persistent RZone UniversalPlacement fixture is missing");
    check(source.size() == 3, "persistent RZone fixture must contain a three-record DAG");

    TemporaryRzFile legacy_snapshot;
    RzFile::save(legacy_snapshot.path, source);
    writeByte(legacy_snapshot.path, 28, 1);
    RZoneTable legacy_loaded(game, 16);
    check(RzFile::load(legacy_snapshot.path, legacy_loaded).record_count == 3 &&
              RzProofTable::load(legacy_snapshot.path).summary().record_count == 3,
          "both readers must continue accepting semantics-v1 snapshots");

    TemporaryRzFile snapshot;
    const RzFileSummary saved = RzFile::save(snapshot.path, source);
    const RzFileSummary summarized = RzFile::summarize(source);
    const RzFileSummary incrementally_summarized =
        RzFile::summarize(source, &terminal_prefix);
    check(saved.record_count == 3 &&
              summarized.record_count == saved.record_count &&
              summarized.payload_checksum == saved.payload_checksum &&
              summarized.file_size == saved.file_size &&
              incrementally_summarized.record_count == saved.record_count &&
              incrementally_summarized.payload_checksum == saved.payload_checksum &&
              saved.file_size == RzFile::kHeaderSize + 3 * RzFile::kRecordSize &&
              std::filesystem::file_size(snapshot.path) == saved.file_size &&
              !std::filesystem::exists(snapshot.path.string() + ".tmp"),
          "RZone writer/summary produced invalid data or left its temporary file");
    const RzProofTable proof_table = RzProofTable::load(snapshot.path);
    check(proof_table.summary().record_count == saved.record_count &&
              proof_table.prefixChecksum(saved.record_count) == saved.payload_checksum &&
              proof_table.find(*fixed) != nullptr &&
              RzProofTable::matches(
                  *proof_table.find(*fixed),
                  tiles({{0, 8}, {1, 8}, {8, 4}}),
                  Phase::Player),
          "independent proof-checker RZone reader rejected or changed a valid snapshot");

    RZoneTable loaded(game, 16, 16);
    const RzFileSummary loaded_summary = RzFile::load(snapshot.path, loaded);
    check(loaded_summary.record_count == saved.record_count &&
              loaded_summary.payload_checksum == saved.payload_checksum &&
              loaded.size() == source.size() &&
              loaded.statistics().loaded_certificates == 3,
          "persistent RZone round trip changed its summary or record count");
    for (RZoneCertificateId id = 1; id <= source.size(); ++id) {
        const RZoneCertificate* expected = source.find(id);
        const RZoneCertificate* actual = loaded.find(id);
        check(expected && actual && actual->id == expected->id &&
                  actual->kind == expected->kind && actual->phase == expected->phase &&
                  actual->winning_tile == expected->winning_tile &&
                  actual->mask == expected->mask &&
                  actual->masked_board == expected->masked_board &&
                  actual->slide == expected->slide &&
                  actual->terminal_position == expected->terminal_position &&
                  actual->child == expected->child && actual->depth == expected->depth &&
                  actual->validated,
              "persistent RZone round trip changed a certificate");
    }
    const auto loaded_fixed = loaded.lookup(
        tiles({{0, 8}, {1, 8}, {8, 4}}), Phase::Player);
    const auto loaded_universal = loaded.lookup(
        tiles({{0, 8}, {1, 8}}), Phase::Environment);
    check(loaded_fixed && loaded_fixed->id == *fixed && loaded_fixed->transform == 0 &&
              loaded_universal && loaded_universal->id == *universal &&
              loaded_universal->transform == 0,
          "loaded RZone certificates are not available through the rebuilt index");

    TemporaryRzFile rewritten;
    const RzFileSummary rewritten_summary = RzFile::save(rewritten.path, loaded);
    check(rewritten_summary.record_count == saved.record_count &&
              rewritten_summary.payload_checksum == saved.payload_checksum,
          "rewriting a loaded RZone snapshot changed IDs or payload");

    RZoneTable limited(game, 16, 1);
    const RzFileSummary limited_summary = RzFile::load(snapshot.path, limited);
    check(limited_summary.record_count == 3 && limited.size() == 3 &&
              limited.find(*universal) != nullptr &&
              limited.statistics().masks_indexed == 1,
          "max_cells must not discard loaded records or change certificate IDs");
    const auto limited_terminal = limited.lookup(
        tiles({{0, 16}}), Phase::Environment);
    check(!limited.lookup(tiles({{0, 8}, {1, 8}}), Phase::Player) &&
              limited_terminal && limited_terminal->id == *terminal &&
              limited_terminal->transform == 0,
          "max_cells must limit only the rebuilt lookup index");
}

void testRZonePhaseIndexesAndRelocation()
{
    MechanicsGame game;
    for (const auto policy : {RZoneLookupPolicy::FullScan, RZoneLookupPolicy::SmallestFirst,
                              RZoneLookupPolicy::RadixTree}) {
        for (const bool symmetry : {false, true}) {
            RZoneTable source(game, 16, 16, policy, symmetry, false);
            const Board terminal = tiles({{0, 16}});
            const auto environment = source.createTerminal(terminal, Phase::Environment);
            check(environment.has_value(), "phase-index terminal fixture missing");
            check(!source.lookup(terminal, Phase::Player) &&
                      source.statistics().lookup_masks_tested == 0,
                  "lookup must not scan masks that only exist in the other phase");
            const auto player = source.createTerminal(terminal, Phase::Player);
            const auto hit = source.lookup(terminal, Phase::Player);
            const unsigned transforms = symmetry ? 8 : 1;
            const bool radix = policy == RZoneLookupPolicy::RadixTree;
            check(player && hit && hit->id == *player && hit->transform == 0 &&
                      source.statistics().masks_indexed == 1 &&
                      source.statistics().lookup_masks_tested == (radix ? 0 : transforms) &&
                      source.statistics().lookup_candidates == (!radix && symmetry ? 2 : 1),
                  "shared masks must count once and exclude opposite-phase candidates");

            std::vector<Board> queries = {Board{}, terminal, tiles({{0, 32}})};
            // Grow the owning mask map and inner buckets after caching their
            // addresses. Include constrained empty cells and shared phase masks.
            for (int position = 0; position < 16; ++position) {
                for (const auto phase : {Phase::Player, Phase::Environment}) {
                    source.createTerminal(tiles({{position, 16}}), phase);
                    source.createTerminal(tiles({{position, 32}}), phase);
                }
            }
            for (const auto direction : {Direction::Left, Direction::Up}) {
                for (int line = 0; line < 4; ++line) {
                    const auto position = [&](int offset) {
                        return direction == Direction::Left ? line * 4 + offset : offset * 4 + line;
                    };
                    const auto child = source.createTerminal(
                        tiles({{position(0), 16}}), Phase::Environment);
                    for (int first = 0; first < 4; ++first) {
                        for (int second = first + 1; second < 4; ++second) {
                            const Board parent = tiles({{position(first), 8}, {position(second), 8}});
                            const auto fixed = source.liftFixedSlide(parent, direction, *child);
                            check(fixed.has_value(), "index growth fixture must produce a slide proof");
                            source.liftUniversalPlacement(parent, *fixed);
                            for (std::uint8_t transform = 0; transform < 8; ++transform) {
                                queries.push_back(solver2048::transformBoard(parent, transform));
                            }
                            queries.push_back(Board{parent.raw | (std::uint64_t{1} << (position(0) * 4))});
                        }
                    }
                }
            }
            check(source.statistics().masks_indexed > 32,
                  "growth fixture must exercise many distinct masks");
            std::uint64_t seed = 0xadc83b19;
            for (int sample = 0; sample < 256; ++sample) {
                std::uint64_t raw = 0;
                for (unsigned position = 0; position < 16; ++position) {
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    raw |= ((seed >> 32) % 4) << (position * 4);
                }
                queries.push_back(Board{raw});
            }

            // Independent oracle scans immutable certificates, with no use of
            // either lookup index, and checks the complete deterministic rank.
            auto compareWithOracle = [&](RZoneTable& table, unsigned max_cells) {
                for (const Board query : queries) {
                    for (const auto phase : {Phase::Player, Phase::Environment}) {
                        std::optional<std::tuple<unsigned, unsigned, RZoneCertificateId, std::uint8_t>> best;
                        for (RZoneCertificateId id = 1; id <= source.size(); ++id) {
                            const auto& certificate = *source.find(id);
                            const unsigned cells = RZoneTable::countCells(certificate.mask);
                            if (certificate.phase != phase || cells > max_cells) continue;
                            for (std::uint8_t transform = 0; transform < transforms; ++transform) {
                                const Board candidate = solver2048::transformBoard(query, transform);
                                if (RZoneTable::maskedBoard(candidate, certificate.mask) != certificate.masked_board) continue;
                                const auto rank = std::make_tuple(cells, certificate.depth, id, transform);
                                if (!best || rank < *best) best = rank;
                            }
                        }
                        const auto actual = table.lookup(query, phase);
                        check(actual.has_value() == best.has_value() &&
                                  (!best || (actual->id == std::get<2>(*best) &&
                                             actual->transform == std::get<3>(*best))),
                              "indexed lookup must match the certificate-scan oracle");
                    }
                }
            };
            compareWithOracle(source, 16);

            TemporaryRzFile snapshot;
            const auto saved = RzFile::save(snapshot.path, source);
            auto moved = [&] {
                RZoneTable loaded(game, 16, 16, policy, symmetry, false);
                RzFile::load(snapshot.path, loaded);
                return RZoneTable(std::move(loaded));
            }(); // Destroy the moved-from owner before using the borrowed buckets.
            compareWithOracle(moved, 16);
            check(RzFile::summarize(moved).payload_checksum == saved.payload_checksum,
                  "index rebuilding and moving must preserve certificate IDs and payload");
            checkThrows([&] { RzFile::load(snapshot.path, moved); },
                        "loading into a populated table must still be rejected");
            compareWithOracle(moved, 16);

            RZoneTable limited(game, 16, 2, policy, symmetry, false);
            RzFile::load(snapshot.path, limited);
            compareWithOracle(limited, 2);
        }
    }
}

void testRZoneIndexCrossOpeningHandoff()
{
    const Board opening = tiles({{0, 8}, {1, 8}});
    MechanicsGame next_game(solver2048::transformBoard(opening, 1));
    SolverOptions options{16, 0, 0, true, true};
    options.rzone_mode = RZoneMode::Lookup;
    options.rzone_lookup_policy = RZoneLookupPolicy::RadixTree;
    options.use_rzone_symmetry = true;
    Solver next(next_game, options);
    RzFileSummary expected;
    std::uint64_t masks = 0;
    {
        MechanicsGame first_game(opening);
        Solver first(first_game, options);
        const auto result = first.solve();
        check(result.outcome == Outcome::Win, "handoff fixture must prove the first opening");
        masks = result.statistics.rzone.masks_indexed;
        expected = RzFile::summarize(first);
        next.adoptPersistentProofs(first);
    }
    const auto result = next.solve();
    check(result.outcome == Outcome::Win && result.statistics.combinedNodes() == 1 &&
              result.root->solved_by == SolvedBy::RZoneTable &&
              result.statistics.rzone.loaded_certificates == expected.record_count &&
              result.statistics.rzone.masks_indexed == masks &&
              RzFile::summarize(next).payload_checksum == expected.payload_checksum,
          "phase bucket references must survive handoff and destruction of the prior Solver");
}

void testFullBoardPlacementFallback()
{
    MechanicsGame game;
    // Either row can win, but a spawn in its gap changes the required proof.
    const Board parent = tiles({{0, 8}, {2, 8}, {4, 8}, {6, 8}});
    RZoneTable table(game, 16);
    std::vector<RZoneMatch> witnesses;
    const auto placements = game.legalPlacements(parent);
    for (const auto& placement : placements) {
        const auto slides = game.legalSlides(placement.board);
        const auto left = std::find_if(slides.begin(), slides.end(), [](const auto& slide) {
            return slide.direction == Direction::Left;
        });
        check(left != slides.end(), "fallback fixture must have a Left slide");
        const auto terminal = table.createTerminal(left->afterstate, Phase::Environment);
        check(terminal.has_value(), "every fallback branch must immediately win");
        const auto child = table.liftFixedSlide(placement.board, Direction::Left, *terminal);
        check(child.has_value(), "fallback child must have a valid proof");
        check(!table.liftUniversalPlacement(parent, *child),
              "fixture must require different proofs for different placements");
        witnesses.push_back({*child, 0});
    }
    const auto fallback = table.liftFullBoardPlacement(parent, witnesses);
    check(fallback.has_value(), "complete branch proofs must produce a full-board fallback");
    const auto* entry = table.find(*fallback);
    check(entry->kind == RZoneStepKind::FullBoardPlacement && entry->mask == 0xffff &&
              entry->masked_board == parent.raw && entry->depth == 3,
          "fallback must preserve the exact board, including empty cells");
    check(table.lookup(parent, Phase::Environment).has_value() &&
              !table.lookup(placements.front().board, Phase::Environment),
          "full-board fallback must not match a changed empty cell");
    auto missing = witnesses;
    missing.pop_back();
    check(!table.liftFullBoardPlacement(parent, missing), "missing placement must be rejected");
    auto invalid = witnesses;
    invalid.front().id = *fallback;
    check(!table.liftFullBoardPlacement(parent, invalid), "wrong child phase must be rejected");
    invalid = witnesses;
    invalid.front().transform = 8;
    check(!table.liftFullBoardPlacement(parent, invalid), "invalid transform must be rejected");

    TemporaryRzFile snapshot;
    RzFile::save(snapshot.path, table);
    RZoneTable loaded(game, 16);
    RzFile::load(snapshot.path, loaded);
    const auto proof = RzProofTable::load(snapshot.path);
    check(loaded.lookup(parent, Phase::Environment).has_value() &&
              proof.find(*fallback)->kind == RZoneStepKind::FullBoardPlacement,
          "both readers must validate and retain full-board proofs");
    RZoneTable disabled(game, 16, 16, RZoneLookupPolicy::FullScan, false, true, false);
    RzFile::load(snapshot.path, disabled);
    const auto disabled_size = disabled.size();
    check(!disabled.liftFullBoardPlacement(parent, witnesses) &&
              disabled.size() == disabled_size &&
              disabled.statistics().full_board_placement_certificates == 0,
          "disabled fallback must not generate full-board placement certificates");
    check(disabled.lookup(parent, Phase::Environment).has_value(),
          "disabled generation must preserve lookup of loaded fallback certificates");
    RZoneTable symmetric(game, 16, 16, RZoneLookupPolicy::SmallestFirst, true);
    RzFile::load(snapshot.path, symmetric);
    for (std::uint8_t transform = 0; transform < 8; ++transform) {
        const auto rotated = solver2048::transformBoard(parent, transform);
        const auto match = symmetric.lookup(rotated, Phase::Environment);
        check(match && match->id == *fallback &&
                  RzProofTable::matches(*proof.find(*fallback), rotated,
                                        Phase::Environment, match->transform),
              "full-board fallback must retain sound D4 lookup and orientation mapping");
    }
    RZoneTable limited(game, 16, 15);
    RzFile::load(snapshot.path, limited);
    check(!limited.lookup(parent, Phase::Environment) &&
              !limited.liftFullBoardPlacement(parent, witnesses),
          "fallback must respect rzone_max_cells");

    const auto offset = RzFile::kHeaderSize + (*fallback - 1) * RzFile::kRecordSize;
    auto reject = [&](const char* message) {
        refreshRzChecksum(snapshot.path);
        RZoneTable rejected(game, 16);
        checkThrows([&] { RzFile::load(snapshot.path, rejected); }, message);
        checkThrows([&] { RzProofTable::load(snapshot.path); }, message);
        check(rejected.size() == 0, "invalid proof must not be partially published");
    };
    writeByte(snapshot.path, offset + 8, 1); // Only an Environment terminal is in this prefix.
    reject("fallback with uncovered placements must be rejected");
    RzFile::save(snapshot.path, table);
    writeByte(snapshot.path, offset + 16, 0xfe);
    reject("fallback must not accept a partial-board mask");
    RzFile::save(snapshot.path, table);
    writeByte(snapshot.path, offset + 12, 2);
    reject("fallback must not accept insufficient proof depth");
    RzFile::save(snapshot.path, table);
    writeByte(snapshot.path, offset + 8, static_cast<std::uint8_t>(*fallback));
    reject("fallback must not refer to itself or later certificates");
    RzFile::save(snapshot.path, table);
    writeByte(snapshot.path, 28, 1);
    reject("old semantics must not silently accept the new certificate kind");

    // A full-board Environment certificate can be lifted through a Player slide.
    RZoneTable lift_table(game, 16);
    const Board compact = tiles({{0, 8}, {1, 8}});
    std::vector<RZoneMatch> compact_witnesses;
    for (const auto& placement : game.legalPlacements(compact)) {
        const auto slides = game.legalSlides(placement.board);
        const auto left = std::find_if(slides.begin(), slides.end(), [](const auto& slide) {
            return slide.direction == Direction::Left;
        });
        const auto terminal = lift_table.createTerminal(left->afterstate, Phase::Environment);
        const auto child = lift_table.liftFixedSlide(placement.board, Direction::Left, *terminal);
        compact_witnesses.push_back({*child, 0});
    }
    const auto full = lift_table.liftFullBoardPlacement(compact, compact_witnesses);
    const auto upper = lift_table.liftFixedSlide(
        tiles({{12, 8}, {13, 8}}), Direction::Up, *full);
    check(upper.has_value(), "full-board fallback must allow upward Player propagation");
    RzFile::save(snapshot.path, lift_table);
    (void)RzProofTable::load(snapshot.path);
}

void testImprovedRZoneTrimming()
{
    MechanicsGame game;
    RZoneTable table(game, 32);
    const auto terminal = table.createTerminal(tiles({{13, 32}}), Phase::Environment);
    const Board first = tiles({{0, 2}, {4, 4}, {8, 8}, {9, 2},
                               {12, 2}, {13, 16}, {14, 16}, {15, 2}});
    RZoneTable untrimmed(game, 32, 16, RZoneLookupPolicy::FullScan, false, false);
    const auto untrimmed_terminal = untrimmed.createTerminal(tiles({{13, 32}}), Phase::Environment);
    const auto original_pair = untrimmed.liftFixedSlide(first, Direction::Left, *untrimmed_terminal);
    check(original_pair && untrimmed.find(*original_pair)->mask == 0x7000 &&
              untrimmed.statistics().trim_attempts == 0 &&
              untrimmed.statistics().trim_cells_removed == 0 &&
              untrimmed.statistics().trimmed_certificates == 0,
          "disabled trimming must retain the original preimage without trim work");
    RZoneTable untrimmed_limited(game, 32, 2, RZoneLookupPolicy::FullScan, false, false);
    const auto limited_end = untrimmed_limited.createTerminal(tiles({{13, 32}}), Phase::Environment);
    check(!untrimmed_limited.liftFixedSlide(first, Direction::Left, *limited_end),
          "disabled trimming must apply max_cells to the original preimage");
    const auto pair = table.liftFixedSlide(first, Direction::Left, *terminal);
    check(pair && table.find(*pair)->mask == 0x6000,
          "first image must trim 2,16,16 to the two adjacent 16s");
    const auto environment = table.liftUniversalPlacement(first, *pair);
    check(environment.has_value(), "trimmed pair must lift across placements");
    const Board second = tiles({{0, 4}, {1, 2}, {4, 4}, {8, 4}, {9, 8},
                                {12, 2}, {13, 8}, {14, 16}, {15, 2}});
    const auto triple = table.liftFixedSlide(second, Direction::Down, *environment);
    check(triple && table.find(*triple)->mask == 0x6200,
          "second image must retain only 8,8,16, including early-win alternatives");
    check(table.liftUniversalPlacement(second, *triple).has_value(),
          "removing the three empty constraints must enable upward placement lift");
    check(table.statistics().trim_cells_removed >= 4 &&
              table.statistics().trimmed_certificates >= 2,
          "trim statistics must record successful removals");
    RZoneTable limited(game, 32, 2);
    const auto limited_terminal = limited.createTerminal(tiles({{13, 32}}), Phase::Environment);
    check(limited.liftFixedSlide(first, Direction::Left, *limited_terminal).has_value(),
          "max_cells must be applied after trimming");

    TemporaryRzFile snapshot;
    RzFile::save(snapshot.path, table);
    RZoneTable loaded(game, 32);
    RzFile::load(snapshot.path, loaded);
    const auto proof = RzProofTable::load(snapshot.path);
    check(proof.find(*triple)->mask == 0x6200 && loaded.find(*pair)->mask == 0x6000,
          "both readers must validate improved certificates");
    for (unsigned legacy : {1u, 2u}) {
        writeByte(snapshot.path, 28, static_cast<std::uint8_t>(legacy));
        RZoneTable rejected(game, 32);
        checkThrows([&] { RzFile::load(snapshot.path, rejected); },
                    "legacy semantics must reject moved winning positions");
        checkThrows([&] { RzProofTable::load(snapshot.path); },
                    "independent legacy checker must reject new semantics");
    }

    // The discussed A,16,B,16 counterexample: freeing B is safe while A=2;
    // freeing A with B fixed to zero is safe, but freeing both is unsound.
    RZoneTable interaction(game, 32);
    const Board child_board = tiles({{1, 16}, {5, 16}});
    const auto end = interaction.createTerminal(tiles({{1, 32}}), Phase::Environment);
    const auto player = interaction.liftFixedSlide(child_board, Direction::Up, *end);
    const auto child = interaction.liftUniversalPlacement(child_board, *player);
    const Board source = tiles({{0, 2}, {1, 16}, {2, 2}, {3, 16},
                                {4, 4}, {5, 4}, {6, 16}, {7, 2}});
    const auto implies = [&](Board board, std::uint16_t mask) {
        return solver2048::improvedSlideImplies(board.raw, mask, Direction::Left, 32,
                                                child_board.raw, 0x0022);
    };
    check(implies(source, 0x00fb), "free B with A fixed must succeed");
    check(implies(Board{source.raw & ~(0xfull << 8)}, 0x00fe),
          "free A with B fixed empty must succeed");
    check(!implies(source, 0x00fa), "joint A/B variation must be rejected");
    const auto parent = interaction.liftFixedSlide(source, Direction::Left, *child);
    check(parent && (interaction.find(*parent)->mask & 1u) &&
              !(interaction.find(*parent)->mask & 4u),
          "greedy trim must keep A after removing B");
    RzFile::save(snapshot.path, interaction);
    (void)RzProofTable::load(snapshot.path);
    const auto offset = RzFile::kHeaderSize + (*parent - 1) * RzFile::kRecordSize;
    writeByte(snapshot.path, offset, readByte(snapshot.path, offset) & 0xf0u);
    writeByte(snapshot.path, offset + 16, readByte(snapshot.path, offset + 16) & 0xfeu);
    refreshRzChecksum(snapshot.path);
    RZoneTable rejected(game, 32);
    checkThrows([&] { RzFile::load(snapshot.path, rejected); },
                "loader must reject the joint-interaction counterexample");
    checkThrows([&] { RzProofTable::load(snapshot.path); },
                "independent checker must reject the joint-interaction counterexample");

    // Compare the line summary against exhaustive whole-board game mechanics,
    // including child zeros, all directions, and several free cells across lines.
    std::uint64_t rng = 0xa5b7c91342ull;
    auto random = [&]() { rng = rng * 6364136223846793005ull + 1; return rng >> 32; };
    for (int sample = 0; sample < 256; ++sample) {
        const unsigned target = sample % 2 ? 8 : 16;
        const unsigned rank_count = sample % 2 ? 3 : 4;
        Board source_board{};
        for (int p = 0; p < 16; ++p) source_board.raw |= (random() % rank_count) << (4 * p);
        std::uint16_t mask = 0xffff;
        std::vector<int> free;
        while (free.size() < 4) {
            const int p = random() % 16;
            if (!(mask & (1u << p))) continue;
            free.push_back(p);
            mask &= ~(1u << p);
        }
        const auto direction = static_cast<Direction>(sample % 4);
        const auto child_mask = static_cast<std::uint16_t>(random());
        Board expected{};
        for (const auto& move : game.legalSlides(source_board))
            if (move.direction == direction) expected = move.afterstate;
        const auto child_pattern = RZoneTable::maskedBoard(expected, child_mask);
        bool exhaustive = true;
        std::function<void(Board, unsigned)> enumerate = [&](Board board, unsigned index) {
            if (!exhaustive) return;
            if (index < free.size()) {
                const unsigned shift = 4 * free[index];
                for (unsigned rank = 0; rank < rank_count; ++rank)
                    enumerate(Board{(board.raw & ~(0xfull << shift)) |
                                    (std::uint64_t{rank} << shift)}, index + 1);
                return;
            }
            bool accepted = false;
            for (const auto& move : game.legalSlides(board)) {
                if (move.direction != direction) continue;
                accepted = game.hasWinningTile(move.afterstate, target) ||
                    RZoneTable::maskedBoard(move.afterstate, child_mask) == child_pattern;
            }
            exhaustive = accepted;
        };
        enumerate(source_board, 0);
        check(exhaustive == solver2048::improvedSlideImplies(
                  source_board.raw, mask, direction, target, child_pattern, child_mask),
              "line-based implication must equal exhaustive whole-board validation");
    }
    check(!solver2048::improvedSlideImplies(0, 0xffff, Direction::Left, 32, 0, 0xffff),
          "matching an empty child must not excuse an illegal slide");
}

void testPersistentRzRejectsIncompatibleAndCorruptFiles()
{
    MechanicsGame game;
    RZoneTable source(game, 16, 16);
    const auto terminal = source.createTerminal(tiles({{0, 16}}), Phase::Environment);
    const auto fixed = source.liftFixedSlide(
        tiles({{0, 8}, {1, 8}}), Direction::Left, *terminal);
    const auto universal = source.liftUniversalPlacement(
        tiles({{0, 8}, {1, 8}}), *fixed);
    check(terminal && fixed && universal,
          "corrupt persistent RZone fixture must contain a three-record DAG");

    TemporaryRzFile snapshot;
    const RzFileSummary saved = RzFile::save(snapshot.path, source);

    RZoneTable wrong_target(game, 32, 16);
    checkThrows([&] { (void)RzFile::load(snapshot.path, wrong_target); },
                "RZone snapshot with a different winning tile must be rejected");

    constexpr std::uint64_t fixed_record = RzFile::kHeaderSize + RzFile::kRecordSize;
    const std::uint8_t original_pattern = readByte(snapshot.path, fixed_record);
    writeByte(snapshot.path, fixed_record, 0x23);
    refreshRzChecksum(snapshot.path);
    RZoneTable invalid_semantics(game, 16, 16);
    checkThrows([&] { (void)RzFile::load(snapshot.path, invalid_semantics); },
                "FixedSlide that fails its universal pattern implication must be rejected");
    checkThrows([&] { (void)RzProofTable::load(snapshot.path); },
                "proof checker must independently reject invalid FixedSlide semantics");
    check(invalid_semantics.size() == 0,
          "failed semantic validation must not partially publish RZone records");

    (void)RzFile::save(snapshot.path, source);
    writeByte(snapshot.path, fixed_record + 8, 2);
    refreshRzChecksum(snapshot.path);
    RZoneTable self_child(game, 16, 16);
    checkThrows([&] { (void)RzFile::load(snapshot.path, self_child); },
                "self-referencing RZone child must be rejected");
    checkThrows([&] { (void)RzProofTable::load(snapshot.path); },
                "proof checker must independently reject a non-child-first RZone DAG");
    check(self_child.size() == 0,
          "failed child-first validation must not partially publish RZone records");

    (void)RzFile::save(snapshot.path, source);
    writeByte(snapshot.path, fixed_record, original_pattern ^ 0x10u);
    RZoneTable bad_checksum(game, 16, 16);
    checkThrows([&] { (void)RzFile::load(snapshot.path, bad_checksum); },
                "RZone payload checksum corruption must be rejected");
    checkThrows([&] { (void)RzProofTable::load(snapshot.path); },
                "proof checker must independently reject RZone checksum corruption");

    (void)RzFile::save(snapshot.path, source);
    std::filesystem::resize_file(snapshot.path, saved.file_size - 1);
    RZoneTable truncated(game, 16, 16);
    checkThrows([&] { (void)RzFile::load(snapshot.path, truncated); },
                "truncated RZone snapshot must be rejected");
    checkThrows([&] { (void)RzProofTable::load(snapshot.path); },
                "proof checker must independently reject a truncated RZone file");
}

void testPersistentTtRoundTripAndCrossOpeningReuse()
{
    const SolverOptions options{8, 0, 0, true, false};
    FakeGame first_game(1);
    first_game.slides[1] = {
        slide(Direction::Up, 10),
        slide(Direction::Right, 11, 4),
    };
    first_game.values = {{10, 100.0}, {11, 50.0}};
    first_game.placements[10] = {placement(0, 2, 20)};
    first_game.placements[11] = {placement(0, 2, 21)};
    first_game.winning_states.insert(21);

    Solver first_solver(first_game, options);
    const SearchResult first = first_solver.solve();
    check(first.outcome == Outcome::Win && first.root_action == Direction::Right,
          "first persistent-TT fixture must prove Win after rejecting a losing action");
    check(first.statistics.player_tt_stores == 1 &&
              first.statistics.environment_tt_stores == 1,
          "first persistent-TT fixture must store only the two proven Win states");

    TemporaryTtFile temporary;
    const TtFileSummary first_saved = TtFile::save(temporary.path, first_solver);
    check(first_saved.player_entries == 1 && first_saved.environment_entries == 1 &&
              first_saved.file_size == TtFile::kHeaderSize + 2 * TtFile::kRecordSize,
          "persistent TT writer produced unexpected first snapshot counts or size");
    check(std::filesystem::file_size(temporary.path) == first_saved.file_size &&
              !std::filesystem::exists(temporary.path.string() + ".tmp"),
          "atomic TT snapshot publication left an invalid size or temporary file");

    FakeGame second_game(2);
    second_game.slides[2] = {slide(Direction::Down, 12)};
    second_game.values[12] = 10.0;
    second_game.placements[12] = {placement(0, 2, 1)};
    // A persistent Player Win witness is stored in canonical/current orientation.
    // The hit must regenerate this action value without expanding its old subtree.
    second_game.slides[1] = first_game.slides[1];
    second_game.values[10] = 100.0;
    second_game.values[11] = 50.0;

    FakeGame adopted_game = second_game;
    Solver adopted_solver(adopted_game, options);
    adopted_solver.adoptPersistentProofs(first_solver);
    const SearchResult adopted = adopted_solver.solve();
    check(adopted.outcome == Outcome::Win &&
              adopted.statistics.player_tt_loaded_entries == 1 &&
              adopted.statistics.environment_tt_loaded_entries == 1 &&
              adopted.statistics.player_persistent_tt_hits == 1,
          "in-memory proof handoff must match persistent TT loading semantics");

    Solver second_solver(second_game, options);
    const TtFileSummary loaded = TtFile::load(temporary.path, second_solver);
    check(loaded.player_entries == 1 && loaded.environment_entries == 1 &&
              loaded.payload_checksum == first_saved.payload_checksum,
          "persistent TT round trip changed counts or checksum");
    const SearchResult second = second_solver.solve();
    check(second.outcome == Outcome::Win && second.root_action == Direction::Down,
          "second opening must reuse the first opening proof and remain Win");
    check(second.statistics.player_tt_loaded_entries == 1 &&
              second.statistics.environment_tt_loaded_entries == 1,
          "loaded TT entry counters are wrong");
    check(second.statistics.player_persistent_tt_hits == 1 &&
              second.statistics.environment_persistent_tt_hits == 0,
          "second opening must record one persistent Player hit");
    const TraceNode& environment = *second.root->children[0];
    const TraceNode& persistent_hit = *environment.children[0];
    check(persistent_hit.tt_hit && persistent_hit.tt_persistent_hit &&
              persistent_hit.tt_key == std::optional<std::uint64_t>(1) &&
              !persistent_hit.tt_source_trace_id,
          "persistent trace leaf must use phase/key identity without a stale trace ID");
    check(persistent_hit.selected_action &&
              persistent_hit.selected_action->direction == Direction::Right &&
              persistent_hit.selected_action_value == std::optional<double>(54.0),
          "persistent Player witness or reward + V action value was not restored");

    const TtFileSummary merged = TtFile::save(temporary.path, second_solver);
    check(merged.player_entries == 2 && merged.environment_entries == 2,
          "second opening must merge two new states into the persistent snapshot");

    FakeGame third_game = second_game;
    Solver third_solver(third_game, options);
    (void)TtFile::load(temporary.path, third_solver);
    const SearchResult third = third_solver.solve();
    check(third.outcome == Outcome::Win && third.root_action == Direction::Down &&
              third.root_action_value == std::optional<double>(10.0),
          "merged persistent TT must restore a root witness and action value");
    check(third.statistics.combinedNodes() == 1 &&
              third.statistics.player_persistent_tt_hits == 1 &&
              third.root->tt_persistent_hit,
          "third solve must visit only its persistent-TT root");
    checkTraceIdsAreDfsDiscoveryOrder(*third.root);
}

void testPersistentTtRejectsIncompatibleAndCorruptFiles()
{
    const SolverOptions options{8, 0, 0, true, false};
    FakeGame game(1);
    game.slides[1] = {slide(Direction::Up, 10, 4)};
    game.values[10] = 5.0;
    game.winning_states.insert(10);

    Solver source(game, options);
    const SearchResult source_result = source.solve();
    check(source_result.outcome == Outcome::Win, "corruption fixture must be a proven Win");
    TemporaryTtFile temporary;
    const TtFileSummary saved = TtFile::save(temporary.path, source);
    check(saved.player_entries == 1 && saved.environment_entries == 0,
          "corruption fixture must contain one Player record");
    check((readByte(temporary.path, 14) & TtFile::kWinOnlyFlag) != 0,
          "persistent TT header must declare Win-only semantics");

    FakeGame wrong_target_game(1);
    Solver wrong_target(wrong_target_game, SolverOptions{16, 0, 0, true, false});
    checkThrows([&] { (void)TtFile::load(temporary.path, wrong_target); },
                "persistent TT with a different winning tile must be rejected");

    FakeGame wrong_symmetry_game(1);
    Solver wrong_symmetry(wrong_symmetry_game, SolverOptions{8, 0, 0, true, true});
    checkThrows([&] { (void)TtFile::load(temporary.path, wrong_symmetry); },
                "persistent TT with a different symmetry mode must be rejected");

    constexpr std::uint64_t first_record = TtFile::kHeaderSize;
    const std::uint8_t original_flags = readByte(temporary.path, 14);
    writeByte(
        temporary.path,
        14,
        static_cast<std::uint8_t>(original_flags & ~TtFile::kWinOnlyFlag));
    FakeGame legacy_game(1);
    Solver legacy_solver(legacy_game, options);
    checkThrows([&] { (void)TtFile::load(temporary.path, legacy_solver); },
                "legacy Win/Loss TT must be rejected by the Win-only Solver");
    checkThrows([&] { (void)ProofTable::load(temporary.path); },
                "proof checker must reject a TT without Win-only semantics");
    writeByte(temporary.path, 14, original_flags);

    const std::uint8_t original_outcome = readByte(temporary.path, first_record + 8);
    writeByte(temporary.path, first_record + 8, 2);
    FakeGame loss_record_game(1);
    Solver loss_record_solver(loss_record_game, options);
    checkThrows([&] { (void)TtFile::load(temporary.path, loss_record_solver); },
                "Win-only persistent TT must reject a Loss record");
    checkThrows([&] { (void)ProofTable::load(temporary.path); },
                "proof checker must reject a Loss TT record");
    writeByte(temporary.path, first_record + 8, original_outcome);

    const std::uint8_t original_witness = readByte(temporary.path, first_record + 9);
    writeByte(temporary.path, first_record + 9, 42);
    FakeGame invalid_witness_game(1);
    Solver invalid_witness(invalid_witness_game, options);
    checkThrows([&] { (void)TtFile::load(temporary.path, invalid_witness); },
                "persistent TT with an illegal witness must be rejected");
    writeByte(temporary.path, first_record + 9, original_witness);
    const TtFileSummary recovered = TtFile::load(temporary.path, invalid_witness);
    check(recovered.entries() == 1,
          "failed TT load must not partially publish entries into the Solver");
    check(ProofTable::load(temporary.path).summary().entries() == 1,
          "proof checker TT reader must accept a valid snapshot");

    const std::uint8_t original_key_byte = readByte(temporary.path, first_record);
    writeByte(temporary.path, first_record, original_key_byte ^ 0x80u);
    FakeGame checksum_game(1);
    Solver checksum_solver(checksum_game, options);
    checkThrows([&] { (void)TtFile::load(temporary.path, checksum_solver); },
                "persistent TT payload checksum corruption must be rejected");
    checkThrows([&] { (void)ProofTable::load(temporary.path); },
                "proof checker must independently reject TT checksum corruption");
    writeByte(temporary.path, first_record, original_key_byte);

    std::filesystem::resize_file(temporary.path, saved.file_size - 1);
    FakeGame truncated_game(1);
    Solver truncated_solver(truncated_game, options);
    checkThrows([&] { (void)TtFile::load(temporary.path, truncated_solver); },
                "truncated persistent TT file must be rejected");
    checkThrows([&] { (void)ProofTable::load(temporary.path); },
                "proof checker must independently reject a truncated TT file");
}

void testPersistentSymmetryWitnessMapping()
{
    const Board source = asymmetricBoard();
    const Board canonical = solver2048::canonicalize(source).board;
    std::uint8_t current_transform = 0;
    Board current;
    for (std::uint8_t transform = 0; transform < 8; ++transform) {
        const Board candidate = solver2048::transformBoard(source, transform);
        if (candidate != canonical) {
            current_transform = transform;
            current = candidate;
            break;
        }
    }
    check(current != canonical,
          "persistent symmetry fixture requires a non-canonical current orientation");

    const Board source_win = board(0x0000000000000022ull);
    const Board current_win = solver2048::transformBoard(source_win, current_transform);
    const Direction current_direction =
        solver2048::transformDirection(Direction::Up, current_transform);
    const SolverOptions options{8, 0, 0, true, true};

    FakeGame source_game(source.raw);
    source_game.slides[source.raw] = {slide(Direction::Up, source_win.raw, 4)};
    source_game.values[source_win.raw] = 5.0;
    source_game.winning_states.insert(source_win.raw);
    Solver source_solver(source_game, options);
    const SearchResult source_result = source_solver.solve();
    check(source_result.outcome == Outcome::Win && source_result.root_action == Direction::Up,
          "persistent symmetry source fixture must prove its slide witness");

    TemporaryTtFile temporary;
    (void)TtFile::save(temporary.path, source_solver);

    FakeGame current_game(current.raw);
    current_game.slides[current.raw] = {slide(current_direction, current_win.raw, 4)};
    current_game.values[current_win.raw] = 7.0;
    Solver current_solver(current_game, options);
    (void)TtFile::load(temporary.path, current_solver);
    const SearchResult current_result = current_solver.solve();
    check(current_result.outcome == Outcome::Win &&
              current_result.root_action == current_direction &&
              current_result.root_action_value == std::optional<double>(11.0),
          "persistent canonical witness must map back and recompute reward + V");
    check(current_result.statistics.player_persistent_tt_hits == 1 &&
              current_result.statistics.player_tt_symmetry_hits == 1 &&
              current_result.root->tt_persistent_hit &&
              current_result.root->tt_symmetry_hit,
          "non-canonical persistent lookup must be classified as a symmetry hit");
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

    std::array<unsigned, 16> expected_opening{};
    expected_opening[0] = 2;
    expected_opening[1] = 2;
    GameApi game("4x6patt", model_path.string(), expected_opening);
    const Board opening = game.fixedOpening();
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
    for (std::uint8_t transform = 0; transform < 8; ++transform) {
        checkNear(game.evaluateAfterstate(solver2048::transformBoard(slides[1].afterstate, transform)),
                  down_value, 1.0,
                  "4x6patt evaluator must be invariant over D4 afterstate symmetries");
    }

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
        check(result.statistics.player_actions == 7, "three-node search player_actions must be 7");
        check(result.statistics.placement_actions == 28,
              "three-node search placement_actions must be 28");
        check(result.statistics.node_limit_cutoffs == 33,
              "three-node search node_limit_cutoffs must be 33");
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
    check(tt_result.statistics.player_tt_stores == 159652,
          "complete target-8 TT search Player entry count mismatch");
    check(tt_result.statistics.environment_tt_stores == 13759,
          "complete target-8 TT search Environment entry count mismatch");
    check(tt_result.statistics.player_tt_hits == 26671,
          "complete target-8 TT search Player hit count mismatch");
    check(tt_result.statistics.environment_tt_hits == 32990,
          "complete target-8 TT search Environment hit count mismatch");
    check(tt_result.statistics.node_limit_cutoffs == 0,
          "unlimited target-8 TT search must not report a node-limit cutoff");
    checkTraceIdsAreDfsDiscoveryOrder(*tt_result.root);

    Solver symmetry_solver(game, SolverOptions{8, 0, 0, true, true});
    SearchResult symmetry_result = symmetry_solver.solve();
    check(symmetry_result.outcome == tt_result.outcome &&
              symmetry_result.root_action == tt_result.root_action &&
              symmetry_result.root_action_value == tt_result.root_action_value,
          "target-8 symmetry TT must preserve the exact-TT root proof and selected action");
    check(symmetry_result.statistics.player_tt_stores == 22171 &&
              symmetry_result.statistics.environment_tt_stores == 1883,
          "complete target-8 symmetry TT entry counts mismatch");
    check(symmetry_result.statistics.player_tt_hits == 4094 &&
              symmetry_result.statistics.environment_tt_hits == 4836,
          "complete target-8 symmetry TT total hit counts mismatch");
    check(symmetry_result.statistics.player_tt_symmetry_hits == 3054 &&
              symmetry_result.statistics.environment_tt_symmetry_hits == 2740,
          "complete target-8 symmetry-derived hit counts mismatch");
    check(symmetry_result.statistics.player_tt_stores < tt_result.statistics.player_tt_stores &&
              symmetry_result.statistics.environment_tt_stores <
                  tt_result.statistics.environment_tt_stores,
          "target-8 symmetry canonicalization must reduce both TT sizes");
    check(symmetry_result.statistics.node_limit_cutoffs == 0,
          "unlimited target-8 symmetry TT search must not report a node-limit cutoff");
    checkTraceIdsAreDfsDiscoveryOrder(*symmetry_result.root);

    SolverOptions generation_options{8, 0, 0, true, true};
    generation_options.rzone_mode = RZoneMode::Generate;
    SearchResult generation_result = Solver(game, generation_options).solve();
    check(generation_result.outcome == symmetry_result.outcome &&
              generation_result.root_action == symmetry_result.root_action &&
              generation_result.root_action_value == symmetry_result.root_action_value,
          "RZone generation-only mode must preserve the target-8 root proof");
    check(generation_result.statistics.player_nodes ==
              symmetry_result.statistics.player_nodes &&
              generation_result.statistics.environment_nodes ==
                  symmetry_result.statistics.environment_nodes &&
              generation_result.statistics.player_actions ==
                  symmetry_result.statistics.player_actions &&
              generation_result.statistics.placement_actions ==
                  symmetry_result.statistics.placement_actions,
          "RZone generation-only mode must preserve exact-TT node/action counts");
    check(generation_result.statistics.rzone.patterns_stored != 0 &&
              generation_result.statistics.rzone.lookups == 0 &&
              generation_result.statistics.rzone.hits() == 0,
          "RZone generation-only mode must build certificates without pruning");

    SolverOptions rzone_options{8, 0, 0, true, true};
    rzone_options.rzone_mode = RZoneMode::Lookup;
    Solver rzone_solver(game, rzone_options);
    SearchResult rzone_result = rzone_solver.solve();
    check(rzone_result.outcome == symmetry_result.outcome &&
              rzone_result.root_action == symmetry_result.root_action &&
              rzone_result.root_action_value == symmetry_result.root_action_value,
          "RZone lookup must preserve the target-8 root proof and selected action");
    check(rzone_result.statistics.rzone.player_hits != 0 &&
              rzone_result.statistics.rzone.environment_hits != 0 &&
              rzone_result.statistics.rzone.partial_hits != 0,
          "target-8 RZone lookup must reuse both Player and Environment partial proofs");
    check(rzone_result.statistics.combinedNodes() <
              symmetry_result.statistics.combinedNodes() &&
              rzone_result.statistics.player_actions < symmetry_result.statistics.player_actions &&
              rzone_result.statistics.placement_actions <
                  symmetry_result.statistics.placement_actions,
          "target-8 RZone lookup must reduce visited nodes and searched actions");
    checkTraceIdsAreDfsDiscoveryOrder(*rzone_result.root);

    SolverOptions smallest_first_options = rzone_options;
    smallest_first_options.rzone_lookup_policy = RZoneLookupPolicy::SmallestFirst;
    SearchResult smallest_first_result = Solver(game, smallest_first_options).solve();
    check(smallest_first_result.outcome == symmetry_result.outcome &&
              smallest_first_result.root_action == symmetry_result.root_action &&
              smallest_first_result.root_action_value == symmetry_result.root_action_value,
          "smallest-first RZone lookup must preserve the target-8 root proof");
    check(smallest_first_result.statistics.rzone.lookup_early_exits ==
              smallest_first_result.statistics.rzone.hits() &&
              smallest_first_result.statistics.rzone.lookup_masks_tested <
                  rzone_result.statistics.rzone.lookup_masks_tested,
          "smallest-first target-8 lookup must early-return and test fewer masks");
    checkTraceIdsAreDfsDiscoveryOrder(*smallest_first_result.root);

    SolverOptions symmetry_rzone_options = smallest_first_options;
    symmetry_rzone_options.use_rzone_symmetry = true;
    Solver symmetry_rzone_solver(game, symmetry_rzone_options);
    SearchResult symmetry_rzone_result = symmetry_rzone_solver.solve();
    check(symmetry_rzone_result.outcome == symmetry_result.outcome &&
              symmetry_rzone_result.root_action == symmetry_result.root_action &&
              symmetry_rzone_result.root_action_value == symmetry_result.root_action_value,
          "symmetry RZone lookup must preserve the target-8 root proof");
    check(symmetry_rzone_result.statistics.rzone.symmetry_hits != 0 &&
              symmetry_rzone_result.statistics.rzone.exact_orientation_hits +
                      symmetry_rzone_result.statistics.rzone.symmetry_hits ==
                  symmetry_rzone_result.statistics.rzone.hits() &&
              symmetry_rzone_result.statistics.rzone.lookup_transforms_tested ==
                  symmetry_rzone_result.statistics.rzone.lookups * 8,
          "target-8 symmetry RZone lookup counters are incomplete");
    checkTraceIdsAreDfsDiscoveryOrder(*symmetry_rzone_result.root);

    for (const bool active : {false, true}) {
        SolverOptions hash_options = symmetry_rzone_options;
        hash_options.rzone_active_pruning = active;
        Solver hash_solver(game, hash_options);
        const auto hash_result = hash_solver.solve();
        SolverOptions radix_options = hash_options;
        radix_options.rzone_lookup_policy = RZoneLookupPolicy::RadixTree;
        Solver radix_solver(game, radix_options);
        const auto radix_result = radix_solver.solve();
        check(radix_result.outcome == hash_result.outcome &&
                  radix_result.root_action == hash_result.root_action &&
                  radix_result.statistics.combinedNodes() == hash_result.statistics.combinedNodes() &&
                  radix_result.statistics.rzone.hits() == hash_result.statistics.rzone.hits() &&
                  RzFile::summarize(radix_solver).payload_checksum ==
                      RzFile::summarize(hash_solver).payload_checksum &&
                  radix_result.statistics.rzone.lookup_masks_tested == 0 &&
                  radix_result.statistics.rzone.radix_node_visits != 0,
              "radix lookup must preserve complete target-8 search and immutable proof payload");
        std::function<void(const TraceNode&, const TraceNode&)> compare =
            [&](const TraceNode& lhs, const TraceNode& rhs) {
                check(lhs.outcome == rhs.outcome && lhs.solved_by == rhs.solved_by &&
                          lhs.rzone_certificate_id == rhs.rzone_certificate_id &&
                          lhs.rzone_transform == rhs.rzone_transform &&
                          lhs.children.size() == rhs.children.size(),
                      "radix lookup must return the same certificate and transform at every node");
                for (std::size_t i = 0; i < lhs.children.size(); ++i) {
                    compare(*lhs.children[i], *rhs.children[i]);
                }
            };
        compare(*radix_result.root, *hash_result.root);
    }

    SolverOptions active_rzone_options = symmetry_rzone_options;
    active_rzone_options.rzone_active_pruning = true;
    Solver active_rzone_solver(game, active_rzone_options);
    SearchResult active_rzone_result = active_rzone_solver.solve();
    check(active_rzone_result.outcome == symmetry_rzone_result.outcome &&
              active_rzone_result.root_action == symmetry_rzone_result.root_action &&
              active_rzone_result.root_action_value == symmetry_rzone_result.root_action_value,
          "RZone active pruning must preserve the complete target-8 root proof");
    check(active_rzone_result.statistics.rzone.active_pruning_children_matched != 0 &&
              active_rzone_result.statistics.rzone.active_pruning_full_lookups_avoided != 0 &&
              active_rzone_result.statistics.combinedNodes() <
                  symmetry_rzone_result.statistics.combinedNodes() &&
              active_rzone_result.statistics.rzone.lookups <
                  symmetry_rzone_result.statistics.rzone.lookups &&
              active_rzone_result.statistics.rzone.lookup_masks_tested <
                  symmetry_rzone_result.statistics.rzone.lookup_masks_tested,
          "target-8 active pruning must avoid full RZone lookup work");
    checkTraceIdsAreDfsDiscoveryOrder(*active_rzone_result.root);

    for (bool active : {false, true}) {
        for (bool trimming : {false, true}) {
            for (bool fallback : {false, true}) {
                if (trimming && fallback) continue; // Already solved above.
                auto options = symmetry_rzone_options;
                options.rzone_active_pruning = active;
                options.rzone_trimming = trimming;
                options.rzone_full_board_placement_fallback = fallback;
                Solver toggled(game, options);
                const auto result = toggled.solve();
                const auto& rz = result.statistics.rzone;
                check(result.outcome == symmetry_rzone_result.outcome &&
                          result.root_action == symmetry_rzone_result.root_action,
                      "RZone generation switches must preserve the target-8 solution");
                if (!trimming) {
                    check(rz.trim_attempts == 0 && rz.trim_cells_removed == 0 &&
                              rz.trimmed_certificates == 0,
                          "Solver must propagate disabled trimming into RZoneTable");
                }
                if (!fallback) {
                    check(rz.full_board_placement_certificates == 0,
                          "Solver must suppress fallback with and without active pruning");
                }
            }
        }
    }

    TemporaryTtFile proof_tt;
    const TtFileSummary proof_summary = TtFile::save(proof_tt.path, symmetry_solver);
    TemporaryTtFile proof_sgf;
    SgfMetadata proof_metadata;
    proof_metadata.winning_tile = 8;
    proof_metadata.use_transposition_table = true;
    proof_metadata.use_tt_symmetry = true;
    proof_metadata.save_tt = true;
    proof_metadata.tt_path = proof_tt.path;
    proof_metadata.tt_format_version = TtFile::kFormatVersion;
    proof_metadata.tt_payload_checksum = proof_summary.payload_checksum;
    proof_metadata.tt_file_size = proof_summary.file_size;
    proof_metadata.tt_player_records = proof_summary.player_entries;
    proof_metadata.tt_environment_records = proof_summary.environment_entries;
    proof_metadata.opening = expected_opening;
    proof_metadata.statistics = symmetry_result.statistics;
    SgfWriter::write(proof_sgf.path, *symmetry_result.root, proof_metadata);

    const ProofTable proof_table = ProofTable::load(proof_tt.path);
    const ProofManifest proof_manifest = solver2048::readProofManifest(proof_sgf.path);
    const ProofVerificationResult verified =
        solver2048::verifyRootProof(proof_manifest, proof_table);
    check(verified.root_outcome == Outcome::Win && verified.artifact_binding_verified,
          "target-8 SGF + TT proof package must verify with a bound artifact");
    check(verified.statistics.tt_records_verified == proof_summary.entries() &&
              verified.statistics.terminal_states_checked != 0 &&
              verified.statistics.witness_edges_checked != 0 &&
              verified.statistics.universal_edges_checked != 0,
          "target-8 proof checker traversal statistics are incomplete");
    const solver2048::TraceVerificationStatistics trace_verified =
        solver2048::verifySgfTrace(proof_sgf.path, proof_manifest, proof_table);
    check(trace_verified.nodes_checked == trace_verified.actions_replayed + 1 &&
              trace_verified.terminal_nodes_checked != 0 &&
              trace_verified.backpropagation_nodes_checked != 0 &&
              trace_verified.tt_leaves_checked != 0 &&
              trace_verified.tt_source_references_checked ==
                  trace_verified.tt_leaves_checked &&
              trace_verified.verified_exact_states == proof_summary.entries() &&
              trace_verified.tt_records_cross_checked == proof_summary.entries() &&
              trace_verified.skipped_actions_checked != 0 &&
              trace_verified.maximum_depth != 0,
          "target-8 SGF source-table audit statistics are incomplete");

    TemporaryTtFile corrupt_trace;
    std::string corrupt_sgf = readFile(proof_sgf.path);
    const std::string source_prefix = "tt_source_node_id = ";
    const std::size_t source_position = corrupt_sgf.find(source_prefix);
    check(source_position != std::string::npos,
          "target-8 proof SGF lacks a trace-local TT source reference");
    const std::size_t source_value = source_position + source_prefix.size();
    const std::size_t source_end = corrupt_sgf.find_first_not_of("0123456789", source_value);
    corrupt_sgf.replace(source_value, source_end - source_value, "999999999");
    {
        std::ofstream output(corrupt_trace.path, std::ios::out | std::ios::binary | std::ios::trunc);
        output << corrupt_sgf;
        check(static_cast<bool>(output), "cannot write corrupt trace fixture");
    }
    checkThrows(
        [&] { (void)solver2048::verifySgfTrace(corrupt_trace.path, proof_manifest, proof_table); },
        "SGF source-table audit must reject a nonexistent TT source node");

    corrupt_sgf = readFile(proof_sgf.path);
    const std::size_t ancestor_position = corrupt_sgf.find(source_prefix);
    const std::size_t ancestor_value = ancestor_position + source_prefix.size();
    const std::size_t ancestor_end =
        corrupt_sgf.find_first_not_of("0123456789", ancestor_value);
    corrupt_sgf.replace(ancestor_value, ancestor_end - ancestor_value, "1");
    {
        std::ofstream output(corrupt_trace.path, std::ios::out | std::ios::binary | std::ios::trunc);
        output << corrupt_sgf;
        check(static_cast<bool>(output), "cannot write active-ancestor trace fixture");
    }
    checkThrows(
        [&] { (void)solver2048::verifySgfTrace(corrupt_trace.path, proof_manifest, proof_table); },
        "SGF source-table audit must reject an active ancestor as a TT proof source");

    ProofManifest wrong_claim = proof_manifest;
    wrong_claim.root_outcome = Outcome::Loss;
    checkThrows([&] { (void)solver2048::verifyRootProof(wrong_claim, proof_table); },
                "proof checker must reject an incorrect SGF root outcome");
    ProofManifest wrong_binding = proof_manifest;
    ++*wrong_binding.tt_payload_checksum;
    checkThrows([&] { (void)solver2048::verifyRootProof(wrong_binding, proof_table); },
                "proof checker must reject an SGF/TT checksum binding mismatch");
    checkThrows(
        [&] {
            (void)solver2048::verifySgfProof(
                proof_sgf.path, wrong_binding, proof_table);
        },
        "SGF source-table checker must reject an SGF/TT checksum binding mismatch");

    TraceNode persistent_root;
    persistent_root.phase = Phase::Player;
    persistent_root.outcome = Outcome::Win;
    persistent_root.solved_by = SolvedBy::TranspositionTable;
    persistent_root.tt_hit = true;
    persistent_root.tt_persistent_hit = true;
    persistent_root.children_total = symmetry_result.root->children_total;
    persistent_root.selected_action = symmetry_result.root->selected_action;
    TemporaryTtFile persistent_sgf;
    SgfMetadata persistent_metadata = proof_metadata;
    persistent_metadata.load_tt = true;
    persistent_metadata.statistics = {};
    persistent_metadata.statistics.player_tt_loaded_entries =
        proof_summary.player_entries;
    persistent_metadata.statistics.environment_tt_loaded_entries =
        proof_summary.environment_entries;
    SgfWriter::write(persistent_sgf.path, persistent_root, persistent_metadata);

    TemporaryTtFile unbound_source_sgf;
    SgfMetadata unbound_source_metadata = proof_metadata;
    unbound_source_metadata.save_tt = false;
    unbound_source_metadata.tt_format_version.reset();
    unbound_source_metadata.tt_payload_checksum.reset();
    unbound_source_metadata.tt_file_size.reset();
    unbound_source_metadata.tt_player_records.reset();
    unbound_source_metadata.tt_environment_records.reset();
    SgfWriter::write(
        unbound_source_sgf.path,
        *symmetry_result.root,
        unbound_source_metadata);

    const solver2048::ProofSequenceVerificationResult sequence_verified =
        solver2048::verifySgfProofSequence(
            {unbound_source_sgf.path, persistent_sgf.path}, proof_table);
    check(sequence_verified.openings.size() == 2 &&
              sequence_verified.openings[1].root_outcome == Outcome::Win &&
              sequence_verified.openings[1].statistics.tt_source_references_checked == 1 &&
              sequence_verified.aggregate_statistics.tt_records_cross_checked ==
                  proof_summary.entries(),
          "ordered multi-opening SGF source verification did not reuse the earlier proof");
    checkThrows(
        [&] {
            (void)solver2048::verifySgfProofSequence(
                {persistent_sgf.path, unbound_source_sgf.path}, proof_table);
        },
        "multi-opening checker must reject SGFs supplied out of chronological order");
    checkThrows(
        [&] {
            (void)solver2048::verifySgfProofSequence(
                {unbound_source_sgf.path, unbound_source_sgf.path}, proof_table);
        },
        "multi-opening checker must reject a duplicate SGF path");

    TemporaryTtFile rzone_proof_tt;
    const TtFileSummary rzone_tt_summary =
        TtFile::save(rzone_proof_tt.path, active_rzone_solver);
    TemporaryRzFile rzone_proof_rz;
    const RzFileSummary rzone_rz_summary =
        RzFile::save(rzone_proof_rz.path, active_rzone_solver);
    TemporaryTtFile rzone_proof_sgf;
    SgfMetadata rzone_metadata;
    rzone_metadata.winning_tile = 8;
    rzone_metadata.use_transposition_table = true;
    rzone_metadata.use_tt_symmetry = true;
    rzone_metadata.rzone_mode = RZoneMode::Lookup;
    rzone_metadata.use_rzone_symmetry = true;
    rzone_metadata.rzone_active_pruning = true;
    rzone_metadata.save_tt = true;
    rzone_metadata.tt_path = rzone_proof_tt.path;
    rzone_metadata.tt_format_version = TtFile::kFormatVersion;
    rzone_metadata.tt_payload_checksum = rzone_tt_summary.payload_checksum;
    rzone_metadata.tt_file_size = rzone_tt_summary.file_size;
    rzone_metadata.tt_player_records = rzone_tt_summary.player_entries;
    rzone_metadata.tt_environment_records = rzone_tt_summary.environment_entries;
    rzone_metadata.save_rz = true;
    rzone_metadata.rz_path = rzone_proof_rz.path;
    rzone_metadata.rz_format_version = RzFile::kFormatVersion;
    rzone_metadata.rz_semantics_version = RzFile::kSemanticsVersion;
    rzone_metadata.rz_prefix_record_count = rzone_rz_summary.record_count;
    rzone_metadata.rz_prefix_checksum = rzone_rz_summary.payload_checksum;
    rzone_metadata.opening = expected_opening;
    rzone_metadata.statistics = active_rzone_result.statistics;
    SgfWriter::write(
        rzone_proof_sgf.path, *active_rzone_result.root, rzone_metadata);

    const ProofTable mixed_tt = ProofTable::load(rzone_proof_tt.path);
    const RzProofTable original_rz = RzProofTable::load(rzone_proof_rz.path);
    const ProofManifest mixed_manifest =
        solver2048::readProofManifest(rzone_proof_sgf.path);
    const ProofVerificationResult mixed_verified =
        solver2048::verifyRootProof(mixed_manifest, mixed_tt, &original_rz);
    check(mixed_verified.root_outcome == Outcome::Win &&
              mixed_verified.artifact_binding_verified &&
              mixed_verified.rzone_artifact_binding_verified &&
              mixed_verified.statistics.rzone_certificates_used != 0,
          "mixed TT/RZone root proof must verify using the SGF-bound RZone prefix");
    const solver2048::TraceVerificationStatistics mixed_trace =
        solver2048::verifySgfTrace(
            rzone_proof_sgf.path, mixed_manifest, mixed_tt, &original_rz);
    check(mixed_trace.rzone_leaves_checked != 0 &&
              mixed_trace.rzone_annotations_checked >= mixed_trace.rzone_leaves_checked &&
              mixed_trace.rzone_records_cross_checked == rzone_rz_summary.record_count &&
              mixed_trace.rzone_source_annotations_checked >=
                  mixed_trace.rzone_records_cross_checked,
          "RZone SGF source-table audit must verify certificate leaves, sources, and records");

    std::array<unsigned, 16> trivial_rzone_opening{};
    trivial_rzone_opening[0] = 4;
    trivial_rzone_opening[1] = 4;
    const Board trivial_rzone_board =
        solver2048::boardFromTiles(trivial_rzone_opening);
    FakeGame trivial_rzone_game(trivial_rzone_board.raw);
    trivial_rzone_game.tile_values[trivial_rzone_board.raw] = trivial_rzone_opening;
    trivial_rzone_game.slides[trivial_rzone_board.raw] =
        solver2048::generateLegalSlides(trivial_rzone_board);
    for (const SlideResult& candidate :
         trivial_rzone_game.slides[trivial_rzone_board.raw]) {
        trivial_rzone_game.tile_values[candidate.afterstate.raw] =
            solver2048::boardTileValues(candidate.afterstate);
        trivial_rzone_game.values[candidate.afterstate.raw] =
            candidate.direction == Direction::Right ? 100.0 : 0.0;
        if (candidate.direction == Direction::Right) {
            trivial_rzone_game.winning_states.insert(candidate.afterstate.raw);
        }
    }
    SolverOptions trivial_rzone_options{8, 0, 0, true, false};
    trivial_rzone_options.rzone_mode = RZoneMode::Lookup;
    Solver trivial_rzone_solver(trivial_rzone_game, trivial_rzone_options);
    SearchResult trivial_rzone_result = trivial_rzone_solver.solve();
    check(trivial_rzone_result.outcome == Outcome::Win &&
              trivial_rzone_result.root->rzone_certificate_id.has_value(),
          "trivial target-8 root must produce an RZone certificate");

    TemporaryTtFile trivial_rzone_tt;
    const TtFileSummary trivial_tt_summary =
        TtFile::save(trivial_rzone_tt.path, trivial_rzone_solver);
    TemporaryRzFile trivial_rzone_rz;
    const RzFileSummary trivial_rz_summary =
        RzFile::save(trivial_rzone_rz.path, trivial_rzone_solver);
    TemporaryTtFile trivial_rzone_sgf;
    SgfMetadata trivial_rzone_metadata;
    trivial_rzone_metadata.winning_tile = 8;
    trivial_rzone_metadata.use_transposition_table = true;
    trivial_rzone_metadata.rzone_mode = RZoneMode::Lookup;
    trivial_rzone_metadata.save_tt = true;
    trivial_rzone_metadata.tt_path = trivial_rzone_tt.path;
    trivial_rzone_metadata.tt_format_version = TtFile::kFormatVersion;
    trivial_rzone_metadata.tt_payload_checksum = trivial_tt_summary.payload_checksum;
    trivial_rzone_metadata.tt_file_size = trivial_tt_summary.file_size;
    trivial_rzone_metadata.tt_player_records = trivial_tt_summary.player_entries;
    trivial_rzone_metadata.tt_environment_records =
        trivial_tt_summary.environment_entries;
    trivial_rzone_metadata.save_rz = true;
    trivial_rzone_metadata.rz_path = trivial_rzone_rz.path;
    trivial_rzone_metadata.rz_format_version = RzFile::kFormatVersion;
    trivial_rzone_metadata.rz_semantics_version = RzFile::kSemanticsVersion;
    trivial_rzone_metadata.rz_prefix_record_count = trivial_rz_summary.record_count;
    trivial_rzone_metadata.rz_prefix_checksum = trivial_rz_summary.payload_checksum;
    trivial_rzone_metadata.opening = trivial_rzone_opening;
    trivial_rzone_metadata.statistics = trivial_rzone_result.statistics;
    SgfWriter::write(
        trivial_rzone_sgf.path,
        *trivial_rzone_result.root,
        trivial_rzone_metadata);

    TraceNode persistent_rzone_root;
    persistent_rzone_root.phase = Phase::Player;
    persistent_rzone_root.outcome = Outcome::Win;
    persistent_rzone_root.solved_by = SolvedBy::RZoneTable;
    persistent_rzone_root.rzone_hit = true;
    persistent_rzone_root.rzone_transform =
        trivial_rzone_result.root->rzone_transform;
    persistent_rzone_root.rzone_certificate_id =
        trivial_rzone_result.root->rzone_certificate_id;
    persistent_rzone_root.selected_action = trivial_rzone_result.root->selected_action;
    persistent_rzone_root.children_total = trivial_rzone_result.root->children_total;
    TemporaryTtFile persistent_rzone_sgf;
    SgfMetadata persistent_rzone_metadata = trivial_rzone_metadata;
    persistent_rzone_metadata.load_rz = true;
    persistent_rzone_metadata.statistics = {};
    persistent_rzone_metadata.statistics.rzone.loaded_certificates =
        trivial_rz_summary.record_count;
    SgfWriter::write(
        persistent_rzone_sgf.path,
        persistent_rzone_root,
        persistent_rzone_metadata);

    const ProofTable trivial_proof_tt = ProofTable::load(trivial_rzone_tt.path);
    const RzProofTable trivial_proof_rz =
        RzProofTable::load(trivial_rzone_rz.path);
    const solver2048::ProofSequenceVerificationResult rzone_sequence =
        solver2048::verifySgfProofSequence(
            {trivial_rzone_sgf.path, persistent_rzone_sgf.path},
            trivial_proof_tt,
            &trivial_proof_rz);
    check(rzone_sequence.openings[1].statistics.rzone_leaves_checked == 1 &&
              rzone_sequence.aggregate_statistics.rzone_records_cross_checked ==
                  trivial_rz_summary.record_count,
          "ordered multi-opening verification did not reuse an earlier RZone source");

    TemporaryTtFile wrong_rzone_transform_trace;
    std::string wrong_rzone_transform_sgf = readFile(rzone_proof_sgf.path);
    std::size_t transform_field = std::string::npos;
    for (unsigned transform = 1; transform < 8 && transform_field == std::string::npos;
         ++transform) {
        transform_field = wrong_rzone_transform_sgf.find(
            "rzone_transform = " + std::to_string(transform));
    }
    check(transform_field != std::string::npos,
          "symmetry RZone proof SGF lacks a transformed certificate annotation");
    const std::size_t transform_value =
        transform_field + std::string("rzone_transform = ").size();
    const std::size_t transform_end =
        wrong_rzone_transform_sgf.find('\n', transform_value);
    wrong_rzone_transform_sgf.replace(
        transform_value, transform_end - transform_value, "0");
    {
        std::ofstream output(
            wrong_rzone_transform_trace.path,
            std::ios::out | std::ios::binary | std::ios::trunc);
        output << wrong_rzone_transform_sgf;
        check(static_cast<bool>(output), "cannot write wrong RZone transform fixture");
    }
    checkThrows(
        [&] {
            (void)solver2048::verifySgfTrace(
                wrong_rzone_transform_trace.path,
                mixed_manifest,
                mixed_tt,
                &original_rz);
        },
        "RZone SGF trace audit must reject an incorrect symmetry transform");

    RZoneTable extended_rz(game, 8, 16);
    (void)RzFile::load(rzone_proof_rz.path, extended_rz);
    const auto appended = extended_rz.createTerminal(tiles({{15, 16}}), Phase::Player);
    check(appended && *appended > rzone_rz_summary.record_count,
          "cross-opening RZone fixture must append a new certificate ID");
    const RzFileSummary extended_summary = RzFile::save(rzone_proof_rz.path, extended_rz);
    const RzProofTable newer_rz = RzProofTable::load(rzone_proof_rz.path);
    check(extended_summary.record_count > rzone_rz_summary.record_count &&
              newer_rz.prefixChecksum(rzone_rz_summary.record_count) ==
                  rzone_rz_summary.payload_checksum,
          "appending cross-opening certificates must preserve the old bound prefix");
    const ProofVerificationResult newer_verified =
        solver2048::verifyRootProof(mixed_manifest, mixed_tt, &newer_rz);
    const solver2048::TraceVerificationStatistics newer_trace =
        solver2048::verifySgfTrace(
            rzone_proof_sgf.path, mixed_manifest, mixed_tt, &newer_rz);
    check(newer_verified.rzone_artifact_binding_verified &&
              newer_verified.statistics.rzone_certificates_used != 0 &&
              newer_trace.rzone_leaves_checked != 0,
          "an SGF must remain verifiable after another opening appends to its .rz file");

    ProofManifest wrong_rz_binding = mixed_manifest;
    ++*wrong_rz_binding.rz_prefix_checksum;
    checkThrows(
        [&] { (void)solver2048::verifyRootProof(wrong_rz_binding, mixed_tt, &newer_rz); },
        "proof checker must reject an SGF/RZone prefix checksum mismatch");

    TemporaryTtFile out_of_prefix_trace;
    std::string bad_rzone_sgf = readFile(rzone_proof_sgf.path);
    const std::size_t rzone_leaf = bad_rzone_sgf.find("solved_by = RZONE_TT");
    const std::size_t certificate_field = bad_rzone_sgf.find(
        "rzone_certificate_id = ", rzone_leaf);
    check(rzone_leaf != std::string::npos && certificate_field != std::string::npos,
          "RZone proof SGF lacks a certificate lookup leaf");
    const std::size_t certificate_value = certificate_field +
        std::string("rzone_certificate_id = ").size();
    const std::size_t certificate_end = bad_rzone_sgf.find('\n', certificate_value);
    bad_rzone_sgf.replace(
        certificate_value,
        certificate_end - certificate_value,
        std::to_string(*appended));
    {
        std::ofstream output(
            out_of_prefix_trace.path,
            std::ios::out | std::ios::binary | std::ios::trunc);
        output << bad_rzone_sgf;
        check(static_cast<bool>(output), "cannot write out-of-prefix RZone trace fixture");
    }
    checkThrows(
        [&] {
            (void)solver2048::verifySgfTrace(
                out_of_prefix_trace.path, mixed_manifest, mixed_tt, &newer_rz);
        },
        "SGF trace must not reference a certificate appended after its bound prefix");
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fail("cannot read test SGF file: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<std::string> parseCommentFields(const std::string& sgf,
                                            const std::string& field)
{
    const std::string prefix = "\n" + field + " = ";
    std::vector<std::string> values;
    std::size_t position = 0;
    while ((position = sgf.find(prefix, position)) != std::string::npos) {
        const std::size_t value_begin = position + prefix.size();
        const std::size_t value_end = sgf.find_first_of("\r\n]", value_begin);
        check(value_end != std::string::npos && value_end != value_begin,
              "SGF comment field " + field + " is empty or unterminated");
        values.push_back(sgf.substr(value_begin, value_end - value_begin));
        position = value_end;
    }
    return values;
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
    root.solved_by = SolvedBy::Backpropagation;
    root.expanded = true;
    root.children_total = 3;
    root.rzone_certificate_id = 7;
    root.rzone_kind = RZoneStepKind::FixedSlide;
    root.rzone_mask = 0x0007;
    root.rzone_masked_board = 0x0000000000000303ull;
    root.rzone_depth = 2;
    root.skipped_actions = {
        SearchAction::slide(Direction::Left),
        SearchAction::slide(Direction::Up),
    };

    auto down = std::make_unique<TraceNode>();
    down->trace_id = 2;
    down->phase = Phase::Environment;
    down->outcome = Outcome::Loss;
    down->solved_by = SolvedBy::Backpropagation;
    down->expanded = true;
    down->dfs_order = 0;
    down->incoming_action = SearchAction::slide(Direction::Down);
    down->reward = 4;
    down->afterstate_value = 38.5;
    down->action_value = 42.5;
    down->children_total = 1;

    auto spawned = std::make_unique<TraceNode>();
    spawned->trace_id = 3;
    spawned->phase = Phase::Player;
    spawned->outcome = Outcome::Loss;
    spawned->solved_by = SolvedBy::Terminal;
    spawned->expanded = true;
    spawned->dfs_order = 0;
    spawned->incoming_action = SearchAction::placement(0, 2);
    spawned->terminal_reason = TerminalReason::NoLegalMove;
    down->children.push_back(std::move(spawned));

    auto right = std::make_unique<TraceNode>();
    right->trace_id = 4;
    right->phase = Phase::Environment;
    right->outcome = Outcome::Loss;
    right->solved_by = SolvedBy::TranspositionTable;
    right->terminal_reason = TerminalReason::TranspositionTable;
    right->tt_hit = true;
    right->tt_symmetry_hit = true;
    right->tt_transform = 3;
    right->tt_source_trace_id = 2;
    right->tt_key = 0x1234;
    right->dfs_order = 1;
    right->incoming_action = SearchAction::slide(Direction::Right);
    right->reward = 0;
    right->afterstate_value = 10.0;
    right->action_value = 10.0;

    root.children.push_back(std::move(down));
    root.children.push_back(std::move(right));

    SgfMetadata metadata;
    metadata.winning_tile = 8;
    metadata.node_limit = 3;
    metadata.use_transposition_table = true;
    metadata.use_tt_symmetry = true;
    metadata.rzone_mode = RZoneMode::Lookup;
    metadata.rzone_lookup_policy = RZoneLookupPolicy::SmallestFirst;
    metadata.use_rzone_symmetry = true;
    metadata.rzone_max_cells = 12;
    metadata.rzone_active_pruning = true;
    metadata.rzone_trimming = false;
    metadata.rzone_full_board_placement_fallback = true;
    metadata.load_tt = true;
    metadata.save_tt = true;
    metadata.tt_path = "solver/tt/test.tt";
    metadata.load_rz = true;
    metadata.save_rz = true;
    metadata.rz_path = "solver/rz/test.rz";
    metadata.rz_format_version = RzFile::kFormatVersion;
    metadata.rz_semantics_version = RzFile::kSemanticsVersion;
    metadata.rz_prefix_record_count = 7;
    metadata.rz_prefix_checksum = 0x123456789abcdef0ull;
    metadata.config_path = "solver/test.cfg";
    metadata.opening[2] = 4;
    metadata.opening[15] = 2;
    metadata.network = "fake]network";
    metadata.model_path = "path\\model";
    metadata.elapsed_seconds = 1.25;
    metadata.statistics.player_nodes = 2;
    metadata.statistics.environment_nodes = 1;
    metadata.statistics.rzone.loaded_certificates = 5;
    metadata.statistics.rzone.patterns_stored = 9;
    metadata.statistics.rzone.active_pruning_children_matched = 11;
    metadata.statistics.rzone.active_pruning_full_lookups_avoided = 10;

    TemporarySgf temporary;
    SgfWriter::write(temporary.path, root, metadata);
    const std::string sgf = readFile(temporary.path);

    const std::string prefix = "(;FF[4]CA[UTF-8]SZ[4]AP[Solving2048:1];W[22];W[19]C[";
    check(sgf.rfind(prefix, 0) == 0,
          "SGF must start with metadata followed by the two fixed-opening placements");
    check(sgf.find("network = fake\\]network") != std::string::npos,
          "SGF root metadata did not escape a closing bracket");
    check(sgf.find("model = path\\\\model") != std::string::npos,
          "SGF root metadata did not escape a backslash");
    check(sgf.find("children_skipped_actions = 3 0") != std::string::npos,
          "SGF must preserve skipped action order");
    check(sgf.find("config_file = solver/test.cfg") != std::string::npos,
          "SGF metadata must preserve the cfg path");
    check(sgf.find("node_count_semantics = visited") != std::string::npos &&
              sgf.find("player_nodes = 2") != std::string::npos &&
              sgf.find("environment_nodes = 1") != std::string::npos &&
              sgf.find("expanded_nodes") == std::string::npos,
          "SGF must expose only the visited-node counters");
    const std::vector<std::string> solved_by = parseCommentFields(sgf, "solved_by");
    check(solved_by == std::vector<std::string>(
                           {"BACKPROPAGATION", "BACKPROPAGATION", "TERMINAL", "TT"}),
          "SGF solved_by fields must preserve each node's resolution source");

    const std::vector<std::uint64_t> tt_sources =
        parseUnsignedCommentFields(sgf, "tt_source_node_id");
    check(tt_sources == std::vector<std::uint64_t>({2}),
          "SGF TT hit must contain one parseable source-node reference");
    check(sgf.find("tt_symmetry = enabled") != std::string::npos &&
              sgf.find("persistent_tt_load = enabled") != std::string::npos &&
              sgf.find("persistent_tt_save = enabled") != std::string::npos &&
              sgf.find("tt_file = solver/tt/test.tt") != std::string::npos,
          "SGF must expose TT mode and persistent snapshot settings");
    check(sgf.find("rzone_mode = lookup") != std::string::npos &&
              sgf.find("rzone_lookup_policy = smallest_first") != std::string::npos &&
              sgf.find("rzone_symmetry = enabled") != std::string::npos &&
              sgf.find("rzone_max_cells = 12") != std::string::npos &&
              sgf.find("rzone_active_pruning = enabled") != std::string::npos &&
              sgf.find("rzone_trimming = disabled") != std::string::npos &&
              sgf.find("rzone_full_board_placement_fallback = enabled") != std::string::npos &&
              sgf.find("rzone_loaded_certificates = 5") != std::string::npos &&
              sgf.find("rzone_patterns_stored = 9") != std::string::npos &&
              sgf.find("rzone_active_pruning_children_matched = 11") != std::string::npos &&
              sgf.find("rzone_active_pruning_full_lookups_avoided = 10") !=
                  std::string::npos &&
              sgf.find("persistent_rz_load = enabled") != std::string::npos &&
              sgf.find("persistent_rz_save = enabled") != std::string::npos &&
              sgf.find("rz_file = solver/rz/test.rz") != std::string::npos &&
              sgf.find("rz_format_version = 1") != std::string::npos &&
              sgf.find("rz_semantics_version = " + std::to_string(RzFile::kSemanticsVersion)) != std::string::npos &&
              sgf.find("rz_prefix_record_count = 7") != std::string::npos &&
              sgf.find("rz_prefix_checksum = 0x123456789abcdef0") != std::string::npos &&
              sgf.find("rzone_certificate_id = 7") != std::string::npos &&
              sgf.find("rzone_transform = 0\n") != std::string::npos,
          "SGF must expose RZone configuration, counters, and node certificate metadata");
    check(sgf.find("retroactive") == std::string::npos,
          "SGF must not expose removed retroactive-analysis metadata");

    check(sgf.find("dfs_visit_order") == std::string::npos &&
              sgf.find("dfs_order") == std::string::npos &&
              sgf.find("node_type") == std::string::npos &&
              sgf.find("children_visited") == std::string::npos &&
              sgf.find("children_expanded") == std::string::npos &&
              sgf.find("children_skipped =") == std::string::npos &&
              sgf.find("tt_hit =") == std::string::npos &&
              sgf.find("tt_source =") == std::string::npos &&
              sgf.find("tt_phase") == std::string::npos &&
              sgf.find("rzone_hit =") == std::string::npos &&
              sgf.find("rzone_symmetry_hit =") == std::string::npos &&
              sgf.find("fixed_opening") == std::string::npos &&
              sgf.find("player_tt_exact_hits") == std::string::npos &&
              sgf.find("environment_tt_exact_hits") == std::string::npos &&
              sgf.find("player_tt_entries =") == std::string::npos &&
              sgf.find("environment_tt_entries =") == std::string::npos &&
              sgf.find("\nnode_id =") == std::string::npos &&
              sgf.find("expanded =") == std::string::npos &&
              sgf.find("children_total =") == std::string::npos &&
              sgf.find("terminal_reason =") == std::string::npos &&
              sgf.find("reward =") == std::string::npos &&
              sgf.find("afterstate_value =") == std::string::npos &&
              sgf.find("action_value =") == std::string::npos &&
              sgf.find("selected_action_value =") == std::string::npos &&
              sgf.find("tt_key =") == std::string::npos &&
              sgf.find("tt_symmetry_hit =") == std::string::npos &&
              sgf.find("tt_transform =") == std::string::npos &&
              sgf.find("rzone_kind =") == std::string::npos &&
              sgf.find("rzone_mask =") == std::string::npos &&
              sgf.find("rzone_masked_board =") == std::string::npos &&
              sgf.find("rzone_depth =") == std::string::npos,
          "SGF must omit fields that are exactly reconstructible from retained data");

    const std::size_t down_position = sgf.find("(;B[2]");
    const std::size_t nested_placement_position = sgf.find("(;W[4]", down_position);
    const std::size_t right_position = sgf.find("(;B[1]");
    check(down_position != std::string::npos && nested_placement_position != std::string::npos &&
              right_position != std::string::npos,
          "SGF is missing an expected action node");
    check(down_position < nested_placement_position && nested_placement_position < right_position,
          "SGF writer must preserve DFS child and nested placement order");
    const std::size_t reference_position = sgf.find("tt_source_node_id = 2", right_position);
    check(reference_position != std::string::npos && down_position < right_position &&
              right_position < reference_position,
          "SGF TT source ID must refer to the earlier implicit preorder node");
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
        {"reward + value ordering and direction tie-break",
         testActionValueOrderingAndDirectionTieBreak},
        {"solver.cfg parsing and validation", testSolverConfigParsingAndValidation},
        {"RZone certificate lifts and replay-free lookup", testRZoneCertificateLiftsAndLookup},
        {"full-board placement fallback and independent validation", testFullBoardPlacementFallback},
        {"RZone improved trimming and completion interactions", testImprovedRZoneTrimming},
        {"RZone active pruning covers Environment siblings",
         testRZoneActivePruningCoversEnvironmentSiblings},
        {"D4 canonicalization and action transforms", testD4CanonicalizationAndActionTransforms},
        {"Player symmetry TT hit and witness mapping",
         testPlayerSymmetryTranspositionHitAndWitnessMapping},
        {"Environment symmetry Win TT hit",
         testEnvironmentSymmetryWinTranspositionHit},
        {"no-cache repeated state expansion", testNoCacheExpandsRepeatedStates},
        {"Player transposition-table hit", testPlayerTranspositionHit},
        {"Loss outcomes are not cached", testLossOutcomesAreNotCached},
        {"Unknown outcomes are not cached", testUnknownOutcomesAreNotCached},
        {"combined node-limit Unknown semantics", testNodeLimitReturnsUnknownForEveryAttemptedChild},
        {"persistent RZone round trip and max-cell indexing",
         testPersistentRzRoundTripAndMaxCellIndexing},
        {"RZone phase indexes, growth, reload, and move", testRZonePhaseIndexesAndRelocation},
        {"RZone index cross-opening handoff", testRZoneIndexCrossOpeningHandoff},
        {"persistent RZone compatibility and corruption rejection",
         testPersistentRzRejectsIncompatibleAndCorruptFiles},
        {"proof sequence manifest round trip", testProofSequenceManifestRoundTrip},
        {"persistent TT round trip and cross-opening reuse",
         testPersistentTtRoundTripAndCrossOpeningReuse},
        {"persistent TT compatibility and corruption rejection",
         testPersistentTtRejectsIncompatibleAndCorruptFiles},
        {"persistent TT symmetry witness mapping", testPersistentSymmetryWitnessMapping},
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
