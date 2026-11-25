#include "app/tray_menu.hpp"

#include "app/model_choice.hpp"
#include "ui/anim.hpp"
#include "ui/orb.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <format>
#include <string_view>

namespace app::tray_menu {
namespace {

constexpr float kPad = 8.0f;
constexpr float kHeaderH = 64.0f;
constexpr float kSectionH = 26.0f;
constexpr float kModelH = 48.0f;
constexpr float kStepsH = 54.0f;
constexpr float kStepGap = 6.0f;
constexpr float kRowH = 36.0f;
constexpr float kValueH = 30.0f;
constexpr float kThemeH = 34.0f; // the theme's choices (stage 4d-1): a line of three, the tray's item height
constexpr float kSepH = 9.0f;

// The items' icons (Segoe Fluent Icons / MDL2 Assets, as the rest of the agent's).
constexpr char32_t kIconSettings = 0xE713; // Setting
constexpr char32_t kIconOpen = 0xE8A7;     // OpenInNewWindow
constexpr char32_t kIconBug = 0xEBE8;      // Bug
constexpr char32_t kIconPower = 0xE7E8;    // PowerButton
constexpr char32_t kIconPause = 0xE769;    // Pause (stage 4d-2)
constexpr char32_t kIconResume = 0xE768;   // Play

constexpr std::string_view kModel = "Model";
constexpr std::string_view kThinking = "Thinking";
constexpr std::string_view kBudget = "Budget per task";

auto find(std::span<const settings::Item> items, std::string_view label) -> const settings::Item* {
    const auto it = std::ranges::find_if(items, [&](const settings::Item& i) { return i.label == label; });
    return it == items.end() ? nullptr : &*it;
}

auto checked_child(const settings::Item& item) -> const settings::Item* {
    const auto it = std::ranges::find_if(item.children, [](const settings::Item& i) { return i.checked; });
    return it == item.children.end() ? nullptr : &*it;
}


} // namespace

namespace {

// `fold`: without the Model and Thinking sections (layout(): a list open on a small screen).
auto build(const config::Config& c, std::span<const settings::Item> items, bool working, bool settings_open, bool budget_open,
           bool debug, bool fold, bool paused) -> std::vector<Row> {
    std::vector<Row> rows;
    float y = kPad;
    const float w = kWidth - 2 * kPad;
    const auto add = [&](Row r, float h) {
        r.box = {kPad, y, w, h};
        y += h;
        rows.push_back(std::move(r));
    };
    const auto choice = settings::choice_of(c.ai);
    add({.kind = Kind::Header, .label = "WinAiAgent",
         .detail = std::format("{} \xC2\xB7 {}", sheet::model_of(choice).short_name, sheet::thinking_name(choice.level)),
         .state = paused ? "paused" : working ? "working" : "ready", .id = kOpenId, .model = std::max(0, sheet::model_index(choice.model))},
        kHeaderH);

    if (!fold) {
        add({.kind = Kind::Section, .label = "MODEL"}, kSectionH);
        if (const auto* model = find(items, kModel)) {
            const auto all = ai::catalog::models();
            for (const auto& m : model->children) { // a model switched off is not listed (4d-2): each found by its name
                const auto it = std::ranges::find(all, std::string_view{m.label}, &ai::catalog::Model::full_name);
                if (it == all.end()) {
                    continue;
                }
                add({.kind = Kind::Model, .label = m.label,
                     .detail = it->vision == ai::catalog::Tri::Yes ? "Sees pictures" : "No pictures", .id = m.id,
                     .checked = m.checked, .model = static_cast<int>(it - all.begin())},
                    kModelH);
            }
        }

        add({.kind = Kind::Section, .label = "THINKING"}, kSectionH);
        if (const auto* thinking = find(items, kThinking)) {
            const auto n = static_cast<float>(thinking->children.size());
            const float step_w = (w - kStepGap * (n - 1)) / n;
            for (std::size_t i = 0; i < thinking->children.size(); ++i) {
                const auto& t = thinking->children[i];
                Row r{.kind = Kind::Thinking, .label = t.label, .id = t.id, .checked = t.checked, .model = std::max(0, sheet::model_index(choice.model)),
                      .level = static_cast<int>(i)};
                r.box = {kPad + static_cast<float>(i) * (step_w + kStepGap), y, step_w, kStepsH};
                rows.push_back(std::move(r));
            }
            y += kStepsH + 6;
        }

        // Stage 4d-1 (SETTINGS_SPEC § 5: Theme): its three choices on a line, as the thinking steps.
        add({.kind = Kind::Section, .label = "THEME"}, kSectionH);
        if (const auto* theme = find(items, "Theme")) {
            const auto n = static_cast<float>(theme->children.size());
            const float step_w = (w - kStepGap * (n - 1)) / n;
            for (std::size_t i = 0; i < theme->children.size(); ++i) {
                const auto& t = theme->children[i];
                Row r{.kind = Kind::Theme, .label = t.label, .id = t.id, .checked = t.checked};
                r.box = {kPad + static_cast<float>(i) * (step_w + kStepGap), y, step_w, kThemeH};
                rows.push_back(std::move(r));
            }
            y += kThemeH + 6;
        }
    }

    (void)budget_open; // stage 4d-2: the budget per task is Roles' (SETTINGS_SPEC § 5 has none in the tray)
    (void)settings_open; // stage 4d-1: the settings are the window's own mode, no list of them here
    add({.kind = Kind::Separator}, kSepH);
    // The shortcut that works: the settings' checked one (the fallback when the one set was taken).
    const auto* shortcut = find(items, "Keyboard shortcut");
    const auto* works = shortcut != nullptr ? checked_child(*shortcut) : nullptr;
    add({.kind = Kind::Item, .label = "Show agent", .detail = works != nullptr ? works->label : c.hotkey, .id = kOpenId, .icon = kIconOpen},
        kRowH);
    add({.kind = Kind::Item, .label = paused ? "Resume agent" : "Pause agent", .id = kPauseId, .icon = paused ? kIconResume : kIconPause}, kRowH);
    add({.kind = Kind::Item, .label = "Settings\xE2\x80\xA6", .detail = "Ctrl ,", .id = settings::kSettingsId, .icon = kIconSettings}, kRowH);
    if (debug && !fold) { // folded on a small screen: F12 opens it all the same (phase 12: room for Rules and memory)
        add({.kind = Kind::Item, .label = "Debug window (F12)", .id = kDebugId, .icon = kIconBug}, kRowH);
    }
    add({.kind = Kind::Item, .label = "Quit", .id = kQuitId, .icon = kIconPower}, kRowH);
    for (auto& r : rows) { // where each is with its lists all the way open (unfold() starts from it)
        r.full = r.box;
    }
    return rows;
}

} // namespace

auto layout(const config::Config& c, std::span<const settings::Item> items, bool working, bool settings_open, bool budget_open,
            bool debug, float room, bool paused) -> std::vector<Row> {
    // Stage 4d-1: no list of the settings any more; a menu taller than the room folds its sections
    // (the header still says the choice).
    auto rows = build(c, items, working, settings_open, budget_open, debug, false, paused);
    if (height(rows) <= room) {
        return rows;
    }
    return build(c, items, working, settings_open, budget_open, debug, true, paused);
}

void Unfold::step(double now) noexcept {
    if (last_ > -1.0e8 && now > last_) {
        scene_.step(static_cast<float>(std::min(now - last_, static_cast<double>(ui::motion::kMaxFrameDt))));
    }
    last_ = std::max(last_, now);
}

void Unfold::set(bool open, double now) noexcept {
    step(now); // from where it is: no jump when asked back midway
    if (open == open_) {
        return;
    }
    open_ = open;
    if (open) {
        scene_.open();
    } else {
        scene_.close();
    }
}

auto Unfold::shown(double now) noexcept -> Shown {
    step(now);
    const auto f = scene_.frame();
    return {.room = f.room, .alpha = f.body_alpha, .turn = f.chevron / 90.0f};
}

auto Unfold::moving(double now) noexcept -> bool {
    step(now);
    return !scene_.done();
}

auto full_height(const std::vector<Row>& rows) -> float {
    float bottom = 0;
    for (const auto& r : rows) {
        bottom = std::max(bottom, r.full.y + r.full.h);
    }
    return bottom + kPad;
}

auto window_height(const config::Config& c, std::span<const settings::Item> items, bool working, bool debug, float room) -> float {
    float tallest = 0;
    for (const bool settings : {false, true}) {
        for (const bool budget : {false, true}) {
            tallest = std::max(tallest, full_height(layout(c, items, working, settings, budget, debug, room)));
        }
    }
    return tallest;
}

void unfold(std::vector<Row>& rows, float settings, float budget) noexcept {
    unfold(rows, Shown{settings, settings, settings}, Shown{budget, budget, budget});
}

void unfold(std::vector<Row>& rows, Shown settings, Shown budget) noexcept {
    // A list opens as a curtain: its rows at their own places, what shows of it growing from its
    // top; the rows under it move with its edge. `shut`: how much of the lists above is not open.
    float shut = 0;
    float edge = 0; // the list's shown bottom, in the rows' moved places
    // A list asked open that the layout left out (a small screen closed the budget's for the
    // settings'): its chevron stays shut.
    const bool has_settings = std::ranges::any_of(rows, [](const Row& r) { return r.in == Row::In::Settings; });
    const bool has_budget = std::ranges::any_of(rows, [](const Row& r) { return r.in == Row::In::Budget; });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto& r = rows[i];
        r.box = r.full;
        r.box.y -= shut;
        if (r.in != Row::In::None) {
            const Shown& list = r.in == Row::In::Settings ? settings : budget;
            const float k = list.room;
            if (i == 0 || rows[i - 1].in != r.in) { // its first row: the list's whole height, its edge
                float whole = 0;
                for (std::size_t j = i; j < rows.size() && rows[j].in == r.in; ++j) {
                    whole += rows[j].full.h;
                }
                edge = r.box.y + whole * k;
            }
            const float at = r.box.y; // its place in the list
            r.box.h = std::clamp(edge - at, 0.0f, r.full.h);
            r.alpha = r.box.h > 0.0f ? list.alpha : 0.0f;
            if (r.box.h <= 0.0f) {
                r.box.y = edge; // not shown yet: at the curtain's edge (the menu's height ends there)
            }
            const bool last = i + 1 == rows.size() || rows[i + 1].in != r.in;
            if (last) {
                shut += (at + r.full.h) - edge; // the rest of the list, not shown yet
            }
        }
        if (r.kind == Kind::Settings) {
            r.turn = has_settings ? settings.turn : 0.0f;
        } else if (r.kind == Kind::Budget) {
            r.turn = has_budget ? budget.turn : 0.0f;
        }
    }
}

auto height(const std::vector<Row>& rows) -> float {
    float bottom = 0;
    for (const auto& r : rows) {
        bottom = std::max(bottom, r.box.y + r.box.h);
    }
    return bottom + kPad;
}

auto hit(const std::vector<Row>& rows, float x, float y) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.kind == Kind::Section || r.kind == Kind::Separator || r.alpha < 0.01f) { // a shut list's rows: none
            continue;
        }
        if (x >= r.box.x && x < r.box.x + r.box.w && y >= r.box.y && y < r.box.y + r.box.h) {
            return i;
        }
    }
    return std::nullopt;
}

auto step(const std::vector<Row>& rows, std::optional<std::size_t> from, int dir) -> std::optional<std::size_t> {
    const auto takes = [](const Row& r) { return r.kind != Kind::Section && r.kind != Kind::Separator && r.alpha >= 0.01f; };
    // The lines: the rows that take a key, one a line (the thinking steps share one), top to bottom —
    // on the stack: a held key repeats this (the menu has some 40 rows; past kMost, the rest none).
    constexpr std::size_t kMost = 128;
    std::array<std::size_t, kMost> lines{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!takes(rows[i])) {
            continue;
        }
        if (count > 0 && std::abs(rows[lines[count - 1]].box.y - rows[i].box.y) < 0.5f) {
            if ((rows[i].kind == Kind::Thinking || rows[i].kind == Kind::Theme) && rows[i].checked) {
                lines[count - 1] = i; // the thinking line, the theme's: the one chosen
            }
            continue;
        }
        if (count < kMost) {
            lines[count++] = i;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    const auto n = static_cast<std::ptrdiff_t>(count);
    std::ptrdiff_t at = dir > 0 ? -1 : n; // none yet: before the first, after the last
    if (from && *from < rows.size()) {
        for (std::ptrdiff_t k = 0; k < n; ++k) {
            if (std::abs(rows[lines[static_cast<std::size_t>(k)]].box.y - rows[*from].box.y) < 0.5f) {
                at = k;
                break;
            }
        }
    }
    const std::ptrdiff_t next = ((at + (dir > 0 ? 1 : -1)) % n + n) % n; // around at the ends
    return lines[static_cast<std::size_t>(next)];
}

auto level_step(const std::vector<Row>& rows, int dir) -> std::optional<std::size_t> {
    const auto chosen = std::ranges::find_if(rows, [](const Row& r) { return r.kind == Kind::Thinking && r.checked; });
    if (chosen == rows.end()) {
        return std::nullopt;
    }
    const int want = chosen->level + (dir > 0 ? 1 : -1);
    const auto next = std::ranges::find_if(rows, [&](const Row& r) { return r.kind == Kind::Thinking && r.level == want; });
    return next == rows.end() ? std::nullopt : std::optional{static_cast<std::size_t>(next - rows.begin())};
}

void live(std::vector<Row>& rows, bool working) noexcept {
    if (!rows.empty() && rows.front().kind == Kind::Header) {
        rows.front().state.assign(working ? "working" : "ready"); // in its own room: no allocation
    }
}

auto reopens(double lost_at, double pressed_at) noexcept -> bool {
    return std::abs(pressed_at - lost_at) >= kReclick; // the close and the press, in either order
}

auto glass_top(RECT icon, RECT work, LONG width, LONG full, LONG shown) -> LONG {
    const POINT window = place(icon, work, {width, full});
    const POINT glass = place(icon, work, {width, shown});
    return std::clamp(glass.y - window.y, 0L, std::max(0L, full - shown));
}

auto place(RECT icon, RECT work, SIZE menu) -> POINT {
    constexpr LONG kGap = 8;
    const LONG cx = (icon.left + icon.right) / 2;
    const LONG cy = (icon.top + icon.bottom) / 2;
    // Above the icon: the taskbar at the bottom, the overflow flyout. At the top the clamp below puts it
    // at the work area's top edge, under the taskbar: nothing more to do for that side.
    POINT p{cx - menu.cx / 2, icon.top - kGap - menu.cy};
    if (icon.right <= work.left) { // at the left: right of it
        p = {icon.right + kGap, cy - menu.cy / 2};
    } else if (icon.left >= work.right) { // at the right: left of it
        p = {icon.left - kGap - menu.cx, cy - menu.cy / 2};
    }
    p.x = std::clamp(p.x, work.left, std::max(work.left, work.right - menu.cx));
    p.y = std::clamp(p.y, work.top, std::max(work.top, work.bottom - menu.cy));
    return p;
}

void draw(ui::Ui& ui, const std::vector<Row>& rows, std::optional<std::size_t> hover, std::array<GLuint, 2> orbs, float width,
          float height, ui::text::Point origin, GLuint live_orb) {
    namespace work = ui::work;
    using ui::Face;
    using Color = ui::render::Color;
    auto& r = ui.renderer();
    const auto faint = [](Color c, float share) { // `c` at `share` of its own alpha
        c.a = static_cast<std::uint8_t>(static_cast<float>(c.a) * share + 0.5f);
        return c;
    };
    const auto orb = [&](int model, ui::layout::Box b) { // a model's still: the catalog's first two have one
        if (const GLuint t = orbs[static_cast<std::size_t>(std::clamp(model, 0, static_cast<int>(orbs.size()) - 1))]; t != 0) {
            r.image({b.x, b.y, b.w, b.h}, ui::Orb::uv(), t, {255, 255, 255, 255});
        }
    };
    // A menu is a pop (the designer's answer on the palette, 2): its surface, the stronger line all
    // round, four round corners. No shadow: its window's region is its own rectangle.
    // The line is laid over the surface (a rect's own border takes the fill's place at the edge: on a
    // window of its own that edge would be see-through).
    r.rect({origin.x, origin.y, width, height}, {.fill = ui::th().pop, .radius = {14, 14, 14, 14}});
    r.rect({origin.x, origin.y, width, height}, {.fill = {}, .radius = {14, 14, 14, 14}, .border = ui::th().line_2, .border_width = 1});
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.alpha < 0.01f) {
            continue; // a shut list's row
        }
        // A row of a list unfolding: drawn at its full height from its top, cut to what shows, fading in.
        const bool partial = row.alpha < 0.999f;
        const ui::layout::Box shows{row.box.x + origin.x, row.box.y + origin.y, row.box.w, row.box.h};
        const ui::layout::Box b{shows.x, shows.y, shows.w, partial ? row.full.h : shows.h};
        const float was = r.opacity();
        if (partial) {
            r.set_opacity(was * row.alpha);
            r.push_clip({shows.x, shows.y, shows.w, shows.h});
        }
        const bool hot = hover == i;
        const float mid = b.y + b.h / 2;
        // MS 14: its light under the mouse on quick both ways — by what the row is, so a list
        // unfolding above it does not hand its light to another row.
        const ui::Id light_id = 0x547261794D656E75ull ^ (static_cast<ui::Id>(row.kind) << 40) ^
                                (row.id != 0 ? static_cast<ui::Id>(row.id) : 0x100000ull + static_cast<ui::Id>(i));
        const float light = std::clamp(ui.hover(light_id, hot), 0.0f, 1.0f);
        const auto lit = [&] {
            if (light > 0.004f) {
                r.rect({b.x, b.y, b.w, b.h}, {.fill = faint(ui::th().w1, light), .radius = {9, 9, 9, 9}});
            }
        };
        switch (row.kind) {
        case Kind::Header: {
            if (live_orb != 0) { // the live orb (design/ui_model: its header's orb moves)
                r.image({b.x + 6, b.y + 12, 40, 40}, ui::Orb::uv(), live_orb, {255, 255, 255, 255});
            } else {
                orb(row.model, {b.x + 6, b.y + 12, 40, 40});
            }
            work::caption(ui, {b.x + 56, b.y + 10, 200, 20}, Face::Semibold, 14.0f, row.label, ui::th().text);
            const float tw = work::text_width(ui, Face::Semibold, 11.0f, row.detail);
            const ui::layout::Box pill{b.x + 56, b.y + 34, 8 + 6 + 5 + tw + 9, 18};
            r.rect({pill.x, pill.y, pill.w, pill.h}, {.fill = ui::th().w1, .radius = {9, 9, 9, 9}, .border = ui::th().accent_line, .border_width = 1});
            r.rect({pill.x + 8, pill.y + 6, 6, 6}, {.fill = ui::th().accent, .radius = {3, 3, 3, 3}});
            work::caption(ui, {pill.x + 19, pill.y, tw + 1, pill.h}, Face::Semibold, 11.0f, row.detail, ui::th().text);
            work::caption(ui, {pill.x + pill.w + 8, pill.y, 100, pill.h}, Face::Text, 11.5f, row.state, ui::th().text_3);
            ui.draw_icon(work::kIconExpand, 13.0f, {b.x + b.w - 18, b.y + 22}, hot ? ui::th().text : ui::th().text_3);
            break;
        }
        case Kind::Section: work::caption(ui, {b.x + 8, b.y + 8, b.w, 14}, Face::Semibold, 10.5f, row.label, ui::th().text_3); break;
        case Kind::Model:
            if (row.checked) {
                r.rect({b.x, b.y + 2, b.w, b.h - 4},
                       {.fill = ui::th().accent_soft, .radius = {10, 10, 10, 10}, .border = ui::th().accent_line, .border_width = 1.2f});
            } else {
                lit();
            }
            orb(row.model, {b.x + 6, b.y + 9, 30, 30});
            work::caption(ui, {b.x + 46, b.y + 6, b.w - 90, 18}, Face::Semibold, 13.0f, row.label, ui::th().text);
            work::caption(ui, {b.x + 46, b.y + 25, b.w - 90, 16}, Face::Text, 11.5f, row.detail, ui::th().text_3);
            if (row.checked) {
                const ui::text::Point c{b.x + b.w - 20, mid};
                r.rect({c.x - 9, c.y - 9, 18, 18}, {.fill = ui::th().accent, .radius = {9, 9, 9, 9}});
                ui.draw_icon(ui::widgets::kIconCheck, 9.0f, c, ui::th().on_accent);
            }
            break;
        case Kind::Thinking: {
            r.rect({b.x, b.y, b.w, b.h}, {.fill = row.checked ? ui::th().accent_soft : hot ? ui::th().w2 : ui::th().w1,
                                          .radius = {10, 10, 10, 10}, .border = row.checked ? ui::th().accent_line : ui::th().line,
                                          .border_width = row.checked ? 1.3f : 1.0f});
            constexpr std::array<int, 4> kDrops{0, 2, 4, 5};
            const int n = kDrops[static_cast<std::size_t>(std::clamp(row.level, 0, 3))];
            const Color dot = row.checked ? ui::th().accent : ui::th().text_3;
            if (n == 0) {
                r.rect({b.x + b.w / 2 - 3, b.y + 13, 6, 6}, {.radius = {3, 3, 3, 3}, .border = dot, .border_width = 1.0f});
            } else {
                const float total = static_cast<float>(n) * 4 + static_cast<float>(n - 1) * 2.5f;
                for (int d = 0; d < n; ++d) {
                    r.rect({b.x + (b.w - total) / 2 + static_cast<float>(d) * 6.5f, b.y + 14, 4, 4}, {.fill = dot, .radius = {2, 2, 2, 2}});
                }
            }
            const float lw = work::text_width(ui, Face::Semibold, 12.5f, row.label);
            work::caption(ui, {b.x + (b.w - lw) / 2, b.y + 26, lw + 1, 18}, Face::Semibold, 12.5f, row.label,
                        row.checked ? ui::th().text : ui::th().text_2);
            break;
        }
        case Kind::Theme: { // a choice on its line (stage 4d-1), as a thinking step without its drops
            r.rect({b.x, b.y, b.w, b.h}, {.fill = row.checked ? ui::th().accent_soft : hot ? ui::th().w2 : ui::th().w1,
                                          .radius = {10, 10, 10, 10}, .border = row.checked ? ui::th().accent_line : ui::th().line,
                                          .border_width = row.checked ? 1.3f : 1.0f});
            const float lw = work::text_width(ui, Face::Semibold, 12.5f, row.label);
            work::caption(ui, {b.x + (b.w - lw) / 2, b.y, lw + 1, b.h}, Face::Semibold, 12.5f, row.label,
                          row.checked ? ui::th().text : ui::th().text_2);
            break;
        }
        case Kind::Budget:
        case Kind::Settings:
        case Kind::Item: {
            lit();
            if (row.kind == Kind::Budget) { // "$" in a ring
                r.rect({b.x + 13, mid - 7, 14, 14}, {.radius = {7, 7, 7, 7}, .border = ui::th().text_3, .border_width = 1.2f});
                work::caption(ui, {b.x + 17, b.y, 8, b.h}, Face::Semibold, 9.5f, "$", ui::th().text_3);
            } else if (row.icon != 0) {
                ui.draw_icon(row.icon, 13.0f, {b.x + 20, mid}, ui::th().text_3);
            }
            work::caption(ui, {b.x + 36, b.y, b.w - 140, b.h}, Face::Text, 13.0f, row.label, ui::th().text);
            float right_x = b.x + b.w - 12;
            if (row.kind != Kind::Item) {
                work::chevron(ui, {right_x - 6, mid}, 11.0f, row.turn * 90.0f, ui::th().text_3); // › turns to ˅ as its list unfolds (MS 10)
                right_x -= 20;
            }
            if (!row.detail.empty()) {
                const float dw = work::text_width(ui, Face::Text, 12.0f, row.detail);
                if (row.kind == Kind::Item) { // a key: in its frame
                    r.rect({right_x - dw - 12, mid - 10, dw + 12, 20}, {.radius = {5, 5, 5, 5}, .border = ui::th().line, .border_width = 1.0f});
                    work::caption(ui, {right_x - dw - 6, b.y, dw + 1, b.h}, Face::Text, 12.0f, row.detail, ui::th().text_3);
                } else {
                    work::caption(ui, {right_x - dw, b.y, dw + 1, b.h}, Face::Text, 12.0f, row.detail, ui::th().text_2);
                }
            }
            break;
        }
        case Kind::BudgetValue:
        case Kind::SettingsItem: {
            lit();
            work::caption(ui, {b.x + 26, b.y, b.w - 150, b.h}, Face::Text, 12.5f, row.label, ui::th().text_2);
            if (row.kind == Kind::BudgetValue && row.checked) {
                r.rect({b.x + 12, mid - 3, 6, 6}, {.fill = ui::th().accent, .radius = {3, 3, 3, 3}});
            }
            if (!row.detail.empty()) {
                const float dw = work::text_width(ui, Face::Text, 12.0f, row.detail);
                work::caption(ui, {b.x + b.w - 12 - dw, b.y, dw + 1, b.h}, Face::Text, 12.0f, row.detail, ui::th().text_3);
            }
            break;
        }
        case Kind::Separator: r.rect({b.x + 6, b.y + 4, b.w - 12, 1}, {.fill = ui::th().line}); break;
        }
        if (partial) {
            r.pop_clip();
            r.set_opacity(was);
        }
    }
}

} // namespace app::tray_menu
