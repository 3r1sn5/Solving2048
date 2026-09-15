#pragma once

#include "search.h"
#include "rz_proof_file.h"
#include "tt_file.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

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
    bool rzone_symmetry = false;
    bool persistent_tt_load = false;
    bool persistent_rz_load = false;
    std::uint64_t player_tt_loaded_entries = 0;
    std::uint64_t environment_tt_loaded_entries = 0;
    std::uint64_t rzone_loaded_certificates = 0;
    std::filesystem::path declared_tt_path;
    std::optional<std::uint16_t> tt_format_version;
    std::optional<std::uint64_t> tt_payload_checksum;
    std::optional<std::uint64_t> tt_file_size;
    std::optional<std::uint64_t> tt_player_records;
    std::optional<std::uint64_t> tt_environment_records;
    std::filesystem::path declared_rz_path;
    std::optional<std::uint16_t> rz_format_version;
    std::optional<std::uint32_t> rz_semantics_version;
    std::optional<std::uint64_t> rz_prefix_record_count;
    std::optional<std::uint64_t> rz_prefix_checksum;

    bool hasArtifactBinding() const;
    bool hasRzArtifactBinding() const;
};

ProofManifest readProofManifest(const std::filesystem::path& sgf_path);

struct ProofVerificationStatistics {
    std::uint64_t tt_records_verified = 0;
    std::uint64_t terminal_states_checked = 0;
    std::uint64_t witness_edges_checked = 0;
    std::uint64_t universal_edges_checked = 0;
    std::uint64_t rzone_certificates_used = 0;
    std::uint64_t memoized_verification_hits = 0;
    std::uint64_t maximum_depth = 0;
};

struct ProofVerificationResult {
    Outcome root_outcome = Outcome::Unknown;
    bool artifact_binding_verified = false;
    bool rzone_artifact_binding_verified = false;
    ProofVerificationStatistics statistics;
};

ProofVerificationResult verifyRootProof(
    const ProofManifest& manifest,
    const ProofTable& table,
    const RzProofTable* rzone_table = nullptr);

struct TraceVerificationStatistics {
    std::uint64_t nodes_checked = 0;
    std::uint64_t actions_replayed = 0;
    std::uint64_t terminal_nodes_checked = 0;
    std::uint64_t backpropagation_nodes_checked = 0;
    std::uint64_t tt_leaves_checked = 0;
    std::uint64_t rzone_leaves_checked = 0;
    std::uint64_t rzone_annotations_checked = 0;
    std::uint64_t node_limit_leaves_checked = 0;
    std::uint64_t skipped_actions_checked = 0;
    std::uint64_t maximum_depth = 0;
    std::uint64_t verified_exact_source_nodes = 0;
    std::uint64_t verified_exact_states = 0;
    std::uint64_t tt_source_references_checked = 0;
    std::uint64_t tt_records_cross_checked = 0;
    std::uint64_t rzone_source_annotations_checked = 0;
    std::uint64_t rzone_records_cross_checked = 0;
};

struct SgfProofVerificationResult {
    Outcome root_outcome = Outcome::Unknown;
    bool artifact_binding_verified = false;
    bool rzone_artifact_binding_verified = false;
    TraceVerificationStatistics statistics;
};

struct ProofSequenceVerificationResult {
    std::vector<SgfProofVerificationResult> openings;
    TraceVerificationStatistics aggregate_statistics;
};

SgfProofVerificationResult verifySgfProof(
    const std::filesystem::path& sgf_path,
    const ProofManifest& manifest,
    const ProofTable& table,
    const RzProofTable* rzone_table = nullptr);

ProofSequenceVerificationResult verifySgfProofSequence(
    const std::vector<std::filesystem::path>& sgf_paths,
    const ProofTable& table,
    const RzProofTable* rzone_table = nullptr);

TraceVerificationStatistics verifySgfTrace(const std::filesystem::path& sgf_path,
                                           const ProofManifest& manifest,
                                           const ProofTable& table,
                                           const RzProofTable* rzone_table = nullptr);

} // namespace solver2048
