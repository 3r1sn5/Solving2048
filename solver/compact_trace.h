#pragma once

#include "search.h"

#include <limits>
#include <stdexcept>

namespace solver2048 {

namespace trace_detail {

// Fixed blocks avoid copying the entire history when the store grows.
template<class T, std::size_t BlockSize>
class Blocks {
public:
    std::size_t size() const { return size_; }
    std::size_t capacityBytes() const { return blocks_.size() * BlockSize * sizeof(T); }

    void push(T value)
    {
        if (size_ == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("compact trace storage exhausted");
        }
        if (size_ % BlockSize == 0) blocks_.push_back(std::make_unique<T[]>(BlockSize));
        blocks_[size_ / BlockSize][size_ % BlockSize] = value;
        ++size_;
    }

    T& at(std::size_t index)
    {
        if (index >= size_) throw std::out_of_range("compact trace index");
        return blocks_[index / BlockSize][index % BlockSize];
    }

    const T& at(std::size_t index) const
    {
        if (index >= size_) throw std::out_of_range("compact trace index");
        return blocks_[index / BlockSize][index % BlockSize];
    }

private:
    std::vector<std::unique_ptr<T[]>> blocks_;
    std::size_t size_ = 0;
};

} // namespace trace_detail

// Complete, in-memory DFS history. Payloads contain only present optional fields.
// IDs retain discovery order even though nodes are sealed in completion order.
class CompactTrace {
public:
    CompactTrace();
    void allocate(std::uint64_t id);
    // Children must already be sealed. Release descendants after recording their
    // links, retaining this node's summary for the parent's RZone/TT backup.
    void seal(TraceNode& node);
    // Decode one summary, without constructing its descendants.
    TraceNode decode(std::uint64_t id) const;
    std::uint64_t firstChild(std::uint64_t id) const;
    std::uint64_t nextSibling(std::uint64_t id) const;
    std::size_t nodes() const { return links_.size() - 1; }
    // Allocated link/payload blocks; excludes DFS work objects and block pointers.
    std::size_t capacityBytes() const { return links_.capacityBytes() + bytes_.capacityBytes(); }

private:
    static constexpr std::uint64_t kUnsealed = std::numeric_limits<std::uint64_t>::max();
    struct Link {
        std::uint64_t offset = kUnsealed;
        std::uint64_t child = 0;
        std::uint64_t next = 0;
    };
    // Use 64-bit links for long searches, rather than a 2^32-record ceiling.
    static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t), "trace requires a 64-bit build");
    trace_detail::Blocks<Link, 4096> links_;
    trace_detail::Blocks<std::uint8_t, 65536> bytes_;
};

} // namespace solver2048
