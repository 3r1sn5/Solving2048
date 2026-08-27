#include "game_api.h"

#include "config.h"
#include "../2048_api.h"

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace solver2048 {
namespace {

unsigned tileExponent(unsigned tile)
{
    if (tile == 0) return 0;
    if ((tile & (tile - 1)) != 0) throw std::invalid_argument("tile value must be a power of two");
    unsigned exponent = 0;
    while (tile > 1) {
        tile >>= 1;
        ++exponent;
    }
    if (exponent > 15) throw std::invalid_argument("tile value exceeds the 64-bit board encoding");
    return exponent;
}

Board makeBoard(const std::array<unsigned, 16>& tiles)
{
    std::uint64_t raw = 0;
    for (std::size_t position = 0; position < tiles.size(); ++position) {
        raw |= std::uint64_t(tileExponent(tiles[position])) << (position * 4);
    }
    return {raw};
}

} // namespace

GameApi::GameApi(std::string network, std::string model_path)
    : network_(std::move(network)), model_path_(std::move(model_path))
{
    if (network_ != "4x6patt") {
        throw std::invalid_argument("the first solver version supports only the 4x6patt network");
    }
    if (!std::filesystem::is_regular_file(model_path_)) {
        throw std::runtime_error("N-tuple model not found: " + model_path_);
    }

    static bool initialized = false;
    if (initialized) {
        throw std::logic_error("GameApi can only initialize the process-wide N-tuple network once");
    }

    moporgic::solver_api::initializeNetwork(network_, model_path_);
    initialized = true;
}

Board GameApi::fixedOpening() const
{
    return makeBoard(config::kFixedOpening);
}

bool GameApi::hasWinningTile(Board value, unsigned winning_tile) const
{
    const unsigned target = tileExponent(winning_tile);
    if (target == 0) throw std::invalid_argument("winning tile must be at least 2");
    for (unsigned position = 0; position < 16; ++position) {
        if (moporgic::solver_api::tileExponentAt(value.raw, position) >= target) return true;
    }
    return false;
}

std::vector<SlideResult> GameApi::legalSlides(Board value) const
{
    std::vector<SlideResult> slides;
    slides.reserve(4);
    for (unsigned opcode = 0; opcode < 4; ++opcode) {
        moporgic::solver_api::RawBoard afterstate = 0;
        const int reward = moporgic::solver_api::slide(value.raw, opcode, afterstate);
        if (reward == -1) continue;
        slides.push_back({static_cast<Direction>(opcode), {afterstate}, reward});
    }
    return slides;
}

std::vector<PlacementResult> GameApi::legalPlacements(Board value) const
{
    std::vector<PlacementResult> placements;
    placements.reserve(32);
    for (int position = 0; position < 16; ++position) {
        if (moporgic::solver_api::tileExponentAt(value.raw, static_cast<unsigned>(position)) != 0) continue;
        placements.push_back({position, 2, {value.raw | (std::uint64_t{1} << (position * 4))}});
        placements.push_back({position, 4, {value.raw | (std::uint64_t{2} << (position * 4))}});
    }
    return placements;
}

double GameApi::evaluateAfterstate(Board afterstate) const
{
    return moporgic::solver_api::evaluateAfterstate(afterstate.raw);
}

std::array<unsigned, 16> GameApi::tileValues(Board value) const
{
    std::array<unsigned, 16> tiles{};
    for (unsigned position = 0; position < tiles.size(); ++position) {
        const unsigned exponent = moporgic::solver_api::tileExponentAt(value.raw, position);
        tiles[position] = exponent == 0 ? 0 : (1u << exponent);
    }
    return tiles;
}

const char* directionName(Direction direction)
{
    switch (direction) {
        case Direction::Up: return "up";
        case Direction::Right: return "right";
        case Direction::Down: return "down";
        case Direction::Left: return "left";
    }
    return "invalid";
}

int directionOpcode(Direction direction)
{
    return static_cast<int>(direction);
}

} // namespace solver2048
