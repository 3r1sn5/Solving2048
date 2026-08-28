#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace solver2048 {

struct Board {
    std::uint64_t raw = 0;

    friend constexpr bool operator==(Board lhs, Board rhs) { return lhs.raw == rhs.raw; }
    friend constexpr bool operator!=(Board lhs, Board rhs) { return !(lhs == rhs); }
};

struct CanonicalBoard {
    Board board;
    std::uint8_t transform = 0;
};

enum class Direction : std::uint8_t {
    Up = 0,
    Right = 1,
    Down = 2,
    Left = 3,
};

struct SlideResult {
    Direction direction = Direction::Up;
    Board afterstate;
    int reward = 0;
};

struct PlacementResult {
    int position = -1;
    unsigned tile = 0;
    Board board;
};

class Game {
public:
    virtual ~Game() = default;

    virtual Board fixedOpening() const = 0;
    virtual bool hasWinningTile(Board board, unsigned winning_tile) const = 0;
    virtual std::vector<SlideResult> legalSlides(Board board) const = 0;
    virtual std::vector<PlacementResult> legalPlacements(Board afterstate) const = 0;
    virtual double evaluateAfterstate(Board afterstate) const = 0;
    virtual std::array<unsigned, 16> tileValues(Board board) const = 0;
};

class GameApi final : public Game {
public:
    GameApi(std::string network,
            std::string model_path,
            const std::array<unsigned, 16>& opening);

    Board fixedOpening() const override;
    bool hasWinningTile(Board board, unsigned winning_tile) const override;
    std::vector<SlideResult> legalSlides(Board board) const override;
    std::vector<PlacementResult> legalPlacements(Board afterstate) const override;
    double evaluateAfterstate(Board afterstate) const override;
    std::array<unsigned, 16> tileValues(Board board) const override;

    const std::string& network() const { return network_; }
    const std::string& modelPath() const { return model_path_; }

private:
    std::string network_;
    std::string model_path_;
    Board opening_;
};

const char* directionName(Direction direction);
int directionOpcode(Direction direction);
Board transformBoard(Board board, std::uint8_t transform);
int transformPosition(int position, std::uint8_t transform);
Direction transformDirection(Direction direction, std::uint8_t transform);
std::uint8_t inverseTransform(std::uint8_t transform);
CanonicalBoard canonicalize(Board board);
const char* symmetryTransformName(std::uint8_t transform);

} // namespace solver2048
