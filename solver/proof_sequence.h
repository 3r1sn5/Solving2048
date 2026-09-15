#pragma once

#include <filesystem>
#include <vector>

namespace solver2048 {

struct ProofSequenceManifest {
    std::filesystem::path tt_path;
    std::filesystem::path rz_path;
    std::vector<std::filesystem::path> sgf_paths;
};

ProofSequenceManifest readProofSequenceManifest(
    const std::filesystem::path& path);
void writeProofSequenceManifest(
    const std::filesystem::path& path,
    const ProofSequenceManifest& manifest);

} // namespace solver2048
