#pragma once

#include "game_api.h"

#include <array>
#include <cstdint>

namespace solver2048 {

// Universal one-step implication for non-winning completions. Lines are
// independent, but "terminal OR child" is a board-wide condition: a mismatch
// matters only when every other line also has a non-winning completion.
inline bool improvedSlideImplies(std::uint64_t parent_board, std::uint16_t parent_mask,
                                 Direction direction, unsigned winning_tile,
                                 std::uint64_t child_board, std::uint16_t child_mask)
{
    unsigned winning_rank = 0;
    for (unsigned tile = winning_tile; tile > 1; tile >>= 1) ++winning_rank;
    for (int p = 0; p < 16; ++p) {
        if ((parent_mask & (1u << p)) && ((parent_board >> (4 * p)) & 15u) >= winning_rank)
            return true; // No non-winning completion.
    }

    bool always_legal = false;
    bool nonwinning_mismatch = false;
    for (int line = 0; line < 4; ++line) {
        std::array<unsigned, 4> input{}, required{};
        unsigned known_mask = 0, required_mask = 0;
        for (int offset = 0; offset < 4; ++offset) {
            int p = 0;
            switch (direction) {
                case Direction::Left: p = 4 * line + offset; break;
                case Direction::Right: p = 4 * line + 3 - offset; break;
                case Direction::Up: p = 4 * offset + line; break;
                case Direction::Down: p = 4 * (3 - offset) + line; break;
            }
            if (parent_mask & (1u << p)) {
                known_mask |= 1u << offset;
                input[offset] = (parent_board >> (4 * p)) & 15u;
            }
            if (child_mask & (1u << p)) {
                required_mask |= 1u << offset;
                required[offset] = (child_board >> (4 * p)) & 15u;
            }
        }

        bool unchanged = false, nonwinning = false, mismatch = false;
        auto enumerate = [&](auto&& self, int offset) -> void {
            // All relevant existential facts are known; further completions
            // cannot change this line's summary.
            if (unchanged && (mismatch || required_mask == 0)) return;
            if (offset < 4) {
                if (known_mask & (1u << offset)) self(self, offset + 1);
                else for (unsigned rank = 0; rank < winning_rank; ++rank) {
                    input[offset] = rank;
                    self(self, offset + 1);
                }
                return;
            }
            std::array<unsigned, 4> compact{}, output{};
            unsigned size = 0, emitted = 0;
            for (unsigned rank : input) if (rank) compact[size++] = rank;
            for (unsigned i = 0; i < size; ++i) {
                unsigned rank = compact[i];
                if (i + 1 < size && rank == compact[i + 1]) { ++rank; ++i; }
                output[emitted++] = rank;
            }
            unchanged = unchanged || output == input;
            for (unsigned rank : output) if (rank >= winning_rank) return;
            nonwinning = true;
            for (int i = 0; i < 4; ++i)
                if ((required_mask & (1u << i)) && output[i] != required[i]) mismatch = true;
        };
        enumerate(enumerate, 0);
        if (!nonwinning) return true; // This line always wins, hence always changes.
        always_legal = always_legal || !unchanged;
        nonwinning_mismatch = nonwinning_mismatch || mismatch;
    }
    return always_legal && !nonwinning_mismatch;
}

} // namespace solver2048
