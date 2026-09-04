#pragma once

#include <cstdint>
#include <filesystem>

namespace solver2048 {

class Solver;

struct TtFileSummary {
    std::uint64_t player_entries = 0;
    std::uint64_t environment_entries = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t file_size = 0;

    std::uint64_t entries() const { return player_entries + environment_entries; }
};

class TtFile {
public:
    static constexpr std::uint16_t kFormatVersion = 1;
    static constexpr std::uint16_t kHeaderSize = 128;
    static constexpr std::uint16_t kRecordSize = 16;
    static constexpr std::uint16_t kWinOnlyFlag = 1u << 1;

    static TtFileSummary load(const std::filesystem::path& path, Solver& solver);
    static TtFileSummary save(const std::filesystem::path& path, const Solver& solver);
};

} // namespace solver2048
