#pragma once

// app::answer — the agent's answer as blocks ready to draw: markdown parses it (md4c, phase
// 8), this shapes each block's text in its faces. The answer only grows while it streams: a block
// whose text grew with the same styles so far is appended to (only its last line is shaped
// again); one that changed otherwise is shaped anew; a text that does not continue the previous
// one starts over.

#include "markdown/markdown.hpp"
#include "ui/text/paragraph.hpp"
#include "ui/ui.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::answer {

// A paragraph's faces (Glyph::face); widgets::StreamStyle colours them in this order.
enum Face : std::uint8_t { kRegular, kBold, kItalic, kBoldItalic, kCode, kLink };

// A link's text in a block (byte offsets) and where it goes.
struct Link {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string href;
};

struct Block {
    markdown::Kind kind = markdown::Kind::Paragraph;
    int level = 0;        // a heading's 1–6
    int depth = 0;        // a list item's nesting
    bool ordered = false; // a numbered list's item…
    int number = 0;       // …and its number
    ui::text::Paragraph paragraph; // empty for a table and a rule
    std::vector<Link> links;
    std::vector<std::vector<std::string>> cells; // a table's rows, its header first
    std::vector<char> align;                     // per column: 'l', 'c' or 'r' (numbers go right)
    bool panel = false;                          // too big for the text (markdown::to_panel)
    markdown::Block source;                      // what it was made from

    // How far it has come: its text's bytes, or a table's rows (widgets::StreamTimes marks it).
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return kind == markdown::Kind::Table ? cells.size() : paragraph.text().size();
    }
};

class Doc {
public:
    // `size`: the prose size in logical pixels (the text scale in it); paragraphs are shaped at
    // size x scale.
    // Room for a long answer from the start: a printing answer does not grow its copy frame by frame.
    Doc(ui::Fonts& fonts, float size, float scale = 1.0f) : fonts_(&fonts), size_(size), scale_(scale) { seen_.reserve(16 * 1024); }

    void reset();
    // A document's page (the Preview, phase 10): every table whole, none sent to the data panel.
    void keep_tables_whole() noexcept { whole_tables_ = true; }
    // `answer` is the whole answer so far; `finished`: nothing more is coming
    // (markdown::Document::sync).
    void sync(std::string_view answer, bool finished = false);
    // Logical pixels: text wraps at `width` (less a list item's or a quote's indent), a code
    // block's lines at `code_width` (less its padding).
    void set_width(float width) { set_width(width, width); }
    void set_width(float width, float code_width);

    [[nodiscard]] auto blocks() const noexcept -> const std::vector<Block>& { return blocks_; }
    [[nodiscard]] auto scale() const noexcept -> float { return scale_; }

    // A block's size (logical pixels, the text scale in it) and line height (a multiple of it).
    [[nodiscard]] auto size_of(const Block& b) const noexcept -> float;
    [[nodiscard]] static auto line_of(const Block& b) noexcept -> float;
    // A block's height and the room between two blocks (logical pixels).
    [[nodiscard]] auto height(const Block& b) const noexcept -> float;
    [[nodiscard]] static auto gap(const Block& above, const Block& below) noexcept -> float;
    // All blocks from the first, their gaps and the last one's margin.
    [[nodiscard]] auto height() const noexcept -> float;
    // Where a block's text starts, right of its left edge.
    [[nodiscard]] static auto indent(const Block& b) noexcept -> float;

    static constexpr float kItemIndent = 16.0f;    // li padding-left
    static constexpr float kNumberIndent = 22.0f;  // an ordered item: room for "12."
    static constexpr float kNestIndent = 18.0f;    // per level of nesting
    static constexpr float kQuoteIndent = 14.0f;   // past its 2 px bar
    static constexpr float kCodePadX = 12.0f;
    static constexpr float kCodePadY = 10.0f;
    static constexpr float kHeaderRow = 32.0f;     // a table in the text (mockup-spec § 4)
    static constexpr float kRow = 36.0f;
    static constexpr float kPanelCard = 56.0f;     // a table too big for the text: its card there

private:
    using Faces = std::array<ui::text::FontSet*, ui::text::Paragraph::kMaxFaces>;
    [[nodiscard]] auto faces_for(const markdown::Block& source) const -> Faces;
    [[nodiscard]] auto make(const markdown::Block& source) -> Block;
    [[nodiscard]] auto size_for(markdown::Kind kind, int level) const noexcept -> float;
    void apply_width(Block& b) const;

    ui::Fonts* fonts_;
    float size_;
    float scale_;
    bool whole_tables_ = false;
    float width_ = 1.0e9f;
    float code_width_ = 1.0e9f;
    std::string seen_; // the answer of the last sync…
    bool finished_ = false; // …and whether it was all
    markdown::Document md_;
    std::vector<Block> blocks_;
};

} // namespace app::answer
