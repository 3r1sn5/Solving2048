#include "sgf.h"
#include "config.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace solver2048 {
namespace {

std::size_t expandedChildren(const TraceNode& node)
{
    std::size_t count = 0;
    for (const auto& child : node.children) {
        if (child->expanded) ++count;
    }
    return count;
}

std::string nodeComment(const TraceNode& node, const SgfMetadata* metadata)
{
    std::ostringstream comment;
    comment << std::setprecision(12);
    comment << "solver_status: " << outcomeName(node.outcome) << '\n'
            << "solved_by = " << solvedByName(node.solved_by) << '\n'
            << "node_id = " << node.trace_id << '\n'
            << "dfs_visit_order = " << node.trace_id << '\n'
            << "node_type = " << phaseName(node.phase) << '\n'
            << "expanded = " << (node.expanded ? "true" : "false") << '\n'
            << "dfs_order = " << node.dfs_order << '\n'
            << "children_total = " << node.children_total << '\n'
            << "children_visited = " << node.children.size() << '\n'
            << "children_expanded = " << expandedChildren(node) << '\n'
            << "children_skipped = " << node.skipped_actions.size() << '\n';

    if (node.tt_hit) {
        comment << "tt_hit = true\n";
        comment << "tt_source = " << (node.tt_persistent_hit ? "persistent" : "trace") << '\n';
        comment << "tt_phase = " << phaseName(node.phase) << '\n';
        if (node.tt_key) {
            comment << "tt_key = 0x" << std::hex << std::setw(16) << std::setfill('0')
                    << *node.tt_key << std::dec << std::setfill(' ') << '\n';
        }
        comment << "tt_symmetry_hit = " << (node.tt_symmetry_hit ? "true" : "false") << '\n';
        if (node.tt_transform) {
            comment << "tt_transform = " << static_cast<unsigned>(*node.tt_transform)
                    << ':' << symmetryTransformName(*node.tt_transform) << '\n';
        }
        if (node.tt_source_trace_id) {
            comment << "tt_source_node_id = " << *node.tt_source_trace_id << '\n';
        }
    }

    if (node.rzone_certificate_id) {
        comment << "rzone_hit = " << (node.rzone_hit ? "true" : "false") << '\n'
                << "rzone_symmetry_hit = "
                << (node.rzone_symmetry_hit ? "true" : "false") << '\n'
                << "rzone_certificate_id = " << *node.rzone_certificate_id << '\n';
        const std::uint8_t transform = node.rzone_transform.value_or(0);
        comment << "rzone_transform = " << static_cast<unsigned>(transform)
                << ':' << symmetryTransformName(transform) << '\n';
        if (node.rzone_kind) {
            comment << "rzone_kind = " << rzoneStepKindName(*node.rzone_kind) << '\n';
        }
        if (node.rzone_mask) {
            comment << "rzone_mask = 0x" << std::hex << std::setw(4) << std::setfill('0')
                    << *node.rzone_mask << std::dec << std::setfill(' ') << '\n';
        }
        if (node.rzone_masked_board) {
            comment << "rzone_masked_board = 0x" << std::hex << std::setw(16)
                    << std::setfill('0') << *node.rzone_masked_board << std::dec
                    << std::setfill(' ') << '\n';
        }
        if (node.rzone_depth) comment << "rzone_depth = " << *node.rzone_depth << '\n';
    }

    if (!node.skipped_actions.empty()) {
        comment << "children_skipped_actions = ";
        for (std::size_t index = 0; index < node.skipped_actions.size(); ++index) {
            if (index != 0) comment << ' ';
            comment << node.skipped_actions[index].summaryToken();
        }
        comment << '\n';
    }
    comment << "terminal_reason = " << terminalReasonName(node.terminal_reason);
    if (node.reward) comment << '\n' << "reward = " << *node.reward;
    if (node.afterstate_value) {
        comment << '\n' << "afterstate_value = " << *node.afterstate_value;
    }
    if (node.action_value) comment << '\n' << "action_value = " << *node.action_value;
    if (node.selected_action) {
        comment << '\n' << "selected_action = " << node.selected_action->summaryToken();
    }
    if (node.selected_action_value) {
        comment << '\n' << "selected_action_value = " << *node.selected_action_value;
    }

    if (metadata) {
        comment << '\n' << "winning_tile = " << metadata->winning_tile
                << '\n' << "node_limit = " << metadata->node_limit
                << '\n' << "transposition_table = "
                << (metadata->use_transposition_table ? "enabled" : "disabled")
                << '\n' << "tt_symmetry = "
                << (metadata->use_tt_symmetry ? "enabled" : "disabled")
                << '\n' << "rzone_mode = " << rzoneModeName(metadata->rzone_mode)
                << '\n' << "rzone_lookup_policy = "
                << rzoneLookupPolicyName(metadata->rzone_lookup_policy)
                << '\n' << "rzone_symmetry = "
                << (metadata->use_rzone_symmetry ? "enabled" : "disabled")
                << '\n' << "rzone_max_cells = " << metadata->rzone_max_cells
                << '\n' << "rzone_shadow_exact_check = "
                << (metadata->rzone_shadow_exact_check ? "enabled" : "disabled")
                << '\n' << "persistent_tt_load = "
                << (metadata->load_tt ? "enabled" : "disabled")
                << '\n' << "persistent_tt_save = "
                << (metadata->save_tt ? "enabled" : "disabled")
                << '\n' << "tt_file = " << metadata->tt_path.string()
                << '\n' << "persistent_rz_load = "
                << (metadata->load_rz ? "enabled" : "disabled")
                << '\n' << "persistent_rz_save = "
                << (metadata->save_rz ? "enabled" : "disabled")
                << '\n' << "rz_file = " << metadata->rz_path.string()
                << '\n' << "config_file = " << metadata->config_path.string()
                << '\n' << "network = " << metadata->network
                << '\n' << "model = " << metadata->model_path
                << '\n' << "fixed_opening = " << formatOpening(metadata->opening)
                << '\n' << "elapsed_seconds = " << metadata->elapsed_seconds
                << '\n' << "player_nodes = " << metadata->statistics.player_nodes
                << '\n' << "environment_nodes = " << metadata->statistics.environment_nodes
                << '\n' << "player_actions = " << metadata->statistics.player_actions
                << '\n' << "placement_actions = " << metadata->statistics.placement_actions
                << '\n' << "node_limit_cutoffs = " << metadata->statistics.node_limit_cutoffs
                << '\n' << "player_tt_hits = " << metadata->statistics.player_tt_hits
                << '\n' << "environment_tt_hits = " << metadata->statistics.environment_tt_hits
                << '\n' << "player_tt_exact_hits = " << metadata->statistics.playerTtExactHits()
                << '\n' << "environment_tt_exact_hits = "
                << metadata->statistics.environmentTtExactHits()
                << '\n' << "player_tt_symmetry_hits = "
                << metadata->statistics.player_tt_symmetry_hits
                << '\n' << "environment_tt_symmetry_hits = "
                << metadata->statistics.environment_tt_symmetry_hits
                << '\n' << "player_persistent_tt_hits = "
                << metadata->statistics.player_persistent_tt_hits
                << '\n' << "environment_persistent_tt_hits = "
                << metadata->statistics.environment_persistent_tt_hits
                << '\n' << "player_tt_loaded_entries = "
                << metadata->statistics.player_tt_loaded_entries
                << '\n' << "environment_tt_loaded_entries = "
                << metadata->statistics.environment_tt_loaded_entries
                << '\n' << "player_tt_new_entries = " << metadata->statistics.player_tt_stores
                << '\n' << "environment_tt_new_entries = "
                << metadata->statistics.environment_tt_stores
                << '\n' << "player_tt_entries = " << metadata->statistics.playerTtEntries()
                << '\n' << "environment_tt_entries = "
                << metadata->statistics.environmentTtEntries()
                << '\n' << "rzone_loaded_certificates = "
                << metadata->statistics.rzone.loaded_certificates
                << '\n' << "rzone_patterns_stored = "
                << metadata->statistics.rzone.patterns_stored
                << '\n' << "rzone_duplicate_patterns = "
                << metadata->statistics.rzone.duplicate_patterns
                << '\n' << "rzone_terminal_certificates = "
                << metadata->statistics.rzone.terminal_certificates
                << '\n' << "rzone_slide_lift_attempts = "
                << metadata->statistics.rzone.slide_lift_attempts
                << '\n' << "rzone_slide_lift_successes = "
                << metadata->statistics.rzone.slide_lift_successes
                << '\n' << "rzone_placement_lift_attempts = "
                << metadata->statistics.rzone.placement_lift_attempts
                << '\n' << "rzone_placement_lift_successes = "
                << metadata->statistics.rzone.placement_lift_successes
                << '\n' << "rzone_lookups = " << metadata->statistics.rzone.lookups
                << '\n' << "rzone_masks_indexed = "
                << metadata->statistics.rzone.masks_indexed
                << '\n' << "rzone_lookup_masks_tested = "
                << metadata->statistics.rzone.lookup_masks_tested
                << '\n' << "rzone_lookup_candidates = "
                << metadata->statistics.rzone.lookup_candidates
                << '\n' << "rzone_pattern_matches = "
                << metadata->statistics.rzone.pattern_matches
                << '\n' << "rzone_lookup_early_exits = "
                << metadata->statistics.rzone.lookup_early_exits
                << '\n' << "rzone_player_hits = " << metadata->statistics.rzone.player_hits
                << '\n' << "rzone_environment_hits = "
                << metadata->statistics.rzone.environment_hits
                << '\n' << "rzone_exact_orientation_hits = "
                << metadata->statistics.rzone.exact_orientation_hits
                << '\n' << "rzone_symmetry_hits = "
                << metadata->statistics.rzone.symmetry_hits
                << '\n' << "rzone_lookup_transforms_tested = "
                << metadata->statistics.rzone.lookup_transforms_tested
                << '\n' << "rzone_partial_hits = " << metadata->statistics.rzone.partial_hits
                << '\n' << "rzone_full_board_hits = "
                << metadata->statistics.rzone.full_board_hits;
        if (metadata->tt_format_version) {
            comment << '\n' << "tt_format_version = " << *metadata->tt_format_version;
        }
        if (metadata->tt_payload_checksum) {
            comment << '\n' << "tt_payload_checksum = 0x" << std::hex << std::setw(16)
                    << std::setfill('0') << *metadata->tt_payload_checksum << std::dec
                    << std::setfill(' ');
        }
        if (metadata->tt_file_size) {
            comment << '\n' << "tt_file_size = " << *metadata->tt_file_size;
        }
        if (metadata->tt_player_records) {
            comment << '\n' << "tt_player_records = " << *metadata->tt_player_records;
        }
        if (metadata->tt_environment_records) {
            comment << '\n' << "tt_environment_records = " << *metadata->tt_environment_records;
        }
        if (metadata->rz_format_version) {
            comment << '\n' << "rz_format_version = " << *metadata->rz_format_version;
        }
        if (metadata->rz_semantics_version) {
            comment << '\n' << "rz_semantics_version = " << *metadata->rz_semantics_version;
        }
        if (metadata->rz_prefix_record_count) {
            comment << '\n' << "rz_prefix_record_count = "
                    << *metadata->rz_prefix_record_count;
        }
        if (metadata->rz_prefix_checksum) {
            comment << '\n' << "rz_prefix_checksum = 0x" << std::hex << std::setw(16)
                    << std::setfill('0') << *metadata->rz_prefix_checksum << std::dec
                    << std::setfill(' ');
        }
    }
    return comment.str();
}

void writeNode(std::ostream& output, const TraceNode& node)
{
    if (!node.incoming_action) throw std::logic_error("non-root SGF node is missing its incoming action");
    const SearchAction& action = *node.incoming_action;
    output << ';' << action.sgfPlayer() << '[' << action.sgfId() << ']'
           << "C[" << SgfWriter::escapeValue(nodeComment(node, nullptr)) << ']';
    for (const auto& child : node.children) {
        output << '(';
        writeNode(output, *child);
        output << ')';
    }
}

} // namespace

std::string SgfWriter::escapeValue(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        if (character == '\\' || character == ']') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

void SgfWriter::write(const std::filesystem::path& output_path,
                      const TraceNode& root,
                      const SgfMetadata& metadata)
{
    if (output_path.empty()) throw std::invalid_argument("SGF output path cannot be empty");
    std::vector<SearchAction> opening_actions;
    for (std::size_t position = 0; position < metadata.opening.size(); ++position) {
        const unsigned tile = metadata.opening[position];
        if (tile == 0) continue;
        opening_actions.push_back(SearchAction::placement(static_cast<int>(position), tile));
    }
    if (opening_actions.size() != 2) {
        throw std::invalid_argument("SGF fixed opening requires exactly two tile placements");
    }
    if (!output_path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(output_path.parent_path(), directory_error);
        if (directory_error) {
            throw std::runtime_error("cannot create SGF output directory: " + directory_error.message());
        }
    }

    std::filesystem::path temporary_path = output_path;
    temporary_path += ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output) throw std::runtime_error("cannot open temporary SGF file: " + temporary_path.string());

        output << "(;FF[4]CA[UTF-8]SZ[4]AP[Solving2048:1]";
        for (std::size_t index = 0; index < opening_actions.size(); ++index) {
            output << ";W[" << opening_actions[index].sgfId() << ']';
            if (index + 1 == opening_actions.size()) {
                output << "C[" << escapeValue(nodeComment(root, &metadata)) << ']';
            }
        }
        for (const auto& child : root.children) {
            output << '(';
            writeNode(output, *child);
            output << ')';
        }
        output << ")\n";
        output.flush();
        if (!output) throw std::runtime_error("failed while writing SGF file: " + temporary_path.string());
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, output_path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw std::runtime_error("cannot publish SGF file: " + rename_error.message());
    }
}

} // namespace solver2048
