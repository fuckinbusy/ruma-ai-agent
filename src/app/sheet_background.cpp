// The background (design/interaction-topbar 6_background; phase 10, design/ui_remastered 11–15):
// long tasks go on behind the bar. With the sheet closed the handle at the top edge says so
// (sheet_handle.cpp: one line under the mouse, nothing more); with the sheet open
// a strip under the content shows the cards ("Needs a decision" — Skip, Review; a result with its
// own buttons). Nothing here takes the focus.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace app::sheet {
namespace {

namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::uint64_t n) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto kind_of(events::Kind k) -> work::Kind {
    switch (k) {
    case events::Kind::Read: return work::Kind::Read;
    case events::Kind::Change: return work::Kind::Change;
    case events::Kind::Delete: return work::Kind::Delete;
    case events::Kind::System: return work::Kind::System;
    case events::Kind::Ok: return work::Kind::Ok;
    }
    return work::Kind::Change;
}

auto icon_of(events::Kind k) -> char32_t {
    switch (k) {
    case events::Kind::Read: return work::kIconSearch;
    case events::Kind::Delete: return work::kIconTrash;
    case events::Kind::System: return work::kIconSystem;
    case events::Kind::Change:
    case events::Kind::Ok: break;
    }
    return work::kIconMove;
}

constexpr float kDropPadX = 18.0f; // .drop padding 16 18
constexpr float kDropPadY = 16.0f;
constexpr float kDropLine = 19.0f; // 13 px at 1.45
constexpr float kIcon = 30.0f;
// The open sheet's strip shows its cards, "+N more" under them. The screen stays the user's.
constexpr std::size_t kCardsShown = 3;
constexpr float kMoreLine = 28.0f;

// The cards shown, in their order: questions first (nothing waits unseen), then the newest.
struct Shown {
    std::array<std::size_t, kCardsShown> at{};
    std::size_t count = 0;
    std::size_t hidden = 0;
};

auto shown_drops(const Model& m, std::size_t max = kCardsShown) -> Shown {
    Shown s;
    max = std::min(max, kCardsShown);
    for (std::size_t i = 0; i < m.drops.size() && s.count < max; ++i) {
        if (m.drops[i].kind == DropKind::Question) {
            s.at[s.count++] = i;
        }
    }
    for (std::size_t i = m.drops.size(); i-- > 0 && s.count < max;) {
        if (m.drops[i].kind != DropKind::Question) {
            s.at[s.count++] = i;
        }
    }
    std::sort(s.at.begin(), s.at.begin() + static_cast<std::ptrdiff_t>(s.count));
    s.hidden = m.drops.size() - s.count;
    return s;
}

} // namespace

auto Painter::wrap2(Face face, float size, std::string_view s, float width) -> std::pair<std::string_view, std::string_view> {
    if (work::text_width(*ui_, face, size, s) <= width) {
        return {s, {}};
    }
    std::size_t cut = std::string_view::npos;
    for (auto i = s.find(' '); i != std::string_view::npos; i = s.find(' ', i + 1)) {
        if (work::text_width(*ui_, face, size, s.substr(0, i)) > width) {
            break;
        }
        cut = i;
    }
    if (cut == std::string_view::npos) {
        return {s, {}};
    }
    return {s.substr(0, cut), s.substr(cut + 1)};
}

auto Painter::drop_height(const Drop& d, float width) -> float {
    const float tw = width - 2 * kDropPadX - kIcon - 12;
    const bool two = !wrap2(Face::Text, 13.0f, d.text, tw).second.empty();
    const float words = 20 + (d.text.empty() ? 0.0f : (two ? 2.0f : 1.0f) * kDropLine);
    return kDropPadY + std::max(kIcon, words) + 14 + work::button_size(*ui_, "Review").h + kDropPadY;
}

auto Painter::cards_height(const Model& m, float width, std::size_t max) -> float {
    const auto shown = shown_drops(m, max);
    float h = 0;
    for (std::size_t k = 0; k < shown.count; ++k) {
        h += kDropGap + drop_height(m.drops[shown.at[k]], width);
    }
    return h + (shown.hidden > 0 ? kMoreLine : 0.0f);
}

auto Painter::draw_cards(Model& m, float x, float y, float width, std::size_t max, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    // Questions first (nothing waits unseen), then the newest; the rest under "+N more".
    const auto shown = shown_drops(m, max);
    for (std::size_t k = 0; k < shown.count; ++k) {
        const auto& d = m.drops[shown.at[k]];
        y += kDropGap;
        const float h = drop_height(d, width);
        const Box card{x, y, width, h};
        y += h;
        renderer.rect({card.x, card.y, card.w, card.h},
                      {.fill = work::surface(*ui_), .radius = {16, 16, 16, 16}, .border = ui::th().line, .border_width = 1});
        add_target(text(buf, "drop:{}:card", d.task), card);
        if (!m.open) { // the open sheet's strip is inside the open window
            add_solid(card);
        }
        const auto [kind, icon] = d.kind == DropKind::Question ? std::pair{work::Kind::Change, work::kIconWarning}
                                  : d.kind == DropKind::Alert  ? std::pair{work::Kind::Read, work::kIconEye}
                                                               : std::pair{work::Kind::Ok, widgets::kIconCheck};
        work::kind_icon(*ui_, {card.x + kDropPadX, card.y + kDropPadY, kIcon, kIcon}, kind, icon);
        const float tx = card.x + kDropPadX + kIcon + 12;
        const float tw = card.w - kDropPadX - (tx - card.x);
        work::caption(*ui_, {tx, card.y + kDropPadY, tw, 20}, Face::Semibold, 14.0f, d.title, ui::th().text);
        const auto [l1, l2] = wrap2(Face::Text, 13.0f, d.text, tw);
        work::caption(*ui_, {tx, card.y + kDropPadY + 20, tw, kDropLine}, Face::Text, 13.0f, l1, ui::th().text_2);
        if (!l2.empty()) {
            work::caption(*ui_, {tx, card.y + kDropPadY + 20 + kDropLine, tw, kDropLine}, Face::Text, 13.0f, l2, ui::th().text_2);
        }
        // Its buttons, right-aligned: Skip and Review for a question; a result's own, then Close.
        const float by = card.y + card.h - kDropPadY - work::button_size(*ui_, "Review").h;
        float bx = card.x + card.w - kDropPadX;
        std::optional<Reaction> pressed;
        const auto button = [&](std::string_view id, std::string_view label, bool primary) {
            const auto s = work::button_size(*ui_, label);
            bx -= s.w;
            const Box b{bx, by, s.w, s.h};
            bx -= 8;
            add_target(text(buf, "drop:{}:{}", d.task, id), b);
            if (work::button(*ui_, id_of(id, d.task), b, label, primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal) &&
                !pressed && !r.command) {
                pressed = drop_press(m, d.task, id);
            }
        };
        if (d.kind == DropKind::Question) {
            button("review", "Review", true);
            button("skip", "Skip", false);
        } else {
            button("close", "Close", d.actions.empty());
            for (auto a = d.actions.rbegin(); a != d.actions.rend(); ++a) {
                button(a->id, a->label, a + 1 == d.actions.rend());
            }
        }
        if (pressed) {
            r.command = std::move(pressed->command);
            r.open = r.open || pressed->open;
            break; // the cards changed
        }
    }
    if (shown.hidden > 0) {
        // The rest wait their turn: they come up as these are closed.
        const auto more = text(buf, "+{} more", shown.hidden);
        const float mw = work::text_width(*ui_, Face::Text, 12.5f, more);
        const Box line{x + (width - mw) / 2, y + 6, mw, kMoreLine - 6};
        work::caption(*ui_, line, Face::Text, 12.5f, more, ui::th().text_3);
        add_target("drops:more", line);
        y += kMoreLine;
    }
    return y;
}

// With the sheet open, the background shows at once: a strip under the
// open view's content — its line ("2 background tasks · 45%", Log) and two cards at most. One
// window: the glass is DWM's and covers the whole window (docs/spikes/region-backdrop.md).
constexpr float kStripHead = 40.0f;
constexpr float kStripPad = 16.0f;
constexpr std::size_t kStripCards = 2;
// Its list (UI update, stage 3: it lived under the tongue): every background task still listed and
// every watcher, a row each — four in sight, the wheel for the rest.
constexpr float kStripRow = 40.0f; // the rows' own height (sheet_peek.cpp's .mrow)
constexpr std::size_t kStripRows = 4;

auto strip_rows(const Model& m) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(m.background, listed)) + m.watchers.size();
}

auto strip_list_height(const Model& m) -> float {
    const auto rows = strip_rows(m);
    return rows == 0 ? 0.0f : static_cast<float>(std::min(rows, kStripRows)) * kStripRow + 6.0f;
}

auto Painter::strip_wanted(const Model& m, View v) const -> bool {
    return m.open && has_background(m) && v != View::Rest;
}

auto Painter::strip_height(const Model& m) -> float {
    return kStripHead + strip_list_height(m) + cards_height(m, wt::bar::width - 2 * kStripPad, kStripCards) + kStripPad;
}

void Painter::draw_strip(Model& m, const Box& strip, Reaction& r) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    add_target("strip", strip);
    renderer.rect({strip.x + kStripPad, strip.y, strip.w - 2 * kStripPad, 1}, {.fill = ui::th().line});
    const std::size_t count = active_count(m);
    const float progress = overall_progress(m);
    const auto label = count == 0      ? std::string_view{"From the background"}
                       : progress >= 0 ? text(buf, "{} background {} \xC2\xB7 {:.0f}%", count, count == 1 ? "task" : "tasks", progress * 100)
                                       : text(buf, "{} background {}", count, count == 1 ? "task" : "tasks");
    work::caption(*ui_, {strip.x + kStripPad, strip.y + 1, strip.w - 2 * kStripPad - 60, kStripHead - 1}, Face::Text, 13.0f, label,
                ui::th().text_2);
    const float lw = work::text_width(*ui_, Face::Text, 13.0f, "Log");
    const Box log{strip.x + strip.w - kStripPad - lw, strip.y + (kStripHead - 20) / 2, lw, 20};
    if (ui_->interact(id_of("strip:log", 0), log).clicked) {
        r.show_log = true;
    }
    add_target("strip:log", log);
    work::caption(*ui_, log, Face::Text, 13.0f, "Log", ui::th().accent);
    float y = strip.y + kStripHead;
    if (const float list_h = strip_list_height(m); list_h > 0) {
        // The task in front is the window itself: every listed one behind it is a row.
        draw_task_rows(m, {strip.x, y, strip.w, list_h - 6.0f}, r);
        y += list_h;
    }
    (void)draw_cards(m, strip.x + kStripPad, y - kDropGap, strip.w - 2 * kStripPad, kStripCards, r);
}

// ---- the list's rows (they lived under the tongue, in its peek) ----

namespace {

constexpr float kRowPad = 16.0f;
constexpr float kRowTile = 28.0f;   // .mi
constexpr float kRowButton = 28.0f; // a row's icon buttons
constexpr float kRowSwitchW = 32.0f;
constexpr float kRowSwitchH = 18.0f;

} // namespace

void Painter::draw_task_rows(Model& m, const Box& rows, Reaction& r) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    // Its rows in sight; the wheel scrolls the rest. A button works only when it is all in sight.
    const float all = static_cast<float>(strip_rows(m)) * kStripRow;
    if (const auto& in = ui_->input();
        in.wheel != 0 && in.mouse_x >= rows.x && in.mouse_x < rows.x + rows.w && in.mouse_y >= rows.y && in.mouse_y < rows.y + rows.h) {
        tasks_scroll_ -= in.wheel * 40.0f;
    }
    tasks_scroll_ = std::clamp(tasks_scroll_, 0.0f, std::max(0.0f, all - rows.h));
    const auto in_sight = [&](const Box& b) { return b.y >= rows.y - 0.5f && b.y + b.h <= rows.y + rows.h + 0.5f; };
    const auto out_of_sight = [&](const Box& row) { return row.y + row.h <= rows.y || row.y >= rows.y + rows.h; };
    const float x = rows.x + kRowPad;
    const float right = rows.x + rows.w - kRowPad;
    float ry = rows.y - tasks_scroll_;
    renderer.push_clip({rows.x, rows.y, rows.w, rows.h});
    // A button only says what it wants; it is done after the rows are drawn (Stop takes a row away).
    enum class Want : std::uint8_t { None, Pause, Stop, Toggle, Remove };
    Want want = Want::None;
    std::uint64_t wanted = 0;
    const float name_h = ui_->line_height(Face::Text, 13.0f);
    for (const auto& t : m.background) {
        if (!listed(t)) {
            continue;
        }
        const Box row{rows.x, ry, rows.w, kStripRow};
        ry += kStripRow;
        if (out_of_sight(row)) {
            continue;
        }
        const events::Kind kind = t.steps.empty() ? events::Kind::Change : t.steps.back().kind;
        work::kind_icon(*ui_, {x, row.y + (kStripRow - kRowTile) / 2, kRowTile, kRowTile}, kind_of(kind), icon_of(kind),
                        t.paused ? work::Badge::Wait : work::Badge::Run, id_of("task:badge", t.task * 4 + 1)); // MS 06
        const Box stop{right - kRowButton, row.y + (kStripRow - kRowButton) / 2, kRowButton, kRowButton};
        const Box pause{stop.x - 4 - kRowButton, stop.y, kRowButton, kRowButton};
        const std::string_view value = t.paused        ? std::string_view{"paused"}
                                       : t.permission  ? std::string_view{"needs a decision"}
                                       : t.progress >= 0 ? text(buf, "{:.0f}%", t.progress * 100)
                                                         : std::string_view{};
        const float vw = value.empty() ? 0.0f : work::text_width(*ui_, Face::Text, 12.5f, value);
        const float nx = x + kRowTile + 10;
        work::caption(*ui_, {nx, row.y + (kStripRow - name_h) / 2, std::max(0.0f, pause.x - 10 - (vw > 0 ? vw + 10 : 0.0f) - nx), name_h},
                    Face::Text, 13.0f, t.question, ui::th().text);
        if (vw > 0) {
            work::caption(*ui_, {pause.x - 10 - vw, row.y, vw, kStripRow}, Face::Text, 12.5f, value, ui::th().text_3);
        }
        const auto task = t.task;
        if (in_sight(pause)) {
            add_target(text(buf, "task:pause:{}", task), pause);
            add_target(text(buf, "task:stop:{}", task), stop);
        }
        if (in_sight(pause) && work::icon_button(*ui_, id_of("task:pause", task), pause, t.paused ? work::kIconPlay : U' ')) {
            want = Want::Pause;
            wanted = task;
        }
        if (!t.paused && in_sight(pause)) { // two solid bars (the icon font's pause is an outline)
            const float cx = pause.x + pause.w / 2;
            const float cy = pause.y + pause.h / 2;
            renderer.rect({cx - 4.0f, cy - 5.5f, 2.5f, 11.0f}, {.fill = ui::th().text_2, .radius = {1, 1, 1, 1}});
            renderer.rect({cx + 1.5f, cy - 5.5f, 2.5f, 11.0f}, {.fill = ui::th().text_2, .radius = {1, 1, 1, 1}});
        }
        if (in_sight(stop) && work::icon_button(*ui_, id_of("task:stop", task), stop, work::kIconClose)) {
            want = Want::Stop;
            wanted = task;
        }
    }
    for (const auto& wa : m.watchers) {
        const Box row{rows.x, ry, rows.w, kStripRow};
        ry += kStripRow;
        if (out_of_sight(row)) {
            continue;
        }
        work::kind_icon(*ui_, {x, row.y + (kStripRow - kRowTile) / 2, kRowTile, kRowTile}, work::Kind::Read, work::kIconEye);
        const Box sw{right - kRowSwitchW, row.y + (kStripRow - kRowSwitchH) / 2, kRowSwitchW, kRowSwitchH};
        const Box remove{sw.x - 6 - kRowButton, row.y + (kStripRow - kRowButton) / 2, kRowButton, kRowButton};
        const float end = wa.on ? sw.x : remove.x;
        const std::string_view value = wa.on ? std::string_view{wa.detail} : std::string_view{"off"};
        const float vw = std::min(work::text_width(*ui_, Face::Text, 12.5f, value), 160.0f);
        const float nx = x + kRowTile + 10;
        work::caption(*ui_, {nx, row.y + (kStripRow - name_h) / 2, std::max(0.0f, end - 10 - vw - 10 - nx), name_h}, Face::Text, 13.0f, wa.title,
                    ui::th().text);
        work::caption(*ui_, {end - 10 - vw, row.y, vw, kStripRow}, Face::Text, 12.5f, value, ui::th().text_3);
        if (in_sight(sw)) {
            add_target(text(buf, "watch:{}", wa.task), sw);
        }
        if (in_sight(sw) && work::toggle(*ui_, id_of("watch", wa.task), sw, wa.on)) {
            want = Want::Toggle;
            wanted = wa.task;
        }
        if (!wa.on) { // off: it can be switched on again, or go
            if (in_sight(remove)) {
                add_target(text(buf, "watch:remove:{}", wa.task), remove);
            }
            if (in_sight(remove) && work::icon_button(*ui_, id_of("watch:remove", wa.task), remove, work::kIconClose)) {
                want = Want::Remove;
                wanted = wa.task;
            }
        }
    }
    renderer.pop_clip();
    if (r.command) {
        return;
    }
    if (want == Want::Pause) {
        r.command = pause_task(m, wanted);
    } else if (want == Want::Stop) {
        r.command = cancel_task(m, wanted);
    } else if (want == Want::Toggle) {
        r.command = toggle_watch(m, wanted);
    } else if (want == Want::Remove) {
        r.command = remove_watch(m, wanted);
    }
}

} // namespace app::sheet
