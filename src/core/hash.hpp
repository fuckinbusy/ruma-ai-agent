#pragma once

// Non-cryptographic hashing for cache keys (shader program cache, layout and shaping caches).

#include <cstdint>
#include <string_view>

namespace core::hash {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;

// FNV-1a 64-bit. Chain calls by passing the previous result as `seed`.
constexpr auto fnv1a64(std::string_view data, std::uint64_t seed = kFnvOffset) noexcept
    -> std::uint64_t {
    std::uint64_t h = seed;
    for (const char c : data) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace core::hash
