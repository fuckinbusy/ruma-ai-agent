// A PDF in the panel (phase 10, design/ui_remastered screen 22): its pages as a map — read as
// text, a scan read on this PC (hatched), not read yet (a ring); the parts of 12 pages read so far;
// the chosen page's text with Copy; "Read pages…" asks the agent for the next pages not read. A PDF
// whose filter did not give its pages has no map, and says why.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using Color = ui::render::Color;

constexpr float kPadX = 22;
constexpr int kColumns = 24;
constexpr float kCellGap = 5;
constexpr char32_t kIconCopy = 0xE8C8;

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto page_of(const events::Pages& pages, int page) -> const events::PageText* {
    const auto at = std::ranges::lower_bound(pages.read, page, {}, &events::PageText::page);
    return at != pages.read.end() && at->page == page ? &*at : nullptr;
}

} // namespace

void Painter::draw_document(Model& m, const events::DataSet& set, const Box& body, Reaction& r) {
    auto& renderer = ui_->renderer();
    const auto& pages = *set.pages;
    std::array<char, 96> buf{};
    std::array<char, 32> name{};
    const float x = body.x + kPadX;
    const float w = std::max(0.0f, body.w - 2 * kPadX);
    float y = body.y + 18;

    // Its name, where it came from; Read pages… on the right.
    const float th = ui_->line_height(Face::Display, 18.0f);
    const auto rs = work::button_size(*ui_, "Read pages\xE2\x80\xA6", true);
    const Box read_more{x + w - rs.w, y, rs.w, rs.h};
    work::caption(*ui_, {x, y, std::max(0.0f, w - rs.w - 12), th}, Face::Display, 18.0f, set.title, ui::th().text);
    add_target("doc:read-pages", read_more);
    if (work::button(*ui_, id_of("doc:read-pages"), read_more, "Read pages\xE2\x80\xA6", work::ButtonStyle::Normal, true)) {
        // The next pages not read, asked in the field: the user sends it, or changes it first.
        int from = 0;
        int to = 0;
        for (int p = 1; p <= pages.count; ++p) {
            if (page_of(pages, p) == nullptr) {
                from = from == 0 ? p : from;
                to = p;
            } else if (from != 0) {
                break;
            }
        }
        if (from != 0) {
            m.input = {};
            type(m, from == to ? std::format("Read page {} of {}", from, set.title) : std::format("Read pages {}\xE2\x80\x93{} of {}", from, to, set.title));
        }
    }
    y += th + 6;
    work::caption(*ui_, {x, y, w, 18}, Face::Mono, 12.0f, set.source, ui::th().text_2);
    y += 18 + 14;

    if (!pages.mapped) { // no map: why, in a line
        const float nh = ui_->line_height(Face::Text, 13.0f);
        work::caption(*ui_, {x, y, w, nh}, Face::Text, 13.0f, pages.note, ui::th().text_3);
        add_target("doc:note", {x, y, w, nh});
        return;
    }

    // The map: 24 pages a row, its parts above it, a legend under it.
    const int rows = std::max(1, (pages.count + kColumns - 1) / kColumns);
    const float cw = (w - 32 - (kColumns - 1) * kCellGap) / kColumns;
    const float ch = rows <= 3 ? 30.0f : rows <= 6 ? 20.0f : 12.0f;
    const float lab = ui_->line_height(Face::Semibold, 11.0f);
    const float card_h = 14 + lab + 10 + rows * (ch + 5 + 14) + 12 + 16 + 14;
    const Box card{x, y, w, card_h};
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    add_target("doc:map", card);
    work::caption(*ui_, {card.x + 16, card.y + 14, 80, lab}, Face::Semibold, 11.0f, "PAGES", ui::th().text_3);
    // Parts of 12 pages: a part is read when all its pages are.
    const int parts = std::max(1, (pages.count + pages.part_size - 1) / pages.part_size);
    int done = 0;
    for (int part = 0; part < parts; ++part) {
        bool all = true;
        for (int p = part * pages.part_size + 1; p <= std::min(pages.count, (part + 1) * pages.part_size) && all; ++p) {
            all = page_of(pages, p) != nullptr;
        }
        done += all ? 1 : 0;
    }
    {
        const auto count = text(buf, "{} of {}", done, parts);
        const float shown = static_cast<float>(std::min(parts, 12));
        const float cnt_w = work::text_width(*ui_, Face::Text, 12.0f, count);
        const float bars_w = shown * 30 + (shown - 1) * 6;
        constexpr std::string_view kLabel = "Parts of 12 pages";
        const float lw = work::text_width(*ui_, Face::Text, 12.0f, kLabel);
        const float right = card.x + card.w - 16;
        const float bx = right - cnt_w - 8 - bars_w;
        work::caption(*ui_, {bx - 8 - lw, card.y + 14, lw, lab}, Face::Text, 12.0f, kLabel, ui::th().text_3);
        for (int i = 0; i < static_cast<int>(shown); ++i) {
            renderer.rect({bx + static_cast<float>(i) * 36, card.y + 14 + lab / 2 - 3, 30, 6},
                          {.fill = i < done ? ui::th().ok : ui::th().w3, .radius = {3, 3, 3, 3}});
        }
        work::caption(*ui_, {right - cnt_w, card.y + 14, cnt_w, lab}, Face::Text, 12.0f, count, ui::th().text_3);
        add_target("doc:parts", {bx - 8 - lw, card.y + 14, right - (bx - 8 - lw), lab});
    }
    const std::uint32_t chosen = m.doc_page != 0 ? m.doc_page : (pages.read.empty() ? 1u : static_cast<std::uint32_t>(pages.read.front().page));
    float gy = card.y + 14 + lab + 10;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < kColumns; ++col) {
            const int p = row * kColumns + col + 1;
            if (p > pages.count) {
                break;
            }
            const Box cell{card.x + 16 + static_cast<float>(col) * (cw + kCellGap), gy, cw, ch};
            const auto* read = page_of(pages, p);
            if (read == nullptr) {
                renderer.rect({cell.x, cell.y, cell.w, cell.h}, {.fill = {}, .radius = {3, 3, 3, 3}, .border = ui::th().w4, .border_width = 1});
            } else if (read->scan) { // hatched: lines across it
                renderer.rect({cell.x, cell.y, cell.w, cell.h}, {.fill = ui::th().warn_soft, .radius = {3, 3, 3, 3}});
                renderer.push_clip({cell.x, cell.y, cell.w, cell.h});
                for (float s = -cell.h; s < cell.w; s += 6) {
                    for (float k = 0; k < cell.h; k += 2) { // a 135° stripe, two pixels a step
                        renderer.rect({cell.x + s + k, cell.y + cell.h - 2 - k, 3, 2}, {.fill = ui::th().warn_line});
                    }
                }
                renderer.pop_clip();
            } else {
                renderer.rect({cell.x, cell.y, cell.w, cell.h}, {.fill = ui::th().accent_line, .radius = {3, 3, 3, 3}});
            }
            if (static_cast<std::uint32_t>(p) == chosen) {
                renderer.rect({cell.x - 2, cell.y - 2, cell.w + 4, cell.h + 4}, {.fill = {}, .radius = {4, 4, 4, 4}, .border = ui::th().text, .border_width = 2});
            }
            add_target(text(name, "doc:page:{}", p), cell);
            const auto hit = ui_->interact(id_of("doc:page", static_cast<std::uint64_t>(p)), cell, false);
            if (const float h = ui_->hover(id_of("doc:page", static_cast<std::uint64_t>(p)), hit.hovered); h > 0.01f) { // rgba(174,185,255,.2) (screen 26)
                renderer.rect({cell.x, cell.y, cell.w, cell.h}, {.fill = faint(ui::th().accent_soft_hi, h), .radius = {3, 3, 3, 3}});
            }
            if (hit.clicked) {
                m.doc_page = static_cast<std::uint32_t>(p);
            }
            // Its number under the first of each six, and the last page.
            if (col % 6 == 0 || p == pages.count) {
                const auto n = text(buf, "{}", p);
                work::caption(*ui_, {cell.x, cell.y + ch + 2, cw + 20, 12}, Face::Text, 10.5f, n, ui::th().text_4);
            }
        }
        gy += ch + 5 + 14;
    }
    // The legend.
    float lx = card.x + 16;
    const float ly = gy + 6;
    const auto legend = [&](std::string_view words, int kind) {
        const Box sw{lx, ly + 1, 12, 14};
        if (kind == 0) {
            renderer.rect({sw.x, sw.y, sw.w, sw.h}, {.fill = ui::th().accent_line, .radius = {2, 2, 2, 2}});
        } else if (kind == 1) {
            renderer.rect({sw.x, sw.y, sw.w, sw.h}, {.fill = ui::th().warn_line, .radius = {2, 2, 2, 2}});
        } else {
            renderer.rect({sw.x, sw.y, sw.w, sw.h}, {.fill = {}, .radius = {2, 2, 2, 2}, .border = ui::th().text_4, .border_width = 1});
        }
        const float tw = work::caption(*ui_, {lx + 18, ly, 260, 16}, Face::Text, 12.0f, words, ui::th().text_3);
        lx += 18 + tw + 16;
    };
    legend("text \xE2\x80\x94 read", 0);
    legend("scan \xE2\x80\x94 recognized on this PC", 1);
    legend("not read", 2);
    y += card_h + 14;

    // The chosen page's text, with Copy.
    const auto* page = page_of(pages, static_cast<int>(chosen));
    const float head = 40;
    const float room = std::max(head + 40, body.y + body.h - y - 14);
    const Box box{x, y, w, room};
    renderer.rect({box.x, box.y, box.w, box.h}, {.fill = {}, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    renderer.rect({box.x + 1, box.y + 1, box.w - 2, head - 1}, {.fill = ui::th().w1, .radius = {11, 11, 0, 0}});
    add_target("doc:text", box);
    const float pw = work::caption(*ui_, {box.x + 14, box.y, 120, head}, Face::Semibold, 12.5f, text(buf, "Page {}", chosen), ui::th().text);
    work::caption(*ui_, {box.x + 14 + pw + 8, box.y, 160, head}, Face::Text, 12.5f,
                page == nullptr ? std::string_view{"not read yet"} : page->scan ? std::string_view{"scan \xC2\xB7 read on this PC"} : std::string_view{"text"},
                ui::th().text_3);
    const Box copy{box.x + box.w - 14 - 28, box.y + (head - 28) / 2, 28, 28};
    add_target("doc:copy", copy);
    if (work::icon_button(*ui_, id_of("doc:copy"), copy, kIconCopy) && page != nullptr) {
        r.clipboard = page->text;
    }
    if (page != nullptr) { // its lines, wrapped, as many as fit
        const float lh = ui_->line_height(Face::Text, 14.0f) * 1.15f;
        float ty = box.y + head + 12;
        std::string_view rest = page->text;
        renderer.push_clip({box.x, box.y + head, box.w, box.h - head});
        while (!rest.empty() && ty + lh <= box.y + box.h - 8) {
            const auto nl = rest.find('\n');
            std::string_view line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            do {
                std::string_view part = line;
                const float tw = box.w - 32;
                if (work::text_width(*ui_, Face::Text, 14.0f, part) > tw) {
                    std::size_t cut = std::string_view::npos;
                    for (auto i = line.find(' '); i != std::string_view::npos; i = line.find(' ', i + 1)) {
                        if (work::text_width(*ui_, Face::Text, 14.0f, line.substr(0, i)) > tw) {
                            break;
                        }
                        cut = i;
                    }
                    part = cut == std::string_view::npos ? line : line.substr(0, cut);
                }
                work::caption(*ui_, {box.x + 16, ty, box.w - 32, lh}, Face::Text, 14.0f, part, ui::th().text);
                ty += lh;
                line = part.size() < line.size() ? line.substr(std::min(line.size(), part.size() + 1)) : std::string_view{};
            } while (!line.empty() && ty + lh <= box.y + box.h - 8);
        }
        renderer.pop_clip();
    }
}

} // namespace app::sheet
