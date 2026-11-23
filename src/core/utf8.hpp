#pragma once

// UTF-8 is the text encoding everywhere inside the program; UTF-16 only at Win32 boundaries.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace core::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD;

struct Decoded {
    char32_t cp;         // kReplacement when invalid
    std::uint8_t length; // bytes consumed; for invalid input the maximal subpart (>= 1)
    bool valid;
};

// Decodes the sequence starting at s[pos] (pos < s.size()). Rejects overlongs, surrogates and
// values above U+10FFFF; invalid input reports how many bytes to skip (Unicode "maximal
// subpart" practice), so a decoder loop never stalls.
auto decode(std::string_view s, std::size_t pos) noexcept -> Decoded;

// True if the whole string is well-formed UTF-8. Hot path: ASCII 8 bytes at a time, otherwise
// a table-driven state machine.
auto is_valid(std::string_view s) noexcept -> bool;

// Invalid sequences count as one code point each (as they would render as U+FFFD).
auto count_code_points(std::string_view s) noexcept -> std::size_t;

// Writes cp as UTF-8 into out and returns the length; 0 for surrogates or cp > U+10FFFF.
auto encode(char32_t cp, char (&out)[4]) noexcept -> std::size_t;

// Win32 boundary conversions; malformed input is an error (Code::Parse), never replaced.
auto to_wide(std::string_view s) -> core::Result<std::wstring>;
auto to_utf8(std::wstring_view w) -> core::Result<std::string>;

} // namespace core::utf8
