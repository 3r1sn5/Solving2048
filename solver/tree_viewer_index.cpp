#include "game_api.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
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
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace solver2048 {
namespace {

constexpr std::uint64_t kOverviewBucket = 16384;
constexpr std::uint16_t kRootAction = 0xffff;
constexpr std::size_t kNodeSize = 64;

enum : std::uint8_t {
    kPlayer = 0,
    kEnvironment = 1,
};
enum : std::uint8_t {
    kUnknown = 0,
    kWin = 1,
    kLoss = 2,
};
enum : std::uint8_t {
    kNone = 0,
    kTerminal = 1,
    kRZone = 2,
    kTt = 3,
    kBackpropagation = 4,
    kNodeLimit = 5,
};
using Properties = std::unordered_map<std::string, std::vector<std::string>>;
using Fields = std::unordered_map<std::string, std::string>;

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

Fields parseComment(const std::string& comment)
{
    Fields fields;
    std::istringstream input(comment);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equal = line.find('=');
        const std::size_t colon = line.find(':');
        std::size_t delimiter = equal;
        if (colon != std::string::npos &&
            (delimiter == std::string::npos || colon < delimiter)) {
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
        Properties properties;
        while (true) {
            skipWhitespace();
            const int next = input_.peek();
            if (next == std::char_traits<char>::eof() || next == ';' || next == '(' ||
                next == ')') {
                return properties;
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
            if (values.empty() || !properties.emplace(name, std::move(values)).second) {
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
    std::uint16_t id = kRootAction;
};

Action decodeAction(char player, const std::string& text, const std::filesystem::path& path)
{
    const unsigned id = parseUnsigned<unsigned>(text, "SGF action", path);
    if (player == 'B' && id <= 3) {
        return {false, id, -1, 0, static_cast<std::uint16_t>(id)};
    }
    if (player == 'W' && id >= 4 && id <= 35) {
        const bool four = id >= 20;
        return {true, 0, static_cast<int>(id - (four ? 20 : 4)), four ? 4u : 2u,
                static_cast<std::uint16_t>(id)};
    }
    fail(path, "action/player combination is invalid: " + std::string(1, player) + text);
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
    fail(path, "SGF contains an illegal action " + std::to_string(action.id));
}

void put16(std::uint8_t* output, std::uint16_t value)
{
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t* output, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void put64(std::uint8_t* output, std::uint64_t value)
{
    for (unsigned index = 0; index < 8; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

struct NodeRecord {
    std::uint64_t parent = 0;
    std::uint64_t board = 0;
    std::uint64_t subtree_end = 0;
    std::uint64_t tt_source = 0;
    std::uint32_t depth = 0;
    std::uint32_t children = 0;
    std::uint16_t action = kRootAction;
    std::uint8_t phase = kPlayer;
    std::uint8_t outcome = kUnknown;
    std::uint8_t solved = kNone;
    std::uint8_t rz_transform = 0;
    std::uint32_t rz_certificate = 0;
    std::uint32_t skipped = 0;
    std::uint64_t first_child = 0;
};

std::array<std::uint8_t, kNodeSize> encodeNode(const NodeRecord& record)
{
    std::array<std::uint8_t, kNodeSize> bytes{};
    put64(bytes.data(), record.subtree_end);
    put64(bytes.data() + 8, record.first_child);
    put32(bytes.data() + 16, record.children);
    put64(bytes.data() + 24, record.parent);
    put64(bytes.data() + 32, record.board);
    put64(bytes.data() + 40, record.tt_source);
    put32(bytes.data() + 48, record.depth);
    put32(bytes.data() + 52, record.rz_certificate);
    put16(bytes.data() + 56, record.action);
    bytes[58] = record.phase;
    bytes[59] = record.outcome;
    bytes[60] = record.solved;
    bytes[62] = record.rz_transform;
    if (record.skipped > std::numeric_limits<std::uint8_t>::max()) {
        throw std::logic_error("skipped action count exceeds node record capacity");
    }
    bytes[63] = static_cast<std::uint8_t>(record.skipped);
    return bytes;
}

class NodeWriter {
public:
    explicit NodeWriter(const std::filesystem::path& path)
        : path_(path)
    {
        descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (descriptor_ < 0) systemFail("cannot open node index");
        resize(1u << 20); // 64 MiB; later growth only remaps virtual address space.
    }

    ~NodeWriter()
    {
        if (mapping_) ::munmap(mapping_, capacity_ * kNodeSize);
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    std::uint64_t create(NodeRecord record)
    {
        const std::uint64_t id = ++count_;
        if (count_ > capacity_) resize(capacity_ * 2);
        record.subtree_end = id;
        const auto bytes = encodeNode(record);
        std::memcpy(mapping_ + (id - 1) * kNodeSize, bytes.data(), bytes.size());
        open_.emplace(id, OpenState{});
        if (record.parent != 0) {
            const auto parent = open_.find(record.parent);
            if (parent == open_.end()) fail(path_, "node refers to a closed parent");
            ++parent->second.children;
            if (parent->second.first_child == 0) parent->second.first_child = id;
        }
        return id;
    }

    void finalize(std::uint64_t id)
    {
        const auto found = open_.find(id);
        if (found == open_.end()) return;
        const std::uint64_t base = (id - 1) * kNodeSize;
        put64(mapping_ + base, count_);
        put64(mapping_ + base + 8, found->second.first_child);
        put32(mapping_ + base + 16, found->second.children);
        open_.erase(found);
    }

    void finish()
    {
        if (::msync(mapping_, count_ * kNodeSize, MS_SYNC) != 0) {
            systemFail("cannot flush node index");
        }
        if (::munmap(mapping_, capacity_ * kNodeSize) != 0) {
            mapping_ = nullptr;
            systemFail("cannot unmap node index");
        }
        mapping_ = nullptr;
        if (::ftruncate(descriptor_, static_cast<off_t>(count_ * kNodeSize)) != 0) {
            systemFail("cannot truncate node index");
        }
        if (::fsync(descriptor_) != 0) systemFail("cannot sync node index");
        if (::close(descriptor_) != 0) {
            descriptor_ = -1;
            systemFail("cannot close node index");
        }
        descriptor_ = -1;
    }

    std::uint64_t count() const { return count_; }

private:
    struct OpenState {
        std::uint32_t children = 0;
        std::uint64_t first_child = 0;
    };

    [[noreturn]] void systemFail(const std::string& message) const
    {
        fail(path_, message + ": " + std::strerror(errno));
    }

    void resize(std::uint64_t records)
    {
        if (records <= capacity_) return;
        if (mapping_) {
            if (::munmap(mapping_, capacity_ * kNodeSize) != 0) {
                mapping_ = nullptr;
                systemFail("cannot remap node index");
            }
            mapping_ = nullptr;
        }
        if (records > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) /
                          kNodeSize) {
            throw std::overflow_error("node index exceeds filesystem offset range");
        }
        const std::uint64_t bytes = records * kNodeSize;
        if (::ftruncate(descriptor_, static_cast<off_t>(bytes)) != 0) {
            systemFail("cannot grow node index");
        }
        void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                              descriptor_, 0);
        if (mapped == MAP_FAILED) {
            mapping_ = nullptr;
            systemFail("cannot map node index");
        }
        mapping_ = static_cast<std::uint8_t*>(mapped);
        capacity_ = records;
    }

    std::filesystem::path path_;
    int descriptor_ = -1;
    std::uint8_t* mapping_ = nullptr;
    std::uint64_t capacity_ = 0;
    std::uint64_t count_ = 0;
    std::unordered_map<std::uint64_t, OpenState> open_;
};

struct Bucket {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t total = 0;
    std::uint64_t win = 0;
    std::uint64_t loss = 0;
    std::uint64_t terminal = 0;
    std::uint64_t rzone = 0;
    std::uint64_t tt = 0;
    std::uint64_t first_terminal = 0;
    std::uint64_t first_rzone = 0;
    std::uint64_t first_tt = 0;
    std::uint32_t maximum_depth = 0;
};

std::string jsonEscape(const std::string& value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(character) << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

struct ParseContext {
    Board board;
    bool search_started = false;
    std::uint64_t node = 0;
    std::uint32_t depth = 0;
};

class Indexer {
public:
    Indexer(const std::filesystem::path& sgf,
            const std::filesystem::path& nodes,
            std::filesystem::path rz,
            std::filesystem::path tt)
        : sgf_(sgf), input_(sgf, std::ios::binary), parser_(sgf, input_), writer_(nodes),
          rz_(std::move(rz)), tt_(std::move(tt))
    {
        if (!input_) fail(sgf_, "cannot open SGF");
    }

    void run()
    {
        parseTree(ParseContext{});
        parser_.expectEnd();
        if (!root_found_ || writer_.count() == 0) fail(sgf_, "solver search root was not found");
        writer_.finish();
        if (!buckets_.empty()) buckets_.back().end = writer_.count();
    }

    void writeMetadata(const std::filesystem::path& output) const
    {
        std::ofstream json(output, std::ios::trunc);
        if (!json) fail(output, "cannot open metadata output");
        std::error_code size_error;
        const std::uint64_t sgf_size = std::filesystem::file_size(sgf_, size_error);
        if (size_error) fail(sgf_, "cannot determine SGF size: " + size_error.message());
        json << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"node_record_size\": " << kNodeSize << ",\n"
             << "  \"node_count\": " << writer_.count() << ",\n"
             << "  \"maximum_depth\": " << maximum_depth_ << ",\n"
             << "  \"overview_bucket_size\": " << kOverviewBucket << ",\n"
             << "  \"sgf\": " << jsonEscape(std::filesystem::absolute(sgf_).lexically_normal().string()) << ",\n"
             << "  \"sgf_size\": " << sgf_size << ",\n"
             << "  \"rz\": " << jsonEscape(rz_.empty() ? "" : std::filesystem::absolute(rz_).lexically_normal().string()) << ",\n"
             << "  \"tt\": " << jsonEscape(tt_.empty() ? "" : std::filesystem::absolute(tt_).lexically_normal().string()) << ",\n"
             << "  \"winning_tile\": " << fieldUnsigned("winning_tile") << ",\n"
             << "  \"outcome\": " << jsonEscape(field("solver_status", "UNKNOWN")) << ",\n"
             << "  \"root_statistics\": {";
        bool first = true;
        for (const auto& [key, value] : root_fields_) {
            json << (first ? "\n" : ",\n") << "    " << jsonEscape(key) << ": "
                 << jsonEscape(value);
            first = false;
        }
        if (!first) json << '\n';
        json << "  }\n}\n";
        if (!json) fail(output, "cannot write metadata output");
    }

    void writeOverview(const std::filesystem::path& output) const
    {
        std::ofstream json(output, std::ios::trunc);
        if (!json) fail(output, "cannot open overview output");
        json << '[';
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            if (index) json << ',';
            const Bucket& bucket = buckets_[index];
            json << "{\"start\":" << bucket.start
                 << ",\"end\":" << bucket.end
                 << ",\"total\":" << bucket.total
                 << ",\"win\":" << bucket.win
                 << ",\"loss\":" << bucket.loss
                 << ",\"terminal\":" << bucket.terminal
                 << ",\"rzone\":" << bucket.rzone
                 << ",\"tt\":" << bucket.tt
                 << ",\"first_terminal\":" << bucket.first_terminal
                 << ",\"first_rzone\":" << bucket.first_rzone
                 << ",\"first_tt\":" << bucket.first_tt
                 << ",\"max_depth\":" << bucket.maximum_depth << '}';
        }
        json << "]\n";
        if (!json) fail(output, "cannot write overview output");
    }

    std::uint64_t nodes() const { return writer_.count(); }
    std::uint32_t maximumDepth() const { return maximum_depth_; }

private:
    ParseContext processNode(ParseContext context, const Properties& properties)
    {
        std::optional<Action> action;
        char player = 0;
        for (const char candidate : {'B', 'W'}) {
            const auto found = properties.find(std::string(1, candidate));
            if (found == properties.end()) continue;
            if (action || found->second.size() != 1) fail(sgf_, "SGF node has invalid actions");
            player = candidate;
            action = decodeAction(candidate, found->second.front(), sgf_);
        }
        if (action) context.board = applyAction(context.board, *action, sgf_);

        Fields fields;
        const auto comment = properties.find("C");
        if (comment != properties.end() && comment->second.size() == 1) {
            fields = parseComment(comment->second.front());
        }
        const bool solver_root = !context.search_started && fields.count("solver_status") &&
                                 fields.count("winning_tile");
        if (solver_root) {
            context.search_started = true;
            context.node = 0;
            context.depth = 0;
            root_found_ = true;
            root_fields_ = fields;
        } else if (!context.search_started || !action) {
            return context;
        } else {
            ++context.depth;
        }

        NodeRecord record;
        record.parent = solver_root ? 0 : context.node;
        record.board = context.board.raw;
        record.depth = context.depth;
        record.action = solver_root ? kRootAction : action->id;
        record.phase = solver_root || player == 'W' ? kPlayer : kEnvironment;
        record.outcome = outcomeCode(field(fields, "solver_status", "UNKNOWN"));
        record.solved = solvedCode(field(fields, "solved_by", "NONE"));
        record.rz_certificate = fieldUnsigned(fields, "rzone_certificate_id");
        record.rz_transform = static_cast<std::uint8_t>(fieldUnsigned(fields, "rzone_transform"));
        record.tt_source = fieldUnsigned(fields, "tt_source_node_id");
        record.skipped = wordCount(field(fields, "children_skipped_actions", ""));
        context.node = writer_.create(record);
        maximum_depth_ = std::max(maximum_depth_, context.depth);
        updateBucket(context.node, record);
        if (context.node % 1000000 == 0) {
            std::cerr << "indexed " << context.node << " search nodes\n";
        }
        return context;
    }

    void parseTree(ParseContext inherited)
    {
        parser_.expect('(');
        ParseContext current = inherited;
        std::uint64_t root = 0;
        bool has_node = false;
        while (parser_.peek() == ';') {
            const std::uint64_t before = current.node;
            current = processNode(current, parser_.readNode());
            if (current.node != before && root == 0) root = current.node;
            has_node = true;
        }
        if (!has_node) fail(sgf_, "empty SGF game tree");
        while (parser_.peek() == '(') parseTree(current);
        parser_.expect(')');
        if (root) writer_.finalize(root);
    }

    void updateBucket(std::uint64_t id, const NodeRecord& record)
    {
        const std::size_t index = static_cast<std::size_t>((id - 1) / kOverviewBucket);
        while (buckets_.size() <= index) {
            Bucket bucket;
            bucket.start = buckets_.size() * kOverviewBucket + 1;
            bucket.end = (buckets_.size() + 1) * kOverviewBucket;
            buckets_.push_back(bucket);
        }
        Bucket& bucket = buckets_[index];
        ++bucket.total;
        bucket.maximum_depth = std::max(bucket.maximum_depth, record.depth);
        if (record.outcome == kWin) ++bucket.win;
        if (record.outcome == kLoss) ++bucket.loss;
        if (record.solved == kTerminal) {
            ++bucket.terminal;
            if (!bucket.first_terminal) bucket.first_terminal = id;
        }
        if (record.rz_certificate) {
            ++bucket.rzone;
            if (!bucket.first_rzone) bucket.first_rzone = id;
        }
        if (record.solved == kTt) {
            ++bucket.tt;
            if (!bucket.first_tt) bucket.first_tt = id;
        }
    }

    static std::string field(const Fields& fields,
                             const std::string& key,
                             const std::string& fallback)
    {
        const auto found = fields.find(key);
        return found == fields.end() ? fallback : found->second;
    }

    std::string field(const std::string& key, const std::string& fallback) const
    {
        return field(root_fields_, key, fallback);
    }

    std::uint64_t fieldUnsigned(const std::string& key) const
    {
        return fieldUnsigned(root_fields_, key);
    }

    std::uint64_t fieldUnsigned(const Fields& fields, const std::string& key) const
    {
        const auto found = fields.find(key);
        return found == fields.end() || found->second.empty()
            ? 0
            : parseUnsigned<std::uint64_t>(found->second, key, sgf_);
    }

    static std::uint32_t wordCount(const std::string& value)
    {
        std::istringstream input(value);
        std::string word;
        std::uint32_t result = 0;
        while (input >> word) ++result;
        return result;
    }

    static std::uint8_t outcomeCode(const std::string& value)
    {
        if (value == "WIN") return kWin;
        if (value == "LOSS") return kLoss;
        return kUnknown;
    }

    static std::uint8_t solvedCode(const std::string& value)
    {
        if (value == "TERMINAL") return kTerminal;
        if (value == "RZONE_TT") return kRZone;
        if (value == "TT") return kTt;
        if (value == "BACKPROPAGATION") return kBackpropagation;
        if (value == "NODE_LIMIT") return kNodeLimit;
        return kNone;
    }

    std::filesystem::path sgf_;
    std::ifstream input_;
    SgfParser parser_;
    NodeWriter writer_;
    std::filesystem::path rz_;
    std::filesystem::path tt_;
    Fields root_fields_;
    std::vector<Bucket> buckets_;
    std::uint32_t maximum_depth_ = 0;
    bool root_found_ = false;
};

struct Options {
    std::filesystem::path sgf;
    std::filesystem::path rz;
    std::filesystem::path tt;
    std::filesystem::path output;
};

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument(option + " requires a path");
            return argv[index];
        };
        if (option == "--sgf") options.sgf = value();
        else if (option == "--rz") options.rz = value();
        else if (option == "--tt") options.tt = value();
        else if (option == "--output") options.output = value();
        else if (option == "-h" || option == "--help") {
            std::cout << "Usage: solver/tree_viewer_index --sgf PATH --output DIR [--rz PATH] [--tt PATH]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (options.sgf.empty() || options.output.empty()) {
        throw std::invalid_argument("--sgf and --output are required");
    }
    return options;
}

int run(int argc, char** argv)
{
    const Options options = parseOptions(argc, argv);
    if (std::filesystem::exists(options.output)) {
        throw std::invalid_argument("output already exists: " + options.output.string());
    }
    if (!std::filesystem::is_regular_file(options.sgf)) {
        throw std::invalid_argument("SGF does not exist: " + options.sgf.string());
    }
    if (!options.rz.empty() && !std::filesystem::is_regular_file(options.rz)) {
        throw std::invalid_argument("RZone snapshot does not exist: " + options.rz.string());
    }
    if (!options.tt.empty() && !std::filesystem::is_regular_file(options.tt)) {
        throw std::invalid_argument("TT snapshot does not exist: " + options.tt.string());
    }
    const std::filesystem::path parent = options.output.parent_path().empty()
        ? std::filesystem::path(".")
        : options.output.parent_path();
    std::filesystem::create_directories(parent);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = parent /
        (options.output.filename().string() + ".tmp." + std::to_string(nonce));
    std::filesystem::create_directory(temporary);
    try {
        Indexer indexer(options.sgf, temporary / "nodes.bin", options.rz, options.tt);
        indexer.run();
        indexer.writeMetadata(temporary / "metadata.json");
        indexer.writeOverview(temporary / "overview.json");
        std::filesystem::rename(temporary, options.output);
        std::cout << "index complete: " << indexer.nodes() << " nodes, depth "
                  << indexer.maximumDepth() << '\n'
                  << "index directory: " << options.output.string() << '\n';
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(temporary, cleanup_error);
        throw;
    }
    return 0;
}

} // namespace
} // namespace solver2048

int main(int argc, char** argv)
{
    try {
        return solver2048::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "tree_viewer_index: " << error.what() << '\n';
        return 1;
    }
}
