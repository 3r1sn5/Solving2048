#pragma once

#include "search.h"
#include "tt_file.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>

namespace solver2048 {

struct ProofEntry {
    Outcome outcome = Outcome::Unknown;
    std::uint8_t children_total = 0;
    std::optional<SearchAction> witness;
};

class ProofTable {
public:
    static ProofTable load(const std::filesystem::path& path);

    unsigned winningTile() const { return winning_tile_; }
    bool usesSymmetry() const { return symmetry_; }
    const TtFileSummary& summary() const { return summary_; }
    const ProofEntry* find(Phase phase, std::uint64_t board_key) const;

private:
    unsigned winning_tile_ = 0;
    bool symmetry_ = false;
    TtFileSummary summary_;
    std::unordered_map<std::uint64_t, ProofEntry> player_entries_;
    std::unordered_map<std::uint64_t, ProofEntry> environment_entries_;
};

struct ProofManifest {
    std::array<unsigned, 16> opening{};
    Board root_board;
    Outcome root_outcome = Outcome::Unknown;
    unsigned winning_tile = 0;
    std::uint64_t node_limit = 0;
    bool transposition_table = false;
    bool tt_symmetry = false;
    std::filesystem::path declared_tt_path;
    std::optional<std::uint16_t> tt_format_version;
    std::optional<std::uint64_t> tt_payload_checksum;
    std::optional<std::uint64_t> tt_file_size;
    std::optional<std::uint64_t> tt_player_records;
    std::optional<std::uint64_t> tt_environment_records;

    bool hasArtifactBinding() const;
};

ProofManifest readProofManifest(const std::filesystem::path& sgf_path);

struct ProofVerificationStatistics {
    std::uint64_t tt_records_verified = 0;
    std::uint64_t terminal_states_checked = 0;
    std::uint64_t witness_edges_checked = 0;
    std::uint64_t universal_edges_checked = 0;
    std::uint64_t memoized_verification_hits = 0;
    std::uint64_t maximum_depth = 0;
};

struct ProofVerificationResult {
    Outcome root_outcome = Outcome::Unknown;
    bool artifact_binding_verified = false;
    ProofVerificationStatistics statistics;
};

ProofVerificationResult verifyRootProof(const ProofManifest& manifest,
                                        const ProofTable& table);

struct TraceVerificationStatistics {
    std::uint64_t nodes_checked = 0;
    std::uint64_t actions_replayed = 0;
    std::uint64_t terminal_nodes_checked = 0;
    std::uint64_t backpropagation_nodes_checked = 0;
    std::uint64_t tt_leaves_checked = 0;
    std::uint64_t node_limit_leaves_checked = 0;
    std::uint64_t skipped_actions_checked = 0;
    std::uint64_t maximum_depth = 0;
};

TraceVerificationStatistics verifySgfTrace(const std::filesystem::path& sgf_path,
                                           const ProofManifest& manifest,
                                           const ProofTable& table);

} // namespace solver2048
