#include "proof_sequence.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>

namespace solver2048 {
namespace {

std::string trim(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        });
    if (first == value.end()) return {};
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), [](unsigned char character) {
            return std::isspace(character) != 0;
        }).base();
    return std::string(first, last);
}

[[noreturn]] void fail(const std::filesystem::path& path,
                       std::size_t line,
                       const std::string& message)
{
    std::string detail = path.string();
    if (line != 0) detail += ':' + std::to_string(line);
    throw std::runtime_error(detail + ": " + message);
}

std::filesystem::path resolve(const std::filesystem::path& base,
                              const std::string& value)
{
    const std::filesystem::path parsed(value);
    return parsed.is_absolute() ? parsed : (base / parsed).lexically_normal();
}

std::filesystem::path relativeForManifest(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& artifact_path)
{
    const std::filesystem::path base =
        std::filesystem::absolute(manifest_path).parent_path().lexically_normal();
    const std::filesystem::path absolute_artifact =
        std::filesystem::absolute(artifact_path).lexically_normal();
    const std::filesystem::path relative = absolute_artifact.lexically_relative(base);
    return relative.empty() ? artifact_path : relative;
}

} // namespace

ProofSequenceManifest readProofSequenceManifest(
    const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) fail(path, 0, "cannot open proof sequence manifest");

    ProofSequenceManifest result;
    bool has_version = false;
    bool has_tt = false;
    bool has_rz = false;
    std::unordered_set<std::string> sgf_paths;
    const std::filesystem::path base =
        std::filesystem::absolute(path).parent_path().lexically_normal();
    std::string line_text;
    std::size_t line_number = 0;
    while (std::getline(input, line_text)) {
        ++line_number;
        const std::size_t comment = line_text.find('#');
        if (comment != std::string::npos) line_text.erase(comment);
        line_text = trim(line_text);
        if (line_text.empty()) continue;
        const std::size_t separator = line_text.find('=');
        if (separator == std::string::npos) {
            fail(path, line_number, "expected key=value");
        }
        const std::string key = trim(line_text.substr(0, separator));
        const std::string value = trim(line_text.substr(separator + 1));
        if (value.empty()) fail(path, line_number, key + " cannot be empty");

        if (key == "format_version") {
            if (has_version) fail(path, line_number, "duplicate format_version");
            if (value != "1") fail(path, line_number, "unsupported format_version");
            has_version = true;
        } else if (key == "tt") {
            if (has_tt) fail(path, line_number, "duplicate tt path");
            result.tt_path = resolve(base, value);
            has_tt = true;
        } else if (key == "rz") {
            if (has_rz) fail(path, line_number, "duplicate rz path");
            result.rz_path = resolve(base, value);
            has_rz = true;
        } else if (key == "sgf") {
            const std::filesystem::path sgf = resolve(base, value);
            const std::string normalized =
                std::filesystem::absolute(sgf).lexically_normal().string();
            if (!sgf_paths.insert(normalized).second) {
                fail(path, line_number, "duplicate sgf path");
            }
            result.sgf_paths.push_back(sgf);
        } else {
            fail(path, line_number, "unknown key: " + key);
        }
    }
    if (!input.eof()) fail(path, line_number, "failed while reading manifest");
    if (!has_version) fail(path, 0, "missing format_version");
    if (!has_tt) fail(path, 0, "missing tt path");
    if (result.sgf_paths.empty()) fail(path, 0, "manifest contains no sgf paths");
    return result;
}

void writeProofSequenceManifest(
    const std::filesystem::path& path,
    const ProofSequenceManifest& manifest)
{
    if (path.empty() || manifest.tt_path.empty() || manifest.sgf_paths.empty()) {
        throw std::invalid_argument(
            "proof sequence output, TT, and SGF paths must not be empty");
    }
    if (!path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            fail(path, 0, "cannot create output directory: " + directory_error.message());
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output) fail(temporary, 0, "cannot open temporary manifest");
        output << "format_version=1\n"
               << "tt=" << relativeForManifest(path, manifest.tt_path).string() << '\n';
        if (!manifest.rz_path.empty()) {
            output << "rz=" << relativeForManifest(path, manifest.rz_path).string() << '\n';
        }
        for (const auto& sgf : manifest.sgf_paths) {
            output << "sgf=" << relativeForManifest(path, sgf).string() << '\n';
        }
        output.flush();
        if (!output) fail(temporary, 0, "failed while writing manifest");
        output.close();
        if (!output) fail(temporary, 0, "failed while closing manifest");

        std::error_code rename_error;
        std::filesystem::rename(temporary, path, rename_error);
        if (rename_error) {
            fail(path, 0, "cannot publish manifest: " + rename_error.message());
        }
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw;
    }
}

} // namespace solver2048
