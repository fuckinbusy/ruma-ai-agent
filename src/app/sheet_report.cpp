// The system report of design/interaction-topbar (4_system_report): the answer as a card, not
// text. On the left the question, the sentence that names the problem and four tiles with their
// last minute (the one in trouble says so in a word and an icon, not only in colour); on the
// right what uses memory, with Close for a program one call can end, and what can be done.
// Close and Do it are the agent's calls (process.kill, asked as any).

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace app::sheet {
namespace {

namespace theme = ui::theme;
namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
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

auto tone_color(events::Tone t) -> Color {
    switch (t) {
    case events::Tone::Warn: return ui::th().warn;
    case events::Tone::Crit: return ui::th().crit;
    case events::Tone::Ok: return ui::th().ok;
    case events::Tone::Accent: return ui::th().accent;
    case events::Tone::Plain: break;
    }
    return ui::th().text_3;
}

// "2.4 GB", "800 MB".
auto size_text(std::array<char, 64>& buf, std::uint64_t bytes) -> std::string_view {
    const double g = static_cast<double>(bytes) / (1024.0 * 1024 * 1024);
    return g >= 0.95 ? text(buf, "{:.1f} GB", g) : text(buf, "{:.0f} MB", g * 1024.0);
}

auto upper(char c) -> char {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr float kTileGap = 10.0f;
constexpr float kTile = 117.0f;     // .tile: 12 + its head + the 26 px value + 6 + the 40 px line + 10
constexpr float kHead = 30.0f;      // .pr.head
constexpr float kProc = 40.0f;      // .pr
constexpr float kRec = 50.0f;       // .rec
constexpr float kBar = 72.0f;       // the biggest program's memory bar

} // namespace

void Painter::draw_report(Model& m, const Box& panel, Reaction& r) {
    if (!m.report) {
        return;
    }
    const auto& rep = *m.report;
    std::array<RowChip, 2> chips{{{"Report ready", work::Tone::Ok, "chip"}, {"Unsafe mode", work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, "Watch memory and warn if it is over 90%\xE2\x80\xA6", wt::type::placeholder_small,
             std::span{chips.data(), m.unsafe ? 2u : 1u}, RowButton::Send, false);
    if (!m.report) {
        return; // a new question took its place
    }
    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_ + wt::body::pad_top, panel.w - 2 * wt::body::pad_x,
                   std::max(0.0f, panel.h - row_h_ - wt::body::pad_top - 12)};
    if (body.w <= 0 || body.h <= 0) {
        return;
    }
    auto& renderer = ui_->renderer();
    const float col = (body.w - wt::body::column_gap) / 2;
    const Box left{body.x, body.y, col, body.h};
    const Box right{body.x + col + wt::body::column_gap, body.y, col, body.h};
    std::array<char, 64> buf{};

    // Left: the question, the sentence, the tiles.
    renderer.push_clip({left.x - 4, left.y, left.w + 8, left.h});
    float y = left.y + draw_question(m, left.x + left.w, left.y, left.w) + theme::bubble::margin_bottom;
    if (doc_) {
        doc_->set_width(left.w);
        const float words = doc_->height();
        draw_markdown({left.x, y, left.w, words}, left, false, r);
        y += words + 8;
        y += 6;
    }
    const float tw = (left.w - kTileGap) / 2;
    for (std::size_t i = 0; i < rep.tiles.size() && i < 4; ++i) {
        const auto& t = rep.tiles[i];
        const Box tile{left.x + static_cast<float>(i % 2) * (tw + kTileGap), y + static_cast<float>(i / 2) * (kTile + kTileGap), tw,
                       kTile};
        renderer.rect({tile.x, tile.y, tile.w, tile.h},
                      {.fill = ui::th().card, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
        add_target(text(buf, "tile:{}", i), tile);
        // .h: the name, and its state in a word (with a sign when it is the trouble).
        const float hy = tile.y + 12;
        const float hh = 18;
        work::caption(*ui_, {tile.x + 14, hy, tile.w - 28, hh}, Face::Text, 12.5f, t.name, ui::th().text_2);
        const Color tc = tone_color(t.tone);
        const bool warn = t.tone == events::Tone::Warn || t.tone == events::Tone::Crit;
        const float sw = work::text_width(*ui_, Face::Text, 11.5f, t.status) + 14 + (warn ? 16 : 0);
        const Box stat{tile.x + tile.w - 14 - sw, hy + 1, sw, hh - 2};
        renderer.rect({stat.x, stat.y, stat.w, stat.h}, {.fill = with_alpha(tc, 0.12f), .radius = {8, 8, 8, 8}});
        if (warn) {
            ui_->draw_icon(work::kIconWarning, 10.0f, {stat.x + 7 + 5, stat.y + stat.h / 2}, tc);
        }
        work::caption(*ui_, {stat.x + 7 + (warn ? 16 : 0), stat.y, stat.w - 14, stat.h}, Face::Text, 11.5f, t.status, tc);
        // .v: the value, then its unit small.
        const float vh = 26.0f * 1.15f;
        const float vy = hy + hh + 4;
        const float vw = work::caption(*ui_, {tile.x + 14, vy, tile.w - 28, vh}, Face::Display, 26.0f, t.value, ui::th().text);
        work::caption(*ui_, {tile.x + 14 + vw + 4, vy + 4, std::max(0.0f, tile.w - 28 - vw - 4), vh}, Face::Text, 12.5f, t.unit,
                    ui::th().text_3);
        // The last minute.
        const Box spark{tile.x + 14, tile.y + tile.h - 10 - 40, tile.w - 28, 40};
        if (!t.series.empty()) {
            work::sparkline(*ui_, spark, t.series, warn ? ui::th().warn : ui::th().accent, t.line);
        }
    }
    renderer.pop_clip();

    // Right: what uses memory.
    renderer.push_clip({right.x - 4, right.y, right.w + 8, right.h});
    y = right.y;
    const float h4 = 18;
    const float hw = work::caption(*ui_, {right.x + 2, y, right.w, h4}, Face::Semibold, 13.0f, "What uses memory", ui::th().text);
    work::caption(*ui_, {right.x + 2 + hw + 6, y, right.w, h4}, Face::Text, 12.0f,
                text(buf, "\xC2\xB7 top {} of {}", rep.processes.size(), rep.process_count), ui::th().text_3);
    y += h4 + 8;
    const float table_h = kHead + static_cast<float>(rep.processes.size()) * kProc;
    renderer.rect({right.x, y, right.w, table_h}, {.fill = {}, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    // .pr: 24px 1fr 40px 118px 78px, gap 10, padding 0 10.
    const float c0 = right.x + 10;
    const float c4w = 78;
    const float c3w = 118;
    const float c2w = 40;
    const float c4 = right.x + right.w - 10 - c4w;
    const float c3 = c4 - 10 - c3w;
    const float c2 = c3 - 10 - c2w;
    const float c1 = c0 + 24 + 10;
    const float c1w = std::max(0.0f, c2 - 10 - c1);
    renderer.rect({right.x + 1, y + 1, right.w - 2, kHead - 1}, {.fill = ui::th().w1, .radius = {11, 11, 0, 0}});
    work::caption(*ui_, {c1, y, c1w, kHead}, Face::Text, 11.5f, "App", ui::th().text_3);
    const float cpu_w = work::text_width(*ui_, Face::Text, 11.5f, "CPU");
    work::caption(*ui_, {c2 + c2w - cpu_w, y, cpu_w, kHead}, Face::Text, 11.5f, "CPU", ui::th().text_3);
    work::caption(*ui_, {c3, y, c3w, kHead}, Face::Text, 11.5f, "Memory", ui::th().text_3);
    std::uint64_t biggest = 1;
    for (const auto& p : rep.processes) {
        biggest = std::max(biggest, p.memory);
    }
    for (std::size_t i = 0; i < rep.processes.size(); ++i) {
        const auto& p = rep.processes[i];
        const Box row{right.x, y + kHead + static_cast<float>(i) * kProc, right.w, kProc};
        renderer.rect({row.x, row.y, row.w, 1}, {.fill = ui::th().line});
        if (i == 0) {
            renderer.rect({row.x + 1, row.y + 1, row.w - 2, row.h - 1}, {.fill = ui::th().warn_soft}); // .pr.hot: a hot row is the warn's
        }
        add_target(text(buf, "proc:{}", i), row);
        const float cy = row.y + row.h / 2;
        // The badge.
        renderer.rect({c0 + 1, cy - 11, 22, 22}, {.fill = ui::th().w3, .radius = {6, 6, 6, 6}}); // an app's letter is neutral
        const std::array<char, 1> letter{upper(p.name.empty() ? '?' : p.name[0])};
        const float lw = work::text_width(*ui_, Face::Semibold, 11.0f, {letter.data(), 1});
        work::caption(*ui_, {c0 + 12 - lw / 2, row.y, lw + 1, row.h}, Face::Semibold, 11.0f, {letter.data(), 1}, ui::th().text_2);
        // The name and its note.
        const float nw = work::caption(*ui_, {c1, row.y, c1w, row.h}, Face::Text, 13.0f, p.name, ui::th().text);
        // A note is there whole or not at all: "12 p…" says nothing.
        if (const float ow = work::text_width(*ui_, Face::Text, 12.0f, p.note); !p.note.empty() && nw + 6 + ow <= c1w) {
            const Box note{c1 + nw + 6, row.y, ow, row.h};
            work::caption(*ui_, note, Face::Text, 12.0f, p.note, ui::th().text_3);
            add_target(text(buf, "proc:note:{}", i), note);
        }
        // CPU, right-aligned.
        const auto cpu = text(buf, "{:.0f}%", p.cpu);
        const float cw = work::text_width(*ui_, Face::Text, 13.0f, cpu);
        work::caption(*ui_, {c2 + c2w - cw, row.y, cw, row.h}, Face::Text, 13.0f, cpu, ui::th().text_2);
        // Memory: a bar for its share of the biggest, then the amount — whole: the bar gives way.
        const auto amount = size_text(buf, p.memory);
        const float aw = work::text_width(*ui_, Face::Text, 12.0f, amount);
        const float bar = std::clamp(c3w - 8 - aw, 0.0f, kBar);
        const float bw = std::max(2.0f, bar * static_cast<float>(static_cast<double>(p.memory) / static_cast<double>(biggest)));
        renderer.rect({c3, cy - 3, bw, 6}, {.fill = ui::th().w4, .radius = {0, 3, 3, 0}});
        const Box mem{c3 + bw + 8, row.y, aw, row.h};
        work::caption(*ui_, mem, Face::Text, 12.0f, amount, ui::th().text_2);
        add_target(text(buf, "proc:mem:{}", i), mem);
        if (p.closable) {
            const auto bs = work::button_size(*ui_, "Close", true);
            const Box b{c4 + c4w - bs.w, cy - bs.h / 2, bs.w, bs.h};
            add_target(text(buf, "proc:close:{}", p.pid), b);
            if (work::button(*ui_, id_of("proc:close", p.pid), b, "Close", work::ButtonStyle::Normal, true) && !r.command) {
                r.command = press(m, text(buf, "close:{}", p.pid));
            }
        }
    }
    y += table_h + 18;

    // What can be done (.rec).
    if (!rep.advice.empty() && m.report) {
        work::caption(*ui_, {right.x + 2, y, right.w, h4}, Face::Semibold, 13.0f, "What you can do", ui::th().text);
        y += h4 + 8;
        for (std::size_t i = 0; i < rep.advice.size(); ++i) {
            const auto& a = rep.advice[i];
            const Box rec{right.x, y, right.w, kRec};
            renderer.rect({rec.x, rec.y, rec.w, rec.h},
                          {.fill = ui::th().card, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
            work::kind_icon(*ui_, {rec.x + 12, rec.y + (rec.h - 30) / 2, 30, 30}, work::Kind::System,
                            a.action.starts_with("close:") ? work::kIconClose : work::kIconSystem);
            const auto bs = work::button_size(*ui_, "Do it", true);
            const Box b{rec.x + rec.w - 12 - bs.w, rec.y + (rec.h - bs.h) / 2, bs.w, bs.h};
            const float gw = work::text_width(*ui_, Face::Text, 12.0f, a.gain);
            work::caption(*ui_, {b.x - 12 - gw, rec.y, gw, rec.h}, Face::Text, 12.0f, a.gain, ui::th().ok);
            const float tx = rec.x + 12 + 30 + 12;
            const float txw = std::max(0.0f, b.x - 12 - gw - 12 - tx);
            const float th = ui_->line_height(Face::Text, 13.0f);
            const float sh = ui_->line_height(Face::Text, 12.0f);
            const float top = rec.y + (rec.h - th - sh) / 2;
            work::caption(*ui_, {tx, top, txw, th}, Face::Text, 13.0f, a.title, ui::th().text);
            work::caption(*ui_, {tx, top + th, txw, sh}, Face::Text, 12.0f, a.sub, ui::th().text_3);
            add_target(text(buf, "advice:{}", i), b);
            if (work::button(*ui_, id_of("advice", i), b, "Do it", i == 0 ? work::ButtonStyle::Primary : work::ButtonStyle::Normal,
                             true) &&
                !r.command) {
                r.command = press(m, a.action);
            }
            y += kRec + 6;
            if (!m.report) {
                break; // pressed: the report waits for the agent
            }
        }
    }
    renderer.pop_clip();
}

} // namespace app::sheet
