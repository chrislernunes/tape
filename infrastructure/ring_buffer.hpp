#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  RingBuffer<T, Capacity>
//
//  Sequenced ring buffer used for market data publishing.
//  Inspired by the LMAX Disruptor pattern.
//
//  Key difference from LockFreeQueue:
//    - Supports multiple consumers reading at their own pace
//    - Publisher fills slots; consumers track their own read cursor
//    - Overwrite semantics: oldest data overwritten when buffer wraps
//    - Slots have sequence numbers for gap detection
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <cassert>

namespace hydra {

template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

    static constexpr std::size_t MASK = Capacity - 1;

public:
    struct Envelope {
        uint64_t sequence{};
        T        data{};
    };

    RingBuffer() noexcept : write_seq_(0) {
        for (std::size_t i = 0; i < Capacity; ++i)
            seq_slots_[i].store(0, std::memory_order_relaxed);
    }

    // ── Publisher ─────────────────────────────────────────────────────────────
    // Returns the sequence number assigned to this message
    uint64_t publish(const T& item) noexcept {
        const uint64_t seq = write_seq_.fetch_add(1, std::memory_order_relaxed);
        const std::size_t idx = seq & MASK;
        buffer_[idx].data     = item;
        // Store sequence last; consumers spin on this
        seq_slots_[idx].store(seq + 1, std::memory_order_release);
        return seq;
    }

    // ── Consumer cursor ───────────────────────────────────────────────────────
    // A consumer tracks its own read_seq independently.
    // Call next_available() to get the next unread envelope.
    struct Cursor {
        uint64_t read_seq{0};
    };

    [[nodiscard]] bool try_read(Cursor& cursor, Envelope& out) const noexcept {
        const uint64_t want_seq = cursor.read_seq;
        const std::size_t idx  = want_seq & MASK;

        uint64_t published = seq_slots_[idx].load(std::memory_order_acquire);
        if (published != want_seq + 1) return false;  // not yet published

        out.sequence = want_seq;
        out.data     = buffer_[idx].data;
        ++cursor.read_seq;
        return true;
    }

    // Check if the consumer has fallen too far behind (overwrite detected)
    [[nodiscard]] bool is_overrun(const Cursor& cursor) const noexcept {
        uint64_t current_write = write_seq_.load(std::memory_order_acquire);
        return current_write > cursor.read_seq + Capacity;
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t    published_count() const noexcept {
        return write_seq_.load(std::memory_order_relaxed);
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Payload storage
    struct alignas(8) Slot {
        T data{};
    };

    alignas(64) std::array<Slot,                   Capacity> buffer_{};
    alignas(64) std::array<std::atomic<uint64_t>,  Capacity> seq_slots_{};
    alignas(64) std::atomic<uint64_t>                        write_seq_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience: a simple circular history buffer (single-thread, no atomics)
//  Used in analytics to keep rolling windows of data.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t Capacity>
class HistoryBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0);
    static constexpr std::size_t MASK = Capacity - 1;

public:
    void push(const T& v) noexcept {
        buf_[write_ & MASK] = v;
        ++write_;
        if (count_ < Capacity) ++count_;
    }

    // Iterate newest-to-oldest
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < count_; ++i) {
            std::size_t idx = (write_ - 1 - i) & MASK;
            fn(buf_[idx]);
        }
    }

    [[nodiscard]] const T& newest()   const noexcept { return buf_[(write_-1) & MASK]; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] bool full()         const noexcept { return count_ == Capacity; }

private:
    std::array<T, Capacity> buf_{};
    std::size_t write_{0};
    std::size_t count_{0};
};

} // namespace hydra
