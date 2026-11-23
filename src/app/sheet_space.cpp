// What takes the space in the data panel (phase 13a § 4, design/ui_system 02b–02c): over the
// table of folders, a drive's bar with its names, then the way back — the arrow, the path (its
// middle folded when it does not fit), Folders | Largest files and Show in Explorer. A step down
// is a row's Open; a step back the arrow or a name of the path. Nothing is made in a frame: the
// names are the set's own (app::space made them with it).

#include "app/sheet_view.hpp"

#include "app/space.hpp"
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
constexpr float kBar = 16;
constexpr float kLegendLine = 18;
constexpr float kWay = 38;
constexpr char32_t kIconBack = 0xE72B;
constexpr char32_t kIconOpenOut = 0xE8A7;
constexpr std::string_view kMore = "\xE2\x80\xA6";  // …
constexpr std::string_view kStep = "\xE2\x80\xBA";  // ›

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

// A folder's step (MS § 7): its clock and its tracks.
const ui::Id kStepClock = id_of("space:step-clock");
const ui::Id kStepOutTrack = id_of("space:step-out");
const ui::Id kStepOutShiftTrack = id_of("space:step-out-shift");
const ui::Id kStepInTrack = id_of("space:step-in");
const ui::Id kStepInShiftTrack = id_of("space:step-in-shift");

// The bar's pieces, the largest first: the chart's four colours; the fifth and further, and Other,
// are the rest's.
auto slice_color(const events::SpaceSlice& s, std::size_t i) -> Color {
    const auto& t = ui::th();
    if (s.name == "Other" || i > 3) {
        return t.c_rest;
    }
    return i == 0 ? t.c1 : i == 1 ? t.c2 : i == 2 ? t.c3 : t.c4;
}

} // namespace

void Painter::space_step(Model& m, std::uint32_t set, std::vector<std::uint32_t> path, bool files) {
    // MS § 7 "Into a folder / back": the list it leaves goes as it was — its rows and its table's
    // state copied here, once, at the click — while the new one comes from the other side.
    const auto shown = std::ranges::find(m.sets, set, &events::DataSet::id);
    const std::size_t depth_was = shown != m.sets.end() ? shown->space_path.size() : 0;
    if (shown != m.sets.end()) {
        space_old_rows_ = shown->rows;
    }
    auto& state = table_state(set);
    space_old_state_ = state;
    const int sort = state.sort_column;
    const bool descending = state.descending;
    const std::size_t depth = path.size();
    space_show(m, set, std::move(path), files);
    state = {}; // other rows: from the top…
    state.sort_column = sort; // …sorted as the user had it
    state.descending = descending;
    if (const auto now_shown = std::ranges::find(m.sets, set, &events::DataSet::id); now_shown != m.sets.end()) {
        state.came.assign(now_shown->rows.rows(), -1.0e9); // the list comes as one, not row by row
    }
    space_old_set_ = set;
    space_way_ = depth >= depth_was ? 1.0f : -1.0f;
    anim_.restart(kStepClock);
    anim_.place(kStepOutTrack, 1.0f);
    anim_.place(kStepOutShiftTrack, 0.0f);
    anim_.place(kStepInTrack, 0.0f);
    anim_.place(kStepInShiftTrack, ui::motion::panel::kStepAside * space_way_);
    redraw_due_ = true;
}

auto Painter::space_stepping(const events::DataSet& set) -> PanelMotion {
    PanelMotion pm{.out = 0, .out_shift = 0, .in = 1, .in_shift = 0};
    if (space_old_set_ != set.id) {
        return pm;
    }
    const bool due = anim_.since(kStepClock) >= std::chrono::duration<double>(ui::motion::panel::kStepInAt).count();
    pm.out = anim_.to(kStepOutTrack, 0.0f, ui::motion::kMicro);
    pm.out_shift = anim_.to(kStepOutShiftTrack, -ui::motion::panel::kStepAside * space_way_, ui::motion::kBase);
    pm.in = anim_.to(kStepInTrack, due ? 1.0f : 0.0f, ui::motion::kQuick);
    pm.in_shift = anim_.to(kStepInShiftTrack, due ? 0.0f : ui::motion::panel::kStepAside * space_way_, ui::motion::kSmooth);
    if (!due) {
        redraw_due_ = true; // its turn comes on a frame
    } else if (pm.out <= 0.004f && pm.in >= 1.0f && pm.in_shift == 0.0f) {
        space_old_set_ = 0; // over: the rows it left are let go
        space_old_rows_ = {};
    }
    return pm;
}

void Painter::draw_space_left(const events::DataSet& set, const Box& table, float opacity) {
    if (table.h <= 40 || opacity <= 0.004f) {
        return;
    }
    auto& renderer = ui_->renderer();
    const auto keep = target_count_;
    const bool was_inert = ui_->inert();
    const float was = renderer.opacity();
    ui_->set_inert(true);
    renderer.set_opacity(was * opacity);
    (void)work::table(*ui_, id_of("space:left", set.id), table, set.columns, space_old_rows_, space_old_state_, true);
    renderer.set_opacity(was);
    ui_->set_inert(was_inert);
    target_count_ = keep;
}

auto Painter::draw_space_head(Model& m, const events::DataSet& set, const Box& body, float y, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const float x = body.x + kPadX;
    const float w = body.w - 2 * kPadX;

    // A drive's bar: its largest folders, Other, the free space as the bare track; their names under it.
    if (!set.space_bar.empty()) {
        std::array<work::Segment, 8> segments{};
        std::size_t n = 0;
        for (std::size_t i = 0; i < set.space_bar.size() && n < segments.size(); ++i) {
            if (!set.space_bar[i].free) {
                segments[n++] = {set.space_bar[i].share, slice_color(set.space_bar[i], i)};
            }
        }
        const Box bar{x, y, w, kBar};
        work::stacked_bar(*ui_, bar, std::span{segments.data(), n}, id_of("space:bar", static_cast<std::size_t>(set.id) * 16)); // MS § 7: its segments grow in
        float lx = x;
        float ly = y + kBar + 10;
        for (std::size_t i = 0; i < set.space_bar.size(); ++i) {
            const auto& s = set.space_bar[i];
            const float nw = work::text_width(*ui_, Face::Text, 12.0f, s.name);
            const float sw = work::text_width(*ui_, Face::Semibold, 12.0f, s.size);
            const float item = 8 + 6 + nw + 6 + sw + 14;
            if (lx > x && lx + item > x + w) {
                lx = x;
                ly += kLegendLine;
            }
            const float my = ly + kLegendLine / 2;
            if (s.free) {
                renderer.rect({lx, my - 4, 8, 8}, {.radius = {2, 2, 2, 2}, .border = ui::th().text_3, .border_width = 1});
            } else {
                renderer.rect({lx, my - 4, 8, 8}, {.fill = slice_color(s, i), .radius = {2, 2, 2, 2}});
            }
            work::caption(*ui_, {lx + 14, ly, nw, kLegendLine}, Face::Text, 12.0f, s.name, ui::th().text_3);
            work::caption(*ui_, {lx + 14 + nw + 6, ly, sw, kLegendLine}, Face::Semibold, 12.0f, s.size, ui::th().text_2);
            lx += item;
        }
        add_target("space:bar", {x, y, w, ly + kLegendLine - y});
        y = ly + kLegendLine + 12;
    }

    // The way back.
    const Box way{x, y, w, kWay};
    renderer.rect({way.x, way.y, way.w, way.h}, {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
    const Box back{way.x + 5, way.y + 5, 28, 28};
    add_target("space:back", back);
    if (work::icon_button(*ui_, id_of("space:back"), back, kIconBack, false, !set.space_path.empty()) && !set.space_path.empty()) {
        auto up = set.space_path;
        up.pop_back();
        space_step(m, set.id, std::move(up), false);
        return -1;
    }
    // Right: Show in Explorer, and Folders | Largest files before it.
    constexpr std::string_view kExplorer = "Show in Explorer";
    const float ew = work::text_width(*ui_, Face::Semibold, 12.0f, kExplorer) + 14 + 8 + 20;
    const Box explorer{way.x + way.w - 5 - ew, way.y + 5, ew, 28};
    add_target("space:explorer", explorer);
    {
        const auto hit = ui_->interact(id_of("space:explorer"), explorer, false);
        const float h = ui_->hover(id_of("space:explorer"), hit.hovered);
        renderer.rect({explorer.x, explorer.y, explorer.w, explorer.h},
                      {.fill = blend(ui::th().w2, ui::th().w3, h), .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconOpenOut, 12.0f, {explorer.x + 10 + 7, explorer.y + 14}, ui::th().text_2);
        work::caption(*ui_, {explorer.x + 10 + 14 + 8, explorer.y, ew - 32, 28}, Face::Semibold, 12.0f, kExplorer, ui::th().text);
        if (hit.clicked) {
            r.open_folder = set.location;
        }
    }
    const std::array<std::string_view, 2> views{"Folders", "Largest files"};
    const std::array<std::string_view, 2> view_targets{"space:folders", "space:files"};
    std::array<float, 2> vw{};
    for (std::size_t i = 0; i < 2; ++i) {
        vw[i] = work::text_width(*ui_, Face::Semibold, 12.0f, views[i]) + 20;
    }
    const Box group{explorer.x - 8 - vw[0] - vw[1], way.y + 5, vw[0] + vw[1], 28};
    renderer.rect({group.x, group.y, group.w, group.h}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
    float gx = group.x;
    for (std::size_t i = 0; i < 2; ++i) {
        const Box b{gx, group.y, vw[i], group.h};
        gx += vw[i];
        const bool on = set.space_files == (i == 1);
        const auto hit = ui_->interact(id_of(view_targets[i]), b, false);
        const float h = ui_->hover(id_of(view_targets[i]), hit.hovered);
        if (on) { // the chosen one's ground slides to it on base, its words turn on quick (the map's unnamed: segments)
            const Box ground = work::chosen_ground(*ui_, id_of("space:view"), group.x, {b.x + 2, b.y + 2, b.w - 4, b.h - 4});
            renderer.rect({ground.x, ground.y, ground.w, ground.h}, {.fill = ui::th().w3, .radius = {5, 5, 5, 5}});
        } else if (h > 0.01f) {
            renderer.rect({b.x + 2, b.y + 2, b.w - 4, b.h - 4}, {.fill = faint(ui::th().w2, h), .radius = {5, 5, 5, 5}});
        }
        work::caption(*ui_, {b.x + 10, b.y, b.w - 10, b.h}, Face::Semibold, 12.0f, views[i],
                      blend(ui::th().text_3, ui::th().text, work::chosen(*ui_, id_of(view_targets[i]), on)));
        add_target(view_targets[i], b);
        if (hit.clicked && !on) {
            space_step(m, set.id, set.space_files ? std::vector<std::uint32_t>{} : set.space_path, i == 1);
            return -1;
        }
    }
    // The path: each name a step back to it; its middle folded into "…" when it does not fit.
    const float px = back.x + back.w + 8;
    const float room = group.x - 12 - px;
    const auto& crumbs = set.space_crumbs;
    const float step_w = work::text_width(*ui_, Face::Text, 12.5f, kStep) + 12;
    float total = 0;
    for (const auto& c : crumbs) {
        total += work::text_width(*ui_, Face::Text, 12.5f, c) + step_w;
    }
    // Folded: the first, "…", then as many of the last as fit.
    std::size_t keep_from = 1;
    if (total > room && crumbs.size() > 2) {
        float need = work::text_width(*ui_, Face::Text, 12.5f, crumbs.front()) + step_w + work::text_width(*ui_, Face::Text, 12.5f, kMore) + step_w;
        keep_from = crumbs.size();
        while (keep_from > 1) {
            const float cw = work::text_width(*ui_, Face::Text, 12.5f, crumbs[keep_from - 1]) + step_w;
            if (need + cw > room && keep_from < crumbs.size()) {
                break;
            }
            need += cw;
            --keep_from;
        }
    }
    float cx = px;
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        if (i > 0 && i < keep_from) {
            if (i == 1) {
                const float mw = work::text_width(*ui_, Face::Text, 12.5f, kMore);
                work::caption(*ui_, {cx, way.y, mw, kWay}, Face::Text, 12.5f, kMore, ui::th().text_3);
                cx += mw + 6;
                work::caption(*ui_, {cx, way.y, step_w, kWay}, Face::Text, 12.5f, kStep, ui::th().text_3);
                cx += step_w - 6;
            }
            continue;
        }
        const bool last = i + 1 == crumbs.size();
        const float cw = std::min(work::text_width(*ui_, last ? Face::Semibold : Face::Text, 12.5f, crumbs[i]), std::max(0.0f, px + room - cx));
        const Box c{cx, way.y + 5, cw, 28};
        const auto name = text(buf, "space:crumb:{}", i);
        add_target(name, c);
        const auto hit = ui_->interact(id_of("space:crumb", i), c, false);
        const float h = ui_->hover(id_of("space:crumb", i), hit.hovered && !last);
        work::caption(*ui_, {c.x, way.y, c.w, kWay}, last ? Face::Semibold : Face::Text, 12.5f, crumbs[i],
                    last ? ui::th().text : (h > 0.5f ? ui::th().text_2 : ui::th().text_3));
        cx += cw + 6;
        if (hit.clicked && !last) {
            space_step(m, set.id, {set.space_path.begin(), set.space_path.begin() + static_cast<std::ptrdiff_t>(i)}, false);
            return -1;
        }
        if (!last) {
            work::caption(*ui_, {cx, way.y, step_w, kWay}, Face::Text, 12.5f, kStep, ui::th().text_3);
            cx += step_w - 6;
        }
    }
    return y + kWay + 12;
}

} // namespace app::sheet
