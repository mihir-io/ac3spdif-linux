// Single-producer / single-consumer lock-free byte ring.
//
// One end of every ring in this program is a PipeWire process callback on a
// realtime thread, where blocking on a mutex risks a dropout. Absolute
// monotonic 64-bit indices keep the arithmetic trivial: they would need
// centuries to wrap at any audio rate, so wraparound is not a case worth
// handling.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ac3spdif {

class ByteRing {
public:
    explicit ByteRing(size_t capacity) : capacity_(capacity), storage_(capacity, 0) {}

    size_t capacity() const { return capacity_; }

    // Safe to call from either side; a snapshot that may be stale by the time
    // it is used, which is fine for the drift controller that consumes it.
    size_t fill_level() const {
        return static_cast<size_t>(write_.load(std::memory_order_acquire)
                                   - read_.load(std::memory_order_acquire));
    }
    size_t free_space() const { return capacity_ - fill_level(); }

    // Producer side. Returns the number of bytes actually written, which is
    // less than `count` when the ring is full.
    size_t write(const void* source, size_t count) {
        uint64_t w = write_.load(std::memory_order_relaxed);
        uint64_t r = read_.load(std::memory_order_acquire);
        size_t writable = std::min<size_t>(count, capacity_ - static_cast<size_t>(w - r));
        if (writable == 0) return 0;
        size_t offset = static_cast<size_t>(w % capacity_);
        size_t first = std::min(writable, capacity_ - offset);
        std::memcpy(storage_.data() + offset, source, first);
        if (writable > first)
            std::memcpy(storage_.data(), static_cast<const uint8_t*>(source) + first,
                        writable - first);
        write_.store(w + writable, std::memory_order_release);
        return writable;
    }

    // Consumer side. Returns the number of bytes actually read.
    size_t read(void* destination, size_t count) {
        uint64_t r = read_.load(std::memory_order_relaxed);
        uint64_t w = write_.load(std::memory_order_acquire);
        size_t readable = std::min<size_t>(count, static_cast<size_t>(w - r));
        if (readable == 0) return 0;
        size_t offset = static_cast<size_t>(r % capacity_);
        size_t first = std::min(readable, capacity_ - offset);
        std::memcpy(destination, storage_.data() + offset, first);
        if (readable > first)
            std::memcpy(static_cast<uint8_t*>(destination) + first, storage_.data(),
                        readable - first);
        read_.store(r + readable, std::memory_order_release);
        return readable;
    }

    // Consumer side, for realtime use: always fills `count` bytes, zero-padding
    // whatever the ring could not supply. Returns the shortfall so the caller
    // can count underruns.
    size_t read_zero_padded(void* destination, size_t count) {
        size_t got = read(destination, count);
        if (got < count) std::memset(static_cast<uint8_t*>(destination) + got, 0, count - got);
        return count - got;
    }

    // Discard buffered bytes without copying them out.
    size_t discard(size_t count) {
        uint64_t r = read_.load(std::memory_order_relaxed);
        uint64_t w = write_.load(std::memory_order_acquire);
        size_t droppable = std::min<size_t>(count, static_cast<size_t>(w - r));
        if (droppable > 0) read_.store(r + droppable, std::memory_order_release);
        return droppable;
    }

private:
    size_t capacity_;
    std::vector<uint8_t> storage_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

}  // namespace ac3spdif
