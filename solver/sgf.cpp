#include "sgf.h"

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
        if (node.tt_source_trace_id) {
            comment << "tt_source_node_id = " << *node.tt_source_trace_id << '\n';
        }
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
    if (node.value) comment << '\n' << "v = " << *node.value;
    if (node.selected_action) {
        comment << '\n' << "selected_action = " << node.selected_action->summaryToken();
    }
    if (node.selected_value) comment << '\n' << "selected_value = " << *node.selected_value;

    if (metadata) {
        comment << '\n' << "winning_tile = " << metadata->winning_tile
                << '\n' << "node_limit = " << metadata->node_limit
                << '\n' << "transposition_table = "
                << (metadata->use_transposition_table ? "enabled" : "disabled")
                << '\n' << "network = " << metadata->network
                << '\n' << "model = " << metadata->model_path
                << '\n' << "fixed_opening = 2@0 2@1"
                << '\n' << "elapsed_seconds = " << metadata->elapsed_seconds
                << '\n' << "player_nodes = " << metadata->statistics.player_nodes
                << '\n' << "environment_nodes = " << metadata->statistics.environment_nodes
                << '\n' << "player_actions = " << metadata->statistics.player_actions
                << '\n' << "placement_actions = " << metadata->statistics.placement_actions
                << '\n' << "node_limit_cutoffs = " << metadata->statistics.node_limit_cutoffs
                << '\n' << "player_tt_hits = " << metadata->statistics.player_tt_hits
                << '\n' << "environment_tt_hits = " << metadata->statistics.environment_tt_hits
                << '\n' << "player_tt_entries = " << metadata->statistics.player_tt_stores
                << '\n' << "environment_tt_entries = " << metadata->statistics.environment_tt_stores;
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

        output << "(;FF[4]CA[UTF-8]SZ[4]AP[Solving2048:1]"
               << ";W[4]"
               << ";W[5]C[" << escapeValue(nodeComment(root, &metadata)) << ']';
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
