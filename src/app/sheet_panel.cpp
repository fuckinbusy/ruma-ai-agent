// The data panel (phase 8, docs/specs/phase-8-ui-remaster.md § 5.4; mockup-spec § 3): right of
// the conversation, the shown turn's data sets as tabs; the set's header — its title, where it
// came from, a filter, Copy (TSV), CSV (a file to save), Open in Explorer — its metric tiles,
// its table, its footer.

#include "app/sheet_view.hpp"
#include "tools/docs/docs.hpp"

#include "app/monitor.hpp"
#include "app/sizes.hpp"
#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet/panel")) * 1099511628211ull + n;
}

auto inside(const Box& b, float x, float y) -> bool {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// MS 09: the panel's content changing (ui/motion_tokens.hpp, tabs::).
const ui::Id kPanelOutTrack = id_of("panel:out");
const ui::Id kPanelOutShiftTrack = id_of("panel:out-shift");
const ui::Id kPanelInTrack = id_of("panel:in");
const ui::Id kPanelInShiftTrack = id_of("panel:in-shift");
constexpr float kPanelAside = ui::motion::tabs::kAside;
constexpr double kPanelInAfter = std::chrono::duration<double>(ui::motion::tabs::kInAt).count();
constexpr std::uint64_t kActivityKey = 1ull << 62; // a turn's Activity (its task under it), not a set

constexpr float kTwoColumns = 1240.0f; // conversation 470 + 1 + the panel
constexpr float kConversation = 470.0f;
constexpr float kMinConversation = 360.0f;
constexpr float kMinPanel = 480.0f;
constexpr float kPadX = 22.0f;
constexpr float kTool = 32.0f;
constexpr float kFilterW = 170.0f;
constexpr float kFooter = 44.0f;
constexpr float kTile = 118.0f;
constexpr std::size_t kMaxTabs = 64; // past the row's room: under "N more"


constexpr char32_t kIconSearch = 0xE721;
constexpr char32_t kIconCopy = 0xE8C8;
constexpr char32_t kIconSave = 0xE74E;
constexpr char32_t kIconSheet = 0xF0E2; // a grid: the workbook
constexpr char32_t kIconFolder = 0xE838;
constexpr char32_t kIconTerminal = 0xE756;
constexpr char32_t kIconAddRule = 0xE710; // "+" (Segoe Fluent Icons Add)

auto icon_of(events::SetKind k) -> char32_t {
    switch (k) {
    case events::SetKind::Files: return 0xE8B7;     // folder
    case events::SetKind::Matches: return 0xE721;   // search
    case events::SetKind::Processes: return 0xE950; // a component
    case events::SetKind::Terminal: return kIconTerminal;
    case events::SetKind::Document: return 0xE8A5;  // a document (phase 10)
    case events::SetKind::Text: return 0xE8C1;      // characters: a picture's text (phase 10)
    case events::SetKind::Converted: return 0xEB9F; // a picture (phase 10)
    case events::SetKind::Preview: return 0xE8A5;   // a document made (phase 10)
    case events::SetKind::Space: return 0xEDA2;     // a drive (phase 13a)
    case events::SetKind::Threat: return 0xEA18;    // a shield (phase 13a)
    default: return 0xE80A;                         // a table
    }
}

// A tab's count: a document's pages, a picture's lines, pictures converted, a table's rows.
auto count_of(const events::DataSet& s) -> int {
    if (s.kind == events::SetKind::Document && s.pages) {
        return s.pages->count;
    }
    if (s.text_read) {
        return static_cast<int>(s.text_read->lines.size());
    }
    if (s.converted) {
        return static_cast<int>(s.converted->rows.size());
    }
    if (s.kind == events::SetKind::Preview) {
        return 1; // one file
    }
    if (s.threat) {
        return static_cast<int>(s.threat->rows.size()); // its findings (phase 13a)
    }
    return static_cast<int>(s.rows.rows());
}

// A tab's name: what the panel shows of it (the card says more: "Recognized text").
// One picture to a question (responsive text; two "Processes" tabs were seen): `s` has
// no tab of its own when the turn made the same set again later (one tool, one target: its source),
// or when it is a moment's process list and the turn sampled the processes over seconds. A
// workbook's sheets and the answer's tables share a source and keep their tabs.
auto replaced_in_turn(const Model& m, std::span<const std::uint32_t> data, const events::DataSet& s) -> bool {
    if (!s.book.empty() || s.source.empty() || s.source == events::kAnswerSource) {
        return false;
    }
    bool later = false;
    for (const auto id : data) {
        if (id == s.id) {
            later = true;
            continue;
        }
        const auto it = std::ranges::find(m.sets, id, &events::DataSet::id);
        if (it == m.sets.end()) {
            continue;
        }
        if ((later && it->book.empty() && it->source == s.source) || (s.source == "process.list" && it->source == "process.sample")) {
            return true;
        }
    }
    return false;
}

auto tab_name(const events::DataSet& s) -> std::string_view {
    return s.kind == events::SetKind::Text ? std::string_view{"Text"} : s.kind == events::SetKind::Converted ? std::string_view{"Converted"} : s.title;
}

// The columns a copy takes: not a type's icon nor a row's button.
auto exported(const ui::work::Column& c) -> bool {
    return c.kind != ui::work::ColumnKind::Icon && c.kind != ui::work::ColumnKind::Action;
}

// The set's rows as shown (filtered, sorted): tab-separated, or CSV (RFC 4180, CRLF; UTF-8 with
// its BOM, cells safe from formulas: tools::docs::csv_cell).
void export_rows(std::string& out, const events::DataSet& s, const ui::work::TableState& t, bool csv) {
    out.clear();
    if (csv) {
        out += "\xEF\xBB\xBF";
    }
    const char sep = csv ? ',' : '\t';
    const std::string_view eol = csv ? "\r\n" : "\n";
    const auto line = [&](auto&& cell_of) {
        bool first = true;
        for (std::size_t c = 0; c < s.columns.size() && c < s.rows.columns; ++c) {
            if (!exported(s.columns[c])) {
                continue;
            }
            if (!first) {
                out += sep;
            }
            first = false;
            const std::string_view cell = cell_of(c);
            if (csv) {
                out += tools::docs::csv_cell(cell);
            } else {
                out += cell; // a tab or a newline in a cell would break the row: none is written
            }
        }
        out += eol;
    };
    line([&](std::size_t c) -> std::string_view { return s.columns[c].title; });
    for (const auto r : t.order) {
        line([&](std::size_t c) -> std::string_view { return s.rows.at(r, c).text; });
    }
}

// A turn the panel shows: the one in front, or an earlier one.
struct ShownTurn {
    std::uint64_t task = 0;
    std::span<const std::uint32_t> data;
    std::span<const events::ActivityEntry> activity;
};

auto turn_of_task(const Model& m, std::uint64_t task) -> ShownTurn {
    if (task == m.task) {
        return {m.task, m.data, m.activity_log};
    }
    for (const auto& t : m.session) {
        if (t.task == task) {
            return {t.task, t.data, t.activity};
        }
    }
    return {};
}

// The turn whose data `id` is.
auto turn_of_set(const Model& m, std::uint32_t id) -> ShownTurn {
    if (std::ranges::find(m.data, id) != m.data.end()) {
        return {m.task, m.data, m.activity_log};
    }
    for (const auto& t : m.session) {
        if (std::ranges::find(t.data, id) != t.data.end()) {
            return {t.task, t.data, t.activity};
        }
    }
    return {};
}

} // namespace

auto Painter::has_report(const Model& m) const -> bool {
    return m.report.has_value() && !m.report->processes.empty();
}

auto Painter::report_set(const Model& m) -> const events::DataSet& {
    // Made again when the report changes: its programs, their memory, a Close for the user's own.
    const auto& rep = *m.report;
    std::uint64_t key = m.task * 1099511628211ull + rep.processes.size();
    for (const auto& p : rep.processes) {
        key = key * 31 + p.pid + p.memory;
    }
    if (key == report_key_ && !report_set_.columns.empty()) {
        return report_set_;
    }
    report_key_ = key;
    report_set_ = events::DataSet{.id = kReportSet,
                                  .title = "What uses memory",
                                  .source = "process.list",
                                  .complete = true,
                                  .kind = events::SetKind::Processes};
    report_set_.columns = {{"", ui::work::ColumnKind::Icon, 20, false, false},
                           {"Program", ui::work::ColumnKind::Text, 0, true, true},
                           {"", ui::work::ColumnKind::Text, 120, false, false},
                           {"CPU", ui::work::ColumnKind::Percent, 168, false, true},
                           {"Memory", ui::work::ColumnKind::Number, 90, false, true},
                           {"", ui::work::ColumnKind::Action, 62, false, false}};
    report_set_.rows.columns = report_set_.columns.size();
    for (const auto& p : rep.processes) {
        auto& c = report_set_.rows.cells;
        c.push_back({"process:" + p.name, 0});
        c.push_back({p.name, 0});
        c.push_back({p.note, 0});
        c.push_back({std::format("{:.0f}%", p.cpu), p.cpu});
        c.push_back({sizes::text(p.memory), static_cast<double>(p.memory)});
        c.push_back({p.closable ? std::string{"Close"} : std::string{}, static_cast<double>(p.pid)});
    }
    report_set_.footer = std::format("The top {} of {} programs", rep.processes.size(), rep.process_count);
    return report_set_;
}

auto Painter::panel_set(const Model& m) const -> const events::DataSet* {
    if (m.active_set == 0) {
        return nullptr;
    }
    const auto it = std::ranges::find(m.sets, m.active_set, &events::DataSet::id);
    return it == m.sets.end() ? nullptr : &*it;
}

auto Painter::conversation_width(float width) -> float {
    return std::max(kMinConversation, std::min(kConversation, width - 1 - kMinPanel));
}

auto Painter::table_state(std::uint32_t set) -> ui::work::TableState& {
    auto it = std::ranges::find(tables_, set, &std::pair<std::uint32_t, ui::work::TableState>::first);
    if (it == tables_.end()) {
        tables_.emplace_back(set, ui::work::TableState{});
        it = tables_.end() - 1;
    }
    return it->second;
}

auto Painter::panel_shown(const Model& m) const -> bool {
    return m.permission.has_value() || approval_going(m) || (m.expanded && has_detail(m)) || m.text_translation.has_value() || panel_set(m) != nullptr || has_report(m) || (m.activity_task != 0 && !turn_of_task(m, m.activity_task).activity.empty());
}

void Painter::draw_panel(Model& m, const Box& panel, Reaction& r) {
    auto& renderer = ui_->renderer();
    panel_table_ = nullptr;
    if (m.permission) {
        draw_approval_panel(m, *m.permission, false, panel, r);
        return;
    }
    if (approval_going(m)) { // answered (MS 11): its objects stay while its card shrinks and its rows go
        draw_approval_panel(m, m.answered->asked, true, panel, r);
        return;
    }
    if (m.text_translation) { // a text's translation: its pairs and terms
        draw_text_panel(m, panel, r);
        return;
    }
    if (m.expanded && has_detail(m)) { // the detailed mode: the agent's terminal, live
        add_target("panel", panel);
        draw_terminal_pane(m, panel, r);
        return;
    }
    const bool activity = m.activity_task != 0 && !turn_of_task(m, m.activity_task).activity.empty();
    const auto* set = activity ? nullptr : panel_set(m);
    if (!activity && set == nullptr && has_report(m)) {
        set = &report_set(m); // the report's programs
    }
    if ((!activity && set == nullptr) || panel.w <= 1 || panel.h <= 1) {
        return;
    }
    add_target("panel", panel);
    panel_ground(panel);
    renderer.rect({panel.x - 1, panel.y, 1, panel.h}, {.fill = ui::th().line});
    std::array<char, 64> buf{};

    // The tabs: the turn's sets, its Activity pinned right.
    const ShownTurn turn = activity                ? turn_of_task(m, m.activity_task)
                           : set == &report_set_ ? turn_of_task(m, m.task)
                                                 : turn_of_set(m, set->id);
    std::array<work::Tab, kMaxTabs + 1> tabs{};
    std::array<std::uint32_t, kMaxTabs> tab_ids{};
    // MS 08: a tab's count counts on base as its rows come (a spring, shown rounded); a set whole
    // when first shown has its number at once.
    const auto counted = [&](const events::DataSet& s) {
        const float n = anim_.to(id_of("tab:count", s.id), static_cast<float>(count_of(s)), ui::motion::kBase,
                                 s.complete ? std::nullopt : std::optional{0.0f});
        return static_cast<int>(std::lround(n));
    };
    std::size_t n = 0;
    int active = 0;
    const auto book_of = [&](std::uint32_t id) -> std::string_view {
        const auto it = std::ranges::find(m.sets, id, &events::DataSet::id);
        return it == m.sets.end() ? std::string_view{} : std::string_view{it->book};
    };
    for (const auto id : turn.data) {
        const auto it = std::ranges::find(m.sets, id, &events::DataSet::id);
        if (it == m.sets.end() || n == kMaxTabs) {
            continue;
        }
        // One picture to a question (responsive text): a set made again later in the turn, or a moment's
        // process list where a sample over seconds came, has no tab (its step's "Open table" shows it).
        if ((set == nullptr || id != set->id) && replaced_in_turn(m, turn.data, *it)) {
            continue;
        }
        // A workbook's sheets: one tab, the book's, its count the sheets (phase 10).
        if (!it->book.empty() && std::any_of(tab_ids.begin(), tab_ids.begin() + static_cast<std::ptrdiff_t>(n),
                                             [&](std::uint32_t other) { return book_of(other) == it->book; })) {
            continue;
        }
        if (set != nullptr && (id == set->id || (!it->book.empty() && set->book == it->book))) {
            active = static_cast<int>(n);
        }
        const auto sheets = static_cast<int>(std::ranges::count_if(turn.data, [&](std::uint32_t other) { return book_of(other) == it->book; }));
        tab_ids[n] = id;
        tabs[n++] = {it->book.empty() ? tab_name(*it) : it->book, icon_of(it->kind), it->book.empty() ? counted(*it) : sheets, !it->complete};
    }
    if (set != nullptr && (set != &report_set_ || n == 0) && n < kMaxTabs &&
        std::find(tab_ids.begin(), tab_ids.begin() + static_cast<std::ptrdiff_t>(n), set->id) == tab_ids.begin() + static_cast<std::ptrdiff_t>(n) &&
        (set->book.empty() || std::none_of(tab_ids.begin(), tab_ids.begin() + static_cast<std::ptrdiff_t>(n),
                                           [&](std::uint32_t other) { return book_of(other) == set->book; }))) {
        active = static_cast<int>(n); // shown from elsewhere (another turn, a step's Open table): its own tab
        tab_ids[n] = set->id;
        tabs[n++] = {tab_name(*set), icon_of(set->kind), counted(*set), !set->complete};
    }
    const std::size_t sets = n;
    int pinned = -1;
    if (!turn.activity.empty()) {
        pinned = static_cast<int>(n);
        tabs[n++] = {"Activity", kIconTerminal, static_cast<int>(turn.activity.size()), false};
        if (activity) {
            active = pinned;
        }
    }
    const int before = active;
    // The row's end: the panel's cross (it could not be closed).
    constexpr float kClose = 28.0f;
    std::array<Box, kMaxTabs + 1> boxes{};
    std::array<Box, kMaxTabs + 1> listed{};
    work::TabsMore more{.open = tabs_more_open_, .defer = true, .items = std::span{listed.data(), n}};
    const Box body = work::tabs(*ui_, id_of("tabs"), panel, std::span{tabs.data(), n}, active, pinned, std::span{boxes.data(), n}, kClose + 8,
                                &more);
    if (more.button.w > 0) {
        add_target("tab:more", more.button);
    }
    for (std::size_t i = 0; i < n; ++i) { // where work::tabs put them (a tab with no room: none)
        if (boxes[i].w <= 0) {
            continue;
        }
        if (static_cast<int>(i) == pinned) {
            add_target("tab:activity", boxes[i]);
            continue;
        }
        const auto name = std::format_to_n(buf.data(), buf.size(), "tab:{}", tab_ids[i]);
        add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, boxes[i]);
    }
    const Box close{panel.x + panel.w - 12 - kClose, panel.y + (46 - kClose) / 2, kClose, kClose};
    add_target("panel:close", close);
    if (work::icon_button(*ui_, id_of("panel:close"), close, work::kIconClose)) {
        hide_panel(m);
        redraw_due_ = true;
        return; // the next frame: one column
    }
    if (active != before) {
        if (active == pinned) {
            show_activity(m, turn.task);
        } else if (static_cast<std::size_t>(active) < sets) {
            show_set(m, tab_ids[static_cast<std::size_t>(active)]);
        }
        redraw_due_ = true;
        // (This frame still draws what was shown: never a frame with an empty panel — MS 09.)
    }
    // MS 09: another content — the old one goes kPanelAside px aside (towards where its tab is) on
    // base, its opacity on quick; the new one comes from the other side on smooth, its opacity on
    // base, kPanelInAt later. Each set keeps its own table state: scroll and sort stay.
    const std::uint64_t key = activity ? kActivityKey | turn.task : static_cast<std::uint64_t>(set->id) + 1;
    if (key != panel_key_) {
        if (panel_key_ != 0) {
            panel_old_key_ = panel_key_;
            panel_way_ = active >= panel_tab_ ? 1.0f : -1.0f;
            panel_changed_at_ = mnow_;
            anim_.place(kPanelOutTrack, 1.0f);
            anim_.place(kPanelOutShiftTrack, 0.0f);
            anim_.place(kPanelInTrack, 0.0f);
            anim_.place(kPanelInShiftTrack, kPanelAside * panel_way_);
        }
        panel_key_ = key;
    }
    panel_tab_ = active;
    const bool due = mnow_ - panel_changed_at_ >= kPanelInAfter;
    PanelMotion pm;
    pm.out = anim_.to(kPanelOutTrack, 0.0f, ui::motion::kQuick);
    const float shift = reduced_motion_ ? 0.0f : kPanelAside * panel_way_; // effects off: the contents cross in place
    pm.out_shift = anim_.geometry(kPanelOutShiftTrack, -shift, ui::motion::kBase);
    pm.in = anim_.to(kPanelInTrack, due ? 1.0f : 0.0f, ui::motion::kBase);
    pm.in_shift = anim_.geometry(kPanelInShiftTrack, due ? 0.0f : shift, ui::motion::kSmooth);
    if (!due) {
        redraw_due_ = true; // its turn comes on a frame
    }
    panel_motion_ = pm;
    const float panel_opacity = renderer.opacity();
    renderer.push_clip({body.x, body.y, body.w, body.h});
    if (pm.out > 0.004f && panel_old_key_ != 0) { // what it showed before, going: a picture, nothing to press
        const auto keep = target_count_;
        Reaction ignored;
        ui_->set_inert(true);
        renderer.set_opacity(panel_opacity * pm.out);
        const Box aside{body.x + pm.out_shift, body.y, body.w, body.h};
        if ((panel_old_key_ & kActivityKey) != 0) {
            const ShownTurn old_turn = turn_of_task(m, panel_old_key_ & ~kActivityKey);
            if (!old_turn.activity.empty()) {
                draw_activity(m, old_turn.activity, aside, ignored);
            }
        } else {
            const auto old_id = static_cast<std::uint32_t>(panel_old_key_ - 1);
            const auto old_set = std::ranges::find(m.sets, old_id, &events::DataSet::id);
            if (old_set != m.sets.end()) {
                draw_set_body(m, *old_set, aside, ignored);
            } else if (has_report(m) && report_set_.id == old_id) {
                draw_set_body(m, report_set_, aside, ignored);
            }
        }
        ui_->set_inert(false);
        target_count_ = keep;
        panel_table_ = nullptr;
    }
    renderer.set_opacity(panel_opacity * pm.in);
    const Box shown_body{body.x + pm.in_shift, body.y, body.w, body.h};
    if (activity) {
        draw_activity(m, turn.activity, shown_body, r);
    } else {
        draw_set_body(m, *set, shown_body, r);
    }
    renderer.set_opacity(panel_opacity);
    renderer.pop_clip();
    // "N more"'s list over the body, drawn after it (final review I2).
    const auto chosen = work::tabs_more_list(*ui_, std::span{tabs.data(), n}, more);
    tabs_more_open_ = more.open;
    for (std::size_t i = 0; i < n && i < tab_ids.size(); ++i) { // its list's rows, by set
        if (listed[i].w > 0 && static_cast<int>(i) != pinned) {
            const auto name = std::format_to_n(buf.data(), buf.size(), "tab:more:{}", tab_ids[i]);
            add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, listed[i]);
        }
    }
    if (chosen) {
        if (*chosen == pinned) {
            show_activity(m, turn.task);
        } else if (static_cast<std::size_t>(*chosen) < sets) {
            show_set(m, tab_ids[static_cast<std::size_t>(*chosen)]);
        }
        redraw_due_ = true;
    }
}

void Painter::draw_set_body(Model& m, const events::DataSet& shown, const Box& body, Reaction& r) {
    if (shown.kind == events::SetKind::Document && shown.pages) { // a PDF by its pages (phase 10)
        draw_document(m, shown, body, r);
        return;
    }
    if (shown.kind == events::SetKind::Preview) { // a document as written (phase 10)
        draw_preview(m, shown, body, r);
        return;
    }
    if (shown.kind == events::SetKind::Threat && shown.threat) { // the threat check's report (phase 13a)
        draw_threat(m, shown, body, r);
        return;
    }
    if (shown.text_read) { // a picture's text, read here (phase 10)
        draw_text_read(shown, body, r);
        return;
    }
    if (shown.converted) { // pictures converted here (phase 10)
        draw_converted(shown, body, r);
        return;
    }
    auto& renderer = ui_->renderer();
    const auto* set = &shown;
    std::array<char, 64> buf{};
    const auto& in = ui_->input();
    // The header: the title, where it came from; the tools on the right.
    const float top = body.y + 18;
    const float title_h = 24.0f;
    float tools_x = body.x + body.w - kPadX;
    const auto tool = [&](std::string_view name, char32_t icon) {
        const Box b{tools_x - kTool, top - 4, kTool, kTool};
        tools_x = b.x - 6;
        add_target(name, b);
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(icon, 15.0f, {b.x + b.w / 2, b.y + b.h / 2}, ui::th().text_2);
        return ui_->interact(id_of(name), b, false).clicked;
    };
    auto& state = table_state(set->id);
    if (!set->location.empty() && set->kind != events::SetKind::Space && tool("panel:explorer", kIconFolder)) { // the tree has its own
        r.open_folder = set->location;
    }
    // CSV | XLSX side by side (phase 10, screen 23): the XLSX keeps numbers, money and dates numbers.
    const float csv_w = work::text_width(*ui_, Face::Semibold, 12.0f, "CSV") + 20;
    const float xlsx_w = work::text_width(*ui_, Face::Semibold, 12.0f, "XLSX") + 20;
    const Box group{tools_x - csv_w - xlsx_w, top - 4, csv_w + xlsx_w, kTool};
    tools_x = group.x - 6;
    renderer.rect({group.x, group.y, group.w, group.h}, {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
    const auto segment = [&](std::string_view name, std::string_view words, const Box& b) {
        const auto hit = ui_->interact(id_of(name), b, false);
        const float h = ui_->hover(id_of(name), hit.hovered);
        if (h > 0.01f) {
            renderer.rect({b.x + 1, b.y + 1, b.w - 2, b.h - 2}, {.fill = faint(ui::th().w3, h), .radius = {7, 7, 7, 7}});
        }
        work::caption(*ui_, {b.x + 10, b.y, b.w - 10, b.h}, Face::Semibold, 12.0f, words, h > 0.5f ? ui::th().text : ui::th().text_2);
        add_target(name, b);
        return hit;
    };
    const Box csv_box{group.x, group.y, csv_w, group.h};
    const Box xlsx_box{group.x + csv_w, group.y, xlsx_w, group.h};
    renderer.rect({xlsx_box.x, xlsx_box.y + 6, 1, xlsx_box.h - 12}, {.fill = ui::th().line});
    if (segment("panel:csv", "CSV", csv_box).clicked) {
        export_rows(panel_text_, *set, state, true);
        r.save = Reaction::SaveFile{set->title + ".csv", panel_text_};
    }
    const auto xlsx_hit = segment("panel:xlsx", "XLSX", xlsx_box);
    if (xlsx_hit.hovered) {
        ui_->tooltip(id_of("panel:xlsx"), xlsx_box, "Save for Excel", "Numbers, money and dates stay numbers. No formulas.", "Ctrl+Shift+E");
    }
    if (xlsx_hit.clicked || std::exchange(m.export_xlsx, false)) { // the rows as shown (phase 9), dates and money typed (phase 10)
        tools::docs::Table table;
        std::vector<std::size_t> taken;
        for (std::size_t c = 0; c < set->columns.size() && c < set->rows.columns; ++c) {
            if (exported(set->columns[c])) {
                const auto& column = set->columns[c];
                const auto kind = column.kind == ui::work::ColumnKind::Size     ? tools::docs::ColumnKind::Size
                                  : column.kind == ui::work::ColumnKind::Date   ? tools::docs::ColumnKind::Date
                                  : column.kind == ui::work::ColumnKind::Number ? (column.tag.empty() ? tools::docs::ColumnKind::Number
                                                                                                      : tools::docs::ColumnKind::Money)
                                                                                : tools::docs::ColumnKind::Text;
                table.columns.emplace_back(column.title, kind);
                taken.push_back(c);
            }
        }
        for (const auto row : state.order) {
            auto& cells = table.rows.emplace_back();
            for (const auto c : taken) {
                const auto& cell = set->rows.at(row, c);
                // A date as shown ("01 Sep 2026") goes as the date it is.
                cells.push_back(set->columns[c].kind == ui::work::ColumnKind::Date && cell.number > 0
                                    ? std::format("{:%Y-%m-%d}", std::chrono::sys_days{std::chrono::year{1899} / std::chrono::December / 30} +
                                                                     std::chrono::days{static_cast<int>(cell.number)})
                                    : cell.text);
            }
        }
        if (auto xlsx = tools::docs::xlsx_from_table(table)) {
            r.save = Reaction::SaveFile{set->title + ".xlsx", std::move(*xlsx)};
        }
    }
    if (tool("panel:copy", kIconCopy)) {
        export_rows(panel_text_, *set, state, false);
        r.clipboard = panel_text_;
    }
    const Box filter{tools_x - kFilterW, top - 4, kFilterW, kTool};
    add_target("panel:filter", filter);
    if (const auto& mouse = ui_->input(); mouse.mouse_x >= filter.x && mouse.mouse_x < filter.x + filter.w && mouse.mouse_y >= filter.y &&
                                          mouse.mouse_y < filter.y + filter.h) {
        ui_->want_cursor(ui::Cursor::IBeam);
    }
    renderer.rect({filter.x, filter.y, filter.w, filter.h},
                  {.fill = ui::th().w1, .radius = {8, 8, 8, 8},
                   // (its focus on quick — the map's unnamed "Filter focus")
                   .border = blend(ui::th().line, ui::th().accent,
                                   std::clamp(anim_.to(id_of("panel:filter:focus"), m.filter_focus ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f)),
                   .border_width = 1});
    ui_->draw_icon(kIconSearch, 14.0f, {filter.x + 17, filter.y + filter.h / 2}, ui::th().text_4);
    if (m.filter.text.empty() && !m.filter_focus) {
        work::caption(*ui_, {filter.x + 32, filter.y, filter.w - 40, filter.h}, Face::Text, 13.0f, "Filter", ui::th().text_4);
    } else {
        const float tw = work::caption(*ui_, {filter.x + 32, filter.y, filter.w - 40, filter.h}, Face::Text, 13.0f, m.filter.text,
                                     ui::th().text);
        if (m.filter_focus) {
            renderer.rect({filter.x + 32 + tw + 1, filter.y + 8, 1.5f, filter.h - 16}, {.fill = ui::th().text});
        }
    }
    if (ui_->interact(id_of("filter"), filter, false).clicked) {
        focus_filter(m, true);
    } else if (m.filter_focus && in.mouse_pressed && !inside(filter, in.mouse_x, in.mouse_y)) {
        focus_filter(m, false); // a click elsewhere gives the typing back to the field
    }
    const float title_w = std::max(0.0f, filter.x - 12 - (body.x + kPadX));
    add_target("panel:title", {body.x + kPadX, top, title_w, title_h});
    const float drawn_w = work::caption(*ui_, {body.x + kPadX, top, title_w, title_h}, Face::Display, 18.0f, set->title, ui::th().text);
    if (set->read_only) { // looked at only (phase 13a, .ro): a quiet badge, never yellow or red
        constexpr std::string_view kRo = "read-only";
        const float bw = work::text_width(*ui_, Face::Text, 11.5f, kRo) + 8 + 11 + 5 + 9;
        const Box ro{body.x + kPadX + drawn_w + 10, top + (title_h - 20) / 2, bw, 20};
        if (ro.x + ro.w <= body.x + kPadX + title_w) {
            renderer.rect({ro.x, ro.y, ro.w, ro.h}, {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line,
                                                      .border_width = 1});
            ui_->draw_icon(kIconLock, 11.0f, {ro.x + 8 + 5.5f, ro.y + 10}, ui::th().text_3);
            work::caption(*ui_, {ro.x + 8 + 11 + 5, ro.y, bw - 8 - 11 - 5 - 9, 20}, Face::Text, 11.5f, kRo, ui::th().text_3);
            add_target("panel:read-only", ro);
        }
    }
    // The source chip (mono) and the snapshot's time.
    const float my = top + title_h + 7;
    if (!set->source.empty()) {
        const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, set->source) + 18 + 18, body.w - 2 * kPadX);
        const Box chip{body.x + kPadX, my, sw, 22};
        renderer.rect({chip.x, chip.y, chip.w, chip.h},
                      {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconTerminal, 13.0f, {chip.x + 15, chip.y + 11}, ui::th().text_3);
        work::caption(*ui_, {chip.x + 27, chip.y, chip.w - 36, chip.h}, Face::Mono, 12.0f, set->source, ui::th().text_2);
        if (!set->snapshot.empty()) {
            work::caption(*ui_, {chip.x + chip.w + 10, my, std::max(0.0f, body.x + body.w - kPadX - chip.x - chip.w - 10), 22},
                        Face::Text, 12.5f, set->snapshot, ui::th().text_3);
        }
    }
    float y = my + 22 + 14;

    // What takes the space (phase 13a): the drive's bar and the way back over its table.
    if (set->kind == events::SetKind::Space && set->space) {
        y = draw_space_head(m, *set, body, y, r);
        if (y < 0) {
            // Made again: the next frame draws it. This one still shows the list it left, where the
            // table was — never a frame without it (MS § 7; map № 14).
            if (space_old_set_ == set->id) {
                draw_space_left(*set, space_table_box_, 1.0f);
            }
            return;
        }
    }

    // Rules and memory: Private windows' own field (phase 13a § 2.1, 04 #5) — an app, a window title
    // or a site, typed, Add; the rule goes to the agent, the shell shows the list again.
    if (set->kind == events::SetKind::Memory) {
        const float fh = 34;
        static constexpr std::array<std::string_view, 3> kKinds{"App", "Title", "Site"};
        float kinds_w = 4;
        for (const auto k : kKinds) {
            kinds_w += work::text_width(*ui_, Face::Text, 12.5f, k) + 22;
        }
        const auto add_size = work::button_size(*ui_, "Add");
        const float right = body.x + body.w - kPadX;
        const Box add{right - add_size.w - 10, y, add_size.w + 10, fh};
        const Box kinds{add.x - 8 - kinds_w, y, kinds_w, fh};
        const Box field{body.x + kPadX, y, std::max(0.0f, kinds.x - 8 - body.x - kPadX), fh};
        add_target("private:field", field);
        renderer.rect({field.x, field.y, field.w, field.h},
                      {.fill = ui::th().w1, .radius = {8, 8, 8, 8},
                       .border = blend(ui::th().line, ui::th().accent, // (its focus on quick, as the filter's)
                                       std::clamp(anim_.to(id_of("private:field:focus"), m.private_focus ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f)),
                       .border_width = 1});
        ui_->draw_icon(kIconAddRule, 13.0f, {field.x + 16, field.y + fh / 2}, ui::th().text_4);
        if (m.private_add.text.empty() && !m.private_focus) {
            work::caption(*ui_, {field.x + 32, field.y, field.w - 40, fh}, Face::Text, 13.0f, "Add an app, a window title or a site\xE2\x80\xA6", ui::th().text_4);
        } else {
            const float tw = work::caption(*ui_, {field.x + 32, field.y, field.w - 40, fh}, Face::Text, 13.0f, m.private_add.text, ui::th().text);
            if (m.private_focus) {
                renderer.rect({field.x + 32 + tw + 1, field.y + 9, 1.5f, fh - 18}, {.fill = ui::th().text});
            }
        }
        if (ui_->interact(id_of("private:field"), field, false).clicked) {
            focus_private(m, true);
        } else if (m.private_focus && in.mouse_pressed && !inside(field, in.mouse_x, in.mouse_y) && !inside(add, in.mouse_x, in.mouse_y) &&
                   !inside(kinds, in.mouse_x, in.mouse_y)) {
            focus_private(m, false);
        }
        renderer.rect({kinds.x, kinds.y, kinds.w, kinds.h},
                      {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
        float kx = kinds.x + 2;
        for (std::size_t k = 0; k < kKinds.size(); ++k) {
            const float kw = work::text_width(*ui_, Face::Text, 12.5f, kKinds[k]) + 22;
            const Box b{kx, kinds.y + 3, kw, fh - 6};
            kx += kw;
            const bool on = m.private_kind == static_cast<int>(k);
            const auto kind_id = id_of("private:kind", static_cast<std::uint64_t>(k));
            const auto kind_hit = ui_->interact(kind_id, b, false);
            if (on) { // the chosen one's ground slides to it on base, its words turn on quick (the map's unnamed: segments)
                const Box ground = work::chosen_ground(*ui_, id_of("private:kinds"), kinds.x, b);
                renderer.rect({ground.x, ground.y, ground.w, ground.h}, {.fill = ui::th().w3, .radius = {6, 6, 6, 6}});
            } else if (const float light = lit(kind_id, kind_hit.hovered); light > 0.004f) {
                renderer.rect({b.x, b.y, b.w, b.h}, {.fill = faint(ui::th().w1, light), .radius = {6, 6, 6, 6}});
            }
            work::caption(*ui_, {b.x + 11, b.y, b.w - 22, b.h}, Face::Text, 12.5f, kKinds[k],
                          blend(ui::th().text_3, ui::th().text, work::chosen(*ui_, kind_id, on)));
            const auto name = std::format_to_n(buf.data(), buf.size(), "private:kind:{}", k);
            add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, b);
            if (kind_hit.clicked) {
                m.private_kind = static_cast<int>(k);
            }
        }
        add_target("private:add", add);
        if (work::button(*ui_, id_of("private:add"), add, "Add", work::ButtonStyle::Normal) && !r.command) {
            r.command = add_private(m);
        }
        y += fh + 14;
    }

    // A workbook's sheets: a switch, each with its rows (phase 10, screen 23).
    if (!set->book.empty()) {
        const float sh = 30;
        float sx = body.x + kPadX + 3;
        float total = 6;
        for (const auto& s2 : m.sets) {
            if (s2.book == set->book) {
                total += 13 + 7 + work::text_width(*ui_, Face::Text, 12.5f, s2.title) + 7 +
                         work::text_width(*ui_, Face::Text, 11.0f, std::format("{}", s2.rows.rows())) + 12 + 24 + 4;
            }
        }
        renderer.rect({body.x + kPadX, y, std::min(total, body.w - 2 * kPadX), sh + 6},
                      {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
        std::optional<std::uint32_t> picked;
        for (const auto& s2 : m.sets) {
            if (s2.book != set->book) {
                continue;
            }
            const auto rows = std::format("{}", s2.rows.rows());
            const float nw = work::text_width(*ui_, Face::Text, 12.5f, s2.title);
            const float cw = work::text_width(*ui_, Face::Text, 11.0f, rows) + 12;
            const Box b{sx, y + 3, 12 + 13 + 7 + nw + 7 + cw + 12, sh};
            sx += b.w + 4;
            const bool on = s2.id == set->id;
            const auto hit = ui_->interact(id_of("sheet", s2.id), b, false);
            const float h = ui_->hover(id_of("sheet", s2.id), hit.hovered);
            if (on || h > 0.01f) {
                renderer.rect({b.x, b.y, b.w, b.h}, {.fill = on ? ui::th().w2 : faint(ui::th().w1, h), .radius = {7, 7, 7, 7}});
            }
            ui_->draw_icon(0xE80A, 13.0f, {b.x + 12 + 6.5f, b.y + sh / 2}, on ? ui::th().text : ui::th().text_3);
            work::caption(*ui_, {b.x + 12 + 13 + 7, b.y, nw, sh}, Face::Text, 12.5f, s2.title, on ? ui::th().text : ui::th().text_3);
            const Box count{b.x + 12 + 13 + 7 + nw + 7, b.y + (sh - 18) / 2, cw, 18};
            renderer.rect({count.x, count.y, count.w, count.h}, {.fill = ui::th().w2, .radius = {9, 9, 9, 9}});
            work::caption(*ui_, {count.x + 6, count.y, cw - 6, 18}, Face::Text, 11.0f, rows, ui::th().text_2);
            const auto name = std::format_to_n(buf.data(), buf.size(), "sheet:{}", s2.id);
            add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, b);
            if (hit.clicked && !on) {
                picked = s2.id;
            }
        }
        if (picked) {
            show_set(m, *picked);
            redraw_due_ = true;
        }
        y += sh + 6 + 12;
    }

    // The machine's tiles over its processes (the report's).
    if (set->kind == events::SetKind::Processes && m.report && !m.report->tiles.empty()) {
        const std::size_t count = std::min<std::size_t>(m.report->tiles.size(), 4);
        const float gap = 10.0f;
        const float tw = (body.w - 2 * kPadX - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
        // MS § 7 "Inline chart": a reading more — the lines move on through its second, a frame
        // each, never a jump. The first it takes in puts the monitor's minute in place of the made one.
        if (!ui_->inert() && m.report->readings != report_readings_) {
            if (report_readings_ != 0 && m.report->readings != 0) {
                report_reading_at_ = now_;
            }
            report_readings_ = m.report->readings;
        }
        const float slide = std::clamp(
            static_cast<float>((now_ - report_reading_at_) / std::chrono::duration<double>(ui::motion::panel::kSampleEvery).count()), 0.0f, 1.0f);
        if (slide < 1.0f) {
            redraw_due_ = true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& t = m.report->tiles[i];
            const Box tile{body.x + kPadX + static_cast<float>(i) * (tw + gap), y, tw, kTile};
            const auto tone = t.tone == events::Tone::Warn ? work::Tone::Warn
                              : t.tone == events::Tone::Crit ? work::Tone::Crit
                                                             : work::Tone::Ok;
            work::metric_tile(*ui_, tile,
                              {.label = t.name, .state = t.status, .tone = tone, .value = t.value, .unit = t.unit, .sub = {},
                               .series = t.series, .line = t.line, .slide = slide, .rolling = t.series.size() >= monitor::kPoints},
                              id_of("panel:tile", i));
            const auto name = std::format_to_n(buf.data(), buf.size(), "panel:tile:{}", i);
            add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, tile);
        }
        y += kTile + 14;
    }

    // The table, then its footer.
    const Box table{body.x + kPadX, y, body.w - 2 * kPadX, std::max(0.0f, body.y + body.h - kFooter - y)};
    add_target("panel:table", table);
    state.filter.assign(m.filter.text);
    // A folder of the tree opened or left (MS § 7): the list it left goes aside on micro, the new
    // one comes from the other side at +60 ms.
    const PanelMotion step = set->kind == events::SetKind::Space ? space_stepping(*set) : PanelMotion{.out = 0, .out_shift = 0, .in = 1, .in_shift = 0};
    if (set->kind == events::SetKind::Space && !ui_->inert()) {
        space_motion_ = step;
        space_table_box_ = table;
    }
    if (table.h > 40) {
        const float table_opacity = renderer.opacity();
        renderer.push_clip({table.x, table.y, table.w, table.h});
        if (step.out > 0.004f) {
            draw_space_left(*set, {table.x + step.out_shift, table.y, table.w, table.h}, step.out);
        }
        renderer.set_opacity(table_opacity * step.in);
        // A listed file's row opens it, its "..." shows it in its folder; a folder's row opens the folder (phase 10).
        const bool listed = (set->kind == events::SetKind::Files || set->kind == events::SetKind::Matches) && set->rows.columns >= 3;
        const auto e = work::table(*ui_, id_of("table", set->id), {table.x + step.in_shift, table.y, table.w, table.h}, set->columns, set->rows,
                                   state, set->complete, listed);
        renderer.set_opacity(table_opacity);
        renderer.pop_clip();
        if (listed && (e.open_row >= 0 || e.more_row >= 0)) {
            const auto row = static_cast<std::size_t>(e.open_row >= 0 ? e.open_row : e.more_row);
            const auto& name = set->rows.at(row, 1).text;
            const std::string_view folder = set->kind == events::SetKind::Files ? std::string_view{set->location} : std::string_view{set->rows.at(row, 2).text};
            std::string path{folder};
            if (!path.empty() && path.back() != '\\') {
                path += '\\';
            }
            path += name;
            if (e.more_row >= 0) {
                r.show_listed = std::move(path);
            } else if (set->rows.at(row, 0).text == "folder") {
                r.open_folder = std::move(path);
            } else {
                r.open_listed = std::move(path);
            }
        }
        // A folder of the tree opened: one step down (its action's number is the child to step into).
        if (e.action_row >= 0 && set->kind == events::SetKind::Space) {
            const double child = set->rows.at(static_cast<std::size_t>(e.action_row), set->rows.columns - 1).number;
            if (child >= 0) {
                auto down = set->space_path;
                down.push_back(static_cast<std::uint32_t>(child));
                space_step(m, set->id, std::move(down), false);
            }
            return; // the set was made again: nothing more of it this frame
        }
        // Rules and memory (phase 12 § 6): Open the rules, Forget an entry, Forget all (the shell asks).
        if (e.action_row >= 0 && !r.command && set->kind == events::SetKind::Memory) {
            const auto row = static_cast<std::size_t>(e.action_row);
            const double which = set->rows.at(row, set->rows.columns - 1).number;
            if (which == 0) {
                r.open_rules = true;
            } else if (which < -1.5) {
                // a built-in private-window sign: always on, nothing to press (phase 13a)
            } else if (which < 0) {
                r.forget_all = true;
            } else {
                work::table_leave(state, set->rows, row); // MS § 7: it goes on micro, its room closes after it
                r.command = forget_row(m, set->id, row);
            }
            return; // the set may have lost the row: nothing more of it this frame
        }
        // A row's action: a program closed (its pid in the action's cell), through the agent.
        if (e.action_row >= 0 && !r.command) {
            for (std::size_t c = 0; c < set->columns.size() && c < set->rows.columns; ++c) {
                if (set->columns[c].kind == ui::work::ColumnKind::Action) {
                    const auto& cell = set->rows.at(static_cast<std::size_t>(e.action_row), c);
                    const auto n = static_cast<std::uint64_t>(cell.number);
                    // Stop names its pid; Uninstall… its program's place in the tab (phase 13a).
                    // Check deeper names its set (the Startup tab: the helper's boot log, final review I7).
                    const auto name = cell.text == "Check deeper"           ? std::format_to_n(buf.data(), buf.size(), "deeper:{}", set->id)
                                      : cell.text.starts_with("Uninstall") ? std::format_to_n(buf.data(), buf.size(), "uninstall:{}", n)
                                                                           : std::format_to_n(buf.data(), buf.size(), "close:{}", n);
                    r.command = press(m, {buf.data(), static_cast<std::size_t>(name.out - buf.data())});
                }
            }
        }
        // The rows in view, for tests: "row:<i>" and "row:<i>:action" (its button's cell).
        const float row_h = set->rows.dense ? 36.0f : 40.0f;
        for (std::size_t k = state.first; k < state.end && k < state.order.size(); ++k) {
            const std::uint32_t row = state.order[k];
            const Box box{table.x + step.in_shift, table.y + 34 + state.tops[k] - state.scroll.offset, table.w, row_h};
            const auto name = std::format_to_n(buf.data(), buf.size(), "row:{}", row);
            add_target({buf.data(), static_cast<std::size_t>(name.out - buf.data())}, box);
            if (!set->columns.empty() && set->columns.back().kind == ui::work::ColumnKind::Action &&
                !set->rows.at(row, set->columns.size() - 1).text.empty()) {
                const float aw = set->columns.back().width;
                const auto action = std::format_to_n(buf.data(), buf.size(), "row:{}:action", row);
                add_target({buf.data(), static_cast<std::size_t>(action.out - buf.data())},
                           {box.x + box.w - 14 - aw, box.y, aw, box.h});
            }
        }
    }
    panel_table_ = &state;
    if (!set->footer.empty()) {
        work::caption(*ui_, {body.x + kPadX, body.y + body.h - kFooter, body.w - 2 * kPadX, kFooter}, Face::Text, 12.5f,
                    set->footer, ui::th().text_3);
    }
}

} // namespace app::sheet
