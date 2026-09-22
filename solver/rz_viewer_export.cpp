#include "game_api.h"
#include "proof_sequence.h"
#include "rz_proof_file.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace solver2048 {
namespace {

using Properties = std::unordered_map<std::string, std::vector<std::string>>;

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& message)
{
    throw std::runtime_error(path.string() + ": " + message);
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
        fail(path, field + " is not a valid unsigned integer: " + text);
    }
    return result;
}

std::unordered_map<std::string, std::string> parseComment(const std::string& comment)
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
        if (!key.empty()) fields.try_emplace(key, trim(line.substr(delimiter + 1)));
    }
    return fields;
}

class SgfParser {
public:
    SgfParser(const std::filesystem::path& path, std::istream& input)
        : path_(path), input_(input) {}

    int peek()
    {
        skipWhitespace();
        return input_.peek();
    }

    void expect(char expected)
    {
        skipWhitespace();
        const int actual = input_.get();
        ++offset_;
        if (actual != expected) {
            fail(path_, "SGF byte " + std::to_string(offset_) + ": expected '" +
                            std::string(1, expected) + "'");
        }
    }

    Properties readNode()
    {
        expect(';');
        Properties result;
        while (true) {
            skipWhitespace();
            const int next = input_.peek();
            if (next == std::char_traits<char>::eof() || next == ';' || next == '(' ||
                next == ')') {
                return result;
            }
            if (!std::isupper(static_cast<unsigned char>(next))) {
                fail(path_, "SGF byte " + std::to_string(offset_) +
                                ": expected a property name");
            }
            std::string name;
            while (std::isupper(static_cast<unsigned char>(input_.peek()))) {
                name.push_back(static_cast<char>(input_.get()));
                ++offset_;
            }
            skipWhitespace();
            std::vector<std::string> values;
            while (input_.peek() == '[') values.push_back(readValue());
            if (values.empty() || !result.emplace(name, std::move(values)).second) {
                fail(path_, "invalid or duplicate SGF property " + name);
            }
        }
    }

    void expectEnd()
    {
        skipWhitespace();
        if (input_.peek() != std::char_traits<char>::eof()) {
            fail(path_, "unexpected data after SGF tree");
        }
    }

private:
    std::string readValue()
    {
        expect('[');
        std::string result;
        while (true) {
            const int value = input_.get();
            ++offset_;
            if (value == std::char_traits<char>::eof()) fail(path_, "unterminated SGF value");
            if (value == ']') return result;
            if (value == '\\') {
                const int escaped = input_.get();
                ++offset_;
                if (escaped == std::char_traits<char>::eof()) fail(path_, "unterminated SGF escape");
                if (escaped == '\r') {
                    if (input_.peek() == '\n') {
                        input_.get();
                        ++offset_;
                    }
                    continue;
                }
                if (escaped == '\n') continue;
                result.push_back(static_cast<char>(escaped));
            } else {
                result.push_back(static_cast<char>(value));
            }
        }
    }

    void skipWhitespace()
    {
        while (input_.peek() != std::char_traits<char>::eof() &&
               std::isspace(static_cast<unsigned char>(input_.peek()))) {
            input_.get();
            ++offset_;
        }
    }

    const std::filesystem::path& path_;
    std::istream& input_;
    std::uint64_t offset_ = 0;
};

struct Action {
    bool placement = false;
    unsigned opcode = 0;
    int position = -1;
    unsigned tile = 0;
};

Action decodeAction(char player, const std::string& text, const std::filesystem::path& path)
{
    const unsigned id = parseUnsigned<unsigned>(text, "SGF action", path);
    if (player == 'B' && id <= 3) return {false, id, -1, 0};
    if (player == 'W' && id >= 4 && id <= 35) {
        const bool four = id >= 20;
        return {true, 0, static_cast<int>(id - (four ? 20 : 4)), four ? 4u : 2u};
    }
    fail(path, "action/player combination is invalid: " + std::string(1, player) + text);
}

std::string actionName(const Action& action)
{
    if (!action.placement) return directionName(static_cast<Direction>(action.opcode));
    return std::to_string(action.tile) + "@" + std::to_string(action.position);
}

Board applyAction(Board board, const Action& action, const std::filesystem::path& path)
{
    if (!action.placement) {
        for (const SlideResult& slide : generateLegalSlides(board)) {
            if (directionOpcode(slide.direction) == static_cast<int>(action.opcode)) {
                return slide.afterstate;
            }
        }
    } else {
        for (const PlacementResult& placement : generateLegalPlacements(board)) {
            if (placement.position == action.position && placement.tile == action.tile) {
                return placement.board;
            }
        }
    }
    fail(path, "SGF contains an illegal action " + actionName(action));
}

std::string hexValue(std::uint64_t value, unsigned width)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0')
           << value;
    return output.str();
}

struct Occurrence {
    RZoneCertificateId certificate = kNoRZoneCertificate;
    std::size_t run = 0;
    std::uint64_t node = 0;
    std::uint64_t parent = 0;
    unsigned depth = 0;
    Phase phase = Phase::Player;
    Board board;
    std::string outcome;
    std::string solved_by;
    std::uint8_t transform = 0;
    std::uint16_t candidate_mask = 0;
    std::string action;
};

class OccurrenceSampler {
public:
    OccurrenceSampler(std::vector<Occurrence>& output, std::uint64_t limit)
        : output_(output), limit_(limit) {}

    void consider(Occurrence occurrence)
    {
        ++total_;
        if (limit_ == 0 || output_.size() < limit_) {
            output_.push_back(std::move(occurrence));
            return;
        }
        // Deterministic reservoir sampling keeps coverage across a very large
        // DFS trace without making viewer JSON size proportional to the SGF.
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 7;
        random_state_ ^= random_state_ << 17;
        const std::uint64_t replacement = random_state_ % total_;
        if (replacement < limit_) {
            output_[static_cast<std::size_t>(replacement)] = std::move(occurrence);
        }
    }

    std::uint64_t total() const { return total_; }
    std::uint64_t limit() const { return limit_; }
    bool truncated() const { return limit_ != 0 && total_ > limit_; }

private:
    std::vector<Occurrence>& output_;
    std::uint64_t limit_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t random_state_ = 0x9e3779b97f4a7c15ull;
};

struct Run {
    std::filesystem::path path;
    Board opening;
    std::string outcome;
    std::unordered_map<std::string, std::string> statistics;
    std::uint64_t annotated_nodes = 0;
};

struct HitStatistics {
    explicit HitStatistics(std::uint64_t record_count)
        : by_certificate(static_cast<std::size_t>(record_count + 1), 0) {}

    std::vector<std::uint64_t> by_certificate;
    std::array<std::uint64_t, 17> mask_cells{};
    std::uint64_t total = 0;
    std::uint64_t certificates_reused = 0;
    std::uint64_t player = 0;
    std::uint64_t environment = 0;
    std::uint64_t exact_orientation = 0;
    std::uint64_t symmetry = 0;
};

struct ParseContext {
    Board board;
    bool search_started = false;
    std::uint64_t trace_node = 0;
    unsigned depth = 0;
};

class SgfExtractor {
public:
    SgfExtractor(const std::filesystem::path& path,
                 std::size_t run_index,
                 const RzProofTable& table,
                 OccurrenceSampler& occurrences,
                 HitStatistics& hit_statistics,
                 bool hits_only)
        : path_(path),
          run_index_(run_index),
          table_(table),
          occurrences_(occurrences),
          hit_statistics_(hit_statistics),
          hits_only_(hits_only) {}

    Run extract()
    {
        input_.open(path_, std::ios::binary);
        if (!input_) fail(path_, "cannot open SGF");
        parser_.emplace(path_, input_);
        ParseContext context;
        parseTree(context);
        parser_->expectEnd();
        if (!run_initialized_) fail(path_, "SGF has no solver root metadata");
        return std::move(run_);
    }

private:
    ParseContext processNode(ParseContext context, const Properties& properties)
    {
        std::optional<Action> action;
        char player = 0;
        for (const char candidate : {'B', 'W'}) {
            const auto found = properties.find(std::string(1, candidate));
            if (found == properties.end()) continue;
            if (action || found->second.size() != 1) fail(path_, "SGF node has invalid actions");
            player = candidate;
            action = decodeAction(candidate, found->second.front(), path_);
        }
        if (action) context.board = applyAction(context.board, *action, path_);

        std::unordered_map<std::string, std::string> fields;
        const auto comment = properties.find("C");
        if (comment != properties.end() && comment->second.size() == 1) {
            fields = parseComment(comment->second.front());
        }
        const bool solver_root = !context.search_started && fields.count("solver_status") != 0 &&
                                 fields.count("winning_tile") != 0;
        const std::uint64_t parent = context.trace_node;
        if (solver_root) {
            context.search_started = true;
            context.trace_node = ++next_trace_node_;
            context.depth = 0;
            run_initialized_ = true;
            run_.path = std::filesystem::absolute(path_).lexically_normal();
            run_.opening = context.board;
            run_.outcome = fields["solver_status"];
            const auto prefix_count = fields.find("rz_prefix_record_count");
            const auto prefix_checksum = fields.find("rz_prefix_checksum");
            if ((prefix_count == fields.end()) != (prefix_checksum == fields.end())) {
                fail(path_, "SGF has an incomplete RZone prefix binding");
            }
            if (prefix_count != fields.end()) {
                const std::uint64_t count = parseUnsigned<std::uint64_t>(
                    prefix_count->second, "rz_prefix_record_count", path_);
                const std::uint64_t checksum = parseUnsigned<std::uint64_t>(
                    prefix_checksum->second, "rz_prefix_checksum", path_);
                if (count > table_.summary().record_count ||
                    table_.prefixChecksum(count) != checksum) {
                    fail(path_, "SGF RZone prefix does not match the supplied snapshot");
                }
            }
            for (const auto& [key, value] : fields) {
                if (key.rfind("rzone_", 0) == 0 || key == "player_nodes" ||
                    key == "environment_nodes" || key == "elapsed_seconds") {
                    run_.statistics.emplace(key, value);
                }
            }
        } else if (context.search_started && action) {
            context.trace_node = ++next_trace_node_;
            ++context.depth;
        }
        if (context.search_started && context.trace_node != 0 &&
            context.trace_node % 1000000 == 0) {
            std::cerr << "SGF " << run_index_ + 1 << ": replayed "
                      << context.trace_node << " trace nodes\n";
        }

        const auto id_field = fields.find("rzone_certificate_id");
        if (context.search_started && id_field != fields.end()) {
            ++run_.annotated_nodes;
            const std::string solved_by =
                fields.count("solved_by") ? fields["solved_by"] : "UNKNOWN";
            const RZoneCertificateId id =
                parseUnsigned<RZoneCertificateId>(id_field->second, "rzone_certificate_id", path_);
            const RzProofEntry* certificate = table_.find(id);
            if (!certificate) fail(path_, "SGF references an RZone certificate outside the snapshot");
            const auto transform_field = fields.find("rzone_transform");
            const unsigned transform = transform_field == fields.end()
                ? 0
                : parseUnsigned<unsigned>(transform_field->second, "rzone_transform", path_);
            if (transform >= 8) fail(path_, "rzone_transform must be in [0, 7]");
            const Phase node_phase =
                solver_root || player == 'W' ? Phase::Player : Phase::Environment;
            if (!RzProofTable::matches(
                    *certificate, context.board, node_phase,
                    static_cast<std::uint8_t>(transform))) {
                fail(path_, "SGF RZone annotation does not match its replayed board");
            }
            if (solved_by == "RZONE_TT") {
                std::uint64_t& certificate_hits =
                    hit_statistics_.by_certificate[static_cast<std::size_t>(id)];
                if (certificate_hits++ == 0) ++hit_statistics_.certificates_reused;
                ++hit_statistics_.total;
                ++hit_statistics_.mask_cells[
                    static_cast<unsigned>(__builtin_popcount(certificate->mask))];
                if (node_phase == Phase::Player) ++hit_statistics_.player;
                else ++hit_statistics_.environment;
                if (transform == 0) ++hit_statistics_.exact_orientation;
                else ++hit_statistics_.symmetry;
            }
            if (hits_only_ && solved_by != "RZONE_TT") return context;
            std::uint16_t candidate_mask = 0;
            const std::uint8_t inverse = inverseTransform(static_cast<std::uint8_t>(transform));
            for (int position = 0; position < 16; ++position) {
                if ((certificate->mask & (std::uint16_t{1} << position)) == 0) continue;
                candidate_mask |= static_cast<std::uint16_t>(
                    std::uint16_t{1} << transformPosition(position, inverse));
            }
            Occurrence occurrence;
            occurrence.certificate = id;
            occurrence.run = run_index_;
            occurrence.node = context.trace_node;
            occurrence.parent = solver_root ? 0 : parent;
            occurrence.depth = context.depth;
            occurrence.phase = node_phase;
            occurrence.board = context.board;
            occurrence.outcome = fields.count("solver_status") ? fields["solver_status"] : "UNKNOWN";
            occurrence.solved_by = solved_by;
            occurrence.transform = static_cast<std::uint8_t>(transform);
            occurrence.candidate_mask = candidate_mask;
            occurrence.action = solver_root || !action ? "root" : actionName(*action);
            occurrences_.consider(std::move(occurrence));
        }
        return context;
    }

    void parseTree(ParseContext inherited)
    {
        parser_->expect('(');
        ParseContext current = inherited;
        bool has_node = false;
        while (parser_->peek() == ';') {
            current = processNode(current, parser_->readNode());
            has_node = true;
        }
        if (!has_node) fail(path_, "empty SGF game tree");
        while (parser_->peek() == '(') parseTree(current);
        parser_->expect(')');
    }

    const std::filesystem::path& path_;
    std::size_t run_index_;
    const RzProofTable& table_;
    OccurrenceSampler& occurrences_;
    HitStatistics& hit_statistics_;
    bool hits_only_ = false;
    std::ifstream input_;
    std::optional<SgfParser> parser_;
    std::uint64_t next_trace_node_ = 0;
    bool run_initialized_ = false;
    Run run_;
};

class JsonWriter {
public:
    explicit JsonWriter(std::ostream& output) : output_(output) {}

    void beginObject() { begin('{', true); }
    void endObject() { end('}'); }
    void beginArray() { begin('[', false); }
    void endArray() { end(']'); }
    void key(const std::string& key)
    {
        comma();
        string(key);
        output_ << ':';
        after_key_ = true;
    }
    void value(const std::string& value) { prefixValue(); string(value); }
    void value(const char* value) { this->value(std::string(value)); }
    void value(std::uint64_t value) { prefixValue(); output_ << value; }
    void value(std::int64_t value) { prefixValue(); output_ << value; }
    void value(bool value) { prefixValue(); output_ << (value ? "true" : "false"); }

private:
    struct Context { bool object; bool first = true; };
    void begin(char token, bool object)
    {
        prefixValue();
        output_ << token;
        stack_.push_back({object, true});
    }
    void end(char token)
    {
        if (stack_.empty()) throw std::logic_error("JSON container mismatch");
        stack_.pop_back();
        output_ << token;
    }
    void prefixValue()
    {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!stack_.empty() && !stack_.back().object) comma();
    }
    void comma()
    {
        if (stack_.empty()) return;
        if (!stack_.back().first) output_ << ',';
        stack_.back().first = false;
    }
    void string(const std::string& value)
    {
        output_ << '"';
        for (unsigned char character : value) {
            switch (character) {
                case '"': output_ << "\\\""; break;
                case '\\': output_ << "\\\\"; break;
                case '\b': output_ << "\\b"; break;
                case '\f': output_ << "\\f"; break;
                case '\n': output_ << "\\n"; break;
                case '\r': output_ << "\\r"; break;
                case '\t': output_ << "\\t"; break;
                default:
                    if (character < 0x20) {
                        output_ << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<unsigned>(character) << std::dec << std::setfill(' ');
                    } else {
                        output_ << static_cast<char>(character);
                    }
            }
        }
        output_ << '"';
    }

    std::ostream& output_;
    std::vector<Context> stack_;
    bool after_key_ = false;
};

const char* kindName(RZoneStepKind kind)
{
    switch (kind) {
        case RZoneStepKind::Terminal: return "terminal";
        case RZoneStepKind::FixedSlide: return "fixed_slide";
        case RZoneStepKind::UniversalPlacement: return "universal_placement";
    }
    return "unknown";
}

const char* phaseLabel(Phase phase)
{
    return phase == Phase::Player ? "player" : "environment";
}

int argument(const RzProofEntry& entry)
{
    if (entry.kind == RZoneStepKind::Terminal) return entry.terminal_position;
    if (entry.kind == RZoneStepKind::FixedSlide && entry.slide) {
        return directionOpcode(*entry.slide);
    }
    return -1;
}

struct TableStatistics {
    std::array<std::uint64_t, 3> kind{};
    std::array<std::uint64_t, 2> phase{};
    std::array<std::uint64_t, 17> mask_cells{};
    std::uint64_t maximum_depth = 0;
    std::uint64_t total_depth = 0;
    std::unordered_set<std::uint16_t> unique_masks;
};

TableStatistics tableStatistics(const RzProofTable& table)
{
    TableStatistics stats;
    for (std::uint64_t id = 1; id <= table.summary().record_count; ++id) {
        const RzProofEntry* entry = table.find(static_cast<RZoneCertificateId>(id));
        if (!entry) throw std::logic_error("RZone IDs are not contiguous");
        const unsigned cells = static_cast<unsigned>(__builtin_popcount(entry->mask));
        ++stats.kind[entry->kind == RZoneStepKind::Terminal
                         ? 0
                         : entry->kind == RZoneStepKind::FixedSlide ? 1 : 2];
        ++stats.phase[entry->phase == Phase::Player ? 0 : 1];
        ++stats.mask_cells[cells];
        stats.maximum_depth = std::max<std::uint64_t>(stats.maximum_depth, entry->depth);
        stats.total_depth += entry->depth;
        stats.unique_masks.insert(entry->mask);
    }
    return stats;
}

struct Options {
    std::filesystem::path rz_path;
    std::filesystem::path output_path;
    std::vector<std::filesystem::path> sgf_paths;
    std::filesystem::path sequence_path;
    bool all_certificates = false;
    bool hits_only = false;
    std::uint64_t max_occurrences = 50000;
    std::uint64_t top_hits = 1000;
};

void printHelp(const char* program)
{
    std::cout
        << "Usage: " << program << " --rz PATH --output PATH [options]\n\n"
        << "Export an RZone snapshot and optional SGF occurrences for the static viewer.\n\n"
        << "  --rz PATH              Validated persistent RZone snapshot\n"
        << "  --sgf PATH             Add an SGF; may be repeated\n"
        << "  --sequence PATH        Read SGF/RZone paths from proof_sequence.txt\n"
        << "  --output PATH          Viewer JSON output\n"
        << "  --all-certificates     Include the complete table even with SGFs\n"
        << "  --hits-only            Export only solved_by=RZONE_TT occurrences\n"
        << "  --max-occurrences N    Reservoir sample limit (default 50000; 0 unlimited)\n"
        << "  --top-hits N           Include N most-reused certificates (default 1000)\n"
        << "  -h, --help             Show this help\n";
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument("missing value after " + option);
            return argv[index];
        };
        if (option == "--rz") options.rz_path = value();
        else if (option == "--output") options.output_path = value();
        else if (option == "--sgf") options.sgf_paths.emplace_back(value());
        else if (option == "--sequence") options.sequence_path = value();
        else if (option == "--all-certificates") options.all_certificates = true;
        else if (option == "--hits-only") options.hits_only = true;
        else if (option == "--max-occurrences") {
            const std::string text = value();
            options.max_occurrences = parseUnsigned<std::uint64_t>(
                text, "--max-occurrences", std::filesystem::path("rz_viewer_export"));
        }
        else if (option == "--top-hits") {
            const std::string text = value();
            options.top_hits = parseUnsigned<std::uint64_t>(
                text, "--top-hits", std::filesystem::path("rz_viewer_export"));
        }
        else if (option == "-h" || option == "--help") {
            printHelp(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (!options.sequence_path.empty()) {
        if (!options.rz_path.empty() || !options.sgf_paths.empty()) {
            throw std::invalid_argument("--sequence cannot be combined with --rz or --sgf");
        }
        const ProofSequenceManifest manifest = readProofSequenceManifest(options.sequence_path);
        if (manifest.rz_path.empty()) {
            throw std::invalid_argument("proof sequence does not declare an RZone file");
        }
        options.rz_path = manifest.rz_path;
        options.sgf_paths = manifest.sgf_paths;
    }
    if (options.rz_path.empty() || options.output_path.empty()) {
        throw std::invalid_argument("--rz/--sequence and --output are required");
    }
    return options;
}

void writeOutput(const Options& options,
                 const RzProofTable& table,
                 const TableStatistics& stats,
                 const std::vector<Run>& runs,
                 const std::vector<Occurrence>& occurrences,
                 const std::unordered_set<RZoneCertificateId>& selected,
                 bool include_all,
                 const OccurrenceSampler& sampler,
                 const HitStatistics& hit_statistics,
                 const std::vector<std::pair<RZoneCertificateId, std::uint64_t>>& top_hits)
{
    if (!options.output_path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.output_path.parent_path(), error);
        if (error) fail(options.output_path, "cannot create output directory: " + error.message());
    }
    std::filesystem::path temporary = options.output_path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) fail(temporary, "cannot open output");
    JsonWriter json(output);
    json.beginObject();
    json.key("schema_version"); json.value(std::uint64_t{1});
    json.key("generator"); json.value("Solving2048 rz_viewer_export");
    json.key("rz"); json.beginObject();
    json.key("path"); json.value(std::filesystem::absolute(options.rz_path).lexically_normal().string());
    json.key("winning_tile"); json.value(static_cast<std::uint64_t>(table.winningTile()));
    json.key("record_count"); json.value(table.summary().record_count);
    json.key("payload_checksum"); json.value(hexValue(table.summary().payload_checksum, 16));
    const std::uint64_t exported_records = include_all
        ? table.summary().record_count
        : static_cast<std::uint64_t>(selected.size());
    json.key("exported_records"); json.value(exported_records);
    json.key("selection"); json.value(include_all ? "all" : "sgf_referenced_closure");
    json.endObject();

    json.key("statistics"); json.beginObject();
    json.key("terminal"); json.value(stats.kind[0]);
    json.key("fixed_slide"); json.value(stats.kind[1]);
    json.key("universal_placement"); json.value(stats.kind[2]);
    json.key("player"); json.value(stats.phase[0]);
    json.key("environment"); json.value(stats.phase[1]);
    json.key("unique_masks"); json.value(static_cast<std::uint64_t>(stats.unique_masks.size()));
    json.key("maximum_depth"); json.value(stats.maximum_depth);
    json.key("total_depth"); json.value(stats.total_depth);
    json.key("mask_cells"); json.beginArray();
    for (std::uint64_t count : stats.mask_cells) json.value(count);
    json.endArray();
    json.endObject();

    json.key("occurrence_export"); json.beginObject();
    json.key("mode"); json.value(options.hits_only ? "rzone_hits" : "all_annotations");
    json.key("total"); json.value(sampler.total());
    json.key("exported"); json.value(static_cast<std::uint64_t>(occurrences.size()));
    json.key("limit"); json.value(sampler.limit());
    json.key("truncated"); json.value(sampler.truncated());
    json.key("sampling"); json.value(sampler.truncated() ? "deterministic_reservoir_v1" : "none");
    json.endObject();

    json.key("hit_statistics"); json.beginObject();
    json.key("total"); json.value(hit_statistics.total);
    json.key("certificates_reused"); json.value(hit_statistics.certificates_reused);
    json.key("player"); json.value(hit_statistics.player);
    json.key("environment"); json.value(hit_statistics.environment);
    json.key("exact_orientation"); json.value(hit_statistics.exact_orientation);
    json.key("symmetry"); json.value(hit_statistics.symmetry);
    json.key("mask_cells"); json.beginArray();
    for (std::uint64_t count : hit_statistics.mask_cells) json.value(count);
    json.endArray();
    json.endObject();

    json.key("top_hits"); json.beginArray();
    for (const auto& [id, count] : top_hits) {
        json.beginArray();
        json.value(static_cast<std::uint64_t>(id));
        json.value(count);
        json.endArray();
    }
    json.endArray();

    json.key("certificate_columns"); json.beginArray();
    for (const char* column : {"id", "kind", "phase", "mask", "masked_board", "child", "depth", "argument", "hit_count"}) {
        json.value(column);
    }
    json.endArray();
    json.key("certificates"); json.beginArray();
    for (std::uint64_t id = 1; id <= table.summary().record_count; ++id) {
        if (!include_all && selected.count(static_cast<RZoneCertificateId>(id)) == 0) continue;
        const RzProofEntry& entry = *table.find(static_cast<RZoneCertificateId>(id));
        json.beginArray();
        json.value(id);
        json.value(kindName(entry.kind));
        json.value(phaseLabel(entry.phase));
        json.value(hexValue(entry.mask, 4));
        json.value(hexValue(entry.masked_board, 16));
        json.value(static_cast<std::uint64_t>(entry.child));
        json.value(static_cast<std::uint64_t>(entry.depth));
        json.value(static_cast<std::int64_t>(argument(entry)));
        json.value(hit_statistics.by_certificate[static_cast<std::size_t>(id)]);
        json.endArray();
    }
    json.endArray();

    json.key("runs"); json.beginArray();
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const Run& run = runs[index];
        json.beginObject();
        json.key("index"); json.value(static_cast<std::uint64_t>(index + 1));
        json.key("path"); json.value(run.path.string());
        json.key("opening_board"); json.value(hexValue(run.opening.raw, 16));
        json.key("outcome"); json.value(run.outcome);
        json.key("annotated_nodes"); json.value(run.annotated_nodes);
        json.key("statistics"); json.beginObject();
        std::vector<std::pair<std::string, std::string>> ordered(
            run.statistics.begin(), run.statistics.end());
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [key, value] : ordered) {
            json.key(key); json.value(value);
        }
        json.endObject();
        json.endObject();
    }
    json.endArray();

    json.key("occurrence_columns"); json.beginArray();
    for (const char* column : {"certificate", "run", "node", "parent", "depth", "phase",
                               "board", "outcome", "solved_by", "transform", "candidate_mask",
                               "action"}) {
        json.value(column);
    }
    json.endArray();
    json.key("occurrences"); json.beginArray();
    for (const Occurrence& occurrence : occurrences) {
        json.beginArray();
        json.value(static_cast<std::uint64_t>(occurrence.certificate));
        json.value(static_cast<std::uint64_t>(occurrence.run + 1));
        json.value(occurrence.node);
        json.value(occurrence.parent);
        json.value(static_cast<std::uint64_t>(occurrence.depth));
        json.value(phaseLabel(occurrence.phase));
        json.value(hexValue(occurrence.board.raw, 16));
        json.value(occurrence.outcome);
        json.value(occurrence.solved_by);
        json.value(static_cast<std::uint64_t>(occurrence.transform));
        json.value(hexValue(occurrence.candidate_mask, 4));
        json.value(occurrence.action);
        json.endArray();
    }
    json.endArray();
    json.endObject();
    output << '\n';
    output.flush();
    if (!output) fail(temporary, "failed while writing JSON");
    output.close();
    if (!output) fail(temporary, "failed while closing JSON");
    std::error_code rename_error;
    std::filesystem::rename(temporary, options.output_path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        fail(options.output_path, "cannot publish JSON: " + rename_error.message());
    }
}

int run(int argc, char** argv)
{
    const Options options = parseOptions(argc, argv);
    const RzProofTable table = RzProofTable::load(options.rz_path);
    const TableStatistics stats = tableStatistics(table);
    std::vector<Occurrence> occurrences;
    if (options.max_occurrences != 0) {
        occurrences.reserve(static_cast<std::size_t>(options.max_occurrences));
    }
    OccurrenceSampler sampler(occurrences, options.max_occurrences);
    HitStatistics hit_statistics(table.summary().record_count);
    std::unordered_set<RZoneCertificateId> selected;
    std::vector<Run> runs;
    runs.reserve(options.sgf_paths.size());
    for (std::size_t index = 0; index < options.sgf_paths.size(); ++index) {
        std::cerr << "Scanning SGF " << index + 1 << '/' << options.sgf_paths.size()
                  << ": " << options.sgf_paths[index].string() << '\n';
        runs.push_back(SgfExtractor(
            options.sgf_paths[index], index, table, sampler, hit_statistics,
            options.hits_only).extract());
    }
    for (const Occurrence& occurrence : occurrences) {
        selected.insert(occurrence.certificate);
    }
    std::vector<std::pair<RZoneCertificateId, std::uint64_t>> top_hits;
    top_hits.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
        options.top_hits, hit_statistics.certificates_reused)));
    for (std::uint64_t id = 1; id <= table.summary().record_count; ++id) {
        const std::uint64_t count = hit_statistics.by_certificate[static_cast<std::size_t>(id)];
        if (count != 0) top_hits.emplace_back(static_cast<RZoneCertificateId>(id), count);
    }
    std::sort(top_hits.begin(), top_hits.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) return lhs.second > rhs.second;
        return lhs.first < rhs.first;
    });
    if (top_hits.size() > options.top_hits) {
        top_hits.resize(static_cast<std::size_t>(options.top_hits));
    }
    for (const auto& [id, count] : top_hits) {
        (void)count;
        selected.insert(id);
    }
    const bool include_all = options.all_certificates || options.sgf_paths.empty();
    if (!include_all) {
        std::vector<RZoneCertificateId> pending(selected.begin(), selected.end());
        while (!pending.empty()) {
            const RZoneCertificateId id = pending.back();
            pending.pop_back();
            const RzProofEntry* entry = table.find(id);
            if (entry && entry->child != kNoRZoneCertificate && selected.insert(entry->child).second) {
                pending.push_back(entry->child);
            }
        }
    }
    writeOutput(options, table, stats, runs, occurrences, selected, include_all, sampler,
                hit_statistics, top_hits);
    std::cout << "RZone integrity: PASS\n"
              << "RZone records: " << table.summary().record_count << '\n'
              << "Exported certificates: "
              << (include_all ? table.summary().record_count : selected.size()) << '\n'
              << "SGFs scanned: " << runs.size() << '\n'
              << "RZone occurrences: " << sampler.total() << " total, "
              << occurrences.size() << " exported"
              << (sampler.truncated() ? " (reservoir sample)\n" : "\n")
              << "RZone TT hits: " << hit_statistics.total << " across "
              << hit_statistics.certificates_reused << " certificates\n"
              << "Viewer JSON: " << options.output_path.string() << '\n';
    return 0;
}

} // namespace
} // namespace solver2048

int main(int argc, char** argv)
{
    try {
        return solver2048::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "rz_viewer_export: " << error.what() << '\n';
        return 1;
    }
}
