#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace solver2048 {
namespace {

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    if (first == value.end()) return {};
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return std::string(first, last);
}

std::runtime_error configError(const std::filesystem::path& path,
                               std::size_t line,
                               const std::string& message)
{
    std::ostringstream detail;
    detail << path.string();
    if (line != 0) detail << ':' << line;
    detail << ": " << message;
    return std::runtime_error(detail.str());
}

std::uint64_t parseUnsigned(const std::string& value,
                            const std::filesystem::path& path,
                            std::size_t line,
                            const std::string& key)
{
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        throw configError(path, line, key + " requires a non-negative integer");
    }
    try {
        std::size_t consumed = 0;
        const std::uint64_t parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) throw std::invalid_argument("trailing data");
        return parsed;
    } catch (const std::exception&) {
        throw configError(path, line, key + " requires a non-negative 64-bit integer");
    }
}

bool parseBool(std::string value,
               const std::filesystem::path& path,
               std::size_t line,
               const std::string& key)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw configError(path, line, key + " requires true, false, 1, or 0");
}

RZoneMode parseRZoneMode(std::string value,
                         const std::filesystem::path& path,
                         std::size_t line)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "disabled") return RZoneMode::Disabled;
    if (value == "generate") return RZoneMode::Generate;
    if (value == "lookup") return RZoneMode::Lookup;
    throw configError(path, line, "rzone_mode requires disabled, generate, or lookup");
}

RZoneLookupPolicy parseRZoneLookupPolicy(std::string value,
                                         const std::filesystem::path& path,
                                         std::size_t line)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "full_scan") return RZoneLookupPolicy::FullScan;
    if (value == "smallest_first") return RZoneLookupPolicy::SmallestFirst;
    throw configError(
        path, line, "rzone_lookup_policy requires full_scan or smallest_first");
}

std::array<unsigned, 16> parseOpening(const std::string& value,
                                      const std::filesystem::path& path,
                                      std::size_t line)
{
    if (value.empty() || value.front() == ',' || value.back() == ',') {
        throw configError(path, line, "opening entries must use tile@position");
    }
    std::array<unsigned, 16> opening{};
    std::array<bool, 16> occupied{};
    std::istringstream stream(value);
    std::string placement;
    std::size_t count = 0;
    while (std::getline(stream, placement, ',')) {
        placement = trim(placement);
        const std::size_t separator = placement.find('@');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 == placement.size() || placement.find('@', separator + 1) != std::string::npos) {
            throw configError(path, line, "opening entries must use tile@position");
        }
        const std::uint64_t tile = parseUnsigned(
            trim(placement.substr(0, separator)), path, line, "opening tile");
        const std::uint64_t position = parseUnsigned(
            trim(placement.substr(separator + 1)), path, line, "opening position");
        if (position >= opening.size()) {
            throw configError(path, line, "opening position must be in [0, 15]");
        }
        if (tile != 2 && tile != 4) {
            throw configError(path, line, "first-version opening tiles must be 2 or 4");
        }
        if (occupied[position]) {
            throw configError(path, line,
                              "opening contains duplicate position " + std::to_string(position));
        }
        occupied[position] = true;
        opening[position] = static_cast<unsigned>(tile);
        ++count;
    }
    if (count != 2) {
        throw configError(path, line, "first-version fixed opening requires exactly two tiles");
    }
    return opening;
}

} // namespace

void validateSolverConfig(const SolverConfig& value)
{
    if (value.winning_tile < 2 ||
        (value.winning_tile & (value.winning_tile - 1)) != 0 ||
        value.winning_tile > 32768) {
        throw std::invalid_argument("winning_tile must be a power of two in [2, 32768]");
    }
    if (value.use_tt_symmetry && !value.use_transposition_table) {
        throw std::invalid_argument("use_tt_symmetry=true requires use_transposition_table=true");
    }
    if ((value.load_tt || value.save_tt) && !value.use_transposition_table) {
        throw std::invalid_argument("load_tt/save_tt require use_transposition_table=true");
    }
    if ((value.load_tt || value.save_tt) && value.tt_path.empty()) {
        throw std::invalid_argument("tt_path cannot be empty when persistent TT is enabled");
    }
    if (value.rzone_max_cells == 0 || value.rzone_max_cells > 16) {
        throw std::invalid_argument("rzone_max_cells must be in [1, 16]");
    }
    if (value.rzone_shadow_exact_check && value.rzone_mode != RZoneMode::Lookup) {
        throw std::invalid_argument("rzone_shadow_exact_check=true requires rzone_mode=lookup");
    }
    if (value.rzone_shadow_exact_check && !value.use_transposition_table) {
        throw std::invalid_argument("rzone_shadow_exact_check=true requires exact TT");
    }
    if (value.rzone_active_pruning && value.rzone_mode != RZoneMode::Lookup) {
        throw std::invalid_argument("rzone_active_pruning=true requires rzone_mode=lookup");
    }
    if ((value.load_rz || value.save_rz) && value.rzone_mode == RZoneMode::Disabled) {
        throw std::invalid_argument(
            "load_rz/save_rz require rzone_mode=generate or rzone_mode=lookup");
    }
    if ((value.load_rz || value.save_rz) && value.rz_path.empty()) {
        throw std::invalid_argument("rz_path cannot be empty when persistent RZone is enabled");
    }
    std::size_t opening_tiles = 0;
    for (const unsigned tile : value.opening) {
        if (tile == 0) continue;
        if (tile != 2 && tile != 4) {
            throw std::invalid_argument("first-version opening tiles must be 2 or 4");
        }
        ++opening_tiles;
    }
    if (opening_tiles != 2) {
        throw std::invalid_argument("first-version fixed opening requires exactly two tiles");
    }
    if (value.network.empty()) throw std::invalid_argument("network cannot be empty");
    if (value.model_path.empty()) throw std::invalid_argument("model_path cannot be empty");
    if (value.sgf_path.empty()) throw std::invalid_argument("sgf_path cannot be empty");
}

SolverConfig loadSolverConfig(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) throw configError(path, 0, "cannot open configuration file");

    SolverConfig result;
    std::unordered_set<std::string> seen;
    static const std::unordered_set<std::string> known = {
        "winning_tile",
        "node_limit",
        "progress_interval",
        "use_transposition_table",
        "use_tt_symmetry",
        "load_tt",
        "save_tt",
        "tt_path",
        "rzone_mode",
        "rzone_lookup_policy",
        "use_rzone_symmetry",
        "rzone_max_cells",
        "rzone_shadow_exact_check",
        "rzone_active_pruning",
        "load_rz",
        "save_rz",
        "rz_path",
        "opening",
        "network",
        "model_path",
        "sgf_path",
    };
    static const std::unordered_set<std::string> optional = {
        // Added after the original strict cfg format. Keep older experiment
        // files usable; absence means the feature is disabled.
        "rzone_active_pruning",
    };

    std::string line_text;
    std::size_t line_number = 0;
    while (std::getline(input, line_text)) {
        ++line_number;
        const std::size_t comment = line_text.find('#');
        if (comment != std::string::npos) line_text.erase(comment);
        line_text = trim(line_text);
        if (line_text.empty()) continue;

        const std::size_t separator = line_text.find('=');
        if (separator == std::string::npos || line_text.find('=', separator + 1) != std::string::npos) {
            throw configError(path, line_number, "expected exactly one key=value assignment");
        }
        const std::string key = trim(line_text.substr(0, separator));
        const std::string value = trim(line_text.substr(separator + 1));
        if (key.empty()) throw configError(path, line_number, "configuration key cannot be empty");
        if (known.count(key) == 0) {
            throw configError(path, line_number, "unknown configuration key: " + key);
        }
        if (!seen.insert(key).second) {
            throw configError(path, line_number, "duplicate configuration key: " + key);
        }

        if (key == "winning_tile") {
            const std::uint64_t parsed = parseUnsigned(value, path, line_number, key);
            if (parsed > std::numeric_limits<unsigned>::max()) {
                throw configError(path, line_number, "winning_tile exceeds unsigned range");
            }
            result.winning_tile = static_cast<unsigned>(parsed);
        } else if (key == "node_limit") {
            result.node_limit = parseUnsigned(value, path, line_number, key);
        } else if (key == "progress_interval") {
            result.progress_interval = parseUnsigned(value, path, line_number, key);
        } else if (key == "use_transposition_table") {
            result.use_transposition_table = parseBool(value, path, line_number, key);
        } else if (key == "use_tt_symmetry") {
            result.use_tt_symmetry = parseBool(value, path, line_number, key);
        } else if (key == "load_tt") {
            result.load_tt = parseBool(value, path, line_number, key);
        } else if (key == "save_tt") {
            result.save_tt = parseBool(value, path, line_number, key);
        } else if (key == "tt_path") {
            result.tt_path = value;
        } else if (key == "rzone_mode") {
            result.rzone_mode = parseRZoneMode(value, path, line_number);
        } else if (key == "rzone_lookup_policy") {
            result.rzone_lookup_policy = parseRZoneLookupPolicy(value, path, line_number);
        } else if (key == "use_rzone_symmetry") {
            result.use_rzone_symmetry = parseBool(value, path, line_number, key);
        } else if (key == "rzone_max_cells") {
            const std::uint64_t parsed = parseUnsigned(value, path, line_number, key);
            if (parsed > std::numeric_limits<unsigned>::max()) {
                throw configError(path, line_number, "rzone_max_cells exceeds unsigned range");
            }
            result.rzone_max_cells = static_cast<unsigned>(parsed);
        } else if (key == "rzone_shadow_exact_check") {
            result.rzone_shadow_exact_check = parseBool(value, path, line_number, key);
        } else if (key == "rzone_active_pruning") {
            result.rzone_active_pruning = parseBool(value, path, line_number, key);
        } else if (key == "load_rz") {
            result.load_rz = parseBool(value, path, line_number, key);
        } else if (key == "save_rz") {
            result.save_rz = parseBool(value, path, line_number, key);
        } else if (key == "rz_path") {
            result.rz_path = value;
        } else if (key == "opening") {
            result.opening = parseOpening(value, path, line_number);
        } else if (key == "network") {
            result.network = value;
        } else if (key == "model_path") {
            result.model_path = value;
        } else if (key == "sgf_path") {
            result.sgf_path = value;
        }
    }
    if (!input.eof()) throw configError(path, line_number, "failed while reading configuration file");

    for (const std::string& key : known) {
        if (optional.count(key) != 0) continue;
        if (seen.count(key) == 0) throw configError(path, 0, "missing required key: " + key);
    }
    try {
        validateSolverConfig(result);
    } catch (const std::exception& error) {
        throw configError(path, 0, error.what());
    }
    return result;
}

std::string formatOpening(const std::array<unsigned, 16>& opening)
{
    std::ostringstream formatted;
    bool first = true;
    for (std::size_t position = 0; position < opening.size(); ++position) {
        if (opening[position] == 0) continue;
        if (!first) formatted << ',';
        formatted << opening[position] << '@' << position;
        first = false;
    }
    return formatted.str();
}

} // namespace solver2048
