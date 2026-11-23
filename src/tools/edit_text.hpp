#pragma once

// tools::edit — fs.edit's text (phase 12 § 1): a file's bytes as text and back as they were (its
// encoding, BOM and line breaks), the edits applied all or none, and the unified diff the
// permission card shows.

#include "core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tools::edit {

enum class Encoding : std::uint8_t { Utf8, Utf8Bom, Utf16Le, Cp1251 };

struct Text {
    Encoding encoding = Encoding::Utf8;
    bool crlf = false; // every line break was CRLF: `utf8` has \n, encode() writes CRLF again
    std::string utf8;  // the text, UTF-8
};

// Refused: a binary file (a NUL in its first 8 KB, not UTF-16 with its BOM).
[[nodiscard]] auto decode(std::string_view bytes) -> core::Result<Text>;
// Refused: a text its encoding cannot hold (Windows-1251 and a character outside it).
[[nodiscard]] auto encode(const Text& text) -> core::Result<std::string>;

struct Edit {
    std::string old_text; // the exact piece of the file
    std::string new_text;
    bool all = false; // every occurrence; otherwise there must be exactly one
};

// The edits in order, each on the text the one before left. Any anchor missing, empty or (without
// `all`) found more than once fails the whole edit: "edit N: not found" / "found K times".
[[nodiscard]] auto apply(std::string_view text, std::span<const Edit> edits) -> core::Result<std::string>;

// A unified diff by lines: hunks "@@ -a,b +c,d @@", lines " ", "-", "+"; `context` lines around
// each change; past `max_lines` lines "… N more lines".
[[nodiscard]] auto unified_diff(std::string_view before, std::string_view after, int context = 3, int max_lines = 200)
    -> std::string;

} // namespace tools::edit
