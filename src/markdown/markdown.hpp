#pragma once

// markdown — the model's answer as blocks to draw (docs/specs/phase-8-ui-remaster.md § 5.5),
// parsed by md4c (GitHub's dialect, HTML as text). The answer streams in: blocks that are
// complete (before a blank line, outside a code fence, the next line not indented) are parsed
// once and kept; the rest is parsed again as it grows, holding back markup that is not closed
// yet — a table without its delimiter row, half a row, a lone "-" or "=" (it would turn the line
// above into a heading for a moment), an unclosed "**", "*", "`" or link.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markdown {

enum class Kind : std::uint8_t { Paragraph, Heading, ListItem, Code, Quote, Rule, Table };

// A styled range of a block's text (byte offsets).
struct Inline {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool link = false;
    std::string href;

    auto operator==(const Inline&) const -> bool = default;
};

struct Block {
    Kind kind = Kind::Paragraph;
    int level = 0;        // a heading's 1–6
    int depth = 0;        // a list item's nesting, 0 at the top
    bool ordered = false; // a list item of a numbered list…
    int number = 0;       // …and its number
    std::string text;     // without markup; a code block's lines joined by '\n'
    std::vector<Inline> inlines;
    std::vector<std::vector<std::string>> cells; // a table's rows, its header first
    std::vector<char> align;                     // per column: 'l', 'c' or 'r'

    auto operator==(const Block&) const -> bool = default;
};

class Document {
public:
    // The whole answer so far. A text that does not continue the last one starts over. A
    // `finished` answer has nothing more coming: its last line is shown whole.
    void sync(std::string_view text, bool finished = false);

    [[nodiscard]] auto blocks() const noexcept -> std::span<const Block> { return blocks_; }
    // Bytes handed to the parser so far (the streaming test watches it).
    [[nodiscard]] auto parsed_bytes() const noexcept -> std::uint64_t { return parsed_bytes_; }

private:
    std::string text_;
    std::size_t stable_ = 0;        // text_[0, stable_) is complete blocks…
    std::size_t stable_blocks_ = 0; // …which are blocks_[0, stable_blocks_)
    std::vector<Block> blocks_;
    std::uint64_t parsed_bytes_ = 0;
    // The last parse showed the whole unstable tail (nothing held back): plain words that follow
    // are the last paragraph's text without a parse (sync's fast path).
    bool whole_ = false;
};

// A table too big for the text (more than 5 rows of data or 3 columns): it goes to the data panel.
auto to_panel(const Block& table) -> bool;

} // namespace markdown
