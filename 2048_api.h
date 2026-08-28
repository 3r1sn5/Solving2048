#pragma once

#include <cstdint>
#include <string>

namespace moporgic::solver_api {

// Read-only facade used by the standalone solver. Tile values in RawBoard are
// encoded as 4-bit base-2 exponents, exactly like moporgic::board.
using RawBoard = std::uint64_t;

struct CanonicalBoard {
    RawBoard raw = 0;
    std::uint8_t transform = 0;
};

void initializeNetwork(const std::string& network, const std::string& model_path);
int slide(RawBoard board, unsigned opcode, RawBoard& afterstate);
double evaluateAfterstate(RawBoard afterstate);
unsigned tileExponentAt(RawBoard board, unsigned position);
RawBoard transformBoard(RawBoard board, unsigned transform);
unsigned transformPosition(unsigned position, unsigned transform);
unsigned transformDirectionOpcode(unsigned opcode, unsigned transform);
unsigned inverseTransform(unsigned transform);
CanonicalBoard canonicalize(RawBoard board);

} // namespace moporgic::solver_api
