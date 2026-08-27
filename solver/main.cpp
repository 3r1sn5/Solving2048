#include "config.h"
#include "game_api.h"
#include "search.h"
#include "sgf.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace solver2048 {
namespace {

struct CommandLine {
    unsigned winning_tile = config::kDefaultWinningTile;
    std::uint64_t node_limit = config::kDefaultNodeLimit;
    std::uint64_t progress_interval = config::kDefaultProgressInterval;
    bool use_transposition_table = config::kDefaultUseTranspositionTable;
    std::string network = config::kDefaultNetwork;
    std::string model_path = config::kDefaultModelPath;
    std::filesystem::path sgf_path = config::kDefaultSgfPath;
    bool help = false;
};

std::uint64_t parseUnsigned(const std::string& text, const char* option)
{
    if (text.empty()) {
        throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
        }
    }

    std::size_t consumed = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
    }
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
    }
    return value;
}

CommandLine parseCommandLine(int argc, char** argv)
{
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto nextValue = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument("missing value after " + option);
            return argv[index];
        };

        if (option == "-h" || option == "--help") {
            command.help = true;
        } else if (option == "--winning-tile") {
            const std::uint64_t value = parseUnsigned(nextValue(), "--winning-tile");
            if (value > 32768) throw std::invalid_argument("--winning-tile exceeds the board encoding");
            command.winning_tile = static_cast<unsigned>(value);
        } else if (option == "--node-limit") {
            command.node_limit = parseUnsigned(nextValue(), "--node-limit");
        } else if (option == "--progress-interval") {
            command.progress_interval = parseUnsigned(nextValue(), "--progress-interval");
        } else if (option == "--tt") {
            command.use_transposition_table = true;
        } else if (option == "--no-tt") {
            command.use_transposition_table = false;
        } else if (option == "--network") {
            command.network = nextValue();
        } else if (option == "--model") {
            command.model_path = nextValue();
        } else if (option == "--sgf") {
            command.sgf_path = nextValue();
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return command;
}

void printHelp(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "First-version fixed-opening adversarial 2048 solver.\n\n"
        << "Options:\n"
        << "  --winning-tile N       Target tile (default: " << config::kDefaultWinningTile << ")\n"
        << "  --node-limit N         Combined expanded-node limit; 0 is unlimited\n"
        << "                         (default: " << config::kDefaultNodeLimit << ")\n"
        << "  --progress-interval N  Print progress every N expanded nodes; 0 disables\n"
        << "  --tt                    Enable the exact transposition table\n"
        << "  --no-tt                 Disable it (default: "
        << (config::kDefaultUseTranspositionTable ? "enabled" : "disabled") << ")\n"
        << "  --network NAME         N-tuple network (first version: 4x6patt)\n"
        << "  --model PATH           N-tuple weight file\n"
        << "  --sgf PATH             Final SGF output path\n"
        << "  -h, --help             Show this help\n\n"
        << "The fixed opening is configured in solver/config.h as 2@0 and 2@1.\n";
}

void printBoard(const std::array<unsigned, 16>& tiles)
{
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            std::cout << std::setw(5) << tiles[row * 4 + column];
        }
        std::cout << '\n';
    }
}

} // namespace
} // namespace solver2048

int main(int argc, char** argv)
{
    using namespace solver2048;
    try {
        const CommandLine command = parseCommandLine(argc, argv);
        if (command.help) {
            printHelp(argv[0]);
            return 0;
        }

        GameApi game(command.network, command.model_path);
        SolverOptions options;
        options.winning_tile = command.winning_tile;
        options.node_limit = command.node_limit;
        options.progress_interval = command.progress_interval;
        options.use_transposition_table = command.use_transposition_table;

        std::cout << "fixed opening (Player to move):\n";
        printBoard(game.tileValues(game.fixedOpening()));
        std::cout << "winning tile: " << command.winning_tile << '\n'
                  << "node limit: " << command.node_limit << " (0 = unlimited)\n"
                  << "transposition table: " << (command.use_transposition_table ? "enabled" : "disabled") << '\n'
                  << "network: " << command.network << '\n'
                  << "model: " << command.model_path << '\n';

        const auto started = std::chrono::steady_clock::now();
        Solver solver(game, options, [&](const SearchStatistics& statistics) {
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << "progress: nodes=" << statistics.combinedNodes()
                      << " player=" << statistics.player_nodes
                      << " environment=" << statistics.environment_nodes
                      << " tt_hits=" << statistics.ttHits()
                      << " elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s\n";
        });
        SearchResult result = solver.solve();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        SgfMetadata metadata;
        metadata.winning_tile = command.winning_tile;
        metadata.node_limit = command.node_limit;
        metadata.use_transposition_table = command.use_transposition_table;
        metadata.network = command.network;
        metadata.model_path = command.model_path;
        metadata.elapsed_seconds = elapsed;
        metadata.statistics = result.statistics;
        SgfWriter::write(command.sgf_path, *result.root, metadata);

        std::cout << std::fixed << std::setprecision(6)
                  << "root result: " << outcomeName(result.outcome) << '\n';
        if (result.outcome == Outcome::Win && result.root_action) {
            std::cout << "root action: " << directionName(*result.root_action) << '\n';
            if (result.root_value) std::cout << "root action V(afterstate): " << *result.root_value << '\n';
        }
        std::cout << "player nodes: " << result.statistics.player_nodes << '\n'
                  << "environment nodes: " << result.statistics.environment_nodes << '\n'
                  << "combined nodes: " << result.statistics.combinedNodes() << '\n'
                  << "player actions: " << result.statistics.player_actions << '\n'
                  << "placement actions: " << result.statistics.placement_actions << '\n'
                  << "node-limit cutoffs: " << result.statistics.node_limit_cutoffs << '\n';
        if (command.use_transposition_table) {
            std::cout << "player TT hits: " << result.statistics.player_tt_hits << '\n'
                      << "environment TT hits: " << result.statistics.environment_tt_hits << '\n'
                      << "player TT entries: " << result.statistics.player_tt_stores << '\n'
                      << "environment TT entries: " << result.statistics.environment_tt_stores << '\n';
        }
        std::cout << "search elapsed: " << elapsed << "s\n"
                  << "SGF output: " << command.sgf_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "solver error: " << error.what() << '\n';
        return 1;
    }
}
