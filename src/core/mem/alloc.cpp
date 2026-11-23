#include "core/mem/mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <cstddef>
#include <string_view>

// Referenced through /INCLUDE (see src/core/CMakeLists.txt) so this object file — and with it
// the global operator new/delete replacements below — is always linked into every executable.
extern "C" int wa_mem_force_link = 0;

namespace core::mem {
namespace {

constexpr std::size_t kNameCapacity = 32;
constexpr std::uint32_t kCanary = 0xA11C0DE5;
constexpr std::size_t kMaxAlign = std::size_t{1} << 15;  // offset must fit in 16 bits
constexpr std::uint64_t kSizeMask = (std::uint64_t{1} << 48) - 1;
constexpr TagId kOverflowTag = kMaxTags - 1;              // reserved, see tag_id()

// 16-byte header placed directly before every user pointer.
struct Header {
    std::uint64_t size_epoch; // low 48 bits: requested size; high 16 bits: leak epoch
    std::uint32_t canary;
    TagId tag;
    std::uint16_t offset; // distance from the malloc'd block start to the user pointer

    [[nodiscard]] auto size() const noexcept -> std::uint64_t { return size_epoch & kSizeMask; }
    [[nodiscard]] auto epoch() const noexcept -> std::uint16_t {
        return static_cast<std::uint16_t>(size_epoch >> 48);
    }
};
static_assert(sizeof(Header) == 16);

// One cache line per tag so threads counting different tags do not contend.
#pragma warning(push)
#pragma warning(disable : 4324) // padded due to alignment specifier: intended
struct alignas(64) Counter {
    std::atomic<std::int64_t> live_bytes{0};
    std::atomic<std::int64_t> live_blocks{0};
    std::atomic<std::uint64_t> total_allocs{0};
    std::atomic<std::int64_t> epoch_bytes{0};  // live blocks allocated in the current epoch
    std::atomic<std::int64_t> epoch_blocks{0};
};
#pragma warning(pop)

// Zero-initialized statics: valid before any constructor runs, so operator new may be
// called during static initialization of other translation units.
Counter g_counters[kMaxTags];
char g_names[kMaxTags][kNameCapacity];
std::atomic<std::uint16_t> g_tag_count{1}; // user tags live in [1, g_tag_count)
std::atomic<std::uint16_t> g_epoch{0};     // 0 = no leak epoch started
SRWLOCK g_names_lock = SRWLOCK_INIT;

thread_local TagId t_current_tag = kUntagged;
thread_local std::uint64_t t_allocations = 0; // this thread's, all tags

auto header_of(void* user) noexcept -> Header* {
    return reinterpret_cast<Header*>(static_cast<std::byte*>(user) - sizeof(Header));
}

auto name_view(TagId tag) noexcept -> std::string_view {
    if (tag == kUntagged) {
        return "untagged";
    }
    if (tag == kOverflowTag) {
        return "<tag-overflow>";
    }
    return {g_names[tag], std::strlen(g_names[tag])};
}

// Tags that can hold counts: untagged, interned user tags and the overflow tag.
template <class F>
void for_each_used_tag(F&& f) {
    const auto count = g_tag_count.load(std::memory_order_acquire);
    for (TagId i = 0; i < count; ++i) {
        f(i);
    }
    f(kOverflowTag);
}

// Prints a diagnostic and stops the process. Must not allocate: the heap may be suspect.
[[noreturn]] void fail_fast(const char* format, ...) noexcept {
    char message[384];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
                  static_cast<DWORD>(std::min<int>(n, sizeof(message) - 1)), &written, nullptr);
        OutputDebugStringA(message);
    }
    if (IsDebuggerPresent()) {
        __debugbreak();
    }
    __fastfail(FAST_FAIL_HEAP_METADATA_CORRUPTION);
}

auto checked_header(void* p) noexcept -> Header* {
    Header* header = header_of(p);
    if (header->canary != kCanary || header->tag >= kMaxTags) {
        fail_fast("core::mem: deallocate(%p) of a block not allocated by core::mem, already "
                  "freed, or with a corrupted header (canary=0x%08X tag=%u). Typical cause: "
                  "memory allocated by another module (DLL) freed here, see "
                  "docs/01-ARCHITECTURE.md.\n",
                  p, static_cast<unsigned>(header->canary), static_cast<unsigned>(header->tag));
    }
    return header;
}

auto read_snapshot() noexcept -> Snapshot {
    Snapshot snap;
    for_each_used_tag([&](TagId i) {
        snap.live_bytes[i] = g_counters[i].live_bytes.load(std::memory_order_relaxed);
        snap.live_blocks[i] = g_counters[i].live_blocks.load(std::memory_order_relaxed);
    });
    return snap;
}

// Sized operator delete: in Debug the size must match the allocation (type confusion, deleting
// through a base without a virtual destructor, a wrong deleter).
void deallocate_sized(void* p, std::size_t size) noexcept {
#ifdef _DEBUG
    if (p != nullptr) {
        const Header* header = checked_header(p);
        const std::uint64_t expected = size == 0 ? 1 : size;
        if (header->size() != expected) {
            fail_fast("core::mem: sized delete of %zu bytes for a block of %llu bytes at %p "
                      "(wrong type or deleter)\n",
                      size, static_cast<unsigned long long>(header->size()), p);
        }
    }
#else
    (void)size;
#endif
    deallocate(p);
}

} // namespace

auto tag_id(std::string_view name) -> TagId {
    if (name.empty() || name == "untagged") {
        return kUntagged;
    }
    name = name.substr(0, kNameCapacity - 1);

    AcquireSRWLockExclusive(&g_names_lock);
    const auto count = g_tag_count.load(std::memory_order_relaxed);
    TagId result = kUntagged;
    bool found = false;
    for (TagId i = 1; i < count; ++i) {
        if (name_view(i) == name) {
            result = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (count < kOverflowTag) {
            std::memcpy(g_names[count], name.data(), name.size());
            g_names[count][name.size()] = '\0';
            result = count;
            g_tag_count.store(static_cast<std::uint16_t>(count + 1), std::memory_order_release);
        } else {
#ifdef _DEBUG
            ReleaseSRWLockExclusive(&g_names_lock);
            fail_fast("core::mem: too many memory tags (max %u); tag names must be a fixed set, "
                      "not built at run time (rejected: '%.*s')\n",
                      static_cast<unsigned>(kOverflowTag - 1), static_cast<int>(name.size()),
                      name.data());
#else
            result = kOverflowTag; // reported by name, never mislabeled as another tag
#endif
        }
    }
    ReleaseSRWLockExclusive(&g_names_lock);
    return result;
}

auto tag_name(TagId tag) -> std::string_view {
    if (tag == kOverflowTag) {
        return name_view(tag);
    }
    if (tag >= g_tag_count.load(std::memory_order_acquire)) {
        return "invalid";
    }
    return name_view(tag);
}

auto current_tag() noexcept -> TagId {
    return t_current_tag;
}

TagScope::TagScope(TagId tag) noexcept : previous_(t_current_tag) {
    t_current_tag = tag;
}

TagScope::~TagScope() {
    t_current_tag = previous_;
}

auto allocate(std::size_t size, std::size_t align) noexcept -> void* {
    align = std::max(align, kDefaultAlign);
    if ((align & (align - 1)) != 0 || align > kMaxAlign || size > kSizeMask) {
        return nullptr;
    }
    const std::size_t extra = sizeof(Header) + (align - kDefaultAlign);

    // malloc returns 16-byte aligned memory on x64; over-allocate for stricter alignment.
    auto* raw = static_cast<std::byte*>(std::malloc(size + extra));
    if (raw == nullptr) {
        return nullptr;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header);
    const auto user = (first + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);

    const TagId tag = t_current_tag;
    const std::uint16_t epoch = g_epoch.load(std::memory_order_relaxed);
    auto* header = reinterpret_cast<Header*>(user - sizeof(Header));
    header->size_epoch = static_cast<std::uint64_t>(size) | (std::uint64_t{epoch} << 48);
    header->canary = kCanary;
    header->tag = tag;
    header->offset = static_cast<std::uint16_t>(user - reinterpret_cast<std::uintptr_t>(raw));

    Counter& c = g_counters[tag];
    c.live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
    c.live_blocks.fetch_add(1, std::memory_order_relaxed);
    c.total_allocs.fetch_add(1, std::memory_order_relaxed);
    ++t_allocations;
    if (epoch != 0) {
        c.epoch_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
        c.epoch_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<void*>(user);
}

void deallocate(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    Header* header = checked_header(p);
    const auto size = static_cast<std::int64_t>(header->size());
    Counter& c = g_counters[header->tag];
    c.live_bytes.fetch_sub(size, std::memory_order_relaxed);
    c.live_blocks.fetch_sub(1, std::memory_order_relaxed);
    const std::uint16_t epoch = header->epoch();
    if (epoch != 0 && epoch == g_epoch.load(std::memory_order_relaxed)) {
        c.epoch_bytes.fetch_sub(size, std::memory_order_relaxed);
        c.epoch_blocks.fetch_sub(1, std::memory_order_relaxed);
    }

    header->canary = 0; // makes a second free of the same block fail fast
    std::free(static_cast<std::byte*>(p) - header->offset);
}

auto reallocate(void* p, std::size_t size) noexcept -> void* {
    if (p == nullptr) {
        return allocate(size);
    }
    if (size == 0) {
        deallocate(p);
        return nullptr;
    }
    if (size > kSizeMask) {
        return nullptr;
    }
    Header* header = checked_header(p);
    const std::uint64_t old_size = header->size();
    if (header->offset != sizeof(Header)) {
        // Over-aligned block: the CRT cannot move it keeping our offset; copy instead.
        TagScope scope{header->tag};
        void* fresh = allocate(size);
        if (fresh == nullptr) {
            return nullptr;
        }
        std::memcpy(fresh, p, static_cast<std::size_t>(std::min<std::uint64_t>(old_size, size)));
        // The copy is the same block to its owner: it keeps the old block's leak epoch.
        const std::uint16_t old_epoch = header->epoch();
        Header* fresh_header = reinterpret_cast<Header*>(static_cast<std::byte*>(fresh) - sizeof(Header));
        const std::uint16_t current = g_epoch.load(std::memory_order_relaxed);
        if (old_epoch != current && current != 0) {
            Counter& c = g_counters[fresh_header->tag];
            c.epoch_bytes.fetch_sub(static_cast<std::int64_t>(size), std::memory_order_relaxed);
            c.epoch_blocks.fetch_sub(1, std::memory_order_relaxed);
        }
        fresh_header->size_epoch = static_cast<std::uint64_t>(size) | (std::uint64_t{old_epoch} << 48);
        deallocate(p);
        return fresh;
    }
    const TagId tag = header->tag;
    const std::uint16_t epoch = header->epoch();
    auto* raw = static_cast<std::byte*>(std::realloc(header, size + sizeof(Header)));
    if (raw == nullptr) {
        return nullptr; // the old block is untouched and still valid
    }
    auto* moved = reinterpret_cast<Header*>(raw);
    moved->size_epoch = static_cast<std::uint64_t>(size) | (std::uint64_t{epoch} << 48);
    const auto delta = static_cast<std::int64_t>(size) - static_cast<std::int64_t>(old_size);
    Counter& c = g_counters[tag];
    c.live_bytes.fetch_add(delta, std::memory_order_relaxed);
    if (epoch != 0 && epoch == g_epoch.load(std::memory_order_relaxed)) {
        c.epoch_bytes.fetch_add(delta, std::memory_order_relaxed);
    }
    return raw + sizeof(Header);
}

auto snapshot() noexcept -> Snapshot {
    return read_snapshot();
}

auto thread_allocations() noexcept -> std::uint64_t {
    return t_allocations;
}

auto total_allocations() noexcept -> std::uint64_t {
    std::uint64_t total = 0;
    for_each_used_tag([&](TagId i) {
        total += g_counters[i].total_allocs.load(std::memory_order_relaxed);
    });
    return total;
}

auto stats() -> std::vector<TagStats> {
    std::vector<TagStats> out;
    stats(out);
    return out;
}

void stats(std::vector<TagStats>& out) {
    const auto snap = read_snapshot(); // read before making room, as stats() always did
    out.clear();
    out.reserve(kMaxTags);
    for_each_used_tag([&](TagId i) {
        out.push_back({name_view(i), snap.live_bytes[i], snap.live_blocks[i],
                       g_counters[i].total_allocs.load(std::memory_order_relaxed)});
    });
}

auto leaks_since(const Snapshot& before) -> std::vector<TagStats> {
    const auto now = read_snapshot(); // read before allocating the result
    std::vector<TagStats> out;
    for_each_used_tag([&](TagId i) {
        const auto blocks = now.live_blocks[i] - before.live_blocks[i];
        if (blocks > 0) {
            out.push_back({name_view(i), now.live_bytes[i] - before.live_bytes[i], blocks,
                           g_counters[i].total_allocs.load(std::memory_order_relaxed)});
        }
    });
    return out;
}

void begin_leak_epoch() noexcept {
    std::uint16_t next = static_cast<std::uint16_t>(g_epoch.load(std::memory_order_relaxed) + 1);
    if (next == 0) {
        next = 1;
    }
    for (Counter& c : g_counters) {
        c.epoch_bytes.store(0, std::memory_order_relaxed);
        c.epoch_blocks.store(0, std::memory_order_relaxed);
    }
    g_epoch.store(next, std::memory_order_release);
}

auto epoch_leaks() -> std::vector<TagStats> {
    std::array<std::int64_t, kMaxTags> bytes{};
    std::array<std::int64_t, kMaxTags> blocks{};
    for (std::size_t i = 0; i < kMaxTags; ++i) { // read before allocating the result
        bytes[i] = g_counters[i].epoch_bytes.load(std::memory_order_relaxed);
        blocks[i] = g_counters[i].epoch_blocks.load(std::memory_order_relaxed);
    }
    std::vector<TagStats> out;
    for_each_used_tag([&](TagId i) {
        if (blocks[i] > 0) {
            out.push_back({name_view(i), bytes[i], blocks[i],
                           g_counters[i].total_allocs.load(std::memory_order_relaxed)});
        }
    });
    return out;
}

} // namespace core::mem

// ---- Global operator new/delete: everything goes through core::mem -------------------------

namespace {

auto checked(std::size_t size, std::size_t align) -> void* {
    if (void* p = core::mem::allocate(size == 0 ? 1 : size, align)) {
        return p;
    }
    throw std::bad_alloc{};
}

} // namespace

void* operator new(std::size_t size) {
    return checked(size, core::mem::kDefaultAlign);
}
void* operator new[](std::size_t size) {
    return checked(size, core::mem::kDefaultAlign);
}
void* operator new(std::size_t size, std::align_val_t align) {
    return checked(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return checked(size, static_cast<std::size_t>(align));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return core::mem::allocate(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return core::mem::allocate(size == 0 ? 1 : size);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return core::mem::allocate(size == 0 ? 1 : size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return core::mem::allocate(size == 0 ? 1 : size, static_cast<std::size_t>(align));
}

void operator delete(void* p) noexcept {
    core::mem::deallocate(p);
}
void operator delete[](void* p) noexcept {
    core::mem::deallocate(p);
}
void operator delete(void* p, std::size_t size) noexcept {
    core::mem::deallocate_sized(p, size);
}
void operator delete[](void* p, std::size_t size) noexcept {
    core::mem::deallocate_sized(p, size);
}
void operator delete(void* p, std::align_val_t) noexcept {
    core::mem::deallocate(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    core::mem::deallocate(p);
}
void operator delete(void* p, std::size_t size, std::align_val_t) noexcept {
    core::mem::deallocate_sized(p, size);
}
void operator delete[](void* p, std::size_t size, std::align_val_t) noexcept {
    core::mem::deallocate_sized(p, size);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    core::mem::deallocate(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    core::mem::deallocate(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    core::mem::deallocate(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    core::mem::deallocate(p);
}
