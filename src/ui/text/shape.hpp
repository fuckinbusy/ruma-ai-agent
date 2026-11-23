#pragma once

// ui::text::shape — itemises UTF-8 text into runs of one font (fallback chain), shapes each run
// with HarfBuzz and returns positioned glyphs. Left-to-right scripts only for now.
// ui::text::ShapeCache — immediate-mode UI asks for the same labels every frame; this keeps
// their shaped glyphs keyed by (text, font set, size) and drops entries unused for a while.

#include "ui/text/fonts.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui::text {

struct Glyph {
    std::uint32_t id = 0;      // glyph index in its font
    std::uint8_t face = 0;     // which font set of a multi-face paragraph shaped it
    std::uint32_t cluster = 0; // byte offset of the first character it renders
    float x_advance = 0;       // pixels
    float x_offset = 0;
    float y_offset = 0;        // up is positive (HarfBuzz convention)
    FontIndex font = 0;
};

struct Shaped {
    std::vector<Glyph> glyphs; // in logical (= visual, LTR) order, clusters non-decreasing
    float width = 0;           // sum of advances
    float ascent = 0;          // line metrics of the chain's first font
    float descent = 0;
    float line_height = 0;
};

// `tabular`: the OpenType feature tnum (figures of one width, for numbers in columns). One line
// of text: a line break in it is a space (phase 10: a question with lines in a label).
auto shape(FontSet& fonts, std::string_view text, float px, bool tabular = false) -> Shaped;

// Shapes text[from, end) with the text before `from` as context and appends the glyphs to
// `out` (clusters are offsets into `text`). Newlines produce no glyphs (the wrapper's), unless
// `one_line`: then a '\n' is a space ("\r\n" one).
void shape_append(FontSet& fonts, std::string_view text, std::size_t from, float px,
                  Shaped& out);
// The same for text[from, end), marking the glyphs with `face`.
void shape_append(FontSet& fonts, std::string_view text, std::size_t from, std::size_t end,
                  float px, Shaped& out, std::uint8_t face, bool tabular = false, bool one_line = false);

class ShapeCache {
public:
    static constexpr int kKeepFrames = 120;

    // Room made once: the spares' list never grows, and a few entries wait for the first new
    // texts (a clock's next second) before any has been evicted.
    ShapeCache();

    struct Stats {
        std::uint64_t shaped = 0;
        std::uint64_t hits = 0;
    };

    // The reference is valid until end_frame() evicts the entry or the cache is destroyed.
    auto get(FontSet& fonts, std::string_view text, float px, bool tabular = false) -> const Shaped&;
    // Drops entries not used during the last kKeepFrames frames.
    void end_frame();

    [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }
    [[nodiscard]] auto stats() const noexcept -> const Stats& { return stats_; }

private:
    struct Entry {
        std::string text;
        const FontSet* fonts = nullptr;
        float px = 0;
        bool tabular = false;
        std::uint64_t last_used = 0;
        Shaped shaped;
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
    // Evicted entries kept for the next new texts (their memory reused), at most kSpare.
    static constexpr std::size_t kSpare = 64;
    static constexpr std::size_t kRoom = 48; // each entry's bytes and glyphs from the start: a status line or a cell
    std::vector<std::unordered_map<std::uint64_t, Entry>::node_type> spare_;
    std::uint64_t frame_ = 0;
    Stats stats_;
};

} // namespace ui::text
