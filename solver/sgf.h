#pragma once

#include "search.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace solver2048 {

struct SgfMetadata {
    unsigned winning_tile = 8;
    std::uint64_t node_limit = 0;
    bool use_transposition_table = false;
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
