#pragma once

// core::mem::Arena — linear (bump) allocator for short-lived data: per-frame UI geometry,
// per-request network buffers, per-conversation text. Blocks come from core::mem under the
// arena's tag; individual allocations are never freed, the arena is rewound or reset as a whole.

#include "core/mem/mem.hpp"

#include <cstddef>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

namespace core::mem {

class Arena {
public:
    explicit Arena(TagId tag, std::size_t block_size = 64 * 1024) noexcept;
    ~Arena();
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Returns nullptr when out of memory or for alignments above 4096.
    [[nodiscard]] auto allocate(std::size_t size, std::size_t align = alignof(std::max_align_t))
        -> void*;

    // Constructs a T in the arena. Destructors never run, so T must not own resources.
    template <class T, class... Args>
    [[nodiscard]] auto make(Args&&... args) -> T* {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors; T must be trivially destructible");
        void* p = allocate(sizeof(T), alignof(T));
        return p ? ::new (p) T{std::forward<Args>(args)...} : nullptr;
    }

    // Frees every block except the oldest and starts over.
    void reset() noexcept;

    [[nodiscard]] auto bytes_used() const noexcept -> std::size_t { return used_; }
    [[nodiscard]] auto block_count() const noexcept -> std::size_t;

    // RAII mark: on destruction rewinds the arena to where it was and frees blocks added since.
    // If the arena was reset or moved from while the scope was alive, it rewinds to the reset
    // state (one block, nothing used).
    class Scope {
    public:
        explicit Scope(Arena& arena) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Arena& arena_;
        void* head_;
        std::size_t offset_;
        std::size_t used_;
        std::uint64_t generation_;
    };

private:
    struct Block;
    void free_until(Block* keep) noexcept;
    void free_all() noexcept;

    Block* head_ = nullptr;  // newest block; allocations come from here
    std::size_t offset_ = 0; // bytes consumed in head_'s data area
    std::size_t used_ = 0;   // bytes handed out, including alignment padding
    std::uint64_t generation_ = 0; // bumped by reset() and moves; invalidates live Scopes
    TagId tag_;
    std::size_t block_size_;
};

// std::pmr adapter: containers allocate from the arena; deallocation is a no-op.
class ArenaResource final : public std::pmr::memory_resource {
public:
    explicit ArenaResource(Arena& arena) noexcept : arena_(arena) {}

private:
    auto do_allocate(std::size_t bytes, std::size_t align) -> void* override;
    void do_deallocate(void*, std::size_t, std::size_t) noexcept override {}
    [[nodiscard]] auto do_is_equal(const std::pmr::memory_resource& other) const noexcept
        -> bool override {
        return this == &other;
    }

    Arena& arena_;
};

} // namespace core::mem
