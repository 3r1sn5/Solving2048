#pragma once

#include <cstdint>
#include <string>

namespace moporgic::solver_api {

// Read-only facade used by the standalone solver. Tile values in RawBoard are
// encoded as 4-bit base-2 exponents, exactly like moporgic::board.
using RawBoard = std::uint64_t;

void initializeNetwork(const std::string& network, const std::string& model_path);
int slide(RawBoard board, unsigned opcode, RawBoard& afterstate);
double evaluateAfterstate(RawBoard afterstate);
unsigned tileExponentAt(RawBoard board, unsigned position);

} // namespace moporgic::solver_api
