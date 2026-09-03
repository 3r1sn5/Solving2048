#pragma once

#include "rzone.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace solver2048 {

struct SolverConfig {
    unsigned winning_tile = 0;
    std::uint64_t node_limit = 0;
    std::uint64_t progress_interval = 0;
    bool use_transposition_table = false;
    bool use_tt_symmetry = false;
    bool load_tt = false;
    bool save_tt = false;
    std::filesystem::path tt_path;
    RZoneMode rzone_mode = RZoneMode::Disabled;
    RZoneLookupPolicy rzone_lookup_policy = RZoneLookupPolicy::FullScan;
    unsigned rzone_max_cells = 16;
    bool rzone_shadow_exact_check = false;
    bool load_rz = false;
    bool save_rz = false;
    std::filesystem::path rz_path;
    std::array<unsigned, 16> opening{};
    std::string network;
    std::string model_path;
    std::filesystem::path sgf_path;
};

namespace config {

inline constexpr const char* kDefaultConfigPath = "solver/solver.cfg";

} // namespace config

SolverConfig loadSolverConfig(const std::filesystem::path& path);
void validateSolverConfig(const SolverConfig& config);
std::string formatOpening(const std::array<unsigned, 16>& opening);

} // namespace solver2048
