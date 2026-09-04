#include "rzone.h"

#include "search.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace solver2048 {
namespace {

constexpr int kBoardSize = 4;
constexpr int kBoardCells = 16;

unsigned cellRank(Board board, int position)
{
    return static_cast<unsigned>((board.raw >> (position * 4)) & 0xfu);
}

std::array<int, kBoardSize> linePositions(Direction direction, int line)
{
    std::array<int, kBoardSize> positions{};
    for (int offset = 0; offset < kBoardSize; ++offset) {
        switch (direction) {
            case Direction::Up:
                positions[offset] = offset * kBoardSize + line;
                break;
            case Direction::Right:
                positions[offset] = line * kBoardSize + (kBoardSize - 1 - offset);
                break;
            case Direction::Down:
                positions[offset] = (kBoardSize - 1 - offset) * kBoardSize + line;
                break;
            case Direction::Left:
                positions[offset] = line * kBoardSize + offset;
                break;
        }
    }
    return positions;
}

} // namespace

RZoneTable::RZoneTable(const Game& game,
                       unsigned winning_tile,
                       unsigned max_cells,
                       RZoneLookupPolicy lookup_policy,
                       bool use_symmetry)
    : game_(game),
      winning_tile_(winning_tile),
      max_cells_(max_cells),
      lookup_policy_(lookup_policy),
      use_symmetry_(use_symmetry)
{
    if (winning_tile_ < 2 || (winning_tile_ & (winning_tile_ - 1)) != 0) {
        throw std::invalid_argument("RZone winning tile must be a power of two");
    }
    if (max_cells_ == 0 || max_cells_ > kBoardCells) {
        throw std::invalid_argument("RZone max cells must be in [1, 16]");
    }
}

std::uint64_t RZoneTable::maskedBoard(Board board, std::uint16_t mask)
{
    return board.raw & expandNibbleMask(mask);
}

std::uint64_t RZoneTable::expandNibbleMask(std::uint16_t mask)
{
    std::uint64_t nibble_mask = 0;
    for (int position = 0; position < kBoardCells; ++position) {
        if ((mask & (std::uint16_t{1} << position)) != 0) {
            nibble_mask |= std::uint64_t{0xf} << (position * 4);
        }
    }
    return nibble_mask;
}

unsigned RZoneTable::countCells(std::uint16_t mask)
{
    unsigned count = 0;
    while (mask != 0) {
        count += mask & 1u;
        mask = static_cast<std::uint16_t>(mask >> 1);
    }
    return count;
}

const SlideResult* RZoneTable::findSlide(
    const std::vector<SlideResult>& slides,
    Direction direction) const
{
    const auto found = std::find_if(slides.begin(), slides.end(), [&](const SlideResult& slide) {
        return slide.direction == direction;
    });
    return found == slides.end() ? nullptr : &*found;
}

bool RZoneTable::maskContainsOnlyNonEmpty(Board board, std::uint16_t mask) const
{
    for (int position = 0; position < kBoardCells; ++position) {
        if ((mask & (std::uint16_t{1} << position)) != 0 && cellRank(board, position) == 0) {
            return false;
        }
    }
    return true;
}

bool RZoneTable::sameCertificate(
    const RZoneCertificate& lhs,
    const RZoneCertificate& rhs) const
{
    return lhs.kind == rhs.kind &&
           lhs.phase == rhs.phase &&
           lhs.winning_tile == rhs.winning_tile &&
           lhs.mask == rhs.mask &&
           lhs.masked_board == rhs.masked_board &&
           lhs.slide == rhs.slide &&
           lhs.terminal_position == rhs.terminal_position &&
           lhs.child == rhs.child &&
           lhs.depth == rhs.depth &&
           lhs.validated == rhs.validated;
}

std::optional<RZoneCertificateId> RZoneTable::publish(RZoneCertificate certificate)
{
    if (!certificate.validated || certificate.mask == 0 ||
        countCells(certificate.mask) > max_cells_ || certificate.depth == 0) {
        return std::nullopt;
    }

    const auto [mask_entry, new_mask] = patterns_.try_emplace(certificate.mask);
    if (new_mask) {
        const unsigned cells = countCells(certificate.mask);
        masks_by_cell_count_[cells].push_back(
            MaskIndexEntry{certificate.mask, expandNibbleMask(certificate.mask)});
        ++statistics_.masks_indexed;
    }
    auto& ids = mask_entry->second[certificate.masked_board];
    for (const RZoneCertificateId id : ids) {
        const RZoneCertificate* existing = find(id);
        if (existing && sameCertificate(*existing, certificate)) {
            ++statistics_.duplicate_patterns;
            return id;
        }
    }
    if (certificates_.size() >= std::numeric_limits<RZoneCertificateId>::max()) {
        throw std::overflow_error("RZone certificate ID space exhausted");
    }
    certificate.id = static_cast<RZoneCertificateId>(certificates_.size() + 1);
    certificates_.push_back(certificate);
    ids.push_back(certificate.id);
    ++statistics_.patterns_stored;
    return certificate.id;
}

const RZoneCertificate* RZoneTable::find(RZoneCertificateId id) const
{
    if (id == kNoRZoneCertificate || id > certificates_.size()) return nullptr;
    return &certificates_[static_cast<std::size_t>(id - 1)];
}

std::optional<RZoneCertificateId> RZoneTable::createTerminal(Board board, Phase phase)
{
    if (!game_.hasWinningTile(board, winning_tile_)) return std::nullopt;
    const std::array<unsigned, 16> tiles = game_.tileValues(board);
    for (int position = 0; position < kBoardCells; ++position) {
        if (tiles[static_cast<std::size_t>(position)] < winning_tile_) continue;
        const std::uint16_t mask = static_cast<std::uint16_t>(std::uint16_t{1} << position);
        RZoneCertificate certificate;
        certificate.kind = RZoneStepKind::Terminal;
        certificate.phase = phase;
        certificate.winning_tile = winning_tile_;
        certificate.mask = mask;
        certificate.masked_board = maskedBoard(board, mask);
        certificate.terminal_position = position;
        certificate.depth = 1;
        certificate.validated = true;
        const std::optional<RZoneCertificateId> id = publish(certificate);
        if (id) ++statistics_.terminal_certificates;
        return id;
    }
    return std::nullopt;
}

std::uint16_t RZoneTable::deriveSlidePreimageMask(
    Board parent,
    Direction direction,
    std::uint16_t child_mask) const
{
    if (child_mask == 0) return 0;
    const std::vector<SlideResult> slides = game_.legalSlides(parent);
    const SlideResult* selected = findSlide(slides, direction);
    if (!selected) return 0;

    const Board child = selected->afterstate;
    std::uint16_t parent_mask = 0;
    for (int line = 0; line < kBoardSize; ++line) {
        const std::array<int, kBoardSize> positions = linePositions(direction, line);
        int maximum_required_offset = -1;
        bool requires_empty_output = false;
        for (int offset = 0; offset < kBoardSize; ++offset) {
            const int position = positions[offset];
            if ((child_mask & (std::uint16_t{1} << position)) == 0) continue;
            maximum_required_offset = std::max(maximum_required_offset, offset);
            requires_empty_output = requires_empty_output || cellRank(child, position) == 0;
        }
        if (maximum_required_offset < 0) continue;

        int consumed_offset = kBoardSize - 1;
        if (!requires_empty_output) {
            int emitted_tiles = 0;
            unsigned pending_tile = 0;
            for (int offset = 0; offset < kBoardSize; ++offset) {
                const unsigned tile = cellRank(parent, positions[offset]);
                if (tile != 0) {
                    if (pending_tile == 0) {
                        pending_tile = tile;
                    } else if (pending_tile == tile) {
                        pending_tile = 0;
                        ++emitted_tiles;
                    } else {
                        pending_tile = tile;
                        ++emitted_tiles;
                    }
                }
                if (emitted_tiles > maximum_required_offset) {
                    consumed_offset = offset;
                    break;
                }
            }
        }
        for (int offset = 0; offset <= consumed_offset; ++offset) {
            parent_mask |= static_cast<std::uint16_t>(std::uint16_t{1} << positions[offset]);
        }
    }

    const Board witness{maskedBoard(parent, parent_mask)};
    if (!findSlide(game_.legalSlides(witness), direction)) {
        for (int line = 0; line < kBoardSize; ++line) {
            const std::array<int, kBoardSize> positions = linePositions(direction, line);
            bool changed = false;
            for (const int position : positions) {
                changed = changed || cellRank(parent, position) != cellRank(child, position);
            }
            if (!changed) continue;
            for (const int position : positions) {
                parent_mask |= static_cast<std::uint16_t>(std::uint16_t{1} << position);
            }
            break;
        }
    }
    return parent_mask;
}

std::optional<RZoneCertificateId> RZoneTable::liftFixedSlide(
    Board parent,
    Direction direction,
    RZoneCertificateId child_id)
{
    ++statistics_.slide_lift_attempts;
    const RZoneCertificate* child = find(child_id);
    if (!child || !child->validated || child->phase != Phase::Environment ||
        child->winning_tile != winning_tile_) {
        ++statistics_.slide_lift_failures;
        return std::nullopt;
    }
    const std::vector<SlideResult> slides = game_.legalSlides(parent);
    const SlideResult* selected = findSlide(slides, direction);
    if (!selected) {
        ++statistics_.slide_lift_failures;
        return std::nullopt;
    }
    const std::uint16_t parent_mask = deriveSlidePreimageMask(parent, direction, child->mask);
    if (parent_mask == 0 || countCells(parent_mask) > max_cells_ ||
        maskedBoard(selected->afterstate, child->mask) != child->masked_board) {
        ++statistics_.slide_lift_failures;
        return std::nullopt;
    }

    RZoneCertificate certificate;
    certificate.kind = RZoneStepKind::FixedSlide;
    certificate.phase = Phase::Player;
    certificate.winning_tile = winning_tile_;
    certificate.mask = parent_mask;
    certificate.masked_board = maskedBoard(parent, parent_mask);
    certificate.slide = direction;
    certificate.child = child_id;
    certificate.depth = child->depth + 1;
    certificate.validated = true;
    const std::optional<RZoneCertificateId> id = publish(certificate);
    if (id) ++statistics_.slide_lift_successes;
    else ++statistics_.slide_lift_failures;
    return id;
}

std::optional<RZoneCertificateId> RZoneTable::liftUniversalPlacement(
    Board parent,
    RZoneCertificateId child_id)
{
    ++statistics_.placement_lift_attempts;
    const RZoneCertificate* child = find(child_id);
    if (!child || !child->validated || child->phase != Phase::Player ||
        child->winning_tile != winning_tile_ ||
        !maskContainsOnlyNonEmpty(parent, child->mask) ||
        maskedBoard(parent, child->mask) != child->masked_board) {
        ++statistics_.placement_lift_blocked;
        return std::nullopt;
    }

    const std::vector<PlacementResult> placements = game_.legalPlacements(parent);
    if (placements.empty() ||
        !std::all_of(placements.begin(), placements.end(), [&](const PlacementResult& placement) {
            return maskedBoard(placement.board, child->mask) == child->masked_board;
        })) {
        ++statistics_.placement_lift_blocked;
        return std::nullopt;
    }

    RZoneCertificate certificate;
    certificate.kind = RZoneStepKind::UniversalPlacement;
    certificate.phase = Phase::Environment;
    certificate.winning_tile = winning_tile_;
    certificate.mask = child->mask;
    certificate.masked_board = child->masked_board;
    certificate.child = child_id;
    certificate.depth = child->depth + 1;
    certificate.validated = true;
    const std::optional<RZoneCertificateId> id = publish(certificate);
    if (id) ++statistics_.placement_lift_successes;
    else ++statistics_.placement_lift_blocked;
    return id;
}

std::optional<RZoneMatch> RZoneTable::lookup(Board board, Phase phase)
{
    ++statistics_.lookups;
    std::array<Board, 8> candidates{};
    const unsigned transform_count = use_symmetry_ ? 8 : 1;
    for (unsigned transform = 0; transform < transform_count; ++transform) {
        candidates[transform] = transform == 0
            ? board
            : transformBoard(board, static_cast<std::uint8_t>(transform));
    }
    statistics_.lookup_transforms_tested += transform_count;
    if (lookup_policy_ == RZoneLookupPolicy::SmallestFirst) {
        return lookupSmallestFirst(candidates, transform_count, phase);
    }
    return lookupFullScan(candidates, transform_count, phase);
}

std::optional<RZoneMatch> RZoneTable::recordLookupHit(
    const RZoneCertificate* certificate,
    Phase phase,
    std::uint8_t transform)
{
    if (!certificate) return std::nullopt;
    if (phase == Phase::Player) ++statistics_.player_hits;
    else ++statistics_.environment_hits;
    if (transform == 0) ++statistics_.exact_orientation_hits;
    else ++statistics_.symmetry_hits;
    if (certificate->mask == std::numeric_limits<std::uint16_t>::max()) {
        ++statistics_.full_board_hits;
    } else {
        ++statistics_.partial_hits;
    }
    return RZoneMatch{certificate->id, transform};
}

std::optional<RZoneMatch> RZoneTable::lookupFullScan(
    const std::array<Board, 8>& candidates,
    unsigned transform_count,
    Phase phase)
{
    const RZoneCertificate* best = nullptr;
    std::uint8_t best_transform = 0;
    for (unsigned cells = 1; cells <= max_cells_; ++cells) {
        for (unsigned transform = 0; transform < transform_count; ++transform) {
            for (const MaskIndexEntry& indexed : masks_by_cell_count_[cells]) {
                ++statistics_.lookup_masks_tested;
                const auto mask_entry = patterns_.find(indexed.mask);
                if (mask_entry == patterns_.end()) {
                    throw std::logic_error("RZone ordered mask index is inconsistent");
                }
                const auto found = mask_entry->second.find(
                    candidates[transform].raw & indexed.nibble_mask);
                if (found == mask_entry->second.end()) continue;
                statistics_.lookup_candidates += found->second.size();
                for (const RZoneCertificateId id : found->second) {
                    const RZoneCertificate* certificate = find(id);
                    if (!certificate || !certificate->validated || certificate->phase != phase ||
                        certificate->winning_tile != winning_tile_) {
                        continue;
                    }
                    ++statistics_.pattern_matches;
                    const unsigned certificate_cells = countCells(certificate->mask);
                    if (!best || certificate_cells < countCells(best->mask) ||
                        (certificate_cells == countCells(best->mask) &&
                         certificate->depth < best->depth) ||
                        (certificate_cells == countCells(best->mask) &&
                         certificate->depth == best->depth && certificate->id < best->id) ||
                        (certificate_cells == countCells(best->mask) &&
                         certificate->depth == best->depth && certificate->id == best->id &&
                         transform < best_transform)) {
                        best = certificate;
                        best_transform = static_cast<std::uint8_t>(transform);
                    }
                }
            }
        }
    }
    return recordLookupHit(best, phase, best_transform);
}

std::optional<RZoneMatch> RZoneTable::lookupSmallestFirst(
    const std::array<Board, 8>& candidates,
    unsigned transform_count,
    Phase phase)
{
    for (unsigned cells = 1; cells <= max_cells_; ++cells) {
        const RZoneCertificate* best = nullptr;
        std::uint8_t best_transform = 0;
        for (unsigned transform = 0; transform < transform_count; ++transform) {
            for (const MaskIndexEntry& indexed : masks_by_cell_count_[cells]) {
                ++statistics_.lookup_masks_tested;
                const auto mask_entry = patterns_.find(indexed.mask);
                if (mask_entry == patterns_.end()) {
                    throw std::logic_error("RZone ordered mask index is inconsistent");
                }
                const auto found = mask_entry->second.find(
                    candidates[transform].raw & indexed.nibble_mask);
                if (found == mask_entry->second.end()) continue;
                statistics_.lookup_candidates += found->second.size();

                for (const RZoneCertificateId id : found->second) {
                    const RZoneCertificate* certificate = find(id);
                    if (!certificate || !certificate->validated || certificate->phase != phase ||
                        certificate->winning_tile != winning_tile_) {
                        continue;
                    }
                    ++statistics_.pattern_matches;
                    if (!best || certificate->depth < best->depth ||
                        (certificate->depth == best->depth && certificate->id < best->id) ||
                        (certificate->depth == best->depth && certificate->id == best->id &&
                         transform < best_transform)) {
                        best = certificate;
                        best_transform = static_cast<std::uint8_t>(transform);
                    }
                }
            }
        }
        if (best) {
            ++statistics_.lookup_early_exits;
            return recordLookupHit(best, phase, best_transform);
        }
    }
    return std::nullopt;
}

const char* rzoneModeName(RZoneMode mode)
{
    switch (mode) {
        case RZoneMode::Disabled: return "disabled";
        case RZoneMode::Generate: return "generate";
        case RZoneMode::Lookup: return "lookup";
    }
    return "disabled";
}

const char* rzoneStepKindName(RZoneStepKind kind)
{
    switch (kind) {
        case RZoneStepKind::Terminal: return "TERMINAL";
        case RZoneStepKind::FixedSlide: return "FIXED_SLIDE";
        case RZoneStepKind::UniversalPlacement: return "UNIVERSAL_PLACEMENT";
    }
    return "TERMINAL";
}

const char* rzoneLookupPolicyName(RZoneLookupPolicy policy)
{
    switch (policy) {
        case RZoneLookupPolicy::FullScan: return "full_scan";
        case RZoneLookupPolicy::SmallestFirst: return "smallest_first";
    }
    return "full_scan";
}

} // namespace solver2048
