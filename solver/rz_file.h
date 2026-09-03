#pragma once

#include <cstdint>
#include <filesystem>

namespace solver2048 {

class RZoneTable;
class Solver;

struct RzFileSummary {
    std::uint64_t record_count = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t file_size = 0;
};

class RzFile {
public:
    static constexpr std::uint16_t kFormatVersion = 1;
    static constexpr std::uint16_t kHeaderSize = 128;
    static constexpr std::uint16_t kRecordSize = 32;
    static constexpr std::uint32_t kSemanticsVersion = 1;

    static RzFileSummary load(const std::filesystem::path& path, RZoneTable& table);
    static RzFileSummary save(const std::filesystem::path& path, const RZoneTable& table);

    static RzFileSummary load(const std::filesystem::path& path, Solver& solver);
    static RzFileSummary save(const std::filesystem::path& path, const Solver& solver);
};

} // namespace solver2048
