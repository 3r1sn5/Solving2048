// Streaming diagnostics for the complete tree layout emitted by SgfWriter.
// This counts directly attached certificates; it is not a proof verifier.
// Build: g++ -std=c++17 -O2 solver/tools/sgf_rzone_diagnostics.cpp -o /tmp/sgf_rzone_diagnostics
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Input {
    FILE* file_;
    std::array<char, 1 << 20> buffer_{};
    std::size_t position_ = 0, size_ = 0;
public:
    explicit Input(const char* path) : file_(std::fopen(path, "rb")) {
        if (!file_) throw std::runtime_error("cannot open SGF");
    }
    ~Input() { std::fclose(file_); }
    int peek() {
        if (position_ == size_) {
            size_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
            position_ = 0;
            if (!size_) {
                if (std::ferror(file_)) throw std::runtime_error("SGF read failed");
                return EOF;
            }
        }
        return static_cast<unsigned char>(buffer_[position_]);
    }
    int get() { const int c = peek(); if (c != EOF) ++position_; return c; }
    void whitespace() {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') get();
    }
    std::string value() {
        if (get() != '[') throw std::runtime_error("expected property value");
        std::string text;
        for (int c; (c = get()) != EOF;) {
            if (c == ']') return text;
            if (c == '\\') {
                c = get();
                if (c == EOF) break;
            }
            text.push_back(static_cast<char>(c));
        }
        throw std::runtime_error("unterminated SGF value");
    }
};

std::string_view field(const std::string& text, const std::string& name) {
    const std::string key = name + " = ";
    std::size_t p = text.find(key);
    while (p != std::string::npos && p && text[p - 1] != '\n') p = text.find(key, p + 1);
    if (p == std::string::npos) return {};
    p += key.size();
    const auto end = text.find('\n', p);
    return std::string_view(text).substr(p, end == std::string::npos ? end : end - p);
}
std::uint64_t integer(std::string_view value) {
    if (value.empty()) throw std::runtime_error("missing integer field");
    std::size_t used = 0;
    const auto result = std::stoull(std::string(value), &used);
    if (used != value.size()) throw std::runtime_error("invalid integer field");
    return result;
}

struct Frame {
    bool annotated = false, environment = false, win = false, backprop = false;
    bool all_children_win = true, multiple = false;
    std::uint64_t certificate = 0, first_certificate = 0;
    std::uint64_t children = 0, certified_children = 0;
};

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: sgf_rzone_diagnostics FILE.sgf");
        Input input(argv[1]);
        std::vector<Frame> stack;
        std::uint64_t nodes = 0, all_win = 0, without = 0, all = 0, some = 0, none = 0, multiple = 0;
        std::uint64_t expected_all = 0, expected_without = 0;
        bool saw_metadata = false, closed = false;
        while (true) {
            input.whitespace();
            const int token = input.get();
            if (token == EOF) break;
            if (closed) throw std::runtime_error("multiple game trees are unsupported");
            if (token == '(') {
                stack.emplace_back();
            } else if (token == ';') {
                if (stack.empty() || stack.back().annotated)
                    throw std::runtime_error("expected SgfWriter tree layout");
                input.whitespace();
                while (input.peek() >= 'A' && input.peek() <= 'Z') {
                    std::string key;
                    while (input.peek() >= 'A' && input.peek() <= 'Z') key.push_back(input.get());
                    input.whitespace();
                    const std::string value = input.value();
                    auto& f = stack.back();
                    if (key == "B") f.environment = true;
                    if (key == "W") f.environment = false;
                    if (key == "C") {
                        if (f.annotated) throw std::runtime_error("duplicate node comment");
                        f.annotated = true;
                        f.win = value.rfind("solver_status: WIN\n", 0) == 0;
                        f.backprop = field(value, "solved_by") == "BACKPROPAGATION";
                        const auto id = field(value, "rzone_certificate_id");
                        if (!id.empty()) {
                            const auto transform = field(value, "rzone_transform");
                            f.certificate = integer(id) | ((transform.empty() ? 0 : integer(transform)) << 32);
                        }
                        if (stack.size() == 1) {
                            if (saw_metadata) throw std::runtime_error("duplicate root metadata");
                            expected_all = integer(field(value, "rzone_environment_all_win_nodes"));
                            expected_without = integer(field(value, "rzone_environment_all_win_without_rzone"));
                            if (field(value, "rzone_mode") == "disabled")
                                throw std::runtime_error("RZone-disabled run has no applicable counters");
                            saw_metadata = true;
                        }
                        ++nodes;
                    }
                    input.whitespace();
                }
            } else if (token == ')') {
                if (stack.empty() || !stack.back().annotated) throw std::runtime_error("incomplete node");
                const Frame f = stack.back();
                stack.pop_back();
                if (f.environment && f.win && f.backprop) {
                    if (!f.children || !f.all_children_win) throw std::runtime_error("invalid all-Win AND node");
                    ++all_win;
                    if (!f.certificate) {
                        ++without;
                        if (!f.certified_children) ++none;
                        else if (f.certified_children == f.children) {
                            ++all;
                            if (f.multiple) ++multiple;
                        } else ++some;
                    }
                }
                if (!stack.empty()) {
                    auto& parent = stack.back();
                    ++parent.children;
                    parent.all_children_win = parent.all_children_win && f.win;
                    if (f.certificate) {
                        ++parent.certified_children;
                        if (!parent.first_certificate) parent.first_certificate = f.certificate;
                        else if (parent.first_certificate != f.certificate) parent.multiple = true;
                    }
                } else closed = true;
            } else throw std::runtime_error("unexpected SGF token");
        }
        if (!closed || !stack.empty() || !saw_metadata) throw std::runtime_error("incomplete SGF tree");
        if (all_win != expected_all || without != expected_without || all + some + none != without)
            throw std::runtime_error("recount disagrees with original diagnostic counters");
        std::cout << "{\n  \"trace_nodes\": " << nodes
                  << ",\n  \"environment_all_win_nodes\": " << all_win
                  << ",\n  \"environment_all_win_without_rzone\": " << without
                  << ",\n  \"environment_no_rzone_all_children_rzone\": " << all
                  << ",\n  \"environment_no_rzone_some_children_rzone\": " << some
                  << ",\n  \"environment_no_rzone_no_children_rzone\": " << none
                  << ",\n  \"environment_no_rzone_multiple_child_rzones\": " << multiple
                  << ",\n  \"original_counters_match\": true\n}\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
