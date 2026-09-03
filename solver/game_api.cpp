#include "game_api.h"

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

} // namespace

Board boardFromTiles(const std::array<unsigned, 16>& tiles)
{
    std::uint64_t raw = 0;
    for (std::size_t position = 0; position < tiles.size(); ++position) {
        raw |= std::uint64_t(tileExponent(tiles[position])) << (position * 4);
    }
    return {raw};
}

bool boardHasWinningTile(Board value, unsigned winning_tile)
{
    const unsigned target = tileExponent(winning_tile);
    if (target == 0) throw std::invalid_argument("winning tile must be at least 2");
    for (unsigned position = 0; position < 16; ++position) {
        if (moporgic::solver_api::tileExponentAt(value.raw, position) >= target) return true;
    }
    return false;
}

std::vector<SlideResult> generateLegalSlides(Board value)
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

std::vector<PlacementResult> generateLegalPlacements(Board value)
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

std::array<unsigned, 16> boardTileValues(Board value)
{
    std::array<unsigned, 16> tiles{};
    for (unsigned position = 0; position < tiles.size(); ++position) {
        const unsigned exponent = moporgic::solver_api::tileExponentAt(value.raw, position);
        tiles[position] = exponent == 0 ? 0 : (1u << exponent);
    }
    return tiles;
}

GameApi::GameApi(std::string network,
                 std::string model_path,
                 const std::array<unsigned, 16>& opening)
    : network_(std::move(network)), model_path_(std::move(model_path)), opening_(boardFromTiles(opening))
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
    return opening_;
}

bool GameApi::hasWinningTile(Board value, unsigned winning_tile) const
{
    return boardHasWinningTile(value, winning_tile);
}

std::vector<SlideResult> GameApi::legalSlides(Board value) const
{
    return generateLegalSlides(value);
}

std::vector<PlacementResult> GameApi::legalPlacements(Board value) const
{
    return generateLegalPlacements(value);
}

double GameApi::evaluateAfterstate(Board afterstate) const
{
    return moporgic::solver_api::evaluateAfterstate(afterstate.raw);
}

std::array<unsigned, 16> GameApi::tileValues(Board value) const
{
    return boardTileValues(value);
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

Board transformBoard(Board value, std::uint8_t transform)
{
    return {moporgic::solver_api::transformBoard(value.raw, transform)};
}

int transformPosition(int position, std::uint8_t transform)
{
    if (position < 0 || position >= 16) {
        throw std::out_of_range("2048 board position must be in [0, 15]");
    }
    return static_cast<int>(
        moporgic::solver_api::transformPosition(static_cast<unsigned>(position), transform));
}

Direction transformDirection(Direction direction, std::uint8_t transform)
{
    const unsigned opcode = moporgic::solver_api::transformDirectionOpcode(
        static_cast<unsigned>(directionOpcode(direction)), transform);
    return static_cast<Direction>(opcode);
}

std::uint8_t inverseTransform(std::uint8_t transform)
{
    return static_cast<std::uint8_t>(moporgic::solver_api::inverseTransform(transform));
}

CanonicalBoard canonicalize(Board value)
{
    const moporgic::solver_api::CanonicalBoard canonical =
        moporgic::solver_api::canonicalize(value.raw);
    return {{canonical.raw}, canonical.transform};
}

const char* symmetryTransformName(std::uint8_t transform)
{
    static constexpr const char* names[8] = {
        "identity",
        "rotate90",
        "rotate180",
        "rotate270",
        "flip_vertical",
        "flip_vertical_rotate90",
        "flip_vertical_rotate180",
        "flip_vertical_rotate270",
    };
    if (transform >= 8) throw std::out_of_range("2048 symmetry transform must be in [0, 7]");
    return names[transform];
}

} // namespace solver2048
