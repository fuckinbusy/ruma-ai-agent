#pragma once

// ui::text::wrap — greedy line breaking over shaped glyphs (simplified UAX #14): breaks after
// spaces and hyphens, between CJK characters (not before closing CJK punctuation), always at
// '\n'; a word wider than the line is broken between characters. Spaces at a break hang: they
// count neither towards the line's width nor start the next line.

#include "ui/text/shape.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ui::text {

struct Line {
    std::uint32_t first_glyph = 0;
    std::uint32_t end_glyph = 0;  // glyphs [first, end) are drawn; hanging spaces excluded
    std::uint32_t first_byte = 0;
    std::uint32_t end_byte = 0;   // text [first, end), without hanging spaces and '\n'
    float width = 0;
};

// Where a line starts: a byte offset and the first glyph at or after it.
struct LineStart {
    std::uint32_t byte = 0;
    std::uint32_t glyph = 0;
};

// Replaces `out` with the lines of the whole text.
void wrap(const Shaped& shaped, std::string_view text, float max_width, std::vector<Line>& out);
// Appends the lines from `start` (a line start) to the end of the text.
void wrap_from(const Shaped& shaped, std::string_view text, float max_width, LineStart start,
               std::vector<Line>& out);

} // namespace ui::text
