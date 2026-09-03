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
    bool load_tt = false;
    bool save_tt = false;
    std::filesystem::path tt_path;
    std::optional<std::uint16_t> tt_format_version;
    std::optional<std::uint64_t> tt_payload_checksum;
    std::optional<std::uint64_t> tt_file_size;
    std::optional<std::uint64_t> tt_player_records;
    std::optional<std::uint64_t> tt_environment_records;
    std::filesystem::path config_path;
    std::array<unsigned, 16> opening{};
    std::string network;
    std::string model_path;
    double elapsed_seconds = 0;
    SearchStatistics statistics;
};

class SgfWriter {
public:
    static void write(const std::filesystem::path& output_path,
                      const TraceNode& root,
                      const SgfMetadata& metadata);
    static std::string escapeValue(const std::string& value);
};

} // namespace solver2048
