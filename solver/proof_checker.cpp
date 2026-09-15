#include "proof_checker.h"

#include "game_api.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace solver2048 {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'S', '2', '0', '4', '8', 'T', 'T', 0};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint32_t kRulesVersion = 1;
constexpr std::uint32_t kBoardEncodingVersion = 1;
constexpr std::uint32_t kChecksumAlgorithmCrc64Ecma = 1;
constexpr std::uint16_t kSymmetryFlag = 1u << 0;
constexpr std::uint8_t kWinCode = 1;
constexpr std::uint8_t kNoWitness = 255;
constexpr std::uint64_t kCrc64Polynomial = 0x42f0e1eba9ea3693ull;

[[noreturn]] void fileFail(const std::filesystem::path& path, const std::string& message)
{
    throw std::runtime_error(path.string() + ": " + message);
}

std::uint16_t get16(const std::uint8_t* input)
{
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(input[1]) << 8;
}

std::uint32_t get32(const std::uint8_t* input)
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(input[index]) << (index * 8);
    }
    return value;
}

std::uint64_t get64(const std::uint8_t* input)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8);
    }
    return value;
}

const std::array<std::uint64_t, 256>& crc64Table()
{
    static const std::array<std::uint64_t, 256> table = [] {
        std::array<std::uint64_t, 256> result{};
        for (std::uint64_t byte = 0; byte < result.size(); ++byte) {
            std::uint64_t value = byte << 56;
            for (unsigned bit = 0; bit < 8; ++bit) {
                value = (value & (std::uint64_t{1} << 63)) != 0
                    ? (value << 1) ^ kCrc64Polynomial
                    : value << 1;
            }
            result[byte] = value;
        }
        return result;
    }();
    return table;
}

void updateCrc64(std::uint64_t& crc, const std::uint8_t* data, std::size_t size)
{
    const auto& table = crc64Table();
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t table_index = static_cast<std::uint8_t>((crc >> 56) ^ data[index]);
        crc = table[table_index] ^ (crc << 8);
    }
}

void readExact(std::istream& input,
               std::uint8_t* output,
               std::size_t size,
               const std::filesystem::path& path)
{
    input.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) fileFail(path, "unexpected end of file");
}

Outcome decodeOutcome(std::uint8_t code)
{
    if (code == kWinCode) return Outcome::Win;
    throw std::invalid_argument("Win-only TT outcome must be Win");
}

std::optional<SearchAction> decodeWitness(std::uint8_t code)
{
    if (code == kNoWitness) return std::nullopt;
    SearchAction action;
    if (code <= 3) {
        action.kind = SearchAction::Kind::Slide;
        action.direction = static_cast<Direction>(code);
        return action;
    }
    if (code <= 19) {
        action.kind = SearchAction::Kind::Placement;
        action.position = code - 4;
        action.tile = 2;
        return action;
    }
    if (code <= 35) {
        action.kind = SearchAction::Kind::Placement;
        action.position = code - 20;
        action.tile = 4;
        return action;
    }
    throw std::invalid_argument("witness action ID is invalid");
}

void validateProofEntry(std::uint64_t board_key,
                        const ProofEntry& entry,
                        Phase phase,
                        bool symmetry,
                        std::uint8_t flags,
                        std::uint32_t reserved)
{
    if (flags != 0 || reserved != 0) {
        throw std::invalid_argument("record flags and reserved fields must be zero");
    }
    if (phase == Phase::Player) {
        if (entry.children_total > 4) {
            throw std::invalid_argument("Player children_total exceeds four slides");
        }
        if (!entry.witness || entry.witness->kind != SearchAction::Kind::Slide) {
            throw std::invalid_argument("Player Win entry requires a slide witness");
        }
    } else {
        if (entry.children_total == 0 || entry.children_total > 32) {
            throw std::invalid_argument("Environment children_total must be in [1, 32]");
        }
        if (entry.witness) {
            throw std::invalid_argument("Environment Win entry must not contain a witness");
        }
    }
    if (symmetry && canonicalize(Board{board_key}).board.raw != board_key) {
        throw std::invalid_argument("record board key is not D4 canonical");
    }
}

using Properties = std::unordered_map<std::string, std::vector<std::string>>;

class SgfPrefixParser {
public:
    SgfPrefixParser(const std::filesystem::path& path, std::istream& input)
        : path_(path), input_(input) {}

    void expect(char expected)
    {
        skipWhitespace();
        const int value = input_.get();
        ++offset_;
        if (value != expected) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    Properties readNode()
    {
        expect(';');
        Properties properties;
        while (true) {
            skipWhitespace();
            const int next = input_.peek();
            if (next == std::char_traits<char>::eof() || next == ';' || next == '(' || next == ')') {
                return properties;
            }
            if (!std::isupper(static_cast<unsigned char>(next))) fail("expected an SGF property name");
            std::string name;
            while (std::isupper(static_cast<unsigned char>(input_.peek()))) {
                name.push_back(static_cast<char>(input_.get()));
                ++offset_;
            }
            skipWhitespace();
            std::vector<std::string> values;
            while (input_.peek() == '[') values.push_back(readValue());
            if (values.empty()) fail("SGF property " + name + " has no value");
            if (!properties.emplace(name, std::move(values)).second) {
                fail("duplicate SGF property " + name + " in one node");
            }
        }
    }

    int peekSignificant()
    {
        skipWhitespace();
        return input_.peek();
    }

    void expectEnd()
    {
        skipWhitespace();
        if (input_.peek() != std::char_traits<char>::eof()) {
            fail("unexpected data after the SGF game tree");
        }
    }

    std::uint64_t offset() const { return offset_; }

private:
    std::string readValue()
    {
        if (input_.get() != '[') fail("expected '['");
        ++offset_;
        std::string value;
        while (true) {
            const int next = input_.get();
            ++offset_;
            if (next == std::char_traits<char>::eof()) fail("unterminated SGF property value");
            if (next == ']') return value;
            if (next == '\\') {
                const int escaped = input_.get();
                ++offset_;
                if (escaped == std::char_traits<char>::eof()) fail("unterminated SGF escape");
                if (escaped == '\r') {
                    if (input_.peek() == '\n') {
                        input_.get();
                        ++offset_;
                    }
                    continue;
                }
                if (escaped == '\n') continue;
                value.push_back(static_cast<char>(escaped));
                continue;
            }
            value.push_back(static_cast<char>(next));
        }
    }

    void skipWhitespace()
    {
        while (true) {
            const int next = input_.peek();
            if (next == std::char_traits<char>::eof() ||
                !std::isspace(static_cast<unsigned char>(next))) return;
            input_.get();
            ++offset_;
        }
    }

    [[noreturn]] void fail(const std::string& message) const
    {
        fileFail(path_, "SGF byte " + std::to_string(offset_) + ": " + message);
    }

    const std::filesystem::path& path_;
    std::istream& input_;
    std::uint64_t offset_ = 0;
};

const std::string& singleProperty(const Properties& properties,
                                  const std::string& name,
                                  const std::filesystem::path& path)
{
    const auto found = properties.find(name);
    if (found == properties.end() || found->second.size() != 1) {
        fileFail(path, "SGF property " + name + " must occur exactly once in the expected node");
    }
    return found->second.front();
}

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::unordered_map<std::string, std::string> parseCommentFields(
    const std::string& comment,
    const std::filesystem::path& path)
{
    std::unordered_map<std::string, std::string> fields;
    std::istringstream input(comment);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equal = line.find('=');
        const std::size_t colon = line.find(':');
        std::size_t delimiter = std::string::npos;
        if (equal != std::string::npos) delimiter = equal;
        if (colon != std::string::npos && (delimiter == std::string::npos || colon < delimiter)) {
            delimiter = colon;
        }
        if (delimiter == std::string::npos) continue;
        const std::string key = trim(line.substr(0, delimiter));
        const std::string value = trim(line.substr(delimiter + 1));
        if (key.empty()) fileFail(path, "SGF comment contains an empty field name");
        if (!fields.emplace(key, value).second) {
            fileFail(path, "SGF comment contains duplicate field " + key);
        }
    }
    return fields;
}

const std::string& requiredField(const std::unordered_map<std::string, std::string>& fields,
                                 const std::string& name,
                                 const std::filesystem::path& path)
{
    const auto found = fields.find(name);
    if (found == fields.end() || found->second.empty()) {
        fileFail(path, "SGF root comment is missing " + name);
    }
    return found->second;
}

template <typename Integer>
Integer parseUnsigned(const std::string& text,
                      const std::string& field,
                      const std::filesystem::path& path)
{
    std::string_view value = text;
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        base = 16;
    }
    Integer result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        fileFail(path, "SGF field " + field + " is not a valid unsigned integer: " + text);
    }
    return result;
}

SearchAction decodeSgfAction(const std::string& text, const std::filesystem::path& path)
{
    const unsigned id = parseUnsigned<unsigned>(text, "W action", path);
    if (id > 35) fileFail(path, "fixed-opening SGF action ID is out of range");
    try {
        const auto action = decodeWitness(static_cast<std::uint8_t>(id));
        if (!action || action->kind != SearchAction::Kind::Placement) {
            fileFail(path, "fixed-opening SGF action must be a tile placement");
        }
        return *action;
    } catch (const std::exception& error) {
        fileFail(path, std::string("invalid fixed-opening SGF action: ") + error.what());
    }
}

Outcome parseOutcome(const std::string& text, const std::filesystem::path& path)
{
    if (text == "WIN") return Outcome::Win;
    if (text == "LOSS") return Outcome::Loss;
    if (text == "UNKNOWN") return Outcome::Unknown;
    fileFail(path, "invalid solver_status: " + text);
}

bool parseEnabled(const std::string& text,
                  const std::string& field,
                  const std::filesystem::path& path)
{
    if (text == "enabled") return true;
    if (text == "disabled") return false;
    fileFail(path, "SGF field " + field + " must be enabled or disabled");
}

SolvedBy parseSolvedBy(const std::string& text, const std::filesystem::path& path)
{
    if (text == "NONE") return SolvedBy::None;
    if (text == "TERMINAL") return SolvedBy::Terminal;
    if (text == "RZONE_TT") return SolvedBy::RZoneTable;
    if (text == "TT") return SolvedBy::TranspositionTable;
    if (text == "BACKPROPAGATION") return SolvedBy::Backpropagation;
    if (text == "NODE_LIMIT") return SolvedBy::NodeLimit;
    fileFail(path, "invalid solved_by: " + text);
}

SearchAction actionFromId(unsigned id, const std::filesystem::path& path)
{
    if (id > 35) fileFail(path, "SGF action ID is out of range: " + std::to_string(id));
    try {
        return *decodeWitness(static_cast<std::uint8_t>(id));
    } catch (const std::exception& error) {
        fileFail(path, std::string("invalid SGF action ID: ") + error.what());
    }
}

unsigned actionId(const SearchAction& action)
{
    if (action.kind == SearchAction::Kind::Slide) {
        return static_cast<unsigned>(action.direction);
    }
    return static_cast<unsigned>((action.tile == 2 ? 4 : 20) + action.position);
}

std::string actionName(const SearchAction& action)
{
    if (action.kind == SearchAction::Kind::Slide) return directionName(action.direction);
    return std::to_string(action.tile) + "@" + std::to_string(action.position);
}

bool sameAction(const SearchAction& lhs, const SearchAction& rhs)
{
    if (lhs.kind != rhs.kind) return false;
    if (lhs.kind == SearchAction::Kind::Slide) return lhs.direction == rhs.direction;
    return lhs.position == rhs.position && lhs.tile == rhs.tile;
}

SearchAction parseActionToken(const std::string& text,
                              const std::string& field,
                              const std::filesystem::path& path)
{
    const std::size_t open = text.rfind('(');
    if (open == std::string::npos) {
        return actionFromId(parseUnsigned<unsigned>(text, field, path), path);
    }
    if (text.empty() || text.back() != ')' || open + 1 == text.size() - 1) {
        fileFail(path, "SGF field " + field + " has an invalid action token: " + text);
    }
    const unsigned id = parseUnsigned<unsigned>(
        text.substr(open + 1, text.size() - open - 2), field, path);
    SearchAction action = actionFromId(id, path);
    if (text.substr(0, open) != actionName(action)) {
        fileFail(path, "SGF field " + field + " action name and ID disagree: " + text);
    }
    return action;
}

std::vector<SearchAction> parseActionList(const std::string& text,
                                          const std::string& field,
                                          const std::filesystem::path& path)
{
    std::vector<SearchAction> actions;
    std::istringstream input(text);
    std::string token;
    while (input >> token) actions.push_back(parseActionToken(token, field, path));
    if (actions.empty()) fileFail(path, "SGF field " + field + " is empty");
    return actions;
}

struct TraceRecord {
    Outcome outcome = Outcome::Unknown;
    SolvedBy solved_by = SolvedBy::None;
    Phase phase = Phase::Player;
    std::optional<std::uint64_t> tt_source_node_id;
    std::optional<std::uint8_t> rzone_transform;
    std::optional<RZoneCertificateId> rzone_certificate_id;
    std::optional<SearchAction> selected_action;
    std::vector<SearchAction> skipped_actions;
};

TraceRecord parseTraceRecord(const Properties& properties,
                             const std::filesystem::path& path,
                             Phase phase)
{
    const auto fields = parseCommentFields(singleProperty(properties, "C", path), path);
    TraceRecord record;
    record.outcome = parseOutcome(requiredField(fields, "solver_status", path), path);
    record.solved_by = parseSolvedBy(requiredField(fields, "solved_by", path), path);
    record.phase = phase;

    const auto selected = fields.find("selected_action");
    if (selected != fields.end()) {
        record.selected_action = parseActionToken(selected->second, "selected_action", path);
    }
    const auto skipped = fields.find("children_skipped_actions");
    if (skipped != fields.end()) {
        record.skipped_actions = parseActionList(
            skipped->second, "children_skipped_actions", path);
    }
    const auto source = fields.find("tt_source_node_id");
    if (source != fields.end()) {
        record.tt_source_node_id = parseUnsigned<std::uint64_t>(
            source->second, "tt_source_node_id", path);
    }
    const auto rzone_transform = fields.find("rzone_transform");
    if (rzone_transform != fields.end()) {
        const std::size_t colon = rzone_transform->second.find(':');
        const std::string number = rzone_transform->second.substr(0, colon);
        const unsigned value = parseUnsigned<unsigned>(number, "rzone_transform", path);
        if (value >= 8) fileFail(path, "SGF rzone_transform must be in [0, 7]");
        record.rzone_transform = static_cast<std::uint8_t>(value);
        if (colon != std::string::npos &&
            rzone_transform->second.substr(colon + 1) !=
                symmetryTransformName(record.rzone_transform.value())) {
            fileFail(path, "SGF rzone_transform number and name disagree");
        }
    }
    const auto rzone_certificate_id = fields.find("rzone_certificate_id");
    if (rzone_certificate_id != fields.end()) {
        record.rzone_certificate_id = parseUnsigned<RZoneCertificateId>(
            rzone_certificate_id->second, "rzone_certificate_id", path);
    }
    return record;
}

std::string stateLabel(Board board, Phase phase)
{
    std::ostringstream output;
    output << (phase == Phase::Player ? "PLAYER" : "ENVIRONMENT")
           << " board=0x" << std::hex << std::setw(16) << std::setfill('0') << board.raw;
    return output.str();
}

SearchAction mapWitnessToCurrent(const SearchAction& canonical_action, std::uint8_t transform)
{
    SearchAction result = canonical_action;
    const std::uint8_t inverse = inverseTransform(transform);
    if (result.kind == SearchAction::Kind::Slide) {
        result.direction = transformDirection(result.direction, inverse);
    } else {
        result.position = transformPosition(result.position, inverse);
    }
    return result;
}

SearchAction mapWitnessToCanonical(const SearchAction& action, std::uint8_t transform)
{
    SearchAction result = action;
    if (result.kind == SearchAction::Kind::Slide) {
        result.direction = transformDirection(result.direction, transform);
    } else {
        result.position = transformPosition(result.position, transform);
    }
    return result;
}

struct StateKey {
    std::uint64_t board = 0;
    Phase phase = Phase::Player;

    bool operator==(const StateKey& other) const
    {
        return board == other.board && phase == other.phase;
    }
};

struct StateKeyHash {
    std::size_t operator()(const StateKey& key) const
    {
        const std::uint64_t mixed = key.board ^
            (key.phase == Phase::Environment ? 0x9e3779b97f4a7c15ull : 0);
        return std::hash<std::uint64_t>{}(mixed);
    }
};

bool verifyRzArtifactBinding(const ProofManifest& manifest,
                             const RzProofTable* rzone_table)
{
    if (!manifest.hasRzArtifactBinding()) return false;
    if (!rzone_table) {
        throw std::runtime_error(
            "SGF binds an RZone snapshot but no .rz file was supplied");
    }
    if (manifest.winning_tile != rzone_table->winningTile()) {
        throw std::runtime_error("SGF winning tile does not match RZone file");
    }
    if (*manifest.rz_format_version != RzFile::kFormatVersion ||
        *manifest.rz_semantics_version != RzFile::kSemanticsVersion) {
        throw std::runtime_error("SGF RZone format or semantics version is incompatible");
    }
    if (*manifest.rz_prefix_record_count > rzone_table->summary().record_count) {
        throw std::runtime_error("SGF RZone prefix is longer than the supplied RZone file");
    }
    if (rzone_table->prefixChecksum(*manifest.rz_prefix_record_count) !=
        *manifest.rz_prefix_checksum) {
        throw std::runtime_error(
            "SGF RZone prefix checksum does not match the supplied RZone file");
    }
    return true;
}

bool verifyTtArtifactBinding(const ProofManifest& manifest,
                             const ProofTable& table)
{
    if (!manifest.hasArtifactBinding()) return false;
    const TtFileSummary& summary = table.summary();
    if (*manifest.tt_format_version != TtFile::kFormatVersion ||
        *manifest.tt_payload_checksum != summary.payload_checksum ||
        *manifest.tt_file_size != summary.file_size ||
        *manifest.tt_player_records != summary.player_entries ||
        *manifest.tt_environment_records != summary.environment_entries) {
        throw std::runtime_error(
            "SGF TT artifact binding does not match the supplied TT file");
    }
    return true;
}

class LogicalVerifier {
public:
    LogicalVerifier(const ProofTable& table,
                    const RzProofTable* rzone_table,
                    RZoneCertificateId rzone_maximum_id,
                    bool rzone_symmetry)
        : table_(table),
          rzone_table_(rzone_table),
          rzone_maximum_id_(rzone_maximum_id),
          rzone_symmetry_(rzone_symmetry) {}

    ProofVerificationStatistics verify(Board root, Phase phase, Outcome expected)
    {
        verifyState(root, phase, expected, 0);
        return statistics_;
    }

private:
    [[noreturn]] void fail(Board board, Phase phase, const std::string& message) const
    {
        throw std::runtime_error("proof verification failed at " + stateLabel(board, phase) +
                                 ": " + message);
    }

    void checkTerminal(Board board, Phase phase, Outcome expected, Outcome actual)
    {
        ++statistics_.terminal_states_checked;
        if (expected != actual) {
            fail(board, phase, std::string("terminal outcome is ") +
                 (actual == Outcome::Win ? "WIN" : "LOSS") + ", expected " +
                 (expected == Outcome::Win ? "WIN" : "LOSS"));
        }
    }

    void verifyState(Board board, Phase phase, Outcome expected, std::uint64_t depth)
    {
        statistics_.maximum_depth = std::max(statistics_.maximum_depth, depth);
        if (expected == Outcome::Unknown) fail(board, phase, "Unknown is not an exact proof outcome");
        if (boardHasWinningTile(board, table_.winningTile())) {
            checkTerminal(board, phase, expected, Outcome::Win);
            return;
        }
        if (expected == Outcome::Win && rzone_table_ &&
            rzone_table_->findMatching(
                board, phase, rzone_maximum_id_, rzone_symmetry_)) {
            ++statistics_.rzone_certificates_used;
            return;
        }

        std::vector<SlideResult> slides;
        std::vector<PlacementResult> placements;
        if (phase == Phase::Player) {
            slides = generateLegalSlides(board);
            if (slides.empty()) {
                checkTerminal(board, phase, expected, Outcome::Loss);
                return;
            }
        } else {
            placements = generateLegalPlacements(board);
            if (placements.empty()) fail(board, phase, "non-winning Environment state has no legal placement");
        }

        const CanonicalBoard canonical = table_.usesSymmetry()
            ? canonicalize(board)
            : CanonicalBoard{board, 0};
        const ProofEntry* entry = table_.find(phase, canonical.board.raw);
        if (!entry) fail(board, phase, "required TT entry is missing");
        if (entry->outcome != expected) {
            fail(board, phase, std::string("TT outcome is ") +
                 (entry->outcome == Outcome::Win ? "WIN" : "LOSS") + ", expected " +
                 (expected == Outcome::Win ? "WIN" : "LOSS"));
        }

        const std::size_t generated_children = phase == Phase::Player
            ? slides.size()
            : placements.size();
        if (entry->children_total != generated_children) {
            fail(board, phase, "children_total mismatch: TT=" +
                 std::to_string(entry->children_total) + ", generated=" +
                 std::to_string(generated_children));
        }

        const StateKey state{canonical.board.raw, phase};
        if (verified_.count(state) != 0) {
            ++statistics_.memoized_verification_hits;
            return;
        }
        if (!visiting_.insert(state).second) fail(board, phase, "cyclic TT proof dependency");
        ++statistics_.tt_records_verified;

        if (phase == Phase::Player && expected == Outcome::Win) {
            if (!entry->witness || entry->witness->kind != SearchAction::Kind::Slide) {
                fail(board, phase, "Player Win is missing a slide witness");
            }
            const SearchAction witness = mapWitnessToCurrent(*entry->witness, canonical.transform);
            const auto child = std::find_if(slides.begin(), slides.end(), [&](const SlideResult& slide) {
                return slide.direction == witness.direction;
            });
            if (child == slides.end()) fail(board, phase, "Player Win witness is not a legal slide");
            ++statistics_.witness_edges_checked;
            verifyState(child->afterstate, Phase::Environment, Outcome::Win, depth + 1);
        } else if (phase == Phase::Player) {
            for (const SlideResult& child : slides) {
                ++statistics_.universal_edges_checked;
                verifyState(child.afterstate, Phase::Environment, Outcome::Loss, depth + 1);
            }
        } else if (expected == Outcome::Win) {
            for (const PlacementResult& child : placements) {
                ++statistics_.universal_edges_checked;
                verifyState(child.board, Phase::Player, Outcome::Win, depth + 1);
            }
        } else {
            if (!entry->witness || entry->witness->kind != SearchAction::Kind::Placement) {
                fail(board, phase, "Environment Loss is missing a placement witness");
            }
            const SearchAction witness = mapWitnessToCurrent(*entry->witness, canonical.transform);
            const auto child = std::find_if(
                placements.begin(), placements.end(), [&](const PlacementResult& placement) {
                    return placement.position == witness.position && placement.tile == witness.tile;
                });
            if (child == placements.end()) {
                fail(board, phase, "Environment Loss witness is not a legal placement");
            }
            ++statistics_.witness_edges_checked;
            verifyState(child->board, Phase::Player, Outcome::Loss, depth + 1);
        }

        visiting_.erase(state);
        verified_.insert(state);
    }

    const ProofTable& table_;
    const RzProofTable* rzone_table_ = nullptr;
    RZoneCertificateId rzone_maximum_id_ = kNoRZoneCertificate;
    bool rzone_symmetry_ = false;
    ProofVerificationStatistics statistics_;
    std::unordered_set<StateKey, StateKeyHash> visiting_;
    std::unordered_set<StateKey, StateKeyHash> verified_;
};

struct TraceChildSummary {
    SearchAction action;
    Outcome outcome = Outcome::Unknown;
    std::optional<RZoneCertificateId> rzone_certificate_id;
    std::optional<std::uint8_t> rzone_transform;
};

struct TraceNodeSummary {
    Outcome outcome = Outcome::Unknown;
    std::optional<RZoneCertificateId> rzone_certificate_id;
    std::optional<std::uint8_t> rzone_transform;
};

struct VerifiedExactSource {
    std::uint64_t canonical_board = 0;
    Phase phase = Phase::Player;
    std::optional<SearchAction> canonical_witness;
    std::size_t children_total = 0;
};

class TraceVerifier {
public:
    TraceVerifier(const std::filesystem::path& path,
                  SgfPrefixParser& parser,
                  const ProofManifest& manifest,
                  const ProofTable& table,
                  const RzProofTable* rzone_table,
                  RZoneCertificateId rzone_maximum_id)
        : path_(path),
          parser_(parser),
          manifest_(manifest),
          table_(table),
          rzone_table_(rzone_table),
          rzone_maximum_id_(rzone_maximum_id),
          rzone_sources_(static_cast<std::size_t>(rzone_maximum_id) + 1, false) {}

    TraceVerificationStatistics verify(const Properties& root_properties)
    {
        const TraceRecord root = parseTraceRecord(root_properties, path_, Phase::Player);
        const TraceNodeSummary summary = verifyNode(manifest_.root_board, root, 0);
        if (summary.outcome != manifest_.root_outcome) {
            fail(manifest_.root_board, Phase::Player,
                 "root trace outcome does not match the SGF manifest");
        }
        if (summary.outcome == Outcome::Unknown) {
            fail(manifest_.root_board, Phase::Player,
                 "Unknown is not an exact root proof outcome");
        }
        const std::uint64_t expected_tt_records = manifest_.transposition_table
            ? table_.summary().entries()
            : 0;
        if (verified_states_.size() != expected_tt_records) {
            fail(manifest_.root_board, Phase::Player,
                 "SGF verified-state table does not cover every bound TT record: SGF=" +
                     std::to_string(verified_states_.size()) + ", TT=" +
                     std::to_string(expected_tt_records));
        }
        statistics_.verified_exact_states = verified_states_.size();
        statistics_.tt_records_cross_checked = expected_tt_records;
        if (rzone_source_count_ != rzone_maximum_id_) {
            fail(manifest_.root_board, Phase::Player,
                 "SGF RZone source table does not cover every bound certificate: SGF=" +
                     std::to_string(rzone_source_count_) + ", RZone=" +
                     std::to_string(rzone_maximum_id_));
        }
        statistics_.rzone_records_cross_checked = rzone_source_count_;
        return statistics_;
    }

private:
    [[noreturn]] void fail(Board board, Phase phase, const std::string& message) const
    {
        fileFail(path_, "trace verification failed at " + stateLabel(board, phase) +
                            ": " + message);
    }

    SearchAction readIncomingAction(const Properties& properties, Phase parent_phase)
    {
        const char expected_property = parent_phase == Phase::Player ? 'B' : 'W';
        const char unexpected_property = parent_phase == Phase::Player ? 'W' : 'B';
        if (properties.find(std::string(1, unexpected_property)) != properties.end()) {
            fileFail(path_, "SGF child uses the wrong player property for its parent phase");
        }
        const std::string name(1, expected_property);
        const unsigned id = parseUnsigned<unsigned>(
            singleProperty(properties, name, path_), name + " action", path_);
        const SearchAction action = actionFromId(id, path_);
        if (parent_phase == Phase::Player && action.kind != SearchAction::Kind::Slide) {
            fileFail(path_, "Player child action must be a slide");
        }
        if (parent_phase == Phase::Environment && action.kind != SearchAction::Kind::Placement) {
            fileFail(path_, "Environment child action must be a placement");
        }
        return action;
    }

    Board replayAction(Board parent,
                       Phase parent_phase,
                       const SearchAction& action)
    {
        if (parent_phase == Phase::Player) {
            const std::vector<SlideResult> slides = generateLegalSlides(parent);
            const auto found = std::find_if(slides.begin(), slides.end(), [&](const SlideResult& slide) {
                return slide.direction == action.direction;
            });
            if (found == slides.end()) fail(parent, parent_phase, "SGF contains an illegal slide action");
            return found->afterstate;
        }
        const std::vector<PlacementResult> placements = generateLegalPlacements(parent);
        const auto found = std::find_if(
            placements.begin(), placements.end(), [&](const PlacementResult& placement) {
                return placement.position == action.position && placement.tile == action.tile;
            });
        if (found == placements.end()) {
            fail(parent, parent_phase, "SGF contains an illegal placement action");
        }
        return found->board;
    }

    TraceChildSummary verifyVariation(Board parent,
                                      Phase parent_phase,
                                      std::uint64_t depth)
    {
        parser_.expect('(');
        const Properties properties = parser_.readNode();
        const SearchAction action = readIncomingAction(properties, parent_phase);
        const Board board = replayAction(parent, parent_phase, action);
        ++statistics_.actions_replayed;

        const Phase expected_phase = parent_phase == Phase::Player
            ? Phase::Environment
            : Phase::Player;
        const TraceRecord record = parseTraceRecord(properties, path_, expected_phase);
        const TraceNodeSummary child = verifyNode(board, record, depth);
        if (parser_.peekSignificant() != ')') {
            fail(board, record.phase, "variation must contain exactly one trace node");
        }
        parser_.expect(')');
        return {
            action,
            child.outcome,
            child.rzone_certificate_id,
            child.rzone_transform,
        };
    }

    std::vector<SearchAction> legalActions(Board board, Phase phase) const
    {
        std::vector<SearchAction> actions;
        if (phase == Phase::Player) {
            for (const SlideResult& slide : generateLegalSlides(board)) {
                actions.push_back(actionFromId(static_cast<unsigned>(slide.direction), path_));
            }
        } else {
            for (const PlacementResult& placement : generateLegalPlacements(board)) {
                SearchAction action;
                action.kind = SearchAction::Kind::Placement;
                action.position = placement.position;
                action.tile = placement.tile;
                actions.push_back(action);
            }
        }
        return actions;
    }

    void checkActionPartition(Board board,
                              const TraceRecord& record,
                              const std::vector<TraceChildSummary>& children,
                              const std::vector<SearchAction>& legal)
    {
        if (children.size() + record.skipped_actions.size() != legal.size()) {
            fail(board, record.phase, "visited and skipped actions do not cover all legal actions");
        }

        std::array<bool, 36> legal_ids{};
        std::array<bool, 36> recorded_ids{};
        for (const SearchAction& action : legal) legal_ids[actionId(action)] = true;
        auto recordAction = [&](const SearchAction& action) {
            const unsigned id = actionId(action);
            if (!legal_ids[id]) fail(board, record.phase, "recorded child or skipped action is illegal");
            if (recorded_ids[id]) fail(board, record.phase, "action occurs more than once in trace node");
            recorded_ids[id] = true;
        };
        for (const TraceChildSummary& child : children) recordAction(child.action);
        for (const SearchAction& action : record.skipped_actions) {
            recordAction(action);
            ++statistics_.skipped_actions_checked;
        }
        if (recorded_ids != legal_ids) {
            fail(board, record.phase, "visited and skipped action set differs from legal action set");
        }

        if (record.phase == Phase::Environment) {
            for (std::size_t index = 0; index < children.size(); ++index) {
                if (!sameAction(children[index].action, legal[index])) {
                    fail(board, record.phase, "Environment actions are not in generator order");
                }
            }
            for (std::size_t index = 0; index < record.skipped_actions.size(); ++index) {
                if (!sameAction(record.skipped_actions[index], legal[children.size() + index])) {
                    fail(board, record.phase, "Environment skipped actions are not the ordered suffix");
                }
            }
        }
    }

    void checkSelected(Board board,
                       const TraceRecord& record,
                       const std::optional<SearchAction>& expected) const
    {
        if (record.selected_action.has_value() != expected.has_value() ||
            (expected && !sameAction(*record.selected_action, *expected))) {
            fail(board, record.phase, "selected_action does not match the decisive witness");
        }
    }

    const RzProofEntry* checkRZoneAnnotation(Board board, const TraceRecord& record)
    {
        const unsigned present = static_cast<unsigned>(record.rzone_certificate_id.has_value()) +
            static_cast<unsigned>(record.rzone_transform.has_value());
        const bool rzone_leaf = record.solved_by == SolvedBy::RZoneTable;
        if (present == 0) {
            if (rzone_leaf) fail(board, record.phase, "RZone leaf is missing its certificate annotation");
            return nullptr;
        }
        if (present != 2) {
            fail(board, record.phase, "RZone certificate annotation is incomplete");
        }
        const std::uint8_t transform = *record.rzone_transform;
        if (transform != 0 && !manifest_.rzone_symmetry) {
            fail(board, record.phase, "RZone transform is present while symmetry lookup is disabled");
        }
        if (!rzone_table_) {
            fail(board, record.phase, "RZone annotation has no bound .rz artifact");
        }
        const RZoneCertificateId id = *record.rzone_certificate_id;
        if (id == kNoRZoneCertificate || id > rzone_maximum_id_) {
            fail(board, record.phase, "RZone certificate ID is outside the SGF-bound prefix");
        }
        const RzProofEntry* entry = rzone_table_->find(id);
        if (!entry) fail(board, record.phase, "RZone certificate ID is absent from the supplied file");
        if (!RzProofTable::matches(*entry, board, record.phase, transform)) {
            fail(board, record.phase, "RZone certificate does not match this board and phase");
        }
        if (record.outcome != Outcome::Win) {
            fail(board, record.phase, "RZone certificate may only annotate a Win node");
        }
        ++statistics_.rzone_annotations_checked;
        return entry;
    }

    void checkBackpropagation(Board board,
                              const TraceRecord& record,
                              const std::vector<TraceChildSummary>& children,
                              const std::vector<SearchAction>& legal)
    {
        if (legal.empty()) fail(board, record.phase, "BACKPROPAGATION node has no legal actions");
        checkActionPartition(board, record, children, legal);
        ++statistics_.backpropagation_nodes_checked;

        if (record.phase == Phase::Player && record.outcome == Outcome::Win) {
            if (children.empty() || children.back().outcome != Outcome::Win) {
                fail(board, record.phase, "Player Win lacks a decisive winning child");
            }
            if (std::any_of(children.begin(), children.end() - 1, [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Win;
                })) {
                fail(board, record.phase, "Player continued after an earlier winning child");
            }
            checkSelected(board, record, children.back().action);
        } else if (record.phase == Phase::Player && record.outcome == Outcome::Loss) {
            if (children.size() != legal.size() ||
                !std::all_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Loss;
                })) {
                fail(board, record.phase, "Player Loss is not supported by all children");
            }
            checkSelected(board, record, std::nullopt);
        } else if (record.phase == Phase::Player) {
            if (children.size() != legal.size() ||
                std::any_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Win;
                }) ||
                !std::any_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Unknown;
                })) {
                fail(board, record.phase, "Player Unknown backpropagation is invalid");
            }
            checkSelected(board, record, std::nullopt);
        } else if (record.outcome == Outcome::Loss) {
            if (children.empty() || children.back().outcome != Outcome::Loss) {
                fail(board, record.phase, "Environment Loss lacks a decisive losing child");
            }
            if (std::any_of(children.begin(), children.end() - 1, [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Loss;
                })) {
                fail(board, record.phase, "Environment continued after an earlier losing child");
            }
            checkSelected(board, record, children.back().action);
        } else if (record.outcome == Outcome::Win) {
            if (children.size() != legal.size() ||
                !std::all_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Win;
                })) {
                fail(board, record.phase, "Environment Win is not supported by all children");
            }
            checkSelected(board, record, std::nullopt);
        } else {
            if (children.size() != legal.size() ||
                std::any_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Loss;
                }) ||
                !std::any_of(children.begin(), children.end(), [](const TraceChildSummary& child) {
                    return child.outcome == Outcome::Unknown;
                })) {
                fail(board, record.phase, "Environment Unknown backpropagation is invalid");
            }
            checkSelected(board, record, std::nullopt);
        }
    }

    void checkTtLeaf(Board board,
                     const TraceRecord& record,
                     const std::vector<TraceChildSummary>& children,
                     const std::vector<SearchAction>& legal,
                     std::uint64_t implicit_node_id)
    {
        if (!manifest_.transposition_table || !children.empty() ||
            !record.skipped_actions.empty()) {
            fail(board, record.phase, "TT leaf metadata or child counts are inconsistent");
        }
        if (!record.tt_source_node_id) {
            fail(board, record.phase,
                 "single-opening verification requires a trace-local TT source node");
        }
        if (*record.tt_source_node_id >= implicit_node_id) {
            fail(board, record.phase, "trace TT source must be an earlier implicit preorder node ID");
        }

        const CanonicalBoard canonical = table_.usesSymmetry()
            ? canonicalize(board)
            : CanonicalBoard{board, 0};
        const ProofEntry* entry = table_.find(record.phase, canonical.board.raw);
        if (!entry) fail(board, record.phase, "TT leaf key is absent from the supplied TT file");
        if (entry->outcome != record.outcome) {
            fail(board, record.phase, "TT leaf outcome differs from the supplied TT entry");
        }

        const auto source = verified_by_id_.find(*record.tt_source_node_id);
        if (source == verified_by_id_.end()) {
            fail(board, record.phase,
                 "TT source node is absent, incomplete, active, or not a verified Win source");
        }
        const VerifiedExactSource& verified = source->second;
        if (verified.canonical_board != canonical.board.raw ||
            verified.phase != record.phase ||
            verified.children_total != legal.size()) {
            fail(board, record.phase,
                 "TT source node state, phase, or legal-child count does not match this leaf");
        }
        std::optional<SearchAction> witness;
        if (verified.canonical_witness) {
            witness = mapWitnessToCurrent(*verified.canonical_witness, canonical.transform);
        }
        checkSelected(board, record, witness);
        ++statistics_.tt_source_references_checked;
        ++statistics_.tt_leaves_checked;
    }

    void checkRZoneLeaf(Board board,
                        const TraceRecord& record,
                        const RzProofEntry& entry,
                        const std::vector<TraceChildSummary>& children)
    {
        if (record.outcome != Outcome::Win || record.tt_source_node_id || !children.empty() ||
            !record.skipped_actions.empty()) {
            fail(board, record.phase, "RZone leaf metadata or child counts are inconsistent");
        }

        std::optional<SearchAction> expected_action;
        if (record.phase == Phase::Player) {
            if (entry.kind != RZoneStepKind::FixedSlide || !entry.slide) {
                fail(board, record.phase, "Player RZone leaf does not reference a FixedSlide certificate");
            }
            SearchAction action;
            action.kind = SearchAction::Kind::Slide;
            action.direction = transformDirection(
                *entry.slide,
                inverseTransform(record.rzone_transform.value_or(0)));
            expected_action = action;
        } else if (entry.kind != RZoneStepKind::UniversalPlacement) {
            fail(board, record.phase,
                 "Environment RZone leaf does not reference a UniversalPlacement certificate");
        }
        checkSelected(board, record, expected_action);
        ++statistics_.rzone_leaves_checked;
    }

    void recordExactSource(Board board,
                           const TraceRecord& record,
                           const std::vector<SearchAction>& legal,
                           std::uint64_t implicit_node_id)
    {
        if (!manifest_.transposition_table || record.outcome != Outcome::Win ||
            record.solved_by != SolvedBy::Backpropagation) {
            return;
        }

        const CanonicalBoard canonical = table_.usesSymmetry()
            ? canonicalize(board)
            : CanonicalBoard{board, 0};
        const ProofEntry* entry = table_.find(record.phase, canonical.board.raw);
        if (!entry || entry->outcome != Outcome::Win) {
            fail(board, record.phase,
                 "verified Win source is absent from the bound TT");
        }

        std::optional<SearchAction> canonical_witness;
        if (record.selected_action) {
            canonical_witness = mapWitnessToCanonical(
                *record.selected_action, canonical.transform);
        }
        const StateKey state{canonical.board.raw, record.phase};
        if (!verified_states_.insert(state).second) {
            fail(board, record.phase,
                 "multiple completed SGF nodes claim to be the first source of one TT state");
        }
        const VerifiedExactSource source{
            canonical.board.raw,
            record.phase,
            canonical_witness,
            legal.size(),
        };
        if (!verified_by_id_.emplace(implicit_node_id, source).second) {
            fail(board, record.phase, "duplicate implicit SGF node ID in verified source table");
        }
        ++statistics_.verified_exact_source_nodes;
    }

    void recordRZoneSource(Board board,
                           const TraceRecord& record,
                           const RzProofEntry* entry,
                           const std::vector<TraceChildSummary>& children)
    {
        if (!entry || record.solved_by == SolvedBy::RZoneTable) return;

        if (record.solved_by == SolvedBy::Terminal) {
            if (entry->kind != RZoneStepKind::Terminal) {
                fail(board, record.phase,
                     "terminal SGF source references a non-terminal RZone certificate");
            }
        } else if (record.solved_by == SolvedBy::Backpropagation &&
                   record.phase == Phase::Player) {
            if (entry->kind != RZoneStepKind::FixedSlide || !entry->slide) {
                fail(board, record.phase,
                     "Player SGF source references a non-FixedSlide RZone certificate");
            }
            SearchAction expected;
            expected.kind = SearchAction::Kind::Slide;
            expected.direction = transformDirection(
                *entry->slide,
                inverseTransform(record.rzone_transform.value_or(0)));
            if (!record.selected_action || !sameAction(*record.selected_action, expected)) {
                fail(board, record.phase,
                     "Player RZone source witness differs from the certificate slide");
            }
            if (children.empty() ||
                children.back().rzone_certificate_id !=
                    std::optional<RZoneCertificateId>(entry->child) ||
                children.back().rzone_transform != record.rzone_transform) {
                fail(board, record.phase,
                     "Player RZone source does not descend from its certificate child");
            }
        } else if (record.solved_by == SolvedBy::Backpropagation &&
                   record.phase == Phase::Environment) {
            if (entry->kind != RZoneStepKind::UniversalPlacement) {
                fail(board, record.phase,
                     "Environment SGF source references a non-UniversalPlacement certificate");
            }
            const bool has_source_child = std::any_of(
                children.begin(), children.end(), [&](const TraceChildSummary& child) {
                    return child.rzone_certificate_id ==
                               std::optional<RZoneCertificateId>(entry->child) &&
                           child.rzone_transform == record.rzone_transform;
                });
            if (!has_source_child) {
                fail(board, record.phase,
                     "Environment RZone source does not contain its certificate child");
            }
        } else {
            fail(board, record.phase,
                 "RZone source annotation is not attached to a verified terminal or Win derivation");
        }

        const RZoneCertificateId id = entry->id;
        if (!rzone_sources_[static_cast<std::size_t>(id)]) {
            rzone_sources_[static_cast<std::size_t>(id)] = true;
            ++rzone_source_count_;
        }
        ++statistics_.rzone_source_annotations_checked;
    }

    TraceNodeSummary verifyNode(Board board, const TraceRecord& record, std::uint64_t depth)
    {
        ++statistics_.nodes_checked;
        statistics_.maximum_depth = std::max(statistics_.maximum_depth, depth);
        const std::uint64_t implicit_node_id = statistics_.nodes_checked;

        std::vector<TraceChildSummary> children;
        while (parser_.peekSignificant() == '(') {
            children.push_back(verifyVariation(
                board, record.phase, depth + 1));
        }

        const bool winning = boardHasWinningTile(board, manifest_.winning_tile);
        const std::vector<SearchAction> legal = winning
            ? std::vector<SearchAction>{}
            : legalActions(board, record.phase);
        const RzProofEntry* rzone_entry = checkRZoneAnnotation(board, record);

        if (record.solved_by == SolvedBy::Terminal) {
            if (record.selected_action || !record.skipped_actions.empty() ||
                !children.empty()) {
                fail(board, record.phase, "terminal node contains TT, witness, skipped, or child data");
            }
            if (winning) {
                if (record.outcome != Outcome::Win) {
                    fail(board, record.phase, "winning-tile terminal metadata is inconsistent");
                }
            } else {
                if (record.phase != Phase::Player || !legal.empty() ||
                    record.outcome != Outcome::Loss) {
                    fail(board, record.phase, "no-legal-move terminal metadata is inconsistent");
                }
            }
            ++statistics_.terminal_nodes_checked;
        } else if (record.solved_by == SolvedBy::TranspositionTable) {
            if (winning) fail(board, record.phase, "winning tile must terminate before TT lookup");
            checkTtLeaf(board, record, children, legal, implicit_node_id);
        } else if (record.solved_by == SolvedBy::RZoneTable) {
            if (winning) fail(board, record.phase, "winning tile must terminate before RZone lookup");
            if (!rzone_entry) fail(board, record.phase, "RZone leaf lacks a valid certificate");
            checkRZoneLeaf(board, record, *rzone_entry, children);
        } else if (record.solved_by == SolvedBy::NodeLimit) {
            if (winning || manifest_.node_limit == 0 || record.outcome != Outcome::Unknown ||
                !children.empty() || record.selected_action || !record.skipped_actions.empty()) {
                fail(board, record.phase, "node-limit leaf metadata is inconsistent");
            }
            ++statistics_.node_limit_leaves_checked;
        } else if (record.solved_by == SolvedBy::Backpropagation) {
            if (winning) fail(board, record.phase, "winning tile must terminate before expansion");
            checkBackpropagation(board, record, children, legal);
        } else {
            fail(board, record.phase, "node has no valid solved_by classification");
        }
        recordRZoneSource(board, record, rzone_entry, children);
        recordExactSource(board, record, legal, implicit_node_id);
        return {
            record.outcome,
            record.rzone_certificate_id,
            record.rzone_transform,
        };
    }

    const std::filesystem::path& path_;
    SgfPrefixParser& parser_;
    const ProofManifest& manifest_;
    const ProofTable& table_;
    const RzProofTable* rzone_table_ = nullptr;
    RZoneCertificateId rzone_maximum_id_ = kNoRZoneCertificate;
    TraceVerificationStatistics statistics_;
    std::unordered_map<std::uint64_t, VerifiedExactSource> verified_by_id_;
    std::unordered_set<StateKey, StateKeyHash> verified_states_;
    std::vector<bool> rzone_sources_;
    std::uint64_t rzone_source_count_ = 0;
};

} // namespace

ProofTable ProofTable::load(const std::filesystem::path& path)
{
    std::error_code size_error;
    const std::uintmax_t raw_file_size = std::filesystem::file_size(path, size_error);
    if (size_error) fileFail(path, "cannot determine file size: " + size_error.message());
    if (raw_file_size > std::numeric_limits<std::uint64_t>::max()) fileFail(path, "file is too large");
    const std::uint64_t file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < TtFile::kHeaderSize) fileFail(path, "file is smaller than the TT header");

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) fileFail(path, "cannot open TT file");
    std::array<std::uint8_t, TtFile::kHeaderSize> header{};
    readExact(input, header.data(), header.size(), path);

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) fileFail(path, "invalid magic");
    if (get16(header.data() + 8) != TtFile::kFormatVersion) fileFail(path, "unsupported format version");
    if (get16(header.data() + 10) != TtFile::kHeaderSize) fileFail(path, "unexpected header size");
    if (get16(header.data() + 12) != TtFile::kRecordSize) fileFail(path, "unexpected record size");
    const std::uint16_t flags = get16(header.data() + 14);
    if ((flags & ~(kSymmetryFlag | TtFile::kWinOnlyFlag)) != 0) {
        fileFail(path, "header contains unknown flags");
    }
    if ((flags & TtFile::kWinOnlyFlag) == 0) {
        fileFail(path, "legacy Win/Loss TT is incompatible with Win-only proof lookup");
    }
    const bool symmetry = (flags & kSymmetryFlag) != 0;
    if (get32(header.data() + 16) != kEndianMarker) fileFail(path, "invalid byte-order marker");
    if (get32(header.data() + 20) != kRulesVersion) fileFail(path, "incompatible rules version");
    if (get32(header.data() + 24) != kBoardEncodingVersion) {
        fileFail(path, "incompatible board encoding version");
    }
    const unsigned winning_tile = get32(header.data() + 28);
    if (winning_tile < 2 || (winning_tile & (winning_tile - 1)) != 0 || winning_tile > 32768) {
        fileFail(path, "invalid winning tile in header");
    }

    const std::uint64_t player_offset = get64(header.data() + 32);
    const std::uint64_t player_count = get64(header.data() + 40);
    const std::uint64_t environment_offset = get64(header.data() + 48);
    const std::uint64_t environment_count = get64(header.data() + 56);
    const std::uint64_t payload_size = get64(header.data() + 64);
    const std::uint64_t expected_checksum = get64(header.data() + 72);
    if (get32(header.data() + 80) != kChecksumAlgorithmCrc64Ecma) {
        fileFail(path, "unsupported checksum algorithm");
    }
    for (std::size_t index = 84; index < header.size(); ++index) {
        if (header[index] != 0) fileFail(path, "header reserved bytes must be zero");
    }

    if (player_count > (std::numeric_limits<std::uint64_t>::max() - TtFile::kHeaderSize) /
                           TtFile::kRecordSize ||
        environment_count > std::numeric_limits<std::uint64_t>::max() / TtFile::kRecordSize) {
        fileFail(path, "record count overflows file offsets");
    }
    const std::uint64_t player_bytes = player_count * TtFile::kRecordSize;
    const std::uint64_t environment_bytes = environment_count * TtFile::kRecordSize;
    if (player_bytes > std::numeric_limits<std::uint64_t>::max() - environment_bytes) {
        fileFail(path, "payload size overflow");
    }
    const std::uint64_t calculated_payload_size = player_bytes + environment_bytes;
    if (player_offset != TtFile::kHeaderSize ||
        environment_offset != TtFile::kHeaderSize + player_bytes ||
        payload_size != calculated_payload_size ||
        payload_size > std::numeric_limits<std::uint64_t>::max() - TtFile::kHeaderSize ||
        file_size != TtFile::kHeaderSize + payload_size) {
        fileFail(path, "section offsets, counts, or file size are inconsistent");
    }
    if (player_count > std::numeric_limits<std::size_t>::max() ||
        environment_count > std::numeric_limits<std::size_t>::max()) {
        fileFail(path, "entry count exceeds this process address space");
    }

    ProofTable table;
    table.winning_tile_ = winning_tile;
    table.symmetry_ = symmetry;
    table.player_entries_.reserve(static_cast<std::size_t>(player_count));
    table.environment_entries_.reserve(static_cast<std::size_t>(environment_count));
    std::uint64_t checksum = 0;

    auto readSection = [&](Phase phase, std::uint64_t count, auto& destination) {
        for (std::uint64_t index = 0; index < count; ++index) {
            std::array<std::uint8_t, TtFile::kRecordSize> bytes{};
            readExact(input, bytes.data(), bytes.size(), path);
            updateCrc64(checksum, bytes.data(), bytes.size());
            const std::uint64_t board_key = get64(bytes.data());
            ProofEntry entry;
            try {
                entry.outcome = decodeOutcome(bytes[8]);
                entry.witness = decodeWitness(bytes[9]);
                entry.children_total = bytes[10];
                validateProofEntry(
                    board_key, entry, phase, symmetry, bytes[11], get32(bytes.data() + 12));
            } catch (const std::exception& error) {
                fileFail(path, std::string(phase == Phase::Player ? "Player" : "Environment") +
                               " record " + std::to_string(index) + ": " + error.what());
            }
            if (!destination.emplace(board_key, std::move(entry)).second) {
                fileFail(path, std::string(phase == Phase::Player ? "Player" : "Environment") +
                               " contains a duplicate key");
            }
        }
    };

    readSection(Phase::Player, player_count, table.player_entries_);
    readSection(Phase::Environment, environment_count, table.environment_entries_);
    if (checksum != expected_checksum) fileFail(path, "payload checksum mismatch");
    if (input.peek() != std::char_traits<char>::eof()) fileFail(path, "unexpected trailing data");
    table.summary_ = {player_count, environment_count, checksum, file_size};
    return table;
}

const ProofEntry* ProofTable::find(Phase phase, std::uint64_t board_key) const
{
    const auto& entries = phase == Phase::Player ? player_entries_ : environment_entries_;
    const auto found = entries.find(board_key);
    return found == entries.end() ? nullptr : &found->second;
}

bool ProofManifest::hasArtifactBinding() const
{
    const unsigned present = static_cast<unsigned>(tt_format_version.has_value()) +
        static_cast<unsigned>(tt_payload_checksum.has_value()) +
        static_cast<unsigned>(tt_file_size.has_value()) +
        static_cast<unsigned>(tt_player_records.has_value()) +
        static_cast<unsigned>(tt_environment_records.has_value());
    if (present != 0 && present != 5) {
        throw std::runtime_error("SGF contains an incomplete TT artifact binding");
    }
    return present == 5;
}

bool ProofManifest::hasRzArtifactBinding() const
{
    const unsigned present = static_cast<unsigned>(rz_format_version.has_value()) +
        static_cast<unsigned>(rz_semantics_version.has_value()) +
        static_cast<unsigned>(rz_prefix_record_count.has_value()) +
        static_cast<unsigned>(rz_prefix_checksum.has_value());
    if (present != 0 && present != 4) {
        throw std::runtime_error("SGF contains an incomplete RZone artifact binding");
    }
    return present == 4;
}

ProofManifest readProofManifest(const std::filesystem::path& sgf_path)
{
    std::ifstream input(sgf_path, std::ios::in | std::ios::binary);
    if (!input) fileFail(sgf_path, "cannot open SGF file");
    SgfPrefixParser parser(sgf_path, input);
    parser.expect('(');
    const Properties root_properties = parser.readNode();
    if (singleProperty(root_properties, "FF", sgf_path) != "4") {
        fileFail(sgf_path, "unsupported SGF FF version");
    }
    if (singleProperty(root_properties, "SZ", sgf_path) != "4") {
        fileFail(sgf_path, "SGF board size must be 4");
    }

    const Properties first_opening = parser.readNode();
    const Properties second_opening = parser.readNode();
    const SearchAction first = decodeSgfAction(singleProperty(first_opening, "W", sgf_path), sgf_path);
    const SearchAction second = decodeSgfAction(singleProperty(second_opening, "W", sgf_path), sgf_path);
    if (first.position == second.position) fileFail(sgf_path, "fixed-opening placements overlap");

    ProofManifest manifest;
    manifest.opening[first.position] = first.tile;
    manifest.opening[second.position] = second.tile;
    manifest.root_board = boardFromTiles(manifest.opening);

    const auto fields = parseCommentFields(
        singleProperty(second_opening, "C", sgf_path), sgf_path);
    manifest.root_outcome = parseOutcome(requiredField(fields, "solver_status", sgf_path), sgf_path);
    manifest.winning_tile = parseUnsigned<unsigned>(
        requiredField(fields, "winning_tile", sgf_path), "winning_tile", sgf_path);
    manifest.node_limit = parseUnsigned<std::uint64_t>(
        requiredField(fields, "node_limit", sgf_path), "node_limit", sgf_path);
    manifest.transposition_table = parseEnabled(
        requiredField(fields, "transposition_table", sgf_path),
        "transposition_table",
        sgf_path);
    manifest.tt_symmetry = parseEnabled(
        requiredField(fields, "tt_symmetry", sgf_path), "tt_symmetry", sgf_path);
    if (manifest.tt_symmetry && !manifest.transposition_table) {
        fileFail(sgf_path, "SGF enables TT symmetry while the transposition table is disabled");
    }
    const auto persistent_tt_load = fields.find("persistent_tt_load");
    if (persistent_tt_load != fields.end()) {
        manifest.persistent_tt_load = parseEnabled(
            persistent_tt_load->second, "persistent_tt_load", sgf_path);
    }
    const auto persistent_rz_load = fields.find("persistent_rz_load");
    if (persistent_rz_load != fields.end()) {
        manifest.persistent_rz_load = parseEnabled(
            persistent_rz_load->second, "persistent_rz_load", sgf_path);
    }
    const auto rzone_symmetry = fields.find("rzone_symmetry");
    if (rzone_symmetry != fields.end()) {
        manifest.rzone_symmetry = parseEnabled(
            rzone_symmetry->second, "rzone_symmetry", sgf_path);
    } else {
        // Before RZone gained an independent setting, the logical verifier
        // followed TT symmetry. Preserve that sound behavior for old SGFs.
        manifest.rzone_symmetry = manifest.tt_symmetry;
    }
    const auto fixed_opening = fields.find("fixed_opening");
    if (fixed_opening != fields.end()) {
        std::ostringstream expected;
        expected << first.tile << '@' << first.position << ','
                 << second.tile << '@' << second.position;
        if (fixed_opening->second != expected.str()) {
            fileFail(sgf_path, "SGF fixed_opening metadata does not match its opening actions");
        }
    }
    const auto tt_file = fields.find("tt_file");
    if (tt_file != fields.end()) manifest.declared_tt_path = tt_file->second;
    const auto rz_file = fields.find("rz_file");
    if (rz_file != fields.end()) manifest.declared_rz_path = rz_file->second;

    auto optionalNumber = [&](const char* name, auto& destination) {
        const auto found = fields.find(name);
        if (found != fields.end()) {
            using Integer = typename std::decay_t<decltype(destination)>::value_type;
            destination = parseUnsigned<Integer>(found->second, name, sgf_path);
        }
    };
    optionalNumber("tt_format_version", manifest.tt_format_version);
    optionalNumber("tt_payload_checksum", manifest.tt_payload_checksum);
    optionalNumber("tt_file_size", manifest.tt_file_size);
    optionalNumber("tt_player_records", manifest.tt_player_records);
    optionalNumber("tt_environment_records", manifest.tt_environment_records);
    optionalNumber("rz_format_version", manifest.rz_format_version);
    optionalNumber("rz_semantics_version", manifest.rz_semantics_version);
    optionalNumber("rz_prefix_record_count", manifest.rz_prefix_record_count);
    optionalNumber("rz_prefix_checksum", manifest.rz_prefix_checksum);
    (void)manifest.hasArtifactBinding();
    (void)manifest.hasRzArtifactBinding();
    return manifest;
}

ProofVerificationResult verifyRootProof(const ProofManifest& manifest,
                                        const ProofTable& table,
                                        const RzProofTable* rzone_table)
{
    if (manifest.root_outcome == Outcome::Unknown) {
        throw std::runtime_error("root outcome is UNKNOWN and cannot be verified as an exact proof");
    }
    if (manifest.winning_tile != table.winningTile()) {
        throw std::runtime_error("SGF winning tile does not match TT file");
    }
    if (manifest.tt_symmetry != table.usesSymmetry()) {
        throw std::runtime_error("SGF TT symmetry mode does not match TT file");
    }

    const bool artifact_binding_verified = verifyTtArtifactBinding(manifest, table);

    const bool rzone_artifact_binding_verified =
        verifyRzArtifactBinding(manifest, rzone_table);
    const RZoneCertificateId rzone_maximum_id = rzone_artifact_binding_verified
        ? static_cast<RZoneCertificateId>(*manifest.rz_prefix_record_count)
        : kNoRZoneCertificate;
    LogicalVerifier verifier(
        table,
        rzone_artifact_binding_verified ? rzone_table : nullptr,
        rzone_maximum_id,
        manifest.rzone_symmetry);
    ProofVerificationResult result;
    result.root_outcome = manifest.root_outcome;
    result.artifact_binding_verified = artifact_binding_verified;
    result.rzone_artifact_binding_verified = rzone_artifact_binding_verified;
    result.statistics = verifier.verify(manifest.root_board, Phase::Player, manifest.root_outcome);
    return result;
}

SgfProofVerificationResult verifySgfProof(const std::filesystem::path& sgf_path,
                                          const ProofManifest& manifest,
                                          const ProofTable& table,
                                          const RzProofTable* rzone_table)
{
    if (manifest.root_outcome == Outcome::Unknown) {
        throw std::runtime_error("root outcome is UNKNOWN and cannot be verified as an exact proof");
    }
    if (manifest.persistent_tt_load || manifest.persistent_rz_load) {
        throw std::runtime_error(
            "SGF-backed source verification currently supports only one-opening artifacts "
            "without persistent TT/RZone input");
    }
    if (manifest.winning_tile != table.winningTile()) {
        throw std::runtime_error("SGF winning tile does not match TT file");
    }
    if (manifest.tt_symmetry != table.usesSymmetry()) {
        throw std::runtime_error("SGF TT symmetry mode does not match TT file");
    }
    const bool artifact_binding_verified = verifyTtArtifactBinding(manifest, table);
    const bool rzone_artifact_binding_verified =
        verifyRzArtifactBinding(manifest, rzone_table);
    const RZoneCertificateId rzone_maximum_id = rzone_artifact_binding_verified
        ? static_cast<RZoneCertificateId>(*manifest.rz_prefix_record_count)
        : kNoRZoneCertificate;

    std::ifstream input(sgf_path, std::ios::in | std::ios::binary);
    if (!input) fileFail(sgf_path, "cannot open SGF file");
    SgfPrefixParser parser(sgf_path, input);
    parser.expect('(');
    const Properties header = parser.readNode();
    if (singleProperty(header, "FF", sgf_path) != "4" ||
        singleProperty(header, "SZ", sgf_path) != "4") {
        fileFail(sgf_path, "unsupported SGF header");
    }

    const Properties first_opening = parser.readNode();
    const Properties root_properties = parser.readNode();
    const SearchAction first = decodeSgfAction(
        singleProperty(first_opening, "W", sgf_path), sgf_path);
    const SearchAction second = decodeSgfAction(
        singleProperty(root_properties, "W", sgf_path), sgf_path);
    std::array<unsigned, 16> opening{};
    if (first.position == second.position) fileFail(sgf_path, "fixed-opening placements overlap");
    opening[first.position] = first.tile;
    opening[second.position] = second.tile;
    if (opening != manifest.opening || boardFromTiles(opening) != manifest.root_board) {
        fileFail(sgf_path, "fixed opening differs from the parsed proof manifest");
    }

    TraceVerifier verifier(
        sgf_path,
        parser,
        manifest,
        table,
        rzone_artifact_binding_verified ? rzone_table : nullptr,
        rzone_maximum_id);
    const TraceVerificationStatistics statistics = verifier.verify(root_properties);
    parser.expect(')');
    parser.expectEnd();
    SgfProofVerificationResult result;
    result.root_outcome = manifest.root_outcome;
    result.artifact_binding_verified = artifact_binding_verified;
    result.rzone_artifact_binding_verified = rzone_artifact_binding_verified;
    result.statistics = statistics;
    return result;
}

TraceVerificationStatistics verifySgfTrace(const std::filesystem::path& sgf_path,
                                           const ProofManifest& manifest,
                                           const ProofTable& table,
                                           const RzProofTable* rzone_table)
{
    return verifySgfProof(sgf_path, manifest, table, rzone_table).statistics;
}

} // namespace solver2048
