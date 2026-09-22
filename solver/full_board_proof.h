#pragma once

#include "search.h"

#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace solver2048 {

// Reader-side index of already validated Player certificates. A full-board
// Environment proof must cover every legal spawn using an earlier certificate
// within its declared prefix and with strictly smaller proof depth. No solver
// outcome, TT entry, or unvalidated record is trusted by this check.
class FullBoardProofIndex {
public:
    template<class Entry>
    void insert(const Entry& entry)
    {
        if (entry.phase != Phase::Player) return;
        auto [it, added] = patterns_.try_emplace(entry.mask);
        if (added) {
            std::uint64_t bits = 0;
            for (int cell = 0; cell < 16; ++cell) {
                if (entry.mask & (1u << cell)) bits |= std::uint64_t{15} << (4 * cell);
            }
            masks_.push_back({entry.mask, bits});
        }
        it->second[entry.masked_board].push_back({entry.id, entry.depth});
    }

    template<class Entry>
    void validate(const Entry& entry) const
    {
        if (entry.phase != Phase::Environment || entry.mask != 0xffff ||
            entry.child == 0 || entry.child >= entry.id || entry.depth < 2) {
            throw std::invalid_argument("FullBoardPlacement phase, mask, prefix, or depth is invalid");
        }
        const auto placements = generateLegalPlacements(Board{entry.masked_board});
        if (placements.empty()) {
            throw std::invalid_argument("FullBoardPlacement has no legal placements");
        }
        for (const auto& placement : placements) {
            if (!covered(placement.board, entry.child, entry.depth)) {
                throw std::invalid_argument("FullBoardPlacement has an uncovered placement");
            }
        }
    }

private:
    struct Witness { RZoneCertificateId id; unsigned depth; };
    struct Mask { std::uint16_t cells; std::uint64_t bits; };
    bool covered(Board board, RZoneCertificateId maximum_id, unsigned depth) const
    {
        for (std::uint8_t transform = 0; transform < 8; ++transform) {
            const auto candidate = transformBoard(board, transform);
            for (const auto& mask : masks_) {
                const auto& patterns = patterns_.at(mask.cells);
                const auto found = patterns.find(candidate.raw & mask.bits);
                if (found == patterns.end()) continue;
                for (const auto& witness : found->second) {
                    if (witness.id > maximum_id) break;
                    if (witness.depth < depth) return true;
                }
            }
        }
        return false;
    }
    std::vector<Mask> masks_;
    std::unordered_map<std::uint16_t,
        std::unordered_map<std::uint64_t, std::vector<Witness>>> patterns_;
};

} // namespace solver2048
