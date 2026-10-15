#pragma once

#include "search.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace solver2048 {

struct SgfMetadata {
    unsigned winning_tile = 8;
    std::uint64_t node_limit = 0;
    bool use_transposition_table = false;
    bool use_tt_symmetry = false;
    RZoneMode rzone_mode = RZoneMode::Disabled;
    RZoneLookupPolicy rzone_lookup_policy = RZoneLookupPolicy::FullScan;
    bool use_rzone_symmetry = false;
    unsigned rzone_max_cells = 16;
    bool rzone_active_pruning = false;
    bool rzone_trimming = true;
    bool rzone_full_board_placement_fallback = true;
    bool load_tt = false;
    bool save_tt = false;
    std::filesystem::path tt_path;
    std::optional<std::uint16_t> tt_format_version;
    std::optional<std::uint64_t> tt_payload_checksum;
    std::optional<std::uint64_t> tt_file_size;
    std::optional<std::uint64_t> tt_player_records;
    std::optional<std::uint64_t> tt_environment_records;
    bool load_rz = false;
    bool save_rz = false;
    std::filesystem::path rz_path;
    std::optional<std::uint16_t> rz_format_version;
    std::optional<std::uint32_t> rz_semantics_version;
    std::optional<std::uint64_t> rz_prefix_record_count;
    std::optional<std::uint64_t> rz_prefix_checksum;
    std::filesystem::path config_path;
    std::array<unsigned, 16> opening{};
    std::string network;
    std::string model_path;
    double elapsed_seconds = 0;
    SearchStatistics statistics;
};

class SgfWriter {
public:
    // Solver results must use this overload to include the compact history.
    static void write(const std::filesystem::path& output_path,
                      const SearchResult& result,
                      const SgfMetadata& metadata);
    // For explicitly constructed tree fixtures.
    static void write(const std::filesystem::path& output_path,
                      const TraceNode& root,
                      const SgfMetadata& metadata);
    static std::string escapeValue(const std::string& value);
private:
    static void writeTrace(const std::filesystem::path& output_path,
                           const TraceNode& root,
                           const SgfMetadata& metadata,
                           const CompactTrace* compact);
};

} // namespace solver2048
