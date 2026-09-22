#include "proof_checker.h"
#include "proof_sequence.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct CommandLine {
    std::vector<std::filesystem::path> sgf_paths;
    std::filesystem::path sequence_path;
    std::filesystem::path tt_path;
    std::filesystem::path rz_path;
    bool help = false;
};

CommandLine parseCommandLine(int argc, char** argv)
{
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "-h" || option == "--help") {
            command.help = true;
            return command;
        }
        if (option != "--sgf" && option != "--sequence" &&
            option != "--tt" && option != "--rz") {
            throw std::invalid_argument("unknown option: " + option);
        }
        if (++index >= argc) throw std::invalid_argument("missing value after " + option);
        if (option == "--sgf") {
            command.sgf_paths.emplace_back(argv[index]);
        } else if (option == "--sequence") {
            if (!command.sequence_path.empty()) {
                throw std::invalid_argument("--sequence may only be specified once");
            }
            command.sequence_path = argv[index];
        } else if (option == "--tt") {
            if (!command.tt_path.empty()) throw std::invalid_argument("--tt may only be specified once");
            command.tt_path = argv[index];
        } else {
            if (!command.rz_path.empty()) throw std::invalid_argument("--rz may only be specified once");
            command.rz_path = argv[index];
        }
    }
    if (!command.sequence_path.empty()) {
        if (!command.sgf_paths.empty() || !command.tt_path.empty() ||
            !command.rz_path.empty()) {
            throw std::invalid_argument(
                "--sequence cannot be combined with --sgf, --tt, or --rz");
        }
        const solver2048::ProofSequenceManifest manifest =
            solver2048::readProofSequenceManifest(command.sequence_path);
        command.sgf_paths = manifest.sgf_paths;
        command.tt_path = manifest.tt_path;
        command.rz_path = manifest.rz_path;
    }
    if (command.sgf_paths.empty()) {
        throw std::invalid_argument("--sgf or --sequence is required");
    }
    return command;
}

void printHelp(const char* program)
{
    std::cout
        << "Usage: " << program
        << " --sgf PATH [--sgf PATH ...] [--tt FINAL_PATH] [--rz FINAL_PATH]\n"
        << "       " << program << " --sequence PATH\n\n"
        << "Verify the exact root Win/Loss proof declared by a solver SGF against a\n"
        << "persistent TT snapshot and, when bound by the SGF, an RZone snapshot.\n"
        << "The checker audits every retained SGF DFS node, independently replays\n"
        << "actions and AND/OR backup rules, builds a verified SGF source table,\n"
        << "and cross-checks every TT/RZone record without replaying a second DAG.\n"
        << "When --sgf is repeated, paths must be in chronological solve order.\n"
        << "--tt may be omitted for a single SGF with transposition tables disabled.\n";
}

const char* proofOutcomeName(solver2048::Outcome outcome)
{
    if (outcome == solver2048::Outcome::Win) return "WIN";
    if (outcome == solver2048::Outcome::Loss) return "LOSS";
    return "UNKNOWN";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const CommandLine command = parseCommandLine(argc, argv);
        if (command.help) {
            printHelp(argv[0]);
            return 0;
        }

        std::optional<solver2048::ProofManifest> single_manifest;
        if (command.sgf_paths.size() == 1) {
            single_manifest = solver2048::readProofManifest(command.sgf_paths.front());
            if (single_manifest->hasRzArtifactBinding() && command.rz_path.empty()) {
                throw std::invalid_argument(
                    "--rz is required because the SGF binds an RZone prefix");
            }
        }

        const auto load_started = std::chrono::steady_clock::now();
        if (command.tt_path.empty() &&
            (!single_manifest || single_manifest->transposition_table)) {
            throw std::invalid_argument("--tt is required for TT-enabled or multi-SGF proofs");
        }
        const solver2048::ProofTable table = command.tt_path.empty()
            ? solver2048::ProofTable::empty(single_manifest->winning_tile)
            : solver2048::ProofTable::load(command.tt_path);
        const double tt_load_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - load_started).count();
        std::optional<solver2048::RzProofTable> rzone_table;
        double rz_load_elapsed = 0;
        if (!command.rz_path.empty()) {
            const auto rz_load_started = std::chrono::steady_clock::now();
            rzone_table = solver2048::RzProofTable::load(command.rz_path);
            rz_load_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - rz_load_started).count();
        }

        const auto verify_started = std::chrono::steady_clock::now();
        const solver2048::RzProofTable* rzone = rzone_table ? &*rzone_table : nullptr;
        solver2048::ProofSequenceVerificationResult sequence;
        if (command.sgf_paths.size() == 1) {
            sequence.openings.push_back(solver2048::verifySgfProof(
                command.sgf_paths.front(), *single_manifest, table, rzone));
            sequence.aggregate_statistics = sequence.openings.front().statistics;
        } else {
            sequence = solver2048::verifySgfProofSequence(
                command.sgf_paths, table, rzone);
        }
        const double verify_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - verify_started).count();

        const auto& result = sequence.openings.back();
        const auto& trace = sequence.aggregate_statistics;
        std::cout << "TT integrity: " << (command.tt_path.empty() ? "NOT SUPPLIED" : "PASS") << '\n'
                  << "RZone integrity: " << (rzone_table ? "PASS" : "NOT SUPPLIED") << '\n'
                  << "SGF root manifest: PASS\n"
                  << "Artifact binding: "
                  << (command.tt_path.empty() ? "NOT APPLICABLE (TT disabled)" :
                      result.artifact_binding_verified ? "PASS" : "UNAVAILABLE (legacy/unbound SGF)")
                  << '\n'
                  << "RZone prefix binding: "
                  << (result.rzone_artifact_binding_verified
                          ? "PASS"
                          : "UNAVAILABLE (unbound SGF)")
                  << '\n'
                  << "AND/OR root proof: PASS\n"
                  << "Complete SGF trace audit: PASS\n"
                  << "Root outcome: " << proofOutcomeName(result.root_outcome) << '\n'
                  << "Proof status: VERIFIED\n"
                  << "Verification model: SGF_SOURCE_TABLE\n"
                  << "SGFs verified: " << sequence.openings.size() << '\n';
        if (sequence.openings.size() > 1) {
            for (std::size_t index = 0; index < sequence.openings.size(); ++index) {
                const auto& opening = sequence.openings[index];
                std::cout << "Opening " << index + 1 << ": outcome="
                          << proofOutcomeName(opening.root_outcome)
                          << " trace_nodes=" << opening.statistics.nodes_checked
                          << " new_exact_sources="
                          << opening.statistics.verified_exact_states
                          << " cumulative_tt_records="
                          << opening.statistics.tt_records_cross_checked
                          << " cumulative_rzone_records="
                          << opening.statistics.rzone_records_cross_checked << '\n';
            }
        }
        std::cout
                  << "Verified exact source nodes: "
                  << trace.verified_exact_source_nodes << '\n'
                  << "Verified exact states: " << trace.verified_exact_states << '\n'
                  << "TT source references checked: "
                  << trace.tt_source_references_checked << '\n'
                  << "TT records cross-checked: " << trace.tt_records_cross_checked << '\n'
                  << "RZone source annotations checked: "
                  << trace.rzone_source_annotations_checked << '\n'
                  << "RZone records cross-checked: "
                  << trace.rzone_records_cross_checked << '\n'
                  << "Trace nodes checked: " << trace.nodes_checked << '\n'
                  << "Trace actions replayed: " << trace.actions_replayed << '\n'
                  << "Trace terminal nodes checked: " << trace.terminal_nodes_checked << '\n'
                  << "Trace backpropagation nodes checked: "
                  << trace.backpropagation_nodes_checked << '\n'
                  << "Trace TT leaves checked: " << trace.tt_leaves_checked << '\n'
                  << "Trace RZone leaves checked: " << trace.rzone_leaves_checked << '\n'
                  << "Trace RZone annotations checked: "
                  << trace.rzone_annotations_checked << '\n'
                  << "Trace node-limit leaves checked: " << trace.node_limit_leaves_checked << '\n'
                  << "Trace skipped actions checked: " << trace.skipped_actions_checked << '\n'
                  << "Maximum trace depth: " << trace.maximum_depth << '\n'
                  << std::fixed << std::setprecision(6)
                  << "TT load elapsed: " << tt_load_elapsed << "s\n"
                  << "RZone load elapsed: " << rz_load_elapsed << "s\n"
                  << "Proof + trace verify elapsed: " << verify_elapsed << "s\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Proof status: FAILED\nError: " << error.what() << '\n';
        return 1;
    }
}
