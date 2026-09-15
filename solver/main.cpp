#include "config.h"
#include "game_api.h"
#include "proof_checker.h"
#include "proof_sequence.h"
#include "rz_file.h"
#include "rz_proof_file.h"
#include "search.h"
#include "sgf.h"
#include "tt_file.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace solver2048 {
namespace {

struct CommandLine {
    SolverConfig settings;
    std::filesystem::path config_path = config::kDefaultConfigPath;
    std::vector<std::array<unsigned, 16>> batch_openings;
    std::filesystem::path openings_file;
    std::filesystem::path output_directory;
    bool verify_batch = false;
    bool sgf_path_overridden = false;
    bool tt_path_overridden = false;
    bool rz_path_overridden = false;
    bool help = false;

    bool batchMode() const { return !batch_openings.empty() || !openings_file.empty(); }
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
    bool batch_requested = false;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const bool takes_value = option == "--config" || option == "--winning-tile" ||
                                 option == "--node-limit" || option == "--progress-interval" ||
                                 option == "--network" || option == "--model" || option == "--sgf" ||
                                 option == "--opening" || option == "--openings-file" ||
                                 option == "--output-dir" ||
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
                             option == "--no-rzone-shadow-exact-check" ||
                             option == "--rzone-active-pruning" ||
                             option == "--no-rzone-active-pruning" ||
                             option == "--verify";
        if (!takes_value && !is_flag) throw std::invalid_argument("unknown option: " + option);
        if (!takes_value) continue;
        if (++index >= argc) throw std::invalid_argument("missing value after " + option);
        if (option == "--config") {
            if (config_path_seen) throw std::invalid_argument("--config may only be specified once");
            command.config_path = argv[index];
            config_path_seen = true;
        }
        if (option == "--opening" || option == "--openings-file") {
            batch_requested = true;
        }
    }

    command.settings = loadSolverConfig(command.config_path, !batch_requested);
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
            command.tt_path_overridden = true;
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
        } else if (option == "--rzone-active-pruning") {
            command.settings.rzone_active_pruning = true;
        } else if (option == "--no-rzone-active-pruning") {
            command.settings.rzone_active_pruning = false;
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
            command.rz_path_overridden = true;
        } else if (option == "--network") {
            command.settings.network = nextValue();
        } else if (option == "--model") {
            command.settings.model_path = nextValue();
        } else if (option == "--sgf") {
            command.settings.sgf_path = nextValue();
            command.sgf_path_overridden = true;
        } else if (option == "--opening") {
            command.batch_openings.push_back(parseOpeningSpec(nextValue()));
        } else if (option == "--openings-file") {
            if (!command.openings_file.empty()) {
                throw std::invalid_argument(
                    "--openings-file may only be specified once");
            }
            command.openings_file = nextValue();
        } else if (option == "--output-dir") {
            if (!command.output_directory.empty()) {
                throw std::invalid_argument("--output-dir may only be specified once");
            }
            command.output_directory = nextValue();
        } else if (option == "--verify") {
            command.verify_batch = true;
        }
    }
    if (!command.openings_file.empty() && !command.batch_openings.empty()) {
        throw std::invalid_argument(
            "--openings-file cannot be combined with --opening");
    }
    if (!command.openings_file.empty()) {
        command.batch_openings = loadOpeningsFile(command.openings_file);
    }
    if (command.batchMode()) {
        if (command.output_directory.empty()) {
            throw std::invalid_argument("batch mode requires --output-dir");
        }
        if (command.sgf_path_overridden || command.tt_path_overridden ||
            command.rz_path_overridden) {
            throw std::invalid_argument(
                "batch mode derives SGF/TT/RZone paths from --output-dir");
        }
        std::unordered_set<std::uint64_t> seen;
        for (const auto& opening : command.batch_openings) {
            if (!seen.insert(boardFromTiles(opening).raw).second) {
                throw std::invalid_argument("batch opening list contains a duplicate");
            }
        }
        if (!command.settings.use_transposition_table) {
            throw std::invalid_argument(
                "first-version batch mode requires the transposition table");
        }
    } else {
        if (!command.output_directory.empty()) {
            throw std::invalid_argument("--output-dir requires batch openings");
        }
        if (command.verify_batch) {
            throw std::invalid_argument("--verify is currently a batch-mode option");
        }
    }
    validateSolverConfig(command.settings, !command.batchMode());
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
        << "  --node-limit N         Override combined visited-node limit; 0 is unlimited\n"
        << "  --progress-interval N  Print progress every N visited nodes; 0 disables\n"
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
        << "  --rzone-active-pruning / --no-rzone-active-pruning\n"
        << "  --network NAME         Override N-tuple network\n"
        << "  --model PATH           Override N-tuple weight file\n"
        << "  --sgf PATH             Override final SGF output path\n"
        << "  --opening SPEC         Add a batch opening; may be repeated\n"
        << "  --openings-file PATH   Read ordered batch openings, one per line\n"
        << "  --output-dir PATH      Write managed batch artifacts below PATH\n"
        << "  --verify               Verify the completed batch proof sequence\n"
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

SolverOptions solverOptions(const SolverConfig& settings)
{
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
    options.rzone_active_pruning = settings.rzone_active_pruning;
    return options;
}

void requireNewBatchPath(const std::filesystem::path& path)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        throw std::runtime_error(
            "cannot inspect batch output path " + path.string() + ": " +
            exists_error.message());
    }
    if (exists) {
        throw std::runtime_error(
            "batch output already exists (resume is not implemented yet): " +
            path.string());
    }
}

std::filesystem::path numberedSgfPath(const std::filesystem::path& directory,
                                      std::size_t index)
{
    std::ostringstream name;
    name << std::setw(6) << std::setfill('0') << index + 1 << ".sgf";
    return directory / name.str();
}

SgfMetadata batchSgfMetadata(
    const CommandLine& command,
    const std::array<unsigned, 16>& opening,
    const SearchResult& result,
    double elapsed,
    bool has_prior_opening,
    bool final_opening,
    const std::filesystem::path& tt_path,
    const std::filesystem::path& rz_path,
    const std::optional<TtFileSummary>& tt_summary,
    const std::optional<RzFileSummary>& rz_summary)
{
    const SolverConfig& settings = command.settings;
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
    metadata.rzone_active_pruning = settings.rzone_active_pruning;
    metadata.load_tt = has_prior_opening;
    metadata.save_tt = final_opening;
    metadata.tt_path = tt_path;
    if (tt_summary) {
        metadata.tt_format_version = TtFile::kFormatVersion;
        metadata.tt_payload_checksum = tt_summary->payload_checksum;
        metadata.tt_file_size = tt_summary->file_size;
        metadata.tt_player_records = tt_summary->player_entries;
        metadata.tt_environment_records = tt_summary->environment_entries;
    }
    const bool rzone_enabled = settings.rzone_mode != RZoneMode::Disabled;
    metadata.load_rz = has_prior_opening && rzone_enabled;
    metadata.save_rz = final_opening && rzone_enabled;
    metadata.rz_path = rz_path;
    if (rz_summary) {
        metadata.rz_format_version = RzFile::kFormatVersion;
        metadata.rz_semantics_version = RzFile::kSemanticsVersion;
        metadata.rz_prefix_record_count = rz_summary->record_count;
        metadata.rz_prefix_checksum = rz_summary->payload_checksum;
    }
    metadata.config_path = command.config_path;
    metadata.opening = opening;
    metadata.network = settings.network;
    metadata.model_path = settings.model_path;
    metadata.elapsed_seconds = elapsed;
    metadata.statistics = result.statistics;
    return metadata;
}

int runBatch(const CommandLine& command)
{
    const SolverConfig& settings = command.settings;
    const std::filesystem::path output_directory =
        std::filesystem::absolute(command.output_directory).lexically_normal();
    const std::filesystem::path sgf_directory = output_directory / "sgf";
    const std::filesystem::path tt_path = output_directory / "proof.tt";
    const std::filesystem::path rz_path = output_directory / "proof.rz";
    const std::filesystem::path sequence_path =
        output_directory / "proof_sequence.txt";
    const std::filesystem::path status_path = output_directory / "batch_status.txt";
    const bool rzone_enabled = settings.rzone_mode != RZoneMode::Disabled;

    requireNewBatchPath(tt_path);
    if (rzone_enabled) requireNewBatchPath(rz_path);
    requireNewBatchPath(sequence_path);
    requireNewBatchPath(status_path);
    for (std::size_t index = 0; index < command.batch_openings.size(); ++index) {
        requireNewBatchPath(numberedSgfPath(sgf_directory, index));
    }
    std::error_code directory_error;
    std::filesystem::create_directories(sgf_directory, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create batch output directory: " + directory_error.message());
    }

    std::ofstream status(status_path, std::ios::out | std::ios::trunc);
    if (!status) {
        throw std::runtime_error("cannot create batch status file: " + status_path.string());
    }
    status << "format_version=1\n"
           << "state=RUNNING\n"
           << "openings_total=" << command.batch_openings.size() << '\n';
    status.flush();

    struct RetainedSolver {
        std::unique_ptr<GameApi> game;
        std::unique_ptr<Solver> solver;
    };
    std::optional<RetainedSolver> retained;
    std::vector<std::filesystem::path> sgf_paths;
    sgf_paths.reserve(command.batch_openings.size());
    std::optional<RzFileSummary> accumulated_rz_summary;

    for (std::size_t index = 0; index < command.batch_openings.size(); ++index) {
        const auto& opening = command.batch_openings[index];
        std::cout << "batch opening " << index + 1 << '/'
                  << command.batch_openings.size() << ": "
                  << formatOpening(opening) << '\n';

        auto game = std::make_unique<GameApi>(
            settings.network, settings.model_path, opening);
        const auto opening_started = std::chrono::steady_clock::now();
        auto solver = std::make_unique<Solver>(
            *game,
            solverOptions(settings),
            [&, index](const SearchStatistics& statistics) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - opening_started).count();
                std::cerr << "progress: opening=" << index + 1
                          << " visited=" << statistics.combinedNodes()
                          << " tt_hits=" << statistics.ttHits()
                          << " rzone_hits=" << statistics.rzone.hits()
                          << " elapsed=" << std::fixed << std::setprecision(3)
                          << elapsed << "s\n";
            });
        if (retained) {
            solver->adoptPersistentProofs(*retained->solver);
            retained.reset();
        }

        SearchResult result = solver->solve();
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - opening_started).count();
        const bool final_opening = index + 1 == command.batch_openings.size();
        const std::filesystem::path sgf_path = numberedSgfPath(sgf_directory, index);

        std::optional<RzFileSummary> rz_summary;
        if (rzone_enabled) {
            rz_summary = RzFile::summarize(
                *solver,
                accumulated_rz_summary ? &*accumulated_rz_summary : nullptr);
        }
        std::optional<TtFileSummary> tt_summary;
        if (result.outcome == Outcome::Win && final_opening) {
            tt_summary = TtFile::save(tt_path, *solver);
            if (rzone_enabled) {
                const RzFileSummary saved = RzFile::save(rz_path, *solver);
                if (saved.record_count != rz_summary->record_count ||
                    saved.payload_checksum != rz_summary->payload_checksum) {
                    throw std::logic_error(
                        "incremental RZone summary differs from the final snapshot");
                }
                rz_summary = saved;
            }
        }
        const SgfMetadata metadata = batchSgfMetadata(
            command,
            opening,
            result,
            elapsed,
            index != 0,
            result.outcome == Outcome::Win && final_opening,
            tt_path,
            rz_path,
            tt_summary,
            rz_summary);
        SgfWriter::write(sgf_path, *result.root, metadata);

        status << "opening=" << index + 1
               << " spec=" << formatOpening(opening)
               << " outcome=" << outcomeName(result.outcome)
               << " visited=" << result.statistics.combinedNodes()
               << " player_tt_entries=" << result.statistics.playerTtEntries()
               << " environment_tt_entries="
               << result.statistics.environmentTtEntries()
               << " rzone_records="
               << (rz_summary ? rz_summary->record_count : 0)
               << " elapsed_seconds=" << std::fixed << std::setprecision(6)
               << elapsed << " sgf=" << sgf_path.string() << '\n';
        status.flush();

        std::cout << "batch result " << index + 1 << ": "
                  << outcomeName(result.outcome)
                  << ", visited=" << result.statistics.combinedNodes()
                  << ", new_tt=" << result.statistics.ttStores()
                  << ", persistent_tt_hits="
                  << result.statistics.player_persistent_tt_hits +
                         result.statistics.environment_persistent_tt_hits
                  << ", rzone_records="
                  << (rz_summary ? rz_summary->record_count : 0)
                  << ", elapsed=" << std::fixed << std::setprecision(6)
                  << elapsed << "s\n";

        if (result.outcome != Outcome::Win) {
            status << "state=FAILED\nfailed_opening=" << index + 1 << '\n';
            status.close();
            throw std::runtime_error(
                "batch stopped because opening " + std::to_string(index + 1) +
                " did not produce WIN; its proof delta was not published");
        }

        sgf_paths.push_back(sgf_path);
        accumulated_rz_summary = rz_summary;
        retained = RetainedSolver{std::move(game), std::move(solver)};
    }

    ProofSequenceManifest manifest;
    manifest.tt_path = tt_path;
    if (rzone_enabled) manifest.rz_path = rz_path;
    manifest.sgf_paths = sgf_paths;
    writeProofSequenceManifest(sequence_path, manifest);
    status << "state=COMPLETE\n"
           << "sequence=" << sequence_path.string() << '\n';
    status.close();

    std::cout << "batch proof sequence: " << sequence_path.string() << '\n'
              << "final TT: " << tt_path.string() << '\n';
    if (rzone_enabled) std::cout << "final RZone: " << rz_path.string() << '\n';

    if (command.verify_batch) {
        retained.reset();
        std::cout << "verifying batch proof sequence...\n";
        const ProofTable table = ProofTable::load(tt_path);
        std::optional<RzProofTable> rzone_table;
        if (rzone_enabled) rzone_table = RzProofTable::load(rz_path);
        const ProofSequenceVerificationResult verification =
            verifySgfProofSequence(
                sgf_paths,
                table,
                rzone_table ? &*rzone_table : nullptr);
        std::cout << "batch proof status: VERIFIED\n"
                  << "SGFs verified: " << verification.openings.size() << '\n'
                  << "TT records cross-checked: "
                  << verification.aggregate_statistics.tt_records_cross_checked
                  << '\n'
                  << "RZone records cross-checked: "
                  << verification.aggregate_statistics.rzone_records_cross_checked
                  << '\n';
    }
    return 0;
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
        if (command.batchMode()) return runBatch(command);

        const SolverConfig& settings = command.settings;

        GameApi game(settings.network, settings.model_path, settings.opening);
        const SolverOptions options = solverOptions(settings);

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
                  << "RZone active pruning: "
                  << (settings.rzone_active_pruning ? "enabled" : "disabled") << '\n'
                  << "network: " << settings.network << '\n'
                  << "model: " << settings.model_path << '\n'
                  << "SGF output: " << settings.sgf_path.string() << '\n';

        auto started = std::chrono::steady_clock::now();
        Solver solver(game, options, [&](const SearchStatistics& statistics) {
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << "progress: visited=" << statistics.combinedNodes()
                      << " player_visited=" << statistics.player_nodes
                      << " environment_visited=" << statistics.environment_nodes
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
        metadata.rzone_active_pruning = settings.rzone_active_pruning;
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
        std::cout << "player nodes visited: " << result.statistics.player_nodes << '\n'
                  << "environment nodes visited: " << result.statistics.environment_nodes << '\n'
                  << "combined nodes visited: " << result.statistics.combinedNodes() << '\n'
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
                      << rzone.full_board_hits << '\n'
                      << "RZone active-pruning AND nodes: "
                      << rzone.active_pruning_and_nodes << '\n'
                      << "RZone active-pruning certificates tested: "
                      << rzone.active_pruning_certificates_tested << '\n'
                      << "RZone active-pruning transform tests: "
                      << rzone.active_pruning_transform_tests << '\n'
                      << "RZone active-pruning children matched: "
                      << rzone.active_pruning_children_matched << '\n'
                      << "RZone active-pruning full lookups avoided: "
                      << rzone.active_pruning_full_lookups_avoided << '\n'
                      << "RZone active-pruning AND nodes closed: "
                      << rzone.active_pruning_and_nodes_closed << '\n'
                      << "RZone active-pruning universal lifts: "
                      << rzone.active_pruning_universal_lift_successes << '/'
                      << rzone.active_pruning_universal_lift_attempts << '\n';
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
