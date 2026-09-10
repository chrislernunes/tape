#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MemoryPool<T, PoolSize>
//
//  Fixed-size slab allocator for order nodes.
//  Allocate / free in O(1).  No heap fragmentation.  Cache friendly.
//
//  Technique: a free-list threaded through the unused slots themselves
//  (classic "intrusive" trick used in trading system allocators).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <stdexcept>
#include <array>
#include <new>

namespace hydra {

template <typename T, std::size_t PoolSize>
class MemoryPool {
    static_assert(PoolSize > 0);
    static_assert(sizeof(T) >= sizeof(void*),
                  "T must be large enough to hold a free-list pointer");

    // ── Slot union: either user data or a next-free pointer ──────────────────
    union Slot {
        alignas(T) std::byte storage[sizeof(T)];
        Slot* next;
    };

public:
    MemoryPool() noexcept {
        // Thread all slots into the free list
        for (std::size_t i = 0; i < PoolSize - 1; ++i)
            slots_[i].next = &slots_[i + 1];
        slots_[PoolSize - 1].next = nullptr;
        free_head_ = &slots_[0];
        allocated_ = 0;
    }

    ~MemoryPool() {
        // Objects must be returned before pool destruction in production
    }

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // ── Allocate raw storage for one T ───────────────────────────────────────
    [[nodiscard]] T* allocate() {
        if (!free_head_) [[unlikely]]
            throw std::bad_alloc{};

        Slot* slot = free_head_;
        free_head_ = slot->next;
        ++allocated_;
        return reinterpret_cast<T*>(slot->storage);
    }

    // ── Construct T in pool ───────────────────────────────────────────────────
    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        T* ptr = allocate();
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // ── Destroy T and return slot to pool ─────────────────────────────────────
    void destroy(T* ptr) noexcept {
        if (!ptr) return;
        ptr->~T();
        deallocate(ptr);
    }

    // ── Return raw slot (no destructor call) ──────────────────────────────────
    void deallocate(T* ptr) noexcept {
        Slot* slot = reinterpret_cast<Slot*>(ptr);
        slot->next = free_head_;
        free_head_ = slot;
        --allocated_;
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t allocated()  const noexcept { return allocated_; }
    [[nodiscard]] std::size_t available()  const noexcept { return PoolSize - allocated_; }
    [[nodiscard]] std::size_t capacity()   const noexcept { return PoolSize; }
    [[nodiscard]] bool        full()       const noexcept { return allocated_ == PoolSize; }

    // Check if a pointer belongs to this pool
    [[nodiscard]] bool owns(const T* ptr) const noexcept {
        const auto* p = reinterpret_cast<const Slot*>(ptr);
        return p >= slots_.data() && p < slots_.data() + PoolSize;
    }

private:
    std::array<Slot, PoolSize> slots_;
    Slot*       free_head_{nullptr};
    std::size_t allocated_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  ArenaAllocator: bump-pointer allocator for short-lived scratch objects
//  (e.g. per-event temp data during matching).
// ─────────────────────────────────────────────────────────────────────────────
template <std::size_t ArenaBytes>
class ArenaAllocator {
public:
    ArenaAllocator() noexcept : offset_(0) {}

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align = 8) noexcept {
        // Align offset
        std::size_t pad = (align - (offset_ % align)) % align;
        if (offset_ + pad + bytes > ArenaBytes) return nullptr;
        void* ptr = buf_.data() + offset_ + pad;
        offset_ += pad + bytes;
        return ptr;
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* emplace(Args&&... args) noexcept {
        void* raw = allocate(sizeof(T), alignof(T));
        if (!raw) return nullptr;
        return new (raw) T(std::forward<Args>(args)...);
    }

    // Reset (frees everything, no individual dealloc)
    void reset() noexcept { offset_ = 0; }

    [[nodiscard]] std::size_t used()  const noexcept { return offset_; }
    [[nodiscard]] std::size_t avail() const noexcept { return ArenaBytes - offset_; }

private:
    alignas(64) std::array<std::byte, ArenaBytes> buf_;
    std::size_t offset_;
};

} // namespace hydra
