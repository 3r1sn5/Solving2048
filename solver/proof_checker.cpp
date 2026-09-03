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
constexpr std::uint8_t kLossCode = 2;
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
    if (code == kLossCode) return Outcome::Loss;
    throw std::invalid_argument("outcome must be exact Win or Loss");
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
        if (entry.outcome == Outcome::Win) {
            if (!entry.witness || entry.witness->kind != SearchAction::Kind::Slide) {
                throw std::invalid_argument("Player Win entry requires a slide witness");
            }
        } else if (entry.witness) {
            throw std::invalid_argument("Player Loss entry must not contain a witness");
        }
    } else {
        if (entry.children_total == 0 || entry.children_total > 32) {
            throw std::invalid_argument("Environment children_total must be in [1, 32]");
        }
        if (entry.outcome == Outcome::Loss) {
            if (!entry.witness || entry.witness->kind != SearchAction::Kind::Placement) {
                throw std::invalid_argument("Environment Loss entry requires a placement witness");
            }
        } else if (entry.witness) {
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

bool parseBoolean(const std::string& text,
                  const std::string& field,
                  const std::filesystem::path& path)
{
    if (text == "true") return true;
    if (text == "false") return false;
    fileFail(path, "SGF field " + field + " must be true or false");
}

SolvedBy parseSolvedBy(const std::string& text, const std::filesystem::path& path)
{
    if (text == "NONE") return SolvedBy::None;
    if (text == "TERMINAL") return SolvedBy::Terminal;
    if (text == "TT") return SolvedBy::TranspositionTable;
    if (text == "BACKPROPAGATION") return SolvedBy::Backpropagation;
    if (text == "NODE_LIMIT") return SolvedBy::NodeLimit;
    fileFail(path, "invalid solved_by: " + text);
}

TerminalReason parseTerminalReason(const std::string& text,
                                   const std::filesystem::path& path)
{
    if (text == "NONE") return TerminalReason::None;
    if (text == "WINNING_TILE") return TerminalReason::WinningTile;
    if (text == "NO_LEGAL_MOVE") return TerminalReason::NoLegalMove;
    if (text == "NODE_LIMIT") return TerminalReason::NodeLimit;
    if (text == "TRANSPOSITION_TABLE") return TerminalReason::TranspositionTable;
    fileFail(path, "invalid terminal_reason: " + text);
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
    if (open == std::string::npos || text.empty() || text.back() != ')' || open + 1 == text.size() - 1) {
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
    TerminalReason terminal_reason = TerminalReason::None;
    Phase phase = Phase::Player;
    bool expanded = false;
    std::uint64_t node_id = 0;
    std::uint64_t dfs_visit_order = 0;
    std::uint64_t dfs_order = 0;
    std::uint64_t children_total = 0;
    std::uint64_t children_visited = 0;
    std::uint64_t children_expanded = 0;
    std::uint64_t children_skipped = 0;
    bool tt_hit = false;
    std::optional<std::string> tt_source;
    std::optional<Phase> tt_phase;
    std::optional<std::uint64_t> tt_key;
    std::optional<std::uint64_t> tt_source_node_id;
    std::optional<std::uint8_t> tt_transform;
    std::optional<SearchAction> selected_action;
    std::vector<SearchAction> skipped_actions;
    std::optional<int> reward;
};

TraceRecord parseTraceRecord(const Properties& properties,
                             const std::filesystem::path& path)
{
    const auto fields = parseCommentFields(singleProperty(properties, "C", path), path);
    TraceRecord record;
    record.outcome = parseOutcome(requiredField(fields, "solver_status", path), path);
    record.solved_by = parseSolvedBy(requiredField(fields, "solved_by", path), path);
    record.terminal_reason = parseTerminalReason(
        requiredField(fields, "terminal_reason", path), path);
    const std::string& phase = requiredField(fields, "node_type", path);
    if (phase == "PLAYER") record.phase = Phase::Player;
    else if (phase == "ENVIRONMENT") record.phase = Phase::Environment;
    else fileFail(path, "invalid node_type: " + phase);
    record.expanded = parseBoolean(requiredField(fields, "expanded", path), "expanded", path);
    record.node_id = parseUnsigned<std::uint64_t>(
        requiredField(fields, "node_id", path), "node_id", path);
    record.dfs_visit_order = parseUnsigned<std::uint64_t>(
        requiredField(fields, "dfs_visit_order", path), "dfs_visit_order", path);
    record.dfs_order = parseUnsigned<std::uint64_t>(
        requiredField(fields, "dfs_order", path), "dfs_order", path);
    record.children_total = parseUnsigned<std::uint64_t>(
        requiredField(fields, "children_total", path), "children_total", path);
    record.children_visited = parseUnsigned<std::uint64_t>(
        requiredField(fields, "children_visited", path), "children_visited", path);
    record.children_expanded = parseUnsigned<std::uint64_t>(
        requiredField(fields, "children_expanded", path), "children_expanded", path);
    record.children_skipped = parseUnsigned<std::uint64_t>(
        requiredField(fields, "children_skipped", path), "children_skipped", path);

    const auto selected = fields.find("selected_action");
    if (selected != fields.end()) {
        record.selected_action = parseActionToken(selected->second, "selected_action", path);
    }
    const auto skipped = fields.find("children_skipped_actions");
    if (skipped != fields.end()) {
        record.skipped_actions = parseActionList(
            skipped->second, "children_skipped_actions", path);
    }
    const auto reward = fields.find("reward");
    if (reward != fields.end()) {
        int value = 0;
        const std::string_view text = reward->second;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            fileFail(path, "SGF field reward is not a valid integer: " + reward->second);
        }
        record.reward = value;
    }

    const auto tt_hit = fields.find("tt_hit");
    if (tt_hit != fields.end()) record.tt_hit = parseBoolean(tt_hit->second, "tt_hit", path);
    const auto tt_source = fields.find("tt_source");
    if (tt_source != fields.end()) record.tt_source = tt_source->second;
    const auto tt_phase = fields.find("tt_phase");
    if (tt_phase != fields.end()) {
        if (tt_phase->second == "PLAYER") record.tt_phase = Phase::Player;
        else if (tt_phase->second == "ENVIRONMENT") record.tt_phase = Phase::Environment;
        else fileFail(path, "invalid tt_phase: " + tt_phase->second);
    }
    const auto tt_key = fields.find("tt_key");
    if (tt_key != fields.end()) {
        record.tt_key = parseUnsigned<std::uint64_t>(tt_key->second, "tt_key", path);
    }
    const auto source = fields.find("tt_source_node_id");
    if (source != fields.end()) {
        record.tt_source_node_id = parseUnsigned<std::uint64_t>(
            source->second, "tt_source_node_id", path);
    }
    const auto transform = fields.find("tt_transform");
    if (transform != fields.end()) {
        const std::size_t colon = transform->second.find(':');
        const std::string number = transform->second.substr(0, colon);
        const unsigned value = parseUnsigned<unsigned>(number, "tt_transform", path);
        if (value >= 8) fileFail(path, "SGF tt_transform must be in [0, 7]");
        record.tt_transform = static_cast<std::uint8_t>(value);
        if (colon != std::string::npos &&
            transform->second.substr(colon + 1) != symmetryTransformName(record.tt_transform.value())) {
            fileFail(path, "SGF tt_transform number and name disagree");
        }
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

class LogicalVerifier {
public:
    explicit LogicalVerifier(const ProofTable& table) : table_(table) {}

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
    ProofVerificationStatistics statistics_;
    std::unordered_set<StateKey, StateKeyHash> visiting_;
    std::unordered_set<StateKey, StateKeyHash> verified_;
};

struct TraceChildSummary {
    SearchAction action;
    Outcome outcome = Outcome::Unknown;
    bool expanded = false;
};

struct TraceNodeSummary {
    Outcome outcome = Outcome::Unknown;
    bool expanded = false;
};

class TraceVerifier {
public:
    TraceVerifier(const std::filesystem::path& path,
                  SgfPrefixParser& parser,
                  const ProofManifest& manifest,
                  const ProofTable& table)
        : path_(path),
          parser_(parser),
          manifest_(manifest),
          table_(table) {}

    TraceVerificationStatistics verify(const Properties& root_properties)
    {
        const TraceRecord root = parseTraceRecord(root_properties, path_);
        if (root.dfs_order != 0) fail(manifest_.root_board, Phase::Player, "root dfs_order must be zero");
        const TraceNodeSummary summary = verifyNode(manifest_.root_board, root, 0);
        if (summary.outcome != manifest_.root_outcome) {
            fail(manifest_.root_board, Phase::Player,
                 "root trace outcome does not match the SGF manifest");
        }
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
                       const SearchAction& action,
                       std::optional<int>& expected_reward)
    {
        if (parent_phase == Phase::Player) {
            const std::vector<SlideResult> slides = generateLegalSlides(parent);
            const auto found = std::find_if(slides.begin(), slides.end(), [&](const SlideResult& slide) {
                return slide.direction == action.direction;
            });
            if (found == slides.end()) fail(parent, parent_phase, "SGF contains an illegal slide action");
            expected_reward = found->reward;
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
        expected_reward.reset();
        return found->board;
    }

    TraceChildSummary verifyVariation(Board parent,
                                      Phase parent_phase,
                                      std::uint64_t sibling_order,
                                      std::uint64_t depth)
    {
        parser_.expect('(');
        const Properties properties = parser_.readNode();
        const SearchAction action = readIncomingAction(properties, parent_phase);
        std::optional<int> expected_reward;
        const Board board = replayAction(parent, parent_phase, action, expected_reward);
        ++statistics_.actions_replayed;

        const TraceRecord record = parseTraceRecord(properties, path_);
        const Phase expected_phase = parent_phase == Phase::Player
            ? Phase::Environment
            : Phase::Player;
        if (record.phase != expected_phase) {
            fail(board, record.phase, "child node_type does not alternate with its parent");
        }
        if (record.dfs_order != sibling_order) {
            fail(board, record.phase, "dfs_order is not the visited sibling order");
        }
        if (parent_phase == Phase::Player) {
            if (!record.reward || *record.reward != *expected_reward) {
                fail(board, record.phase, "slide reward does not match independent replay");
            }
        } else if (record.reward) {
            fail(board, record.phase, "placement child must not contain a reward");
        }

        const TraceNodeSummary child = verifyNode(board, record, depth);
        if (parser_.peekSignificant() != ')') {
            fail(board, record.phase, "variation must contain exactly one trace node");
        }
        parser_.expect(')');
        return {action, child.outcome, child.expanded};
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

    void checkCounts(Board board,
                     const TraceRecord& record,
                     const std::vector<TraceChildSummary>& children) const
    {
        if (record.children_visited != children.size()) {
            fail(board, record.phase, "children_visited does not match SGF child variations");
        }
        const std::uint64_t expanded = static_cast<std::uint64_t>(std::count_if(
            children.begin(), children.end(), [](const TraceChildSummary& child) {
                return child.expanded;
            }));
        if (record.children_expanded != expanded) {
            fail(board, record.phase, "children_expanded does not match child records");
        }
        if (record.children_skipped != record.skipped_actions.size()) {
            fail(board, record.phase,
                 "children_skipped does not match children_skipped_actions");
        }
    }

    void checkActionPartition(Board board,
                              const TraceRecord& record,
                              const std::vector<TraceChildSummary>& children,
                              const std::vector<SearchAction>& legal)
    {
        if (record.children_total != legal.size()) {
            fail(board, record.phase, "children_total does not match generated legal actions");
        }
        if (children.size() + record.skipped_actions.size() != legal.size()) {
            fail(board, record.phase, "visited and skipped actions do not cover children_total");
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

    void checkBackpropagation(Board board,
                              const TraceRecord& record,
                              const std::vector<TraceChildSummary>& children,
                              const std::vector<SearchAction>& legal)
    {
        if (!record.expanded || record.terminal_reason != TerminalReason::None || record.tt_hit) {
            fail(board, record.phase, "BACKPROPAGATION node metadata is inconsistent");
        }
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
                     const std::vector<SearchAction>& legal)
    {
        if (!manifest_.transposition_table || record.expanded || !record.tt_hit ||
            record.terminal_reason != TerminalReason::TranspositionTable || !children.empty() ||
            record.children_visited != 0 || record.children_expanded != 0 ||
            record.children_skipped != 0 || !record.skipped_actions.empty()) {
            fail(board, record.phase, "TT leaf metadata or child counts are inconsistent");
        }
        if (!record.tt_source || (*record.tt_source != "trace" && *record.tt_source != "persistent")) {
            fail(board, record.phase, "TT leaf has an invalid or missing tt_source");
        }
        if (!record.tt_phase || *record.tt_phase != record.phase) {
            fail(board, record.phase, "TT leaf tt_phase does not match node_type");
        }
        if (*record.tt_source == "trace") {
            if (!record.tt_source_node_id || *record.tt_source_node_id >= record.node_id) {
                fail(board, record.phase, "trace TT source must be an earlier node_id");
            }
        } else if (record.tt_source_node_id) {
            fail(board, record.phase, "persistent TT leaf must not name a trace source node");
        }

        const CanonicalBoard canonical = table_.usesSymmetry()
            ? canonicalize(board)
            : CanonicalBoard{board, 0};
        if (!record.tt_key || *record.tt_key != canonical.board.raw) {
            fail(board, record.phase, "TT leaf key does not match the independently canonicalized board");
        }
        if (table_.usesSymmetry()) {
            if (!record.tt_transform || *record.tt_transform != canonical.transform) {
                fail(board, record.phase, "TT leaf transform does not match canonicalization");
            }
        }
        const ProofEntry* entry = table_.find(record.phase, canonical.board.raw);
        if (!entry) fail(board, record.phase, "TT leaf key is absent from the supplied TT file");
        if (entry->outcome != record.outcome || entry->children_total != legal.size() ||
            record.children_total != legal.size()) {
            fail(board, record.phase, "TT leaf claim differs from the supplied TT entry or game rules");
        }
        std::optional<SearchAction> witness;
        if (entry->witness) witness = mapWitnessToCurrent(*entry->witness, canonical.transform);
        checkSelected(board, record, witness);
        ++statistics_.tt_leaves_checked;
    }

    TraceNodeSummary verifyNode(Board board, const TraceRecord& record, std::uint64_t depth)
    {
        ++statistics_.nodes_checked;
        statistics_.maximum_depth = std::max(statistics_.maximum_depth, depth);
        const std::uint64_t expected_id = statistics_.nodes_checked;
        if (record.node_id != expected_id || record.dfs_visit_order != expected_id) {
            fail(board, record.phase, "node_id/dfs_visit_order is not continuous DFS preorder");
        }

        std::vector<TraceChildSummary> children;
        while (parser_.peekSignificant() == '(') {
            children.push_back(verifyVariation(
                board, record.phase, static_cast<std::uint64_t>(children.size()), depth + 1));
        }
        checkCounts(board, record, children);

        const bool winning = boardHasWinningTile(board, manifest_.winning_tile);
        const std::vector<SearchAction> legal = winning
            ? std::vector<SearchAction>{}
            : legalActions(board, record.phase);

        if (record.solved_by == SolvedBy::Terminal) {
            if (record.tt_hit || record.selected_action || !record.skipped_actions.empty() ||
                !children.empty()) {
                fail(board, record.phase, "terminal node contains TT, witness, skipped, or child data");
            }
            if (winning) {
                if (record.outcome != Outcome::Win ||
                    record.terminal_reason != TerminalReason::WinningTile || record.expanded ||
                    record.children_total != 0) {
                    fail(board, record.phase, "winning-tile terminal metadata is inconsistent");
                }
            } else {
                if (record.phase != Phase::Player || !legal.empty() ||
                    record.outcome != Outcome::Loss ||
                    record.terminal_reason != TerminalReason::NoLegalMove || !record.expanded ||
                    record.children_total != 0) {
                    fail(board, record.phase, "no-legal-move terminal metadata is inconsistent");
                }
            }
            ++statistics_.terminal_nodes_checked;
        } else if (record.solved_by == SolvedBy::TranspositionTable) {
            if (winning) fail(board, record.phase, "winning tile must terminate before TT lookup");
            checkTtLeaf(board, record, children, legal);
        } else if (record.solved_by == SolvedBy::NodeLimit) {
            if (winning || manifest_.node_limit == 0 || record.outcome != Outcome::Unknown ||
                record.terminal_reason != TerminalReason::NodeLimit || record.expanded ||
                record.tt_hit || record.children_total != 0 || !children.empty() ||
                record.selected_action || !record.skipped_actions.empty()) {
                fail(board, record.phase, "node-limit leaf metadata is inconsistent");
            }
            ++statistics_.node_limit_leaves_checked;
        } else if (record.solved_by == SolvedBy::Backpropagation) {
            if (winning) fail(board, record.phase, "winning tile must terminate before expansion");
            checkBackpropagation(board, record, children, legal);
        } else {
            fail(board, record.phase, "node has no valid solved_by classification");
        }
        return {record.outcome, record.expanded};
    }

    const std::filesystem::path& path_;
    SgfPrefixParser& parser_;
    const ProofManifest& manifest_;
    const ProofTable& table_;
    TraceVerificationStatistics statistics_;
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
    if ((flags & ~kSymmetryFlag) != 0) fileFail(path, "header contains unknown flags");
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
    if (requiredField(fields, "node_type", sgf_path) != "PLAYER") {
        fileFail(sgf_path, "fixed-opening root node_type must be PLAYER");
    }
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
    (void)manifest.hasArtifactBinding();
    return manifest;
}

ProofVerificationResult verifyRootProof(const ProofManifest& manifest,
                                        const ProofTable& table)
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

    bool artifact_binding_verified = false;
    if (manifest.hasArtifactBinding()) {
        const TtFileSummary& summary = table.summary();
        if (*manifest.tt_format_version != TtFile::kFormatVersion ||
            *manifest.tt_payload_checksum != summary.payload_checksum ||
            *manifest.tt_file_size != summary.file_size ||
            *manifest.tt_player_records != summary.player_entries ||
            *manifest.tt_environment_records != summary.environment_entries) {
            throw std::runtime_error("SGF TT artifact binding does not match the supplied TT file");
        }
        artifact_binding_verified = true;
    }

    LogicalVerifier verifier(table);
    ProofVerificationResult result;
    result.root_outcome = manifest.root_outcome;
    result.artifact_binding_verified = artifact_binding_verified;
    result.statistics = verifier.verify(manifest.root_board, Phase::Player, manifest.root_outcome);
    return result;
}

TraceVerificationStatistics verifySgfTrace(const std::filesystem::path& sgf_path,
                                           const ProofManifest& manifest,
                                           const ProofTable& table)
{
    if (manifest.winning_tile != table.winningTile()) {
        throw std::runtime_error("SGF winning tile does not match TT file");
    }
    if (manifest.tt_symmetry != table.usesSymmetry()) {
        throw std::runtime_error("SGF TT symmetry mode does not match TT file");
    }
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

    TraceVerifier verifier(sgf_path, parser, manifest, table);
    const TraceVerificationStatistics statistics = verifier.verify(root_properties);
    parser.expect(')');
    parser.expectEnd();
    return statistics;
}

} // namespace solver2048
