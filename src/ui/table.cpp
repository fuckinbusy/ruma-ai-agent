#include "ui/table.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <numeric>

namespace ui::work {
namespace {

using layout::Box;
using render::Color;

constexpr float kHeader = 34.0f;
constexpr float kRow = 40.0f;
constexpr float kDenseRow = 36.0f;
constexpr float kDivider = 28.0f;
constexpr float kTotal = 40.0f;
constexpr float kPad = 14.0f;    // the rows' padding 0 14…
constexpr float kGap = 14.0f;    // …and the columns' gap
constexpr float kValue = 60.0f;  // a size's value, then its bar
constexpr float kBarGap = 8.0f;
constexpr std::size_t kMaxColumns = 16;
constexpr float kMore = 26.0f;                    // an openable row's "..." at its right end
constexpr char32_t kIconMore = 0xE712;
// Phase 13a (design/ui_system README): a row's spark, its note, a badge where a button would be.
constexpr char32_t kIconLock = 0xE72E;
constexpr std::string_view kDown = "\xE2\x86\x93"; // ↓
constexpr std::string_view kUp = "\xE2\x86\x91";   // ↑

// MS 08 (ui/motion_tokens.hpp, table::).
constexpr double kStaggerSeconds = std::chrono::duration<double>(motion::table::kStagger).count();
constexpr double kBarAfterSeconds = std::chrono::duration<double>(motion::table::kBarAt).count();
constexpr double kRowYoungSeconds = std::chrono::duration<double>(motion::table::kRowYoung).count();
constexpr double kSweepSeconds = std::chrono::duration<double>(motion::table::kSkeletonSweep).count();
constexpr double kLeaveCloseAfterSeconds = std::chrono::duration<double>(motion::table::kLeaveCloseAt).count();
constexpr std::size_t kSkeletonRows = 6;

// A token at `k` of its own strength (a light coming or going).
auto faded(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

auto rect(const Box& b) -> render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto all(float r) -> render::Corners {
    return {r, r, r, r};
}

// A row not there yet: a bar where its name will be (each row's of another length, as the
// mockup's 46, 30, 38, 52, 34, 44 %), at `opacity`; `lit`: its light runs across — only while the
// data is waited for.
void skeleton_row(Ui& ui, const Box& row, float x, float inner, std::uint32_t index, float opacity, bool lit) {
    constexpr std::array<float, 6> kShares{0.46f, 0.30f, 0.38f, 0.52f, 0.34f, 0.44f};
    skeleton_bar(ui, {x, row.y + row.h / 2 - 5, inner * kShares[index % kShares.size()], 10}, opacity, lit);
}

// `words` in `text`, any case (ASCII letters).
auto contains(std::string_view text, std::string_view words) -> bool {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::ranges::search(text, words, [&](char a, char b) { return lower(a) == lower(b); }).begin() != text.end();
}

// A group with a title has its divider; an untitled one (the busy processes) none.
auto titled(const DataRows& rows, int group) -> bool {
    return group >= 0 && static_cast<std::size_t>(group) < rows.group_titles.size() &&
           !rows.group_titles[static_cast<std::size_t>(group)].empty();
}

auto right_aligned(ColumnKind k) -> bool {
    return k == ColumnKind::Number || k == ColumnKind::Date;
}

// The order (sorted, groups kept together), each row's top, each column's largest number.
void rebuild(TableState& s, const DataRows& rows, std::span<const Column> columns, float row_h) {
    const std::size_t n = rows.rows();
    s.order.resize(n);
    std::iota(s.order.begin(), s.order.end(), 0u);
    if (!s.filter.empty()) {
        std::erase_if(s.order, [&](std::uint32_t r) {
            for (std::size_t c = 0; c < rows.columns; ++c) {
                if (contains(rows.at(r, c).text, s.filter)) {
                    return false;
                }
            }
            return true;
        });
    }
    const bool grouped = rows.group.size() == n && n > 0;
    if (s.sort_column >= 0 && static_cast<std::size_t>(s.sort_column) < std::min(columns.size(), rows.columns)) {
        const auto c = static_cast<std::size_t>(s.sort_column);
        const bool text = columns[c].kind == ColumnKind::Text;
        const bool down = s.descending;
        std::stable_sort(s.order.begin(), s.order.end(), [&](std::uint32_t a, std::uint32_t b) {
            if (grouped && rows.group[a] != rows.group[b]) {
                return rows.group[a] < rows.group[b];
            }
            if (text) {
                const auto& x = rows.at(a, c).text;
                const auto& y = rows.at(b, c).text;
                return down ? y < x : x < y;
            }
            const double x = rows.at(a, c).number;
            const double y = rows.at(b, c).number;
            return down ? x > y : x < y;
        });
    }
    s.tops.resize(s.order.size());
    float y = 0;
    int group = INT_MIN;
    for (std::size_t i = 0; i < s.order.size(); ++i) {
        if (grouped && rows.group[s.order[i]] != group) {
            group = rows.group[s.order[i]];
            y += titled(rows, group) ? kDivider : 0.0f;
        }
        s.tops[i] = y;
        y += row_h;
    }
    s.most.assign(columns.size(), 0.0);
    for (std::size_t c = 0; c < columns.size() && c < rows.columns; ++c) {
        for (std::size_t r = 0; r < n; ++r) {
            s.most[c] = std::max(s.most[c], rows.at(r, c).number);
        }
    }
    s.built_rows = n;
    s.built_column = s.sort_column;
    s.built_descending = s.descending;
    s.built_filter = s.filter;
}

} // namespace

void skeleton_bar(Ui& ui, const Box& bar, float opacity, bool lit) {
    auto& r = ui.renderer();
    const float radius = std::min(5.0f, bar.h / 2);
    const float was = r.opacity();
    r.set_opacity(was * opacity);
    r.rect(rect(bar), {.fill = ui::th().w1, .radius = all(radius)});
    if (lit) {
        const float light = 60.0f;
        const auto t = static_cast<float>(std::fmod(ui.now() / kSweepSeconds, 1.0));
        r.push_clip(rect(bar));
        r.rect({bar.x - light + (bar.w + light) * t, bar.y, light, bar.h}, {.fill = ui::th().w3, .radius = all(radius)});
        r.pop_clip();
        ui.request_frame();
    }
    r.set_opacity(was);
}

void table_text(const void* state, std::string& out) {
    const auto& s = *static_cast<const TableState*>(state);
    out.clear();
    if (s.copy_rows == nullptr) {
        return;
    }
    const auto copied = [](const Column& c) {
        return c.kind != ColumnKind::Icon && c.kind != ColumnKind::Action && c.kind != ColumnKind::Spark;
    };
    for (const auto row : s.order) {
        if (!out.empty()) {
            out += '\n';
        }
        bool first = true;
        for (std::size_t c = 0; c < s.copy_columns.size(); ++c) {
            if (!copied(s.copy_columns[c])) {
                continue;
            }
            if (!first) {
                out += '\t';
            }
            out += s.copy_rows->at(row, c).text;
            first = false;
        }
    }
}

void table_leave(TableState& s, const DataRows& rows, std::size_t row) {
    const auto place = std::ranges::find(s.order, static_cast<std::uint32_t>(row));
    if (place == s.order.end() || row >= rows.rows()) {
        return; // not shown: nothing to see going
    }
    const auto i = static_cast<std::size_t>(place - s.order.begin());
    s.left_cells.assign(rows.cells.begin() + static_cast<std::ptrdiff_t>(row * rows.columns),
                        rows.cells.begin() + static_cast<std::ptrdiff_t>((row + 1) * rows.columns));
    s.left_top = i < s.tops.size() ? s.tops[i] : 0.0f;
    s.left_place = i;
    s.leaving = true;
    s.leave_pending = true;
    // The rows after it keep their own times and places: their numbers move down by one.
    if (row < s.came.size()) {
        s.came.erase(s.came.begin() + static_cast<std::ptrdiff_t>(row));
    }
    if (row < s.from_top.size()) {
        s.from_top.erase(s.from_top.begin() + static_cast<std::ptrdiff_t>(row));
    }
}

auto table(Ui& ui, Id id, const Box& box, std::span<const Column> columns, const DataRows& rows, TableState& s,
           bool complete, bool openable) -> TableEvent {
    TableEvent e;
    auto& r = ui.renderer();
    const float scale = ui.scale();
    const float row_h = rows.dense ? kDenseRow : kRow;
    const std::size_t ncols = std::min({columns.size(), rows.columns, kMaxColumns});

    // The columns: fixed widths, the rest shared by the growing ones.
    std::array<float, kMaxColumns> cx{};
    std::array<float, kMaxColumns> cw{};
    float fixed = 0;
    int grows = 0;
    for (std::size_t c = 0; c < ncols; ++c) {
        fixed += columns[c].grow ? 0.0f : columns[c].width;
        grows += columns[c].grow ? 1 : 0;
    }
    const float inner = box.w - 2 * kPad - kGap * static_cast<float>(ncols > 0 ? ncols - 1 : 0) - (openable ? kMore : 0.0f);
    const float share = grows > 0 ? std::max(0.0f, inner - fixed) / static_cast<float>(grows) : 0.0f;
    float x = box.x + kPad;
    for (std::size_t c = 0; c < ncols; ++c) {
        cx[c] = x;
        cw[c] = columns[c].grow ? share : columns[c].width;
        x += cw[c] + kGap;
    }

    // The header sorts (once all the rows are in): again on the same column turns it round.
    for (std::size_t c = 0; c < ncols; ++c) {
        if (!columns[c].sortable) {
            continue;
        }
        const auto hit = ui.interact(id * 1099511628211ull + 1 + c, {cx[c], box.y, cw[c], kHeader}, false);
        if (hit.clicked && complete) {
            if (s.sort_column == static_cast<int>(c)) {
                s.descending = !s.descending;
            } else {
                s.sort_column = static_cast<int>(c);
                s.descending = true; // the largest, the newest first
            }
            e.sorted = true;
        }
    }
    // MS 08. Rows that came since the last frame: kStagger apart, kStaggerMaxItems at most (the rest
    // with the last; rows out of view cost nothing — only drawn rows have tracks). Fewer rows: other data.
    const double now = ui.now();
    if (rows.rows() < s.came.size()) {
        s.came.resize(rows.rows()); // fewer rows: those that stay do not come in again
    }
    // A row that left (table_leave): it goes on micro where it stood; then its room closes on smooth.
    float left_alpha = 0.0f, left_room = 0.0f;
    if (s.leaving) {
        auto& anim = ui.animator();
        const Id lid = id * 1099511628211ull + 0x3FFE0;
        if (s.leave_pending) {
            s.leave_pending = false;
            s.left_at = now;
            anim.place(lid, 1.0f);
            anim.place(lid + 1, 1.0f);
        }
        const bool holds = now - s.left_at < kLeaveCloseAfterSeconds;
        left_alpha = anim.to(lid, 0.0f, motion::kMicro);
        left_room = anim.geometry(lid + 1, holds ? 1.0f : 0.0f, motion::kSmooth);
        if (holds) {
            ui.request_frame();
        } else if (left_room <= 0.002f) {
            s.leaving = false;
            s.left_cells.clear();
            left_room = 0.0f;
        }
    }
    if (rows.rows() > s.came.size()) {
        std::size_t k = 0;
        const std::size_t from = s.came.size();
        s.came.resize(rows.rows());
        for (std::size_t i = from; i < s.came.size(); ++i, ++k) {
            s.came[i] = now + kStaggerSeconds * static_cast<double>(std::min(k, motion::kStaggerMaxItems));
        }
    }
    if (s.built_rows != rows.rows() || s.built_column != s.sort_column || s.built_descending != s.descending ||
        s.built_filter != s.filter) {
        // Sorted again (the same rows, another order): each row goes from where it is drawn now to
        // its new place (FLIP). More rows or another filter: the places as they are.
        const bool resorted = s.built_rows == rows.rows() && s.built_filter == s.filter && s.built_column != -2;
        // Another filter over the same rows (the map's unnamed): a row that stays goes to its new
        // place the same way; one that was filtered out comes in as a new row does.
        const bool refiltered = s.built_rows == rows.rows() && s.built_filter != s.filter && s.built_column != -2;
        if (resorted || refiltered) {
            s.from_top.resize(rows.rows(), 0.0f);
            const float left = 1.0f - s.flip.x; // mid-way: from where it is
            if (refiltered) {
                std::ranges::fill(s.was_shown, std::uint8_t{0});
                s.was_shown.resize(rows.rows(), 0);
            }
            for (std::size_t i = 0; i < s.order.size() && i < s.tops.size(); ++i) {
                const std::uint32_t row = s.order[i];
                s.from_top[row] = s.tops[i] + (s.from_top[row] - s.tops[i]) * left;
                if (refiltered) {
                    s.was_shown[row] = 1;
                }
            }
        }
        rebuild(s, rows, columns, row_h);
        if (refiltered) {
            for (std::size_t i = 0; i < s.order.size() && i < s.tops.size(); ++i) {
                const std::uint32_t row = s.order[i];
                if (s.was_shown[row] == 0) { // it was not there: in its place at once, coming in on quick
                    s.from_top[row] = s.tops[i];
                    if (row < s.came.size()) {
                        s.came[row] = now;
                    }
                }
            }
        }
        if ((resorted || refiltered) && !ui.animator().reduced()) { // effects off: in their new places at once
            s.flip.snap(0.0f);
            s.flip.set(1.0f, motion::kSmooth);
        } else {
            s.flip.snap(1.0f);
        }
    }
    if (s.flip_at >= 0 && now > s.flip_at) {
        s.flip.step(static_cast<float>(now - s.flip_at));
    }
    s.flip_at = now;
    const bool flipping = s.flip.x != s.flip.target || s.flip.v != 0.0f;
    if (flipping) {
        ui.request_frame();
    }
    s.waited = s.waited || !complete;

    r.rect(rect(box), {.fill = {}, .radius = all(12), .border = ui::th().line, .border_width = 1});

    // The rows in view, under the header and over the total.
    const float total_h = rows.total.empty() ? 0.0f : kTotal;
    const Box body{box.x, box.y + kHeader, box.w, std::max(0.0f, box.h - kHeader - total_h)};
    const float content_h = (s.tops.empty() ? 0.0f : s.tops.back() + row_h) + (s.leaving ? row_h * left_room : 0.0f); // and a left row's closing room
    const Box content = scroll_view(ui, s.scroll, body, content_h);
    const float top = s.scroll.offset;
    s.first = static_cast<std::size_t>(std::upper_bound(s.tops.begin(), s.tops.end(), top) - s.tops.begin());
    s.first = s.first > 0 ? s.first - 1 : 0;
    s.end = static_cast<std::size_t>(std::lower_bound(s.tops.begin(), s.tops.end(), top + body.h) - s.tops.begin());
    const auto& in = ui.input();
    s.hover = -1;
    const bool mouse_in = in.mouse_x >= body.x && in.mouse_x < body.x + body.w && in.mouse_y >= body.y &&
                          in.mouse_y < body.y + body.h;
    for (std::size_t i = s.first; i < s.end; ++i) {
        const std::uint32_t row = s.order[i];
        // Coming in (MS 08): nothing before its turn; young, its opacity on quick and kRowShift px
        // down to its place on base, its bar on smooth kBarAt later; older, as it is.
        const double age = row < s.came.size() ? now - s.came[row] : 1.0e9;
        if (age < 0) {
            ui.request_frame(); // its turn comes on frames
            if (s.waited) {
                skeleton_row(ui, {box.x, content.y + s.tops[i], box.w, row_h}, cx[0], inner, row, 1.0f, !complete);
            }
            continue;
        }
        float came = 1.0f, shift = 0.0f, grown = 1.0f;
        if (age < kRowYoungSeconds) {
            auto& anim = ui.animator();
            const Id rid = id * 1099511628211ull + 0x40000 + static_cast<Id>(row) * 4;
            came = anim.to(rid, 1.0f, motion::kQuick, 0.0f);
            shift = anim.geometry(rid + 1, 0.0f, motion::kBase, motion::table::kRowShift);
            grown = age >= kBarAfterSeconds ? anim.geometry(rid + 2, 1.0f, motion::kSmooth, 0.0f) : 0.0f;
            if (age < kBarAfterSeconds) {
                ui.request_frame();
            }
        }
        // FLIP: from its old place (a view's height away at most) to its new one.
        float moved = 0.0f;
        if (flipping && row < s.from_top.size()) {
            moved = std::clamp(s.from_top[row] - s.tops[i], -body.h, body.h) * (1.0f - s.flip.x);
        }
        // Under a row that left: down by what is left of its room.
        const float held = s.leaving && i >= s.left_place ? row_h * left_room : 0.0f;
        const float y = content.y + s.tops[i] + shift + moved + held;
        if (came < 0.996f && s.waited) { // the skeleton it came over, going as it comes
            skeleton_row(ui, {box.x, content.y + s.tops[i], box.w, row_h}, cx[0], inner, row, 1.0f - came, !complete);
        }
        const float row_opacity = r.opacity();
        r.set_opacity(row_opacity * came);
        const bool new_group = rows.group.size() == rows.rows() && (i == 0 || rows.group[s.order[i - 1]] != rows.group[row]);
        if (new_group && titled(rows, rows.group[row])) {
            const Box divider{box.x + 1, y - kDivider, box.w - 2, kDivider};
            r.rect(rect(divider), {.fill = ui::th().w1});
            const auto g = static_cast<std::size_t>(rows.group[row]);
            if (g < rows.group_titles.size()) {
                caption(ui, {box.x + kPad, divider.y, box.w - 2 * kPad, kDivider}, Face::Text, 11.5f, rows.group_titles[g], ui::th().text_4);
            }
        }
        const bool hovered = mouse_in && in.mouse_y >= y && in.mouse_y < y + row_h;
        // .055 and a 2 px accent bar at its left (screen 26). MS 14: the light on quick, the bar from
        // 0 to its 2 px on base; they go the same way, and what shows only under the mouse with them.
        const Id light_id = id * 1099511628211ull + 0x50000 + row;
        const float lit = std::clamp(ui.hover(light_id, hovered), 0.0f, 1.0f);
        const float bar = ui.animator().geometry(light_id ^ 0x426172ull, hovered ? motion::micro::kAccentBar : 0.0f, motion::kBase, 0.0f);
        if (hovered) {
            s.hover = static_cast<int>(row);
        }
        if (lit > 0.004f) {
            r.rect({box.x + 1, y, box.w - 2, row_h}, {.fill = faded(ui::th().w1, lit)});
        }
        if (bar > 0.02f) {
            r.rect({box.x + 1, y, bar, row_h}, {.fill = ui::th().accent});
        }
        r.rect({box.x + 1, y, box.w - 2, 1}, {.fill = ui::th().line});
        const float opacity = r.opacity();
        if (row < rows.faded.size() && rows.faded[row] != 0) {
            r.set_opacity(opacity * 0.45f);
        }
        bool first_text = true;
        for (std::size_t c = 0; c < ncols; ++c) {
            const auto& cell = rows.at(row, c);
            const Box b{cx[c], y, cw[c], row_h};
            ui.set_cell(static_cast<int>(row), static_cast<int>(c)); // a selection copies a row's cells with tabs
            // Its keys by its place in the shown order, whichever rows above are drawn (final review C1).
            ui.order_base(((static_cast<std::uint64_t>(i) + 1) << 12) | (static_cast<std::uint64_t>(c) << 6));
            switch (columns[c].kind) {
            case ColumnKind::Text:
                if (first_text && row < rows.notes.size() && !rows.notes[row].empty()) {
                    // Its name, and a dim line under it (phase 13a, .pnn: 13.5 and 11.5 at 34 %).
                    caption(ui, {b.x, y + 4, b.w, row_h / 2 - 2}, Face::Text, 13.5f, cell.text, ui::th().text);
                    caption(ui, {b.x, y + row_h / 2, b.w, row_h / 2 - 4}, Face::Text, 11.5f, rows.notes[row], ui::th().text_4);
                } else {
                    caption(ui, b, Face::Text, 13.5f, cell.text, first_text ? ui::th().text : ui::th().text_2);
                }
                first_text = false;
                break;
            case ColumnKind::Spark:
                if (row < rows.series.size() && !rows.series[row].empty()) {
                    const Box spark{b.x, y + (row_h - 22) / 2, std::min(108.0f, b.w), 22};
                    sparkline(ui, spark, rows.series[row], static_cast<int>(row) == rows.hot_row ? ui::th().warn : ui::th().accent);
                }
                break;
            case ColumnKind::Mono: caption(ui, b, Face::Mono, 12.5f, cell.text, ui::th().text_3); break;
            case ColumnKind::Number:
            case ColumnKind::Date: {
                const float size = columns[c].kind == ColumnKind::Date ? 13.0f : 13.5f;
                const auto& shaped = ui.shape(Face::Text, cell.text, size, true);
                const float w = std::min(shaped.width / scale, b.w);
                ui.draw_text(Face::Text, shaped, size, {b.x + b.w - w, ui.centred_baseline(Face::Text, size, y, row_h)},
                             ui::th().text_2);
                break;
            }
            case ColumnKind::Size:
            case ColumnKind::Percent: {
                const float vw = std::min(kValue, b.w);
                const auto& shaped = ui.shape(Face::Text, cell.text, 13.0f, true);
                const float w = std::min(shaped.width / scale, vw);
                ui.draw_text(Face::Text, shaped, 13.0f, {b.x + vw - w, ui.centred_baseline(Face::Text, 13.0f, y, row_h)},
                             ui::th().text_2);
                const Box track{b.x + vw + kBarGap, y + row_h / 2 - 3, std::max(0.0f, b.w - vw - kBarGap), 6};
                if (track.w > 0 && !cell.text.empty()) { // a folder: no size, no bar
                    r.rect(rect(track), {.fill = ui::th().w2, .radius = all(3)});
                    const double most = columns[c].kind == ColumnKind::Size ? s.most[c] : 100.0;
                    const float share_of = most > 0 ? static_cast<float>(std::clamp(cell.number / most, 0.0, 1.0)) : 0.0f;
                    const float fill = track.w * std::max(share_of, 0.015f) * grown; // the smallest still shows; growing in (MS 08)
                    r.rect({track.x, track.y, fill, track.h}, {.fill = ui::th().w4, .radius = all(std::min(3.0f, fill / 2))});
                }
                break;
            }
            case ColumnKind::Icon: type_icon(ui, b, cell.text); break;
            case ColumnKind::Action:
                if (row < rows.badges.size() && !rows.badges[row].empty()) {
                    // A badge instead of the button, without the mouse (phase 13a, .ro): nothing to press.
                    const float bw = std::min(b.w, text_width(ui, Face::Text, 11.5f, rows.badges[row]) + 8 + 11 + 5 + 9);
                    const Box badge{b.x + b.w - bw, y + (row_h - 20) / 2, bw, 20};
                    r.rect(rect(badge), {.fill = ui::th().w2, .radius = all(10), .border = ui::th().line, .border_width = 1});
                    ui.draw_icon(kIconLock, 11.0f, {badge.x + 8 + 5.5f, badge.y + 10}, ui::th().text_3);
                    caption(ui, {badge.x + 8 + 11 + 5, badge.y, badge.w - 8 - 11 - 5 - 9, 20}, Face::Text, 11.5f, rows.badges[row], ui::th().text_3);
                } else if ((hovered || lit > 0.01f) && !cell.text.empty()) {
                    // Its button with the row's light (MS 14): pressed only while the mouse is on the row.
                    const auto size = button_size(ui, cell.text, true);
                    const Box button_box{b.x + b.w - size.w, y + (row_h - size.h) / 2, size.w, size.h};
                    const float before = r.opacity();
                    const bool was_inert = ui.inert();
                    r.set_opacity(before * lit);
                    ui.set_inert(was_inert || !hovered);
                    if (button(ui, id * 1099511628211ull + 0x10000 + row, button_box, cell.text, ButtonStyle::Normal, true)) {
                        e.action_row = static_cast<int>(row);
                    }
                    ui.set_inert(was_inert);
                    r.set_opacity(before);
                }
                break;
            }
        }
        ui.set_cell(-1, -1);
        r.set_opacity(opacity);
        if (openable) {
            // Its "..." first (it takes its click before the row), then the row itself.
            const Box row_box{box.x + 1, y, box.w - 2, row_h};
            if (hovered || lit > 0.01f) {
                const Box more{box.x + box.w - kPad - 22, y + (row_h - 22) / 2, 22, 22};
                const Id more_id = id * 1099511628211ull + 0x30000 + row;
                const auto mh = hovered ? ui.interact(more_id, more, false) : Interaction{};
                const float mlit = std::clamp(ui.hover(more_id, mh.hovered), 0.0f, 1.0f); // its own light, on quick
                const float before = r.opacity();
                r.set_opacity(before * lit);
                if (mlit > 0.004f) {
                    r.rect(rect(more), {.fill = faded(ui::th().w3, mlit), .radius = all(6)});
                }
                ui.draw_icon(kIconMore, 14.0f, {more.x + 11, more.y + 11}, mlit > 0.5f ? ui::th().text : ui::th().text_2);
                r.set_opacity(before);
                if (mh.clicked) {
                    e.more_row = static_cast<int>(row);
                }
            }
            const Id row_id = id * 1099511628211ull + 0x20000 + row;
            const auto hit = ui.interact(row_id, row_box, false);
            if (const float p = ui.press(row_id, hit.held); p > 0.01f) { // pressed: rgba(174,185,255,.1)
                r.rect(rect(row_box), {.fill = faded(ui::th().accent_soft, p)});
            }
            if (hit.clicked) { // a click on its "..." was the "..."'s: taken first, the row never had it
                e.open_row = static_cast<int>(row);
            }
        }
        r.set_opacity(row_opacity);
    }
    if (s.leaving && left_alpha > 0.004f && s.left_cells.size() >= ncols) {
        // The row that left, as it was (its words; a bar or a button of it is not kept): going.
        const float y = content.y + s.left_top;
        const float was = r.opacity();
        r.set_opacity(was * left_alpha);
        r.rect({box.x + 1, y, box.w - 2, 1}, {.fill = ui::th().line});
        bool first_text = true;
        for (std::size_t c = 0; c < ncols; ++c) {
            const auto& cell = s.left_cells[c];
            const Box b{cx[c], y, cw[c], row_h};
            switch (columns[c].kind) {
            case ColumnKind::Text:
                label(ui, b, Face::Text, 13.5f, cell.text, first_text ? ui::th().text : ui::th().text_2);
                first_text = false;
                break;
            case ColumnKind::Mono: label(ui, b, Face::Mono, 12.5f, cell.text, ui::th().text_3); break;
            case ColumnKind::Number:
            case ColumnKind::Date:
            case ColumnKind::Size:
            case ColumnKind::Percent: {
                const auto& shaped = ui.shape(Face::Text, cell.text, 13.0f, true);
                const float room = columns[c].kind == ColumnKind::Size || columns[c].kind == ColumnKind::Percent ? std::min(kValue, b.w) : b.w;
                const float w = std::min(shaped.width / scale, room);
                ui.draw_text(Face::Text, shaped, 13.0f, {b.x + room - w, ui.centred_baseline(Face::Text, 13.0f, y, row_h)}, ui::th().text_2);
                break;
            }
            case ColumnKind::Icon: type_icon(ui, b, cell.text); break;
            case ColumnKind::Spark:
            case ColumnKind::Action: break;
            }
        }
        r.set_opacity(was);
    }
    // While the data still comes: skeleton rows where it will be, their light running; the data
    // whole, they go on quick (MS 08).
    if (const float waiting = ui.animator().to(id * 1099511628211ull + 0x3FFF0, complete ? 0.0f : 1.0f, motion::kQuick); waiting > 0.004f) {
        const float from = content.y + content_h;
        for (std::size_t k = 0; k < kSkeletonRows; ++k) {
            const float y = from + static_cast<float>(k) * row_h;
            if (y >= body.y + body.h) {
                break;
            }
            skeleton_row(ui, {box.x, y, box.w, row_h}, cx[0], inner, static_cast<std::uint32_t>(s.order.size() + k), waiting, !complete);
        }
    }
    end_scroll(ui, s.scroll, body, surface(ui, ui::th().panel)); // a table is the data panel's: its fade goes into the panel
    // Ctrl+A over it: all its rows, as shown (sorted, filtered), not only those in view.
    s.copy_rows = &rows;
    s.copy_columns = columns.subspan(0, ncols);
    ui.pane_text(ui.pane(), &table_text, &s);

    // The header over it all: its titles, the sorted one bright with its arrow. Its keys before the rows'.
    ui.order_base(1u << 10);
    r.rect({box.x + 1, box.y + 1, box.w - 2, kHeader - 1}, {.fill = ui::th().w1, .radius = {11, 11, 0, 0}});
    for (std::size_t c = 0; c < ncols; ++c) {
        const bool sorted = s.sort_column == static_cast<int>(c);
        const Color tc = sorted ? ui::th().text : ui::th().text_3;
        const auto arrow = s.descending ? kDown : kUp;
        const float aw = sorted ? text_width(ui, Face::Text, 12.0f, arrow) + 4 : 0.0f;
        // Its type tag (phase 10): 9.5 bold, the accent on its tint, 6 after the title.
        const float gw = columns[c].tag.empty() ? 0.0f : text_width(ui, Face::Semibold, 9.5f, columns[c].tag) + 10;
        const float tagged = gw > 0 ? gw + 6 : 0.0f;
        const float tw = std::min(text_width(ui, Face::Text, 12.0f, columns[c].title), std::max(0.0f, cw[c] - aw - tagged));
        const float tx = right_aligned(columns[c].kind) ? cx[c] + cw[c] - tw - aw - tagged : cx[c];
        caption(ui, {tx, box.y, tw + 1, kHeader}, Face::Text, 12.0f, columns[c].title, tc);
        if (gw > 0) {
            const float gh = ui.line_height(Face::Semibold, 9.5f) + 2;
            const Box tag{tx + tw + 6, box.y + (kHeader - gh) / 2, gw, gh};
            r.rect(rect(tag), {.fill = {174, 185, 255, 31}, .radius = all(4)});
            caption(ui, {tag.x + 5, tag.y, gw - 5, gh}, Face::Semibold, 9.5f, columns[c].tag, ui::th().accent);
        }
        if (sorted) { // its arrow comes on quick (MS 08)
            const float o = r.opacity();
            r.set_opacity(o * ui.animator().to(id * 1099511628211ull + 0x3FF00 + c, 1.0f, motion::kQuick, 0.0f));
            caption(ui, {tx + tw + tagged + 4, box.y, aw, kHeader}, Face::Text, 12.0f, arrow, tc);
            r.set_opacity(o);
        }
    }

    if (!rows.total.empty()) {
        const Box total{box.x, box.y + box.h - kTotal, box.w, kTotal};
        r.rect({total.x + 1, total.y, total.w - 2, 1}, {.fill = ui::th().line});
        caption(ui, {total.x + kPad, total.y, total.w - 2 * kPad, kTotal}, Face::Text, 12.5f, rows.total, ui::th().text_3);
    }
    return e;
}

} // namespace ui::work
