#pragma once

// ui::text_map — every text a frame drew, where it came from and where it lies (responsive text,
// docs/specs/ui-responsive-text.md § 4): what a selection is made of, what Ctrl+C copies, and what
// the tests check for text cut with no way to read it whole.
//
// Each run is one line of one text: its box and baseline, its glyphs (to find a character under
// the mouse), its own copy of the text (the frame's strings are gone when the map is read) and,
// when it was shown cut, the whole of it. The map's memory grows to the largest frame and stays.

#include "ui/layout.hpp"
#include "ui/text/shape.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

using Id = std::uint64_t;

// A text's place in reading order: its pane (the conversation, the data panel, a card over them;
// 0: not selectable — a button's word) and its order in it. A text drawn on several lines keeps one
// key; its lines are told apart by their bytes.
struct TextKey {
    Id pane = 0;
    std::uint64_t order = 0;
    auto operator<=>(const TextKey&) const = default;
};

// Where a drawn text comes from, when its drawer knows better than the defaults (the current pane,
// the next order).
struct TextFrom {
    TextKey key;
    std::string_view full;        // the whole text, when what is drawn is cut
    bool soft_break_after = false; // its line ends where the text wrapped (copied with nothing added)
    int row = -1;                  // a table's cell
    int col = -1;
};

// A character in a text: its key and the byte offset in its source.
struct TextPos {
    TextKey key;
    std::uint32_t byte = 0;
    auto operator<=>(const TextPos&) const = default;
};

struct TextRun {
    TextKey key;
    layout::Box box;              // the line's, logical pixels
    float baseline = 0;
    std::uint32_t first_byte = 0; // where its text starts in its source
    const text::Shaped* shaped = nullptr;
    std::uint32_t first_glyph = 0;
    std::uint32_t end_glyph = 0;
    float scale = 1.0f; // the glyphs' advances are physical
    bool cut = false;       // drawn shorter than it is (an ellipsis)
    bool reachable = true;  // cut, but its whole text shows somewhere (a tooltip) and is copied
    bool soft_break_after = false;
    int row = -1;
    int col = -1;
    // In the map's own storage: text() and full().
    std::uint32_t text_at = 0;
    std::uint32_t text_length = 0;
    std::uint32_t full_at = 0;
    std::uint32_t full_length = 0;
};

class TextMap {
public:
    void begin_frame();
    // `text`: the run's characters as drawn; `full`: the whole text when cut (else empty).
    void add(const TextRun& run, std::string_view text, std::string_view full = {});

    [[nodiscard]] auto runs() const noexcept -> std::span<const TextRun> { return runs_; }
    [[nodiscard]] auto text(const TextRun& run) const -> std::string_view;
    // The whole text: its own when it was not cut.
    [[nodiscard]] auto full(const TextRun& run) const -> std::string_view;
    // The character under (x, y): the nearest edge between two characters of the run whose box
    // holds the point; past its end, its end.
    [[nodiscard]] auto at(float x, float y) const -> std::optional<TextPos>;
    // The edge between two characters of `run` nearest to x: a byte of its source (its start
    // left of it, its end right of it).
    [[nodiscard]] auto byte_at(const TextRun& run, float x) const -> std::uint32_t;
    // The x of a byte in a run (its left edge; the run's end past it).
    [[nodiscard]] auto x_of(const TextRun& run, std::uint32_t byte) const -> float;

private:
    std::vector<TextRun> runs_;
    std::string chars_;
};

} // namespace ui
