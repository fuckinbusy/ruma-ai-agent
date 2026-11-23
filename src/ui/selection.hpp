#pragma once

// ui::selection — text selected with the mouse as on a web page (responsive text, docs/specs/
// ui-responsive-text.md § 4): from one character to another in reading order inside one pane, kept
// by its texts' keys and bytes, so it stays on the same characters while they are drawn again,
// stream in or scroll out of view. What it copies is its sources' bytes: a line wrapped on screen
// adds nothing, a text's own line end is "\n", cells of a row are joined by a tab.

#include "ui/text_map.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui {

class Selection {
public:
    // A selection from `anchor` (where the press was) to `focus` (where the mouse is), in one pane.
    void set(TextPos anchor, TextPos focus) noexcept;
    void extend(TextPos focus) noexcept;
    // All of a pane: its texts as drawn and seen, or what its provider gives (a table's rows).
    void select_all(Id pane) noexcept;
    void clear() noexcept;

    [[nodiscard]] auto active() const noexcept -> bool { return whole_ || (pane_ != 0 && anchor_ != focus_); }
    [[nodiscard]] auto whole() const noexcept -> bool { return whole_; }
    [[nodiscard]] auto pane() const noexcept -> Id { return pane_; }
    [[nodiscard]] auto anchor() const noexcept -> TextPos { return anchor_; }
    // Its ends in reading order.
    [[nodiscard]] auto range() const noexcept -> std::pair<TextPos, TextPos>;
    // The bytes of `run`'s source it covers: [first, second); first == second: none.
    [[nodiscard]] auto covered(const TextRun& run) const noexcept -> std::pair<std::uint32_t, std::uint32_t>;

    // Keeps the covered part of each run of this frame: what is copied when it is out of view.
    void seen(const TextMap& map);
    // Its text: the covered runs (this frame's, then the kept ones) in reading order.
    void copy(const TextMap& map, std::string& out) const;

private:
    struct Kept {
        TextKey key;
        std::uint32_t first_byte = 0;
        bool soft_break_after = false;
        int row = -1;
        int col = -1;
        std::string text; // its whole run
    };
    Id pane_ = 0;
    TextPos anchor_{};
    TextPos focus_{};
    bool whole_ = false;
    std::vector<Kept> kept_;
    std::size_t kept_count_ = 0; // the entries in use: their strings' room is kept for the next ones
};

} // namespace ui
