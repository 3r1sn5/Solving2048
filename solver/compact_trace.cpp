#include "compact_trace.h"

#include <array>
#include <cstring>

namespace solver2048 {
namespace {

struct Encoder {
    std::array<std::uint8_t, 256> bytes{};
    std::size_t size = 0;

    template<class T> void put(T value)
    {
        if (size + sizeof(T) > bytes.size()) throw std::length_error("compact trace record");
        std::memcpy(bytes.data() + size, &value, sizeof(T));
        size += sizeof(T);
    }
};

struct Decoder {
    const trace_detail::Blocks<std::uint8_t, 65536>& bytes;
    std::size_t offset;

    template<class T> T get()
    {
        T value;
        auto* destination = reinterpret_cast<std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) destination[i] = bytes.at(offset++);
        return value;
    }
};

std::uint8_t byteField(std::size_t value)
{
    if (value > 255) throw std::overflow_error("compact trace byte field");
    return static_cast<std::uint8_t>(value);
}

SearchAction decodeAction(std::uint8_t value)
{
    if (value < 4) return SearchAction::slide(static_cast<Direction>(value));
    if (value < 36) return SearchAction::placement((value - 4) % 16, value < 20 ? 2 : 4);
    throw std::runtime_error("invalid compact trace action");
}

template<class Wire, class Value>
void encodeOptional(Encoder& encoder, std::uint32_t& flags, unsigned& bit,
                    const std::optional<Value>& value)
{
    if (value) {
        flags |= 1u << bit;
        encoder.put<Wire>(static_cast<Wire>(*value));
    }
    ++bit;
}

void encodeOptionalAction(Encoder& encoder, std::uint32_t& flags, unsigned& bit,
                          const std::optional<SearchAction>& value)
{
    std::optional<std::uint8_t> encoded;
    if (value) encoded = byteField(value->sgfId());
    encodeOptional<std::uint8_t>(encoder, flags, bit, encoded);
}

template<class Wire, class Value>
void decodeOptional(Decoder& decoder, std::uint32_t flags, unsigned& bit,
                    std::optional<Value>& value)
{
    if (flags & (1u << bit)) value = static_cast<Value>(decoder.get<Wire>());
    ++bit;
}

void decodeOptionalAction(Decoder& decoder, std::uint32_t flags, unsigned& bit,
                          std::optional<SearchAction>& value)
{
    std::optional<std::uint8_t> encoded;
    decodeOptional<std::uint8_t>(decoder, flags, bit, encoded);
    if (encoded) value = decodeAction(*encoded);
}

} // namespace

CompactTrace::CompactTrace()
{
    links_.push({}); // ID zero means no node.
}

void CompactTrace::allocate(std::uint64_t id)
{
    if (id != links_.size() || id == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("compact trace ID must be sequential and nonzero");
    }
    links_.push({});
}

void CompactTrace::seal(TraceNode& node)
{
    if (node.trace_id == 0) throw std::logic_error("compact trace node has no ID");
    auto& link = links_.at(node.trace_id);
    if (link.offset != kUnsealed) throw std::logic_error("compact trace already sealed");
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        auto& child = links_.at(node.children[i]->trace_id);
        if (child.offset == kUnsealed) throw std::logic_error("unsealed compact trace child");
        child.next = i + 1 < node.children.size() ? node.children[i + 1]->trace_id : 0;
    }
    link.child = node.children.empty() ? 0 : node.children.front()->trace_id;

    Encoder encoder;
    std::uint32_t flags = node.expanded | (node.tt_hit << 1) |
        (node.tt_persistent_hit << 2) | (node.tt_symmetry_hit << 3) |
        (node.rzone_hit << 4) | (node.rzone_symmetry_hit << 5);
    encoder.put<std::uint32_t>(0); // Filled after encoding optional-field presence bits.
    encoder.put<std::uint8_t>(static_cast<std::uint8_t>(node.phase));
    encoder.put<std::uint8_t>(static_cast<std::uint8_t>(node.outcome));
    encoder.put<std::uint8_t>(static_cast<std::uint8_t>(node.terminal_reason));
    encoder.put<std::uint8_t>(static_cast<std::uint8_t>(node.solved_by));
    encoder.put<std::uint8_t>(byteField(node.dfs_order));
    encoder.put<std::uint8_t>(byteField(node.children_total));
    unsigned bit = 6;
    encodeOptional<std::uint64_t>(encoder, flags, bit, node.tt_source_trace_id);
    encodeOptional<std::uint64_t>(encoder, flags, bit, node.tt_key);
    encodeOptional<std::uint8_t>(encoder, flags, bit, node.tt_transform);
    encodeOptional<std::uint8_t>(encoder, flags, bit, node.rzone_transform);
    encodeOptional<std::uint32_t>(encoder, flags, bit, node.rzone_certificate_id);
    encodeOptional<std::uint8_t>(encoder, flags, bit, node.rzone_kind);
    encodeOptional<std::uint16_t>(encoder, flags, bit, node.rzone_mask);
    encodeOptional<std::uint64_t>(encoder, flags, bit, node.rzone_masked_board);
    encodeOptional<std::uint32_t>(encoder, flags, bit, node.rzone_depth);
    encodeOptionalAction(encoder, flags, bit, node.incoming_action);
    encodeOptional<std::int32_t>(encoder, flags, bit, node.reward);
    encodeOptional<double>(encoder, flags, bit, node.afterstate_value);
    encodeOptional<double>(encoder, flags, bit, node.action_value);
    encodeOptionalAction(encoder, flags, bit, node.selected_action);
    encodeOptional<double>(encoder, flags, bit, node.selected_action_value);
    encoder.put<std::uint8_t>(byteField(node.skipped_actions.size()));
    for (const auto& action : node.skipped_actions) {
        encoder.put<std::uint8_t>(byteField(action.sgfId()));
    }
    std::memcpy(encoder.bytes.data(), &flags, sizeof(flags));
    const std::uint64_t offset = bytes_.size();
    for (std::size_t i = 0; i < encoder.size; ++i) bytes_.push(encoder.bytes[i]);
    link.offset = offset;

    // The parent still needs this node's outcome/RZone/witness summary, but never
    // its descendants or skipped-action array. All of those are now in the store.
    std::vector<std::unique_ptr<TraceNode>>().swap(node.children);
    std::vector<SearchAction>().swap(node.skipped_actions);
}

TraceNode CompactTrace::decode(std::uint64_t id) const
{
    const auto& link = links_.at(id);
    if (link.offset == kUnsealed) throw std::logic_error("unsealed compact trace");
    Decoder decoder{bytes_, static_cast<std::size_t>(link.offset)};
    TraceNode node;
    node.trace_id = id;
    const auto flags = decoder.get<std::uint32_t>();
    node.expanded = flags & 1;
    node.tt_hit = flags & 2;
    node.tt_persistent_hit = flags & 4;
    node.tt_symmetry_hit = flags & 8;
    node.rzone_hit = flags & 16;
    node.rzone_symmetry_hit = flags & 32;
    node.phase = static_cast<Phase>(decoder.get<std::uint8_t>());
    node.outcome = static_cast<Outcome>(decoder.get<std::uint8_t>());
    node.terminal_reason = static_cast<TerminalReason>(decoder.get<std::uint8_t>());
    node.solved_by = static_cast<SolvedBy>(decoder.get<std::uint8_t>());
    node.dfs_order = decoder.get<std::uint8_t>();
    node.children_total = decoder.get<std::uint8_t>();
    unsigned bit = 6;
    decodeOptional<std::uint64_t>(decoder, flags, bit, node.tt_source_trace_id);
    decodeOptional<std::uint64_t>(decoder, flags, bit, node.tt_key);
    decodeOptional<std::uint8_t>(decoder, flags, bit, node.tt_transform);
    decodeOptional<std::uint8_t>(decoder, flags, bit, node.rzone_transform);
    decodeOptional<std::uint32_t>(decoder, flags, bit, node.rzone_certificate_id);
    decodeOptional<std::uint8_t>(decoder, flags, bit, node.rzone_kind);
    decodeOptional<std::uint16_t>(decoder, flags, bit, node.rzone_mask);
    decodeOptional<std::uint64_t>(decoder, flags, bit, node.rzone_masked_board);
    decodeOptional<std::uint32_t>(decoder, flags, bit, node.rzone_depth);
    decodeOptionalAction(decoder, flags, bit, node.incoming_action);
    decodeOptional<std::int32_t>(decoder, flags, bit, node.reward);
    decodeOptional<double>(decoder, flags, bit, node.afterstate_value);
    decodeOptional<double>(decoder, flags, bit, node.action_value);
    decodeOptionalAction(decoder, flags, bit, node.selected_action);
    decodeOptional<double>(decoder, flags, bit, node.selected_action_value);
    const auto count = decoder.get<std::uint8_t>();
    for (unsigned i = 0; i < count; ++i) node.skipped_actions.push_back(decodeAction(decoder.get<std::uint8_t>()));
    return node;
}

std::uint64_t CompactTrace::firstChild(std::uint64_t id) const { return links_.at(id).child; }
std::uint64_t CompactTrace::nextSibling(std::uint64_t id) const { return links_.at(id).next; }

} // namespace solver2048
