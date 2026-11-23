// The answer's Markdown (phase 8, docs/specs/phase-8-ui-remaster.md § 5.5; the mockups'
// text answer, .superpowers/remaster/mockup-spec.md § 2 and § 4): headings, prose with bold,
// italics, code and links, lists, quotes, code blocks, rules and tables, each letter fading in
// as it comes (widgets::stream_text).

#include "app/present.hpp"
#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/anim.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <string_view>

namespace app::sheet {
namespace {

namespace theme = ui::theme;
namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
using markdown::Kind;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

constexpr float kTableMax = 520.0f;              // a table in the text: max-width 520
constexpr float kCellPad = 14.0f;                // its rows' padding 0 14, columns 14 apart
constexpr std::size_t kMeasuredRows = 50;        // a column is as wide as its first rows need

// The colours of answer::Face, in its order: regular, bold, italic, bold italic, code, link —
// from the theme of this frame (made each time it is asked for: a theme can change). What is read is
// `text` whatever its weight; code is `text` too (the designer: no other highlighting); a link is
// the accent.
auto prose_style() -> widgets::StreamStyle {
    const auto& th = ui::th();
    return {{th.text, th.text, th.text, th.text, th.text, th.accent}, theme::type::prose_line};
}
auto heading_style() -> widgets::StreamStyle {
    const auto& th = ui::th();
    return {{th.text, th.text, th.text, th.text, th.text, th.accent}, 1.3f};
}
auto quote_style() -> widgets::StreamStyle {
    const auto& th = ui::th();
    return {{th.text_2, th.text, th.text_2, th.text, th.text, th.accent}, theme::type::prose_line};
}
auto code_style() -> widgets::StreamStyle {
    const auto& th = ui::th();
    return {{th.text, th.text, th.text, th.text, th.text, th.text}, 1.55f};
}

// A document's Preview (phase 10, screen 24): the same blocks in dark ink on a white page.
// (The designer: paper is the same in both themes — it is a picture of a document, not the interface.)
constexpr Color kPaperText{40, 44, 56, 255};   // color-ok: a document's paper, the same in both themes
constexpr Color kPaperStrong{16, 20, 30, 255}; // color-ok: a document's paper
constexpr Color kPaperSoft{92, 98, 112, 255};  // color-ok: a document's paper
constexpr Color kPaperLink{37, 99, 235, 255};  // color-ok: a document's paper
constexpr widgets::StreamStyle kPaperProse{{kPaperText, kPaperStrong, kPaperText, kPaperStrong, kPaperStrong, kPaperLink}, theme::type::prose_line};
constexpr widgets::StreamStyle kPaperHeading{{kPaperStrong, kPaperStrong, kPaperStrong, kPaperStrong, kPaperStrong, kPaperLink}, 1.3f};
constexpr widgets::StreamStyle kPaperQuote{{kPaperSoft, kPaperText, kPaperSoft, kPaperText, kPaperStrong, kPaperLink}, theme::type::prose_line};
constexpr widgets::StreamStyle kPaperCode{{kPaperStrong, kPaperStrong, kPaperStrong, kPaperStrong, kPaperStrong, kPaperStrong}, 1.55f};

// The colours around the text: the screen's, or the page's.
struct Ink {
    Color rule, code_block, code_border, code_tint, bar, number, link_line, table_border, header_tint, head, first, rest;
};
auto screen_ink() -> Ink {
    return {ui::th().line, ui::th().sunk, ui::th().line, ui::th().w2, ui::th().accent, ui::th().text_3, ui::th().accent,
            ui::th().line, ui::th().w1, ui::th().text_3, ui::th().text, ui::th().text_2};
}
// color-ok: a document's paper is a picture of a page — black lines on it in both themes (the scan
// cannot see these: their type is Ink).
constexpr Ink kPaperInk{{0, 0, 0, 36}, {0, 0, 0, 10}, {0, 0, 0, 30}, {0, 0, 0, 14}, kPaperLink, kPaperSoft, kPaperLink,
                        {0, 0, 0, 46}, {0, 0, 0, 12}, kPaperSoft, kPaperStrong, kPaperText};

auto style_of(const answer::Block& b, bool paper) -> widgets::StreamStyle {
    switch (b.kind) {
    case Kind::Heading: return paper ? kPaperHeading : heading_style();
    case Kind::Quote: return paper ? kPaperQuote : quote_style();
    case Kind::Code: return paper ? kPaperCode : code_style();
    default: return paper ? kPaperProse : prose_style();
    }
}

// Calls fn(line, x0, x1) for each line's part of the text's bytes [begin, end): logical pixels
// from the line's start.
template <class F>
void spans(const ui::text::Paragraph& p, float scale, std::uint32_t begin, std::uint32_t end, F fn) {
    const auto& glyphs = p.shaped().glyphs;
    for (std::size_t i = 0; i < p.lines().size(); ++i) {
        const auto& l = p.lines()[i];
        float x = 0;
        float x0 = -1;
        float x1 = 0;
        for (auto k = l.first_glyph; k < l.end_glyph; ++k) {
            const float advance = glyphs[k].x_advance / scale;
            if (glyphs[k].cluster >= begin && glyphs[k].cluster < end) {
                x0 = x0 < 0 ? x : x0;
                x1 = x + advance;
            }
            x += advance;
        }
        if (x0 >= 0) {
            fn(i, x0, x1);
        }
    }
}

auto web(std::string_view href) -> bool {
    return href.starts_with("https://") || href.starts_with("http://");
}

} // namespace

void Painter::draw_markdown(const Box& box, const Box& visible, bool caret, Reaction& r) {
    if (doc_) {
        draw_markdown(*doc_, times_, box, visible, caret, r);
    }
}

void Painter::draw_markdown(const answer::Doc& doc, std::span<const widgets::StreamTimes> times, const Box& box,
                            const Box& visible, bool caret, Reaction& r) {
    auto& renderer = ui_->renderer();
    const float scale = ui_->scale();
    const auto& blocks = doc.blocks();
    const Ink ink = paper_ ? kPaperInk : screen_ink();
    std::array<char, 64> buf{};
    std::size_t& link = links_drawn_; // numbered across the frame's answers
    float y = box.y;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& when = i < times.size() ? times[i] : shown_times_; // none: all shown
        const auto& b = blocks[i];
        if (i > 0) {
            y += answer::Doc::gap(blocks[i - 1], b);
        }
        const float h = doc.height(b);
        const float top = y;
        y += h;
        // Its keys by its place, whichever blocks above are drawn (final review C1).
        ui_->order_base((1ull << 40) | ((static_cast<std::uint64_t>(i) + 1) << 20));
        const bool last = i + 1 == blocks.size();
        if (top > visible.y + visible.h || top + h < visible.y) {
            link += b.links.size(); // the links keep their numbers off screen
            tables_drawn_ += b.kind == Kind::Table && b.panel ? 1 : 0;
            continue;
        }
        const float size = doc.size_of(b);
        const float line = size * answer::Doc::line_of(b);
        const float x = box.x + answer::Doc::indent(b);
        switch (b.kind) {
        case Kind::Rule:
            renderer.rect({box.x, top, box.w, 1}, {.fill = ink.rule});
            continue;
        case Kind::Table:
            if (b.panel) {
                draw_table_card(b, {box.x, top, std::min(box.w, kTableMax), h}, tables_drawn_++);
            } else {
                draw_table(b, {box.x, top, std::min(box.w, kTableMax), h}, visible, when);
            }
            continue;
        case Kind::Code: {
            const Box block{box.x, top, box.w, h};
            add_target("md:code", block);
            renderer.rect({block.x, block.y, block.w, block.h},
                          {.fill = ink.code_block, .radius = {8, 8, 8, 8}, .border = ink.code_border, .border_width = 1});
            widgets::stream_text(*ui_, {box.x + 1 + answer::Doc::kCodePadX, top + 1 + answer::Doc::kCodePadY,
                                        box.w - 2 - 2 * answer::Doc::kCodePadX, h - 2 - 2 * answer::Doc::kCodePadY},
                                 b.paragraph, when, caret && last, paper_ ? kPaperCode : code_style());
            continue;
        }
        case Kind::Heading: add_target("md:heading", {box.x, top, box.w, h}); break;
        case Kind::Quote:
            // A 2 px accent bar beside it.
            renderer.rect({box.x, top + 2, 2, h - 4}, {.fill = ink.bar, .radius = {1, 1, 1, 1}});
            break;
        case Kind::ListItem:
            if (b.ordered) {
                // The number, right-aligned before the text, in tabular figures.
                std::array<char, 16> digits{};
                const auto end = std::to_chars(digits.data(), digits.data() + digits.size() - 1, b.number).ptr;
                *end = '.';
                const std::string_view number{digits.data(), static_cast<std::size_t>(end + 1 - digits.data())};
                const auto& shaped = ui_->shape(Face::Text, number, theme::type::prose, true);
                const float base = ui_->centred_baseline(Face::Text, theme::type::prose, top, line);
                ui_->draw_text(Face::Text, shaped, theme::type::prose, {x - 6 - shaped.width / scale, base}, ink.number);
            } else {
                // li::before: a 6 px dot, top .62em
                renderer.rect({x - answer::Doc::kItemIndent + 2, top + size * 0.62f, 6, 6},
                              paper_ ? ui::render::RectStyle{.fill = kPaperSoft, .radius = {3, 3, 3, 3}}
                                     : ui::render::RectStyle{.fill = ui::th().text_3, .radius = {3, 3, 3, 3}});
            }
            break;
        default: break;
        }
        // Code in a sentence: a tint behind it.
        for (const auto& in : b.source.inlines) {
            if (in.code) {
                spans(b.paragraph, scale, in.begin, in.end, [&](std::size_t l, float x0, float x1) {
                    const float th = size * 1.35f;
                    renderer.rect({x + x0 - 3, top + static_cast<float>(l) * line + (line - th) / 2, x1 - x0 + 6, th},
                                  {.fill = ink.code_tint, .radius = {4, 4, 4, 4}});
                });
            }
        }
        widgets::stream_text(*ui_, {x, top, box.x + box.w - x, h}, b.paragraph, when, caret && last, style_of(b, paper_));
        // Links: each line's part takes the mouse; a web address opens in the browser.
        for (const auto& l : b.links) {
            const auto name = text(buf, "md:link:{}", link++);
            spans(b.paragraph, scale, l.begin, l.end, [&](std::size_t n, float x0, float x1) {
                const Box hit{x + x0, top + static_cast<float>(n) * line, x1 - x0, line};
                add_target(name, hit);
                const auto in = ui_->interact(id_of(name, n), hit, false);
                if (in.hovered) {
                    const float base = top + static_cast<float>(n) * line + (line + size) / 2;
                    renderer.rect({hit.x, base, hit.w, 1}, {.fill = ink.link_line});
                }
                if (in.clicked && web(l.href)) {
                    r.open_url = l.href;
                } else if (in.clicked && l.href.starts_with("page:")) { // "p. 7": that page in the panel (phase 10)
                    std::uint32_t page = 0;
                    const auto digits = std::string_view{l.href}.substr(5);
                    std::from_chars(digits.data(), digits.data() + digits.size(), page);
                    r.page = page;
                }
            });
        }
    }
}

// Spec § 5.5: more than 5 rows or 3 columns go to the data panel; the text keeps this card.
void Painter::draw_table_card(const answer::Block& b, const Box& card, std::size_t n) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    add_target("md:table:panel", card);
    const auto in = ui_->interact(id_of("md:table:panel", n), card, false);
    if (in.clicked) {
        table_pick_ = n;
        table_picked_ = &b;
    }
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = faint(ui::th().w1, lit(id_of("md:table:panel", n), in.hovered)), .radius = {10, 10, 10, 10},
                                                     .border = ui::th().line, .border_width = 1});
    std::size_t cols = 0;
    for (const auto& row : b.cells) {
        cols = std::max(cols, row.size());
    }
    const std::size_t rows = b.cells.empty() ? 0 : b.cells.size() - 1;
    work::caption(*ui_, {card.x + 16, card.y + 9, card.w - 52, 20}, Face::Semibold, 13.5f,
                text(buf, "Table \xC2\xB7 {} rows \xC3\x97 {} columns", rows, cols), ui::th().text);
    work::caption(*ui_, {card.x + 16, card.y + 29, card.w - 52, 18}, Face::Text, 12.5f, "In the data panel", ui::th().text_3);
    ui_->draw_icon(work::kIconChevronRight, 14.0f, {card.x + card.w - 22, card.y + card.h / 2}, ui::th().text_3);
}

void Painter::open_answer_table(Model& m, const std::vector<std::uint32_t>& data, std::size_t first) {
    if (!table_pick_ || *table_pick_ < first || *table_pick_ >= tables_drawn_) {
        return;
    }
    std::size_t k = *table_pick_ - first;
    table_pick_.reset();
    const auto* picked = std::exchange(table_picked_, nullptr);
    for (const auto id : data) {
        const auto set = std::ranges::find(m.sets, id, &events::DataSet::id);
        if (set != m.sets.end() && set->source == events::kAnswerSource && k-- == 0) {
            show_set(m, id);
            redraw_due_ = true; // the panel shows it next frame
            return;
        }
    }
    if (picked == nullptr) {
        return;
    }
    // No set for it yet: the card's own rows, one such set at a time (no task holds it; the
    // agent's set, once come, is the card's again).
    std::erase_if(m.sets, [](const events::DataSet& s) { return s.id == kCardTable; });
    auto made = present::answer_table(picked->source);
    made.id = kCardTable;
    m.sets.push_back(std::move(made));
    show_set(m, kCardTable);
    redraw_due_ = true;
}

void Painter::draw_table(const answer::Block& b, const Box& table, const Box& visible, const widgets::StreamTimes& times) {
    if (b.cells.empty()) {
        return;
    }
    auto& renderer = ui_->renderer();
    const Ink ink = paper_ ? kPaperInk : screen_ink();
    add_target("md:table", table);
    renderer.rect({table.x, table.y, table.w, table.h},
                  {.radius = {paper_ ? 2.0f : 10.0f, paper_ ? 2.0f : 10.0f, paper_ ? 2.0f : 10.0f, paper_ ? 2.0f : 10.0f},
                   .border = ink.table_border, .border_width = 1});
    renderer.rect({table.x + 1, table.y + 1, table.w - 2, answer::Doc::kHeaderRow},
                  {.fill = ink.header_tint, .radius = {paper_ ? 1.0f : 9.0f, paper_ ? 1.0f : 9.0f, 0, 0}});

    // Columns as wide as their text needs; what is left goes to the first, a shortfall shrinks
    // them all.
    const std::size_t cols = b.align.size();
    std::array<float, 16> widths{};
    const std::size_t n = std::min(cols, widths.size());
    float natural = 0;
    for (std::size_t c = 0; c < n; ++c) {
        for (std::size_t row = 0; row < b.cells.size() && row <= kMeasuredRows; ++row) {
            if (c < b.cells[row].size()) {
                // Measured as it is drawn: a right-aligned column in tabular figures, the wider ones.
                widths[c] = std::max(widths[c], ui_->shape(Face::Text, b.cells[row][c], row == 0 ? 12.0f : 13.5f, b.align[c] == 'r').width / ui_->scale());
            }
        }
        natural += widths[c];
    }
    const float inner = table.w - 2 - 2 * kCellPad - kCellPad * static_cast<float>(n > 0 ? n - 1 : 0);
    if (natural <= inner) {
        widths[0] += inner - natural;
    } else if (natural > 0) {
        for (std::size_t c = 0; c < n; ++c) {
            widths[c] *= inner / natural;
        }
    }

    const double now = ui_->now();
    bool fading = false;
    renderer.push_clip({table.x, table.y, table.w, table.h});
    for (std::size_t row = 0; row < b.cells.size(); ++row) {
        const float top = row == 0 ? table.y + 1
                                   : table.y + 1 + answer::Doc::kHeaderRow + answer::Doc::kRow * static_cast<float>(row - 1);
        const float rh = row == 0 ? answer::Doc::kHeaderRow : answer::Doc::kRow;
        if (top > visible.y + visible.h || top + rh < visible.y) {
            continue;
        }
        const double age = now - times.time_of(static_cast<std::uint32_t>(row));
        const float alpha = ui::motion::response(ui::motion::kQuick, age); // a table's row as it is written: quick
        fading = fading || alpha < 1.0f;
        if (row > 0) {
            renderer.rect({table.x + 1, top, table.w - 2, 1}, {.fill = with_alpha(paper_ ? ink.table_border : ui::th().line, alpha)});
        }
        float x = table.x + 1 + kCellPad;
        for (std::size_t c = 0; c < n; ++c) {
            if (c < b.cells[row].size() && !b.cells[row][c].empty()) {
                const float sz = row == 0 ? 12.0f : 13.5f;
                const bool right = b.align[c] == 'r';
                const auto& shaped = ui_->shape(Face::Text, b.cells[row][c], sz, right);
                const float w = shaped.width / ui_->scale();
                const float tx = right ? x + widths[c] - w : b.align[c] == 'c' ? x + (widths[c] - w) / 2 : x;
                const Color color = row == 0 ? ink.head : c == 0 ? ink.first : ink.rest;
                const bool cut = w > widths[c] + 0.5f;
                if (cut) {
                    renderer.push_clip({x, top, widths[c], rh});
                }
                ui_->draw_text(Face::Text, shaped, sz, {cut ? x : tx, ui_->centred_baseline(Face::Text, sz, top, rh)},
                               with_alpha(color, alpha));
                if (cut) {
                    renderer.pop_clip();
                }
            }
            x += widths[c] + kCellPad;
        }
    }
    renderer.pop_clip();
    if (fading) {
        ui_->request_frame(); // a row is still fading in
    }
}

} // namespace app::sheet
