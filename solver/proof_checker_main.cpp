#include "proof_checker.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct CommandLine {
    std::filesystem::path sgf_path;
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
        if (option != "--sgf" && option != "--tt" && option != "--rz") {
            throw std::invalid_argument("unknown option: " + option);
        }
        if (++index >= argc) throw std::invalid_argument("missing value after " + option);
        if (option == "--sgf") {
            if (!command.sgf_path.empty()) throw std::invalid_argument("--sgf may only be specified once");
            command.sgf_path = argv[index];
        } else if (option == "--tt") {
            if (!command.tt_path.empty()) throw std::invalid_argument("--tt may only be specified once");
            command.tt_path = argv[index];
        } else {
            if (!command.rz_path.empty()) throw std::invalid_argument("--rz may only be specified once");
            command.rz_path = argv[index];
        }
    }
    if (command.sgf_path.empty()) throw std::invalid_argument("--sgf is required");
    if (command.tt_path.empty()) throw std::invalid_argument("--tt is required");
    return command;
}

void printHelp(const char* program)
{
    std::cout
        << "Usage: " << program << " --sgf PATH --tt PATH [--rz PATH]\n\n"
        << "Verify the exact root Win/Loss proof declared by a solver SGF against a\n"
        << "persistent TT snapshot and, when bound by the SGF, an RZone snapshot.\n"
        << "The checker audits every retained SGF DFS node, independently replays\n"
        << "actions and AND/OR backup rules, builds a verified SGF source table,\n"
        << "and cross-checks every TT/RZone record without replaying a second DAG.\n";
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

        const solver2048::ProofManifest manifest =
            solver2048::readProofManifest(command.sgf_path);
        if (manifest.hasRzArtifactBinding() && command.rz_path.empty()) {
            throw std::invalid_argument("--rz is required because the SGF binds an RZone prefix");
        }

        const auto load_started = std::chrono::steady_clock::now();
        const solver2048::ProofTable table = solver2048::ProofTable::load(command.tt_path);
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
        const solver2048::SgfProofVerificationResult result =
            solver2048::verifySgfProof(command.sgf_path, manifest, table, rzone);
        const double verify_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - verify_started).count();

        const auto& trace = result.statistics;
        std::cout << "TT integrity: PASS\n"
                  << "RZone integrity: " << (rzone_table ? "PASS" : "NOT SUPPLIED") << '\n'
                  << "SGF root manifest: PASS\n"
                  << "Artifact binding: "
                  << (result.artifact_binding_verified ? "PASS" : "UNAVAILABLE (legacy/unbound SGF)")
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
