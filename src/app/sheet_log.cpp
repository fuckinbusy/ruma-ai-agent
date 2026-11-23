// The change log of design/interaction-topbar (5_change_log): every task the agent ran, newest
// first, as restore points on the left — its dot by kind, what it did, when; on the right the
// picked task's changes as a diff (+ created, − deleted, → moved, ✎ renamed) under the folder
// they share, each line with its Undo, and "Undo whole task". The row searches; Today, Week,
// All narrow it. Under it: "Undo last · Ctrl+Z".

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

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto right(const Box& b) -> float {
    return b.x + b.w;
}

auto id_of(std::string_view name) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet"));
}

auto id_of(std::string_view name, std::size_t n) -> ui::Id {
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
    return work::Kind::Read;
}

// The op's sign and its colour (.legend).
auto sign_of(events::LogOp op) -> std::string_view {
    switch (op) {
    case events::LogOp::Created: return "+";
    case events::LogOp::Deleted: return "\xE2\x88\x92";
    case events::LogOp::Moved: return "\xE2\x86\x92";
    case events::LogOp::Renamed: return "\xE2\x9C\x8E";
    case events::LogOp::Context: break;
    }
    return "";
}

auto color_of(events::LogOp op) -> Color {
    switch (op) {
    case events::LogOp::Created: return ui::th().ok;
    case events::LogOp::Deleted: return ui::th().crit;
    case events::LogOp::Moved: return ui::th().text_3;
    case events::LogOp::Renamed: return ui::th().warn;
    case events::LogOp::Context: break;
    }
    return ui::th().text_3;
}

constexpr char32_t kIconFolder = 0xE8D5; // FolderFill
constexpr float kList = 300.0f;      // .cols 300px 1fr
constexpr float kItem = 56.0f;       // .ti: padding 10, a 13.5 px title over a 12 px line
constexpr float kItemGap = 4.0f;     // .tl gap
constexpr float kRow = 32.0f;        // .tr
constexpr float kFoot = 58.0f;       // .foot
constexpr float kUndo = 26.0f;       // .tr .undo

// A text button with the undo arrow before its label; returns a click.
constexpr float kArrow = 13.0f;   // the arrow, 6 px before the label
constexpr float kArrowGap = 6.0f;

auto undo_button(ui::Ui& ui, ui::Id id, const Box& box, std::string_view label, bool enabled) -> bool {
    const bool clicked = work::button(ui, id, box, "", work::ButtonStyle::Normal, false, enabled);
    const Color c = enabled ? ui::th().text : ui::th().text_3;
    const float w = kArrow + kArrowGap + work::text_width(ui, Face::Semibold, wt::type::button, label);
    const float x = box.x + (box.w - w) / 2;
    ui.draw_icon(work::kIconUndo, kArrow, {x + kArrow / 2, box.y + box.h / 2}, c);
    (void)work::caption(ui, {x + kArrow + kArrowGap, box.y, box.w, box.h}, Face::Semibold, wt::type::button, label, c);
    return clicked;
}

auto undo_button_size(ui::Ui& ui, std::string_view label) -> ui::layout::Size {
    auto s = work::button_size(ui, label);
    s.w += kArrow + kArrowGap;
    return s;
}

} // namespace

void Painter::draw_log(Model& m, const Box& panel, Reaction& r) {
    if (!m.log) {
        return;
    }
    // The row: the search, then Today / Week / All and the close button.
    static constexpr std::array<std::string_view, 3> kRanges{"Today", "Week", "All"};
    const auto seg = work::segmented_size(*ui_, kRanges);
    const float ib = wt::bar::icon_button;
    draw_row(m, panel, r, "Search the log\xE2\x80\xA6", wt::type::placeholder, {}, RowButton::None, false, false,
             seg.w + 10 + ib);
    const float cy = panel.y + row_h_ - wt::bar::height / 2; // by the field's last line
    const Box close{right(panel) - wt::bar::pad_right - ib, cy - ib / 2, ib, ib};
    if (work::icon_button(*ui_, id_of("log:close"), close, work::kIconClose)) {
        close_log(m);
        return;
    }
    add_target("close", close);
    const Box seg_box{close.x - 10 - seg.w, cy - seg.h / 2, seg.w, seg.h};
    const int picked = work::segmented(*ui_, id_of("log:range"), seg_box, kRanges, static_cast<int>(m.log_range));
    {
        std::array<char, 64> buf{};
        float x = seg_box.x + wt::segment::pad;
        for (std::size_t i = 0; i < kRanges.size(); ++i) { // the items as segmented() lays them out
            const float w = 2 * wt::segment::item_pad_x + work::text_width(*ui_, Face::Text, wt::type::segment, kRanges[i]);
            add_target(text(buf, "log:range:{}", i), {x, seg_box.y + wt::segment::pad, w, seg_box.h - 2 * wt::segment::pad});
            x += w + 2.0f;
        }
    }
    m.log_range = static_cast<LogRange>(picked);

    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_ + wt::body::pad_top, panel.w - 2 * wt::body::pad_x,
                   std::max(0.0f, panel.h - row_h_ - wt::body::pad_top - kFoot - 12)};
    auto& renderer = ui_->renderer();
    const auto& in = ui_->input();
    const auto wheel_over = [&](const Box& b) {
        return in.wheel != 0 && in.mouse_x >= b.x && in.mouse_x < b.x + b.w && in.mouse_y >= b.y && in.mouse_y < b.y + b.h;
    };
    std::array<char, 64> buf{};

    // The tasks (.tl).
    const Box list{body.x, body.y, std::min(kList, body.w), body.h};
    const auto* selected = log_selected(m);
    std::size_t count = 0;
    for (const auto& t : m.log->tasks) {
        count += log_visible(m, t) ? 1 : 0;
    }
    if (wheel_over(list)) {
        log_scroll_ -= in.wheel * kItem;
    }
    log_scroll_ = std::clamp(log_scroll_, 0.0f, std::max(0.0f, static_cast<float>(count) * (kItem + kItemGap) - list.h));
    renderer.push_clip({list.x, list.y, list.w, list.h});
    std::size_t n = 0;
    for (const auto& t : m.log->tasks) {
        if (!log_visible(m, t)) {
            continue;
        }
        const Box item{list.x, list.y - log_scroll_ + static_cast<float>(n) * (kItem + kItemGap), list.w, kItem};
        const bool on = &t == selected;
        const auto hit = ui_->interact(id_of("log:task", t.task), item);
        if (hit.clicked) {
            m.log_task = t.task;
        }
        // The chosen one's ground comes and goes on quick (the map's unnamed: the log's pick).
        const float chosen_now = work::chosen(*ui_, id_of("log:task", t.task), on);
        if (chosen_now > 0.004f) {
            renderer.rect({item.x, item.y, item.w, item.h},
                          {.fill = faint(ui::th().w2, chosen_now), .radius = {10, 10, 10, 10},
                           .border = faint(ui::th().line, chosen_now), .border_width = 1});
        }
        if (const float light = lit(id_of("log:task", t.task), hit.hovered && !on); light > 0.004f) {
            renderer.rect({item.x, item.y, item.w, item.h}, {.fill = faint(ui::th().w1, light), .radius = {10, 10, 10, 10}});
        }
        // .dot: a ring in the task's colour; grey when nothing changed.
        const Color ring = t.changes == 0 ? ui::th().text_4 : work::kind_color(kind_of(t.kind));
        renderer.rect({item.x + 12, item.y + item.h / 2 - 5.5f, 11, 11},
                      {.fill = {}, .radius = {5.5f, 5.5f, 5.5f, 5.5f}, .border = ring, .border_width = 2});
        const float time_w = work::text_width(*ui_, Face::Text, 12.0f, t.when);
        const float tx = item.x + 12 + 11 + 12;
        const float tw = std::max(0.0f, item.x + item.w - 12 - time_w - 10 - tx);
        const float th = ui_->line_height(Face::Semibold, 13.5f);
        const float sh = ui_->line_height(Face::Text, 12.0f);
        const float top = item.y + (item.h - th - sh) / 2;
        work::caption(*ui_, {tx, top, tw, th}, t.changes == 0 ? Face::Text : Face::Semibold, 13.5f, t.title,
                    t.changes == 0 ? ui::th().text_2 : ui::th().text);
        work::caption(*ui_, {tx, top + th, tw, sh}, Face::Text, 12.0f, t.summary, ui::th().text_3);
        work::caption(*ui_, {item.x + item.w - 12 - time_w, item.y, time_w, item.h}, Face::Text, 12.0f, t.when, ui::th().text_3);
        add_target(text(buf, "log:task:{}", n), item);
        ++n;
    }
    renderer.pop_clip();
    if (count == 0) {
        work::caption(*ui_, {list.x + 12, list.y + 10, list.w - 24, 20}, Face::Text, 13.0f,
                    m.input.text.empty() ? "Nothing in this range" : "Nothing found", ui::th().text_3);
    }

    // The picked task (.dh, .legend, .tree).
    const Box right_col{list.x + list.w + wt::body::column_gap, body.y,
                        std::max(0.0f, body.w - list.w - wt::body::column_gap), body.h};
    if (selected != nullptr && right_col.w > 0) {
        const auto& t = *selected;
        if (tree_task_ != t.task) {
            tree_task_ = t.task;
            tree_scroll_ = 0;
        }
        float y = right_col.y;
        const auto whole = undo_button_size(*ui_, "Undo whole task");
        const float title_w = std::max(0.0f, right_col.w - whole.w - 10 - 120);
        const float tw = work::caption(*ui_, {right_col.x, y, title_w, whole.h}, Face::Display, 16.0f, t.title, ui::th().text);
        const auto tag = work::tag_size(*ui_, t.summary);
        work::tag(*ui_, {right_col.x + tw + 10, y + (whole.h - tag.h) / 2, tag.w, tag.h}, t.summary, kind_of(t.kind));
        if (t.undoable) {
            const Box b{right_col.x + right_col.w - whole.w, y, whole.w, whole.h};
            if (undo_button(*ui_, id_of("log:undo-task"), b, "Undo whole task", true)) {
                r.command = undo(m, t.task, std::nullopt);
            }
            add_target("log:undo-task", b);
        }
        y += whole.h + 12;
        // .legend
        float lx = right_col.x + 2;
        for (const auto [op, word] : {std::pair{events::LogOp::Created, std::string_view{"created"}},
                                      std::pair{events::LogOp::Deleted, std::string_view{"deleted"}},
                                      std::pair{events::LogOp::Moved, std::string_view{"moved"}},
                                      std::pair{events::LogOp::Renamed, std::string_view{"renamed"}}}) {
            lx += work::caption(*ui_, {lx, y, 20, 16}, Face::Mono, 13.0f, sign_of(op), color_of(op)) + 5;
            lx += work::caption(*ui_, {lx, y, 120, 16}, Face::Text, 12.0f, word, ui::th().text_3) + 14;
        }
        y += 16 + 12;
        if (r.command) {
            return; // the undo has begun: the view moves on
        }
        if (t.rows.empty()) {
            work::caption(*ui_, {right_col.x + 2, y, right_col.w, 20}, Face::Text, 13.0f,
                        t.changes == 0 ? "This task changed nothing." : "Nothing here can be undone.", ui::th().text_3);
        } else {
            const float avail = right_col.y + right_col.h - y;
            const float content = static_cast<float>(t.rows.size()) * kRow + 12;
            const Box tree{right_col.x, y, right_col.w, std::min(avail, content)};
            if (wheel_over(tree)) {
                tree_scroll_ -= in.wheel * kRow;
            }
            tree_scroll_ = std::clamp(tree_scroll_, 0.0f, std::max(0.0f, content - tree.h));
            renderer.rect({tree.x, tree.y, tree.w, tree.h},
                          {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
            renderer.push_clip({tree.x, tree.y + 1, tree.w, tree.h - 2});
            for (std::size_t i = 0; i < t.rows.size(); ++i) {
                const auto& row = t.rows[i];
                const Box line{tree.x, tree.y + 6 + static_cast<float>(i) * kRow - tree_scroll_, tree.w, kRow};
                if (line.y + line.h < tree.y || line.y > tree.y + tree.h) {
                    continue;
                }
                const bool hover = in.mouse_x >= line.x && in.mouse_x < line.x + line.w && in.mouse_y >= line.y &&
                                   in.mouse_y < line.y + line.h;
                if (hover && row.change) {
                    renderer.rect({line.x + 1, line.y, line.w - 2, line.h}, {.fill = ui::th().w1});
                }
                float x = line.x + 12 + static_cast<float>(row.depth) * 22;
                const float was = renderer.opacity();
                if (row.undone) {
                    renderer.set_opacity(was * 0.45f); // taken back already
                }
                if (row.op != events::LogOp::Context) {
                    (void)work::caption(*ui_, {x, line.y, 16, line.h}, Face::Mono, 13.0f, sign_of(row.op), color_of(row.op));
                    x += 16 + 9;
                }
                if (row.folder) {
                    ui_->draw_icon(kIconFolder, 18.0f, {x + 9, line.y + line.h / 2}, ui::th().f_folder);
                    x += 18 + 9;
                }
                const float undo_x = line.x + line.w - 10 - kUndo;
                const bool context = row.op == events::LogOp::Context;
                x += work::caption(*ui_, {x, line.y, std::max(0.0f, undo_x - 8 - x), line.h}, Face::Text, 13.0f, row.name,
                                 context ? ui::th().text_3 : ui::th().text);
                if (!row.note.empty() || row.undone) {
                    x += 6;
                    (void)work::caption(*ui_, {x, line.y, std::max(0.0f, undo_x - 8 - x), line.h}, Face::Text, 12.0f,
                                      row.undone ? std::string_view{"undone"} : std::string_view{row.note}, ui::th().text_3);
                }
                renderer.set_opacity(was);
                if (row.change) {
                    const Box ub{undo_x, line.y + (line.h - kUndo) / 2, kUndo, kUndo};
                    const auto hit = ui_->interact(id_of("log:undo", *row.change), ub);
                    const float undo_light = lit(id_of("log:undo", *row.change), hover || hit.hovered);
                    if (undo_light > 0.004f) {
                        renderer.rect({ub.x, ub.y, ub.w, ub.h}, {.fill = faint(ui::th().w2, undo_light), .radius = {6, 6, 6, 6}});
                    }
                    ui_->draw_icon(work::kIconUndo, 14.0f, {ub.x + ub.w / 2, ub.y + ub.h / 2},
                                   blend(ui::th().text_3, ui::th().text, undo_light));
                    add_target(text(buf, "log:undo:{}", *row.change), ub);
                    if (hit.clicked && !r.command) {
                        r.command = undo(m, t.task, *row.change);
                    }
                }
            }
            renderer.pop_clip();
        }
    }

    // .foot: a line, the note, "Undo last".
    const float fy = panel.y + panel.h - kFoot;
    renderer.rect({panel.x, fy, panel.w, 1}, {.fill = ui::th().line});
    ui_->draw_icon(work::kIconTrash, 15.0f, {panel.x + 20 + 8, fy + kFoot / 2}, ui::th().text_3);
    work::caption(*ui_, {panel.x + 20 + 16 + 10, fy, 420, kFoot}, Face::Text, 12.5f,
                "Deleted items stay in the Recycle Bin for 30 days", ui::th().text_3);
    const bool any = std::ranges::any_of(m.log->tasks, [](const events::LogTask& t) { return t.undoable; });
    const auto last = undo_button_size(*ui_, "Undo last \xC2\xB7 Ctrl+Z");
    const Box lb{right(panel) - 20 - last.w, fy + (kFoot - last.h) / 2, last.w, last.h};
    if (undo_button(*ui_, id_of("log:undo-last"), lb, "Undo last \xC2\xB7 Ctrl+Z", any) && !r.command) {
        r.command = undo_last(m);
    }
    add_target("log:undo-last", lb);
}

} // namespace app::sheet
