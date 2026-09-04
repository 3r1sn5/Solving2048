#include "config.h"
#include "game_api.h"
#include "rz_file.h"
#include "search.h"
#include "sgf.h"
#include "tt_file.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace solver2048 {
namespace {

struct CommandLine {
    SolverConfig settings;
    std::filesystem::path config_path = config::kDefaultConfigPath;
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

RZoneMode parseRZoneModeOption(const std::string& text)
{
    if (text == "disabled") return RZoneMode::Disabled;
    if (text == "generate") return RZoneMode::Generate;
    if (text == "lookup") return RZoneMode::Lookup;
    throw std::invalid_argument("--rzone-mode requires disabled, generate, or lookup");
}

RZoneLookupPolicy parseRZoneLookupPolicyOption(const std::string& text)
{
    if (text == "full_scan") return RZoneLookupPolicy::FullScan;
    if (text == "smallest_first") return RZoneLookupPolicy::SmallestFirst;
    throw std::invalid_argument(
        "--rzone-lookup-policy requires full_scan or smallest_first");
}

CommandLine parseCommandLine(int argc, char** argv)
{
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "-h" || option == "--help") {
            command.help = true;
            return command;
        }
    }

    bool config_path_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const bool takes_value = option == "--config" || option == "--winning-tile" ||
                                 option == "--node-limit" || option == "--progress-interval" ||
                                 option == "--network" || option == "--model" || option == "--sgf" ||
                                 option == "--tt-file" || option == "--rzone-mode" ||
                                 option == "--rz-file" ||
                                 option == "--rzone-lookup-policy" ||
                                 option == "--rzone-max-cells";
        const bool is_flag = option == "--tt" || option == "--no-tt" ||
                             option == "--tt-symmetry" || option == "--no-tt-symmetry" ||
                             option == "--load-tt" || option == "--no-load-tt" ||
                             option == "--save-tt" || option == "--no-save-tt" ||
                             option == "--load-rz" || option == "--no-load-rz" ||
                             option == "--save-rz" || option == "--no-save-rz" ||
                             option == "--rzone-symmetry" ||
                             option == "--no-rzone-symmetry" ||
                             option == "--rzone-shadow-exact-check" ||
                             option == "--no-rzone-shadow-exact-check";
        if (!takes_value && !is_flag) throw std::invalid_argument("unknown option: " + option);
        if (!takes_value) continue;
        if (++index >= argc) throw std::invalid_argument("missing value after " + option);
        if (option == "--config") {
            if (config_path_seen) throw std::invalid_argument("--config may only be specified once");
            command.config_path = argv[index];
            config_path_seen = true;
        }
    }

    command.settings = loadSolverConfig(command.config_path);
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto nextValue = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument("missing value after " + option);
            return argv[index];
        };

        if (option == "--config") {
            (void)nextValue();
        } else if (option == "--winning-tile") {
            const std::uint64_t value = parseUnsigned(nextValue(), "--winning-tile");
            if (value > 32768) throw std::invalid_argument("--winning-tile exceeds the board encoding");
            command.settings.winning_tile = static_cast<unsigned>(value);
        } else if (option == "--node-limit") {
            command.settings.node_limit = parseUnsigned(nextValue(), "--node-limit");
        } else if (option == "--progress-interval") {
            command.settings.progress_interval = parseUnsigned(nextValue(), "--progress-interval");
        } else if (option == "--tt") {
            command.settings.use_transposition_table = true;
        } else if (option == "--no-tt") {
            command.settings.use_transposition_table = false;
            command.settings.load_tt = false;
            command.settings.save_tt = false;
        } else if (option == "--tt-symmetry") {
            command.settings.use_tt_symmetry = true;
        } else if (option == "--no-tt-symmetry") {
            command.settings.use_tt_symmetry = false;
        } else if (option == "--load-tt") {
            command.settings.load_tt = true;
        } else if (option == "--no-load-tt") {
            command.settings.load_tt = false;
        } else if (option == "--save-tt") {
            command.settings.save_tt = true;
        } else if (option == "--no-save-tt") {
            command.settings.save_tt = false;
        } else if (option == "--tt-file") {
            command.settings.tt_path = nextValue();
        } else if (option == "--rzone-mode") {
            command.settings.rzone_mode = parseRZoneModeOption(nextValue());
            if (command.settings.rzone_mode == RZoneMode::Disabled) {
                command.settings.load_rz = false;
                command.settings.save_rz = false;
            }
        } else if (option == "--rzone-lookup-policy") {
            command.settings.rzone_lookup_policy =
                parseRZoneLookupPolicyOption(nextValue());
        } else if (option == "--rzone-symmetry") {
            command.settings.use_rzone_symmetry = true;
        } else if (option == "--no-rzone-symmetry") {
            command.settings.use_rzone_symmetry = false;
        } else if (option == "--rzone-max-cells") {
            const std::uint64_t value = parseUnsigned(nextValue(), "--rzone-max-cells");
            if (value > 16) throw std::invalid_argument("--rzone-max-cells must be in [1, 16]");
            command.settings.rzone_max_cells = static_cast<unsigned>(value);
        } else if (option == "--rzone-shadow-exact-check") {
            command.settings.rzone_shadow_exact_check = true;
        } else if (option == "--no-rzone-shadow-exact-check") {
            command.settings.rzone_shadow_exact_check = false;
        } else if (option == "--load-rz") {
            command.settings.load_rz = true;
        } else if (option == "--no-load-rz") {
            command.settings.load_rz = false;
        } else if (option == "--save-rz") {
            command.settings.save_rz = true;
        } else if (option == "--no-save-rz") {
            command.settings.save_rz = false;
        } else if (option == "--rz-file") {
            command.settings.rz_path = nextValue();
        } else if (option == "--network") {
            command.settings.network = nextValue();
        } else if (option == "--model") {
            command.settings.model_path = nextValue();
        } else if (option == "--sgf") {
            command.settings.sgf_path = nextValue();
        }
    }
    validateSolverConfig(command.settings);
    return command;
}

void printHelp(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "First-version fixed-opening adversarial 2048 solver.\n\n"
        << "Options:\n"
        << "  --config PATH          Configuration file (default: " << config::kDefaultConfigPath << ")\n"
        << "  --winning-tile N       Override target tile from cfg\n"
        << "  --node-limit N         Override combined expanded-node limit; 0 is unlimited\n"
        << "  --progress-interval N  Print progress every N expanded nodes; 0 disables\n"
        << "  --tt / --no-tt         Override exact transposition-table mode\n"
        << "  --tt-symmetry           Canonicalize TT keys over all 8 D4 symmetries\n"
        << "  --no-tt-symmetry        Use exact board orientation\n"
        << "  --load-tt / --no-load-tt  Load or skip the persistent TT snapshot\n"
        << "  --save-tt / --no-save-tt  Save or skip the merged persistent TT snapshot\n"
        << "  --tt-file PATH          Override persistent TT file path\n"
        << "  --rzone-mode MODE      disabled, generate, or lookup\n"
        << "  --rzone-lookup-policy POLICY  full_scan or smallest_first\n"
        << "  --rzone-symmetry / --no-rzone-symmetry  Search all 8 D4 orientations\n"
        << "  --rzone-max-cells N    Maximum cells in an RZone pattern (1..16)\n"
        << "  --load-rz / --no-load-rz  Load or skip the persistent RZone snapshot\n"
        << "  --save-rz / --no-save-rz  Save or skip the merged persistent RZone snapshot\n"
        << "  --rz-file PATH          Override persistent RZone file path\n"
        << "  --rzone-shadow-exact-check / --no-rzone-shadow-exact-check\n"
        << "  --network NAME         Override N-tuple network\n"
        << "  --model PATH           Override N-tuple weight file\n"
        << "  --sgf PATH             Override final SGF output path\n"
        << "  -h, --help             Show this help\n\n"
        << "With no options, all runtime settings are loaded from the default cfg.\n"
        << "A persistent TT file must match the selected winning tile and symmetry mode.\n";
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

        const SolverConfig& settings = command.settings;

        GameApi game(settings.network, settings.model_path, settings.opening);
        SolverOptions options;
        options.winning_tile = settings.winning_tile;
        options.node_limit = settings.node_limit;
        options.progress_interval = settings.progress_interval;
        options.use_transposition_table = settings.use_transposition_table;
        options.use_tt_symmetry = settings.use_tt_symmetry;
        options.rzone_mode = settings.rzone_mode;
        options.rzone_lookup_policy = settings.rzone_lookup_policy;
        options.use_rzone_symmetry = settings.use_rzone_symmetry;
        options.rzone_max_cells = settings.rzone_max_cells;
        options.rzone_shadow_exact_check = settings.rzone_shadow_exact_check;

        std::cout << "configuration: " << command.config_path.string() << '\n';
        std::cout << "fixed opening (Player to move):\n";
        printBoard(game.tileValues(game.fixedOpening()));
        std::cout << "opening: " << formatOpening(settings.opening) << '\n'
                  << "winning tile: " << settings.winning_tile << '\n'
                  << "node limit: " << settings.node_limit << " (0 = unlimited)\n"
                  << "progress interval: " << settings.progress_interval << '\n'
                  << "transposition table: " << (settings.use_transposition_table ? "enabled" : "disabled") << '\n'
                  << "TT symmetry canonicalization: "
                  << (settings.use_tt_symmetry ? "enabled" : "disabled") << '\n'
                  << "persistent TT load: " << (settings.load_tt ? "enabled" : "disabled") << '\n'
                  << "persistent TT save: " << (settings.save_tt ? "enabled" : "disabled") << '\n'
                  << "TT file: " << settings.tt_path.string() << '\n'
                  << "RZone mode: " << rzoneModeName(settings.rzone_mode) << '\n'
                  << "RZone lookup policy: "
                  << rzoneLookupPolicyName(settings.rzone_lookup_policy) << '\n'
                  << "RZone symmetry lookup: "
                  << (settings.use_rzone_symmetry ? "enabled" : "disabled") << '\n'
                  << "RZone max cells: " << settings.rzone_max_cells << '\n'
                  << "persistent RZone load: "
                  << (settings.load_rz ? "enabled" : "disabled") << '\n'
                  << "persistent RZone save: "
                  << (settings.save_rz ? "enabled" : "disabled") << '\n'
                  << "RZone file: " << settings.rz_path.string() << '\n'
                  << "RZone shadow exact check: "
                  << (settings.rzone_shadow_exact_check ? "enabled" : "disabled") << '\n'
                  << "network: " << settings.network << '\n'
                  << "model: " << settings.model_path << '\n'
                  << "SGF output: " << settings.sgf_path.string() << '\n';

        auto started = std::chrono::steady_clock::now();
        Solver solver(game, options, [&](const SearchStatistics& statistics) {
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << "progress: nodes=" << statistics.combinedNodes()
                      << " player=" << statistics.player_nodes
                      << " environment=" << statistics.environment_nodes
                      << " tt_hits=" << statistics.ttHits()
                      << " rzone_hits=" << statistics.rzone.hits()
                      << " elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s\n";
        });
        std::optional<TtFileSummary> loaded_tt;
        if (settings.load_tt) {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(settings.tt_path, exists_error);
            if (exists_error) {
                throw std::runtime_error("cannot inspect TT file: " + exists_error.message());
            }
            if (exists) {
                if (!std::filesystem::is_regular_file(settings.tt_path)) {
                    throw std::runtime_error("TT path is not a regular file: " +
                                             settings.tt_path.string());
                }
                const auto load_started = std::chrono::steady_clock::now();
                loaded_tt = TtFile::load(settings.tt_path, solver);
                const double load_elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - load_started).count();
                std::cout << "loaded persistent TT: player=" << loaded_tt->player_entries
                          << " environment=" << loaded_tt->environment_entries
                          << " elapsed=" << std::fixed << std::setprecision(6)
                          << load_elapsed << "s\n";
            } else {
                std::cout << "persistent TT file not found; starting with empty tables\n";
            }
        }
        std::optional<RzFileSummary> loaded_rz;
        if (settings.load_rz) {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(settings.rz_path, exists_error);
            if (exists_error) {
                throw std::runtime_error(
                    "cannot inspect RZone file: " + exists_error.message());
            }
            if (exists) {
                if (!std::filesystem::is_regular_file(settings.rz_path)) {
                    throw std::runtime_error(
                        "RZone path is not a regular file: " + settings.rz_path.string());
                }
                const auto load_started = std::chrono::steady_clock::now();
                loaded_rz = RzFile::load(settings.rz_path, solver);
                const double load_elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - load_started).count();
                std::cout << "loaded persistent RZone: certificates="
                          << loaded_rz->record_count
                          << " elapsed=" << std::fixed << std::setprecision(6)
                          << load_elapsed << "s\n";
            } else {
                std::cout
                    << "persistent RZone file not found; starting with an empty table\n";
            }
        }

        started = std::chrono::steady_clock::now();
        SearchResult result = solver.solve();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::optional<TtFileSummary> saved_tt;
        double save_elapsed = 0;
        if (settings.save_tt) {
            const auto save_started = std::chrono::steady_clock::now();
            saved_tt = TtFile::save(settings.tt_path, solver);
            save_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - save_started).count();
        }
        std::optional<RzFileSummary> saved_rz;
        double rz_save_elapsed = 0;
        if (settings.save_rz) {
            const auto save_started = std::chrono::steady_clock::now();
            saved_rz = RzFile::save(settings.rz_path, solver);
            rz_save_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - save_started).count();
        }

        SgfMetadata metadata;
        metadata.winning_tile = settings.winning_tile;
        metadata.node_limit = settings.node_limit;
        metadata.use_transposition_table = settings.use_transposition_table;
        metadata.use_tt_symmetry = settings.use_tt_symmetry;
        metadata.rzone_mode = settings.rzone_mode;
        metadata.rzone_lookup_policy = settings.rzone_lookup_policy;
        metadata.use_rzone_symmetry = settings.use_rzone_symmetry;
        metadata.rzone_max_cells = settings.rzone_max_cells;
        metadata.rzone_shadow_exact_check = settings.rzone_shadow_exact_check;
        metadata.load_tt = settings.load_tt;
        metadata.save_tt = settings.save_tt;
        metadata.tt_path = settings.tt_path;
        if (saved_tt) {
            metadata.tt_format_version = TtFile::kFormatVersion;
            metadata.tt_payload_checksum = saved_tt->payload_checksum;
            metadata.tt_file_size = saved_tt->file_size;
            metadata.tt_player_records = saved_tt->player_entries;
            metadata.tt_environment_records = saved_tt->environment_entries;
        }
        metadata.load_rz = settings.load_rz;
        metadata.save_rz = settings.save_rz;
        metadata.rz_path = settings.rz_path;
        if (saved_rz) {
            metadata.rz_format_version = RzFile::kFormatVersion;
            metadata.rz_semantics_version = RzFile::kSemanticsVersion;
            metadata.rz_prefix_record_count = saved_rz->record_count;
            metadata.rz_prefix_checksum = saved_rz->payload_checksum;
        }
        metadata.network = settings.network;
        metadata.model_path = settings.model_path;
        metadata.elapsed_seconds = elapsed;
        metadata.statistics = result.statistics;
        metadata.config_path = command.config_path;
        metadata.opening = settings.opening;
        SgfWriter::write(settings.sgf_path, *result.root, metadata);

        std::cout << std::fixed << std::setprecision(6)
                  << "root result: " << outcomeName(result.outcome) << '\n';
        if (result.outcome == Outcome::Win && result.root_action) {
            std::cout << "root action: " << directionName(*result.root_action) << '\n';
            if (result.root_action_value) {
                std::cout << "root action reward + V(afterstate): "
                          << *result.root_action_value << '\n';
            }
        }
        std::cout << "player nodes: " << result.statistics.player_nodes << '\n'
                  << "environment nodes: " << result.statistics.environment_nodes << '\n'
                  << "combined nodes: " << result.statistics.combinedNodes() << '\n'
                  << "player actions: " << result.statistics.player_actions << '\n'
                  << "placement actions: " << result.statistics.placement_actions << '\n'
                  << "node-limit cutoffs: " << result.statistics.node_limit_cutoffs << '\n';
        if (settings.use_transposition_table) {
            std::cout << "player TT hits: " << result.statistics.player_tt_hits << '\n'
                      << "environment TT hits: " << result.statistics.environment_tt_hits << '\n'
                      << "player exact-orientation TT hits: "
                      << result.statistics.playerTtExactHits() << '\n'
                      << "environment exact-orientation TT hits: "
                      << result.statistics.environmentTtExactHits() << '\n'
                      << "player symmetry-derived TT hits: "
                      << result.statistics.player_tt_symmetry_hits << '\n'
                      << "environment symmetry-derived TT hits: "
                      << result.statistics.environment_tt_symmetry_hits << '\n'
                      << "player persistent TT hits: "
                      << result.statistics.player_persistent_tt_hits << '\n'
                      << "environment persistent TT hits: "
                      << result.statistics.environment_persistent_tt_hits << '\n'
                      << "player TT loaded entries: "
                      << result.statistics.player_tt_loaded_entries << '\n'
                      << "environment TT loaded entries: "
                      << result.statistics.environment_tt_loaded_entries << '\n'
                      << "player TT new entries: " << result.statistics.player_tt_stores << '\n'
                      << "environment TT new entries: "
                      << result.statistics.environment_tt_stores << '\n'
                      << "player TT entries: " << result.statistics.playerTtEntries() << '\n'
                      << "environment TT entries: "
                      << result.statistics.environmentTtEntries() << '\n';
        }
        if (settings.rzone_mode != RZoneMode::Disabled) {
            const RZoneStatistics& rzone = result.statistics.rzone;
            std::cout << "RZone loaded certificates: " << rzone.loaded_certificates << '\n'
                      << "RZone new certificates: " << rzone.patterns_stored << '\n'
                      << "RZone duplicate certificates: " << rzone.duplicate_patterns << '\n'
                      << "RZone terminal certificates: " << rzone.terminal_certificates << '\n'
                      << "RZone slide lifts: " << rzone.slide_lift_successes << '/'
                      << rzone.slide_lift_attempts << '\n'
                      << "RZone placement lifts: " << rzone.placement_lift_successes << '/'
                      << rzone.placement_lift_attempts << '\n'
                      << "RZone lookups: " << rzone.lookups << '\n'
                      << "RZone unique masks: " << rzone.masks_indexed << '\n'
                      << "RZone masks tested: " << rzone.lookup_masks_tested << '\n'
                      << "RZone lookup candidates: " << rzone.lookup_candidates << '\n'
                      << "RZone pattern matches: " << rzone.pattern_matches << '\n'
                      << "RZone early exits: " << rzone.lookup_early_exits << '\n'
                      << "RZone player hits: " << rzone.player_hits << '\n'
                      << "RZone environment hits: " << rzone.environment_hits << '\n'
                      << "RZone exact/symmetry hits: " << rzone.exact_orientation_hits << '/'
                      << rzone.symmetry_hits << '\n'
                      << "RZone transforms tested: " << rzone.lookup_transforms_tested << '\n'
                      << "RZone partial/full hits: " << rzone.partial_hits << '/'
                      << rzone.full_board_hits << '\n';
        }
        if (saved_tt) {
            std::cout << "saved persistent TT: entries=" << saved_tt->entries()
                      << " bytes=" << saved_tt->file_size
                      << " checksum=0x" << std::hex << saved_tt->payload_checksum << std::dec
                      << " elapsed=" << std::fixed << std::setprecision(6) << save_elapsed << "s\n";
        }
        if (saved_rz) {
            std::cout << "saved persistent RZone: certificates="
                      << saved_rz->record_count
                      << " bytes=" << saved_rz->file_size
                      << " checksum=0x" << std::hex << saved_rz->payload_checksum
                      << std::dec << " elapsed=" << std::fixed << std::setprecision(6)
                      << rz_save_elapsed << "s\n";
        }
        std::cout << "search elapsed: " << elapsed << "s\n"
                  << "SGF output: " << settings.sgf_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "solver error: " << error.what() << '\n';
        return 1;
    }
}
