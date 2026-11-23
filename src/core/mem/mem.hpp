#pragma once

// core::mem — the only place in the project that talks to the system allocator.
//
// Every allocation (including global operator new, so STL containers too) goes through
// allocate()/deallocate() and is counted per tag. A tag is a short name ("ui/text") set for
// the current thread with TagScope. Leak checks compare a Snapshot with the current state.
// Ownership is always RAII: Unique<T>, core::Handle<Traits>, containers, arenas.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace core::mem {

inline constexpr std::size_t kMaxTags = 64;
inline constexpr std::size_t kDefaultAlign = 16;

using TagId = std::uint16_t;
inline constexpr TagId kUntagged = 0;

// Interns a tag name (max 31 bytes kept). Up to kMaxTags - 2 user tags: one slot is "untagged",
// one is reserved for overflow. Tag names must be a fixed set; exceeding it is a bug:
// Debug stops the process with a message, Release counts the rest under "<tag-overflow>".
auto tag_id(std::string_view name) -> TagId;
auto tag_name(TagId tag) -> std::string_view;
auto current_tag() noexcept -> TagId;

// Sets the current thread's tag for its lifetime and restores the previous one.
class TagScope {
public:
    explicit TagScope(TagId tag) noexcept;
    ~TagScope();
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;

private:
    TagId previous_;
};

// Raw tracked allocation; returns nullptr when the system is out of memory, for alignments
// above 32 KiB or sizes of 2^48 bytes and more.
// Prefer Unique/containers; raw calls belong in allocators built on top of this.
[[nodiscard]] auto allocate(std::size_t size, std::size_t align = kDefaultAlign) noexcept -> void*;
void deallocate(void* p) noexcept;
// realloc semantics for C libraries' memory callbacks (FreeType's FT_Memory, ...): null p
// allocates, size 0 frees and returns nullptr, failure returns nullptr and keeps p valid.
// The block keeps its tag; the result has the default alignment (16).
[[nodiscard]] auto reallocate(void* p, std::size_t size) noexcept -> void*;

struct TagStats {
    std::string_view name;
    std::int64_t live_bytes = 0;
    std::int64_t live_blocks = 0;
    std::uint64_t total_allocs = 0;
};

struct Snapshot {
    std::array<std::int64_t, kMaxTags> live_bytes{};
    std::array<std::int64_t, kMaxTags> live_blocks{};
};

auto snapshot() noexcept -> Snapshot;
// Number of allocations made so far in the whole process (all tags); never allocates. For
// "this hot path allocates nothing" checks.
auto total_allocations() noexcept -> std::uint64_t;
// The same, made by the calling thread only: a hot path's own allocations, whatever other
// threads of the process do meanwhile.
auto thread_allocations() noexcept -> std::uint64_t;
// Tags that were ever used, with their current counters.
auto stats() -> std::vector<TagStats>;
// The same into `out` (cleared first): allocates nothing once `out` has had room for kMaxTags.
void stats(std::vector<TagStats>& out);
// Tags whose live block count grew since `before`; live_* fields hold the growth. This is a net
// per-tag delta: freeing an older block of the same tag can hide a new leak. Use it for local
// measurements; use the leak epoch for "nothing allocated in this run is still alive".
auto leaks_since(const Snapshot& before) -> std::vector<TagStats>;

// Leak epoch: every block remembers the epoch it was allocated in. begin_leak_epoch() starts a
// new epoch (process-wide, one at a time — call it at a quiet point such as the start of main
// or of a test run); epoch_leaks() lists tags with blocks from the current epoch still alive,
// regardless of how many older blocks were freed meanwhile.
void begin_leak_epoch() noexcept;
auto epoch_leaks() -> std::vector<TagStats>;

// Deletion goes through the replaced global operator delete, i.e. through deallocate().
template <class T>
using Unique = std::unique_ptr<T>;

template <class T, class... Args>
auto make_unique(TagId tag, Args&&... args) -> Unique<T> {
    TagScope scope{tag};
    return std::make_unique<T>(std::forward<Args>(args)...);
}

} // namespace core::mem
