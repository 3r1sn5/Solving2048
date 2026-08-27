#pragma once

#include <array>
#include <cstdint>

namespace solver2048::config {

inline constexpr unsigned kDefaultWinningTile = 8;
inline constexpr std::uint64_t kDefaultNodeLimit = 100'000;
inline constexpr std::uint64_t kDefaultProgressInterval = 100'000;
// Keep the original raw DFS baseline as the default. Enable this for the exact
// DAG search experiment, or override it with --tt / --no-tt.
inline constexpr bool kDefaultUseTranspositionTable = false;
inline constexpr const char* kDefaultNetwork = "4x6patt";
inline constexpr const char* kDefaultModelPath = "models/4x6patt.w";
inline constexpr const char* kDefaultSgfPath = "solver/tree.sgf";

// Row-major tile values. The opening is complete and the root is a Player node.
inline constexpr std::array<unsigned, 16> kFixedOpening = {
    2, 2, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
};

} // namespace solver2048::config
