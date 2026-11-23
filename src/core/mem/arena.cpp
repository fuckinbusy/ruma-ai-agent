#include "core/mem/arena.hpp"

#include <algorithm>
#include <cstdint>

namespace core::mem {

// Block header; the data area starts right after it (16-byte aligned).
struct Arena::Block {
    Block* prev;
    std::size_t capacity; // bytes in the data area
};

namespace {

constexpr std::size_t kMaxArenaAlign = 4096;
// Far above any real request and far below where size + header + alignment could wrap.
constexpr std::size_t kMaxArenaRequest = std::size_t{1} << 46;
constexpr std::size_t kBlockHeader = 16; // sizeof(Arena::Block), checked in allocate()

auto data_of(void* block) noexcept -> std::byte* {
    return static_cast<std::byte*>(block) + kBlockHeader;
}

} // namespace

Arena::Arena(TagId tag, std::size_t block_size) noexcept
    : tag_(tag), block_size_(std::max<std::size_t>(block_size, 256)) {}

Arena::~Arena() {
    free_all();
}

Arena::Arena(Arena&& other) noexcept
    : head_(std::exchange(other.head_, nullptr)), offset_(std::exchange(other.offset_, 0)),
      used_(std::exchange(other.used_, 0)), tag_(other.tag_), block_size_(other.block_size_) {
    ++other.generation_; // scopes on the moved-from arena must not restore its old state
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        free_all();
        head_ = std::exchange(other.head_, nullptr);
        offset_ = std::exchange(other.offset_, 0);
        used_ = std::exchange(other.used_, 0);
        tag_ = other.tag_;
        block_size_ = other.block_size_;
        ++generation_;
        ++other.generation_;
    }
    return *this;
}

auto Arena::allocate(std::size_t size, std::size_t align) -> void* {
    static_assert(sizeof(Block) == kBlockHeader);
    if (align == 0 || (align & (align - 1)) != 0 || align > kMaxArenaAlign) {
        return nullptr;
    }
    if (head_ != nullptr) {
        const auto base = reinterpret_cast<std::uintptr_t>(data_of(head_));
        const auto start = (base + offset_ + align - 1) & ~(std::uintptr_t{align} - 1);
        const std::size_t padding = start - (base + offset_);
        const std::size_t room = head_->capacity - offset_;
        if (padding <= room && size <= room - padding) { // no wrap-around for huge sizes
            offset_ += padding + size;
            used_ += padding + size;
            return reinterpret_cast<void*>(start);
        }
    }
    if (size > kMaxArenaRequest) {
        return nullptr;
    }

    // New block: the usual size, or exactly big enough for an oversized request.
    const std::size_t needed = size + (align > 16 ? align - 16 : 0);
    const std::size_t capacity = std::max(block_size_ - sizeof(Block), needed);
    void* raw = nullptr;
    {
        TagScope scope{tag_};
        raw = core::mem::allocate(sizeof(Block) + capacity, 16);
    }
    if (raw == nullptr) {
        return nullptr;
    }
    auto* block = ::new (raw) Block{head_, capacity};
    head_ = block;
    offset_ = 0;

    const auto base = reinterpret_cast<std::uintptr_t>(data_of(head_));
    const auto start = (base + align - 1) & ~(std::uintptr_t{align} - 1);
    const std::size_t padding = start - base;
    offset_ = padding + size;
    used_ += padding + size;
    return reinterpret_cast<void*>(start);
}

void Arena::reset() noexcept {
    if (head_ == nullptr) {
        return;
    }
    Block* oldest = head_;
    while (oldest->prev != nullptr) {
        oldest = oldest->prev;
    }
    free_until(oldest);
    offset_ = 0;
    used_ = 0;
    ++generation_;
}

auto Arena::block_count() const noexcept -> std::size_t {
    std::size_t n = 0;
    for (const Block* b = head_; b != nullptr; b = b->prev) {
        ++n;
    }
    return n;
}

void Arena::free_until(Block* keep) noexcept {
    while (head_ != nullptr && head_ != keep) {
        Block* prev = head_->prev;
        core::mem::deallocate(head_);
        head_ = prev;
    }
}

void Arena::free_all() noexcept {
    free_until(nullptr);
    offset_ = 0;
    used_ = 0;
}

Arena::Scope::Scope(Arena& arena) noexcept
    : arena_(arena), head_(arena.head_), offset_(arena.offset_), used_(arena.used_),
      generation_(arena.generation_) {}

Arena::Scope::~Scope() {
    if (arena_.generation_ != generation_) {
        // reset() or a move happened while this scope was alive: the mark is meaningless, so
        // rewind to the reset state instead of restoring stale counters.
        arena_.reset();
        return;
    }
    arena_.free_until(static_cast<Block*>(head_));
    arena_.offset_ = offset_;
    arena_.used_ = used_;
}

auto ArenaResource::do_allocate(std::size_t bytes, std::size_t align) -> void* {
    if (void* p = arena_.allocate(bytes, align)) {
        return p;
    }
    throw std::bad_alloc{}; // the memory_resource contract
}

} // namespace core::mem
