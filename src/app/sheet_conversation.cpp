// The conversation (phase 8, docs/specs/phase-8-ui-remaster.md § 5.3; mockup-spec § 1–2): the
// bar with its status pill, the column's header with History and New chat, the turn in front —
// who asked, the question, the actions (a live block while the agent works, one line after), the
// answer, the result, the data cards, the model's chips — and the earlier turns folded under it.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/anim.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>

namespace app::sheet {
namespace {

namespace theme = ui::theme;
namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto lerp(Color a, Color b, float t) -> Color {
    const auto mix = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * std::clamp(t, 0.0f, 1.0f)));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

auto inside(const Box& b, float x, float y) -> bool {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

constexpr float kContentMax = 720.0f;  // its content at most
constexpr float kHeader = 46.0f;       // the column's header
constexpr float kCardContent = 876.0f; // a word's or a phrase's card: the 920 window less its padding
constexpr float kPadTop = 18.0f;       // a turn's padding 18 22 20
constexpr float kPadX = 22.0f;
constexpr float kPadBottom = 20.0f;
constexpr float kWho = 22.0f;          // the who row, 9 under it
constexpr float kActionsLine = 40.0f;  // the folded actions
constexpr float kActionRow = 30.0f;    // a step in the live block, 2 apart
constexpr float kCard = 54.0f;         // a data card: 10 + its 34 px tile + 10
constexpr float kEarlierRow = 46.0f;
constexpr float kMaxHeight = 760.0f;   // a one-column window's tallest (the mockups' 600–968)
constexpr float kMinBody = 160.0f;
constexpr float kTwoColumnsWidth = 1240.0f; // with the data panel (mockup-spec § 1)
constexpr float kTwoMinHeight = 600.0f;
constexpr float kTwoMaxHeight = 860.0f;
constexpr std::size_t kHistoryRows = 8;


constexpr char32_t kIconHistory = 0xE81C;
constexpr char32_t kIconNewChat = 0xE710;
constexpr char32_t kIconBook = 0xE82D;
constexpr char32_t kIconTable = 0xE80A;
constexpr char32_t kIconCheck = 0xE73E;

constexpr std::string_view kFollowUp = "Ask a follow-up\xE2\x80\xA6";
constexpr std::string_view kRefine = "Refine or add to the task\xE2\x80\xA6";
constexpr std::string_view kUnsafe = "Unsafe mode";

auto all_shown_now(const Model& m) -> bool {
    return !m.streaming;
}


// "0:07", "2:14": the turn's time so far.
auto clock_text(std::array<char, 64>& buf, double seconds) -> std::string_view {
    const auto s = static_cast<long>(std::max(0.0, seconds));
    return text(buf, "{}:{:02}", s / 60, s % 60);
}

auto set_of(const Model& m, std::uint32_t id) -> const events::DataSet* {
    const auto it = std::ranges::find(m.sets, id, &events::DataSet::id);
    return it == m.sets.end() ? nullptr : &*it;
}

auto kind_name(events::SetKind k) -> std::string_view {
    switch (k) {
    case events::SetKind::Files: return "Files";
    case events::SetKind::Matches: return "Found";
    case events::SetKind::Processes: return "Processes";
    case events::SetKind::Table: return "Table";
    case events::SetKind::Metrics: return "Metrics";
    case events::SetKind::Approval: return "To change";
    case events::SetKind::Translation: return "Translation";
    case events::SetKind::Terms: return "Terms";
    case events::SetKind::Terminal: return "Terminal";
    case events::SetKind::Text: return "Text";
    case events::SetKind::Converted: return "Converted";
    case events::SetKind::Preview: return "Document";
    }
    return "Data";
}

constexpr float kPlanRow = 24.0f; // an item of the plan

// The plan's place (phase 12 § 3): the active item, or the first not done; and how many items.
auto plan_step(const events::Checklist& c) -> std::pair<std::size_t, std::size_t> {
    const auto n = c.items.size();
    if (const auto active = std::ranges::find(c.items, events::ItemState::Active, &events::ChecklistItem::state); active != c.items.end()) {
        return {static_cast<std::size_t>(active - c.items.begin()) + 1, n};
    }
    const auto over = static_cast<std::size_t>(std::ranges::count_if(c.items, [](const events::ChecklistItem& i) {
        return i.state == events::ItemState::Done || i.state == events::ItemState::Skipped;
    }));
    return {std::min(over + 1, n), n};
}

} // namespace

auto Painter::draw_checklist(const events::Checklist& c, float x, float y, float w, bool measure) -> float {
    const float h = 8 + kPlanRow * static_cast<float>(c.items.size()) + 8;
    if (measure || c.items.empty()) {
        return c.items.empty() ? 0.0f : h;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> nb{};
    const Box block{x, y, w, h};
    renderer.rect({block.x, block.y, block.w, block.h},
                  {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
    add_target("checklist", block);
    for (std::size_t i = 0; i < c.items.size(); ++i) {
        const auto& item = c.items[i];
        const Box row{block.x + 12, block.y + 8 + kPlanRow * static_cast<float>(i), block.w - 24, kPlanRow};
        add_target(text(nb, "checklist:{}", i), row);
        const float cy = row.y + row.h / 2;
        Color ink = ui::th().text_2;
        Face face = Face::Text;
        switch (item.state) {
        case events::ItemState::Done:
            renderer.rect({row.x, cy - 9, 18, 18}, {.fill = ui::th().ok_soft, .radius = {9, 9, 9, 9}});
            ui_->draw_icon(kIconCheck, 11.0f, {row.x + 9, cy}, ui::th().ok);
            break;
        case events::ItemState::Active:
            work::spinner(*ui_, {row.x + 9, cy}, 13.0f, ui::th().accent);
            ink = ui::th().text;
            face = Face::Semibold;
            break;
        case events::ItemState::Skipped:
            renderer.rect({row.x + 4, cy - 0.75f, 10, 1.5f}, {.fill = ui::th().text_3});
            ink = ui::th().text_3;
            break;
        case events::ItemState::Pending:
            renderer.rect({row.x + 4.5f, cy - 4.5f, 9, 9},
                          {.fill = {}, .radius = {4.5f, 4.5f, 4.5f, 4.5f}, .border = ui::th().text_3, .border_width = 1.5f});
            break;
        }
        work::caption(*ui_, {row.x + 28, row.y, row.w - 28, row.h}, face, 13.0f, item.title, ink);
    }
    return h;
}

auto Painter::draw_question_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    if (!m.query) {
        return 0;
    }
    const auto& q = *m.query;
    constexpr float kPad = 16.0f;
    constexpr float kOption = 46.0f;
    constexpr float kGap = 6.0f;
    const float th = ui_->line_height(Face::Semibold, 14.5f);
    const auto [first, second] = two_lines(*ui_, Face::Semibold, 14.5f, q.question, w - 2 * kPad);
    const float title = th * (second.empty() ? 1.0f : 2.0f);
    const auto button = work::button_size(*ui_, "Other\xE2\x80\xA6");
    const float n = static_cast<float>(q.options.size());
    const float h = kPad + title + 12 + n * kOption + (n - 1) * kGap + 14 + button.h + kPad;
    if (measure) {
        return h + 12;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> nb{};
    const Box card{x, y0, w, h};
    renderer.rect({card.x, card.y, card.w, card.h},
                  {.fill = ui::th().accent_soft, .radius = {14, 14, 14, 14}, .border = ui::th().accent_line,
                   .border_width = 1});
    add_target("ask:card", card);
    work::caption(*ui_, {card.x + kPad, card.y + kPad, card.w - 2 * kPad, th}, Face::Semibold, 14.5f, first, ui::th().text);
    if (!second.empty()) {
        work::caption(*ui_, {card.x + kPad, card.y + kPad + th, card.w - 2 * kPad, th}, Face::Semibold, 14.5f, second, ui::th().text);
    }
    // The recommended option first, marked; the others in their order.
    float oy = card.y + kPad + title + 12;
    const auto row = [&](std::size_t i) {
        const auto& o = q.options[i];
        const Box box{card.x + kPad, oy, card.w - 2 * kPad, kOption};
        oy += kOption + kGap;
        const bool on = q.multi && i < m.picks.size() && m.picks[i];
        add_target(text(nb, "ask:option:{}", i), box);
        if (work::option(*ui_, id_of("ask:option", i), box, o.label, o.description, on) && !r.command) {
            r.command = choose(m, i);
            if (!m.query) {
                return; // answered: the question (and `q`) is gone
            }
        }
        if (q.recommended == i) {
            const float rw = work::text_width(*ui_, Face::Semibold, 11.5f, "Recommended");
            const Box chip{box.x + box.w - 12 - rw - 16, box.y + 8, rw + 16, 20};
            renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = with_alpha(ui::th().accent, 0.18f), .radius = {10, 10, 10, 10}});
            work::caption(*ui_, {chip.x + 8, chip.y, rw, chip.h}, Face::Semibold, 11.5f, "Recommended", ui::th().accent);
            add_target("ask:recommended", chip);
        }
    };
    if (q.recommended && *q.recommended < q.options.size()) {
        row(*q.recommended);
    }
    if (!m.query) {
        return h + 12;
    }
    for (std::size_t i = 0; m.query && i < q.options.size(); ++i) {
        if (q.recommended != i) {
            row(i);
        }
    }
    if (!m.query) {
        return h + 12; // answered by that click
    }
    // Other (the field takes their own answer), Skip; with several to choose, Send on the right.
    const float by = card.y + card.h - kPad - button.h;
    const Box other{card.x + kPad, by, button.w, button.h};
    add_target("ask:other", other);
    if (work::button(*ui_, id_of("ask:other"), other, "Other\xE2\x80\xA6") && !r.command) {
        answer_other(m);
    }
    const auto skip_size = work::button_size(*ui_, "Skip");
    const Box skip{other.x + other.w + 8, by, skip_size.w, skip_size.h};
    add_target("ask:skip", skip);
    if (work::button(*ui_, id_of("ask:skip"), skip, "Skip") && !r.command) {
        r.command = skip_question(m);
        return h + 12;
    }
    if (q.multi) {
        const auto send_size = work::button_size(*ui_, "Send");
        const Box send{card.x + card.w - kPad - send_size.w - 10, by, send_size.w + 10, send_size.h};
        add_target("ask:send", send);
        const bool any = std::ranges::any_of(m.picks, [](bool p) { return p; });
        if (work::button(*ui_, id_of("ask:send"), send, "Send", work::ButtonStyle::Primary, false, any) && !r.command) {
            r.command = send_choices(m);
        }
    }
    return h + 12;
}

// One step in a live list: ✓ done, a spinner running (its tool and its own time), ○ next (phase 10:
// the conversation's live block). Its targets: prefix:id, :time, :tool.
namespace {
constexpr double kStepCheckAfter = std::chrono::duration<double>(ui::motion::steps::kCheckAt).count();
constexpr double kFoldAfter = std::chrono::duration<double>(ui::motion::steps::kFoldAt).count();
constexpr double kFoldOldWordsAfter = std::chrono::duration<double>(ui::motion::steps::kFoldOldWordsAt).count();
constexpr double kFoldWordsAfter = std::chrono::duration<double>(ui::motion::steps::kFoldWordsAt).count();
const ui::Id kAnswerHeightTrack = id_of("answer:height");
const ui::Id kConvNewTrack = id_of("conv:new");
const ui::Id kNoActionsTrack = id_of("turn:no-actions");
const ui::Id kGoneNoteAlphaTrack = id_of("remembered:gone-alpha");
const ui::Id kGoneNoteRoomTrack = id_of("remembered:gone-room");
constexpr double kRememberedLit = std::chrono::duration<double>(ui::motion::memory::kLit).count();
constexpr double kNoteCloseAfter = std::chrono::duration<double>(ui::motion::table::kLeaveCloseAt).count();
const ui::Id kTailShareTrack = id_of("answer:tail-share");
const ui::Id kTailAlphaTrack = id_of("answer:tail-alpha");
constexpr double kTailAfter = std::chrono::duration<double>(ui::motion::stream::kCardsAt).count();
constexpr double kTailAlphaAfter = std::chrono::duration<double>(ui::motion::stream::kCardsOpacityAt).count();
constexpr double kTurnLiveFor = std::chrono::duration<double>(ui::motion::hold::kTurnLive).count(); // a turn is followed this long after its last text or work (its cards open in it)
const ui::Id kActHeightTrack = id_of("actions:height");
const ui::Id kActListTrack = id_of("actions:list");
const ui::Id kActLiveWordsTrack = id_of("actions:live-words");
const ui::Id kActFoldedWordsTrack = id_of("actions:folded-words");
} // namespace

auto Painter::step_motion(std::uint64_t id) const noexcept -> StepMotion {
    const auto it = std::ranges::find_if(steps_seen_, [id](const StepSeen& s) { return s.key != 0 && s.id == id; });
    return it == steps_seen_.end() ? StepMotion{} : it->motion;
}

void Painter::draw_step_row(const events::Step& s, const Box& row, std::string_view prefix) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    std::array<char, 64> nb{}; // the targets' names: never in `buf`, which holds the note
    add_target(text(nb, "{}:{}", prefix, s.id), row);
    const float cy = row.y + row.h / 2;
    const bool waiting = s.state == events::StepState::Wait;
    const bool running = s.state == events::StepState::Run;
    const bool done = s.state == events::StepState::Done;
    const bool bad = s.state == events::StepState::Failed || s.state == events::StepState::Skipped;
    const float o = renderer.opacity();
    // MS 06: the step wakes from its dot (quick); done, its spinner goes as its circle comes (base,
    // from .6 of its size) and its check draws itself (smooth, from +40 ms). A row first seen as it
    // is — an old turn's, a list opened later — stands so at once.
    const std::uint64_t key = id_of(prefix, static_cast<std::size_t>(s.id) * 8) | 1u; // and its + 2, + 4
    auto seen = std::ranges::find(steps_seen_, key, &StepSeen::key);
    if (seen == steps_seen_.end()) {
        seen = steps_seen_.begin() + static_cast<std::ptrdiff_t>(steps_seen_next_++ % steps_seen_.size());
        *seen = {.key = key, .id = s.id, .state = s.state, .done_at = -1.0e9, .motion = {}};
    } else if (done && seen->state != events::StepState::Done) {
        seen->done_at = now_;
    }
    seen->state = s.state;
    const bool check_due = done && now_ - seen->done_at >= kStepCheckAfter;
    if (done && !check_due) {
        redraw_due_ = true; // its turn comes on frames
    }
    StepMotion mo;
    mo.run = anim_.to(key, waiting ? 0.0f : 1.0f, ui::motion::kQuick);
    mo.done = anim_.to(key + 2, done ? 1.0f : 0.0f, ui::motion::kBase);
    mo.check = anim_.to(key + 4, check_due ? 1.0f : 0.0f, ui::motion::kSmooth);
    seen->motion = mo;
    renderer.set_opacity(o * (0.5f + 0.5f * mo.run)); // "next" waits dim
    if (mo.run < 0.996f) { // its dot
        const float was = renderer.opacity();
        renderer.set_opacity(was * (1.0f - mo.run));
        renderer.rect({row.x + 4.5f, cy - 4.5f, 9, 9},
                      {.fill = {}, .radius = {4.5f, 4.5f, 4.5f, 4.5f}, .border = ui::th().text_3, .border_width = 1.5f});
        renderer.set_opacity(was);
    }
    if (bad) {
        const float was = renderer.opacity();
        renderer.set_opacity(was * mo.run);
        renderer.rect({row.x + 4, cy - 5, 10, 10}, {.fill = ui::th().crit, .radius = {5, 5, 5, 5}});
        renderer.set_opacity(was);
    } else {
        if (const float spin = mo.run * (1.0f - mo.done); spin > 0.004f && (running || done)) {
            const float was = renderer.opacity();
            renderer.set_opacity(was * spin);
            work::spinner(*ui_, {row.x + 9, cy}, 13.0f, ui::th().accent);
            renderer.set_opacity(was);
        }
        if (mo.done > 0.004f) {
            const float was = renderer.opacity();
            renderer.set_opacity(was * mo.done);
            const float d = 18.0f * (ui::motion::steps::kCircleFrom + (1.0f - ui::motion::steps::kCircleFrom) * mo.done);
            const float cx = row.x + 9;
            renderer.rect({cx - d / 2, cy - d / 2, d, d}, {.fill = ui::th().ok_soft, .radius = {d / 2, d / 2, d / 2, d / 2}});
            const std::array<ui::render::Point, 3> check{{{cx - 3.6f, cy + 0.2f}, {cx - 1.2f, cy + 2.6f}, {cx + 3.8f, cy - 2.6f}}};
            renderer.stroke(check, 1.6f, ui::th().ok, mo.check);
            renderer.set_opacity(was);
        }
    }
    // A running step: its own time (since it began) in the accent, what runs it before
    // that in mono — the user sees it go on (without it, nothing showed that it did).
    const std::string_view note = running ? clock_text(buf, s.since >= 0 ? now_ - s.since : 0.0)
                                  : waiting ? std::string_view{"next"}
                                            : std::string_view{};
    const float nw = note.empty() ? 0.0f : work::text_width(*ui_, Face::Text, 12.0f, note);
    float right = row.x + row.w - nw;
    if (running && !note.empty()) {
        add_target(text(nb, "{}:{}:time", prefix, s.id), {right, row.y, nw, row.h});
    }
    if (running && !s.tool.empty()) {
        const float tlw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, s.tool), row.w * 0.35f);
        right -= 12 + tlw;
        add_target(text(nb, "{}:{}:tool", prefix, s.id), {right, row.y, tlw, row.h});
        work::caption(*ui_, {right, row.y, tlw, row.h}, Face::Mono, 12.0f, s.tool, ui::th().text_3);
    }
    work::caption(*ui_, {row.x + 28, row.y, std::max(0.0f, right - 10 - row.x - 28), row.h}, Face::Text, 13.0f, s.title,
                bad ? ui::th().text : lerp(ui::th().text, ui::th().text, mo.run * (1.0f - mo.done))); // white while it runs
    if (!note.empty()) {
        work::caption(*ui_, {row.x + row.w - nw, row.y, nw, row.h}, Face::Text, 12.0f, note,
                    running ? ui::th().accent : ui::th().text_3);
    }
    renderer.set_opacity(o);
}

auto Painter::conversation_shown(const Model& m, View v) const -> bool {
    return conversation_view(m, v); // the model's rule (the paste's chips go by it too)
}

auto Painter::conversation_shape(const Model& m) const -> Shape {
    // With data the panel beside it: 1240 wide, as tall as a table wants; without, 860 and as
    // tall as the turns.
    const bool two = panel_shown(m);
    const float room_h = std::max(1.0f, room_.h - kBottomRoom);
    const float most = std::min(two ? kTwoMaxHeight : kMaxHeight, room_h);
    const float least = std::min(two ? kTwoMinHeight : wt::bar::height + kHeader + kMinBody, most);
    const float content = std::max(kHeader + conv_content_, m.permission || approval_going(m) ? panel_content_ : 0.0f);
    const float h = std::clamp(wt::bar::height + content, least, most);
    const float one = translation_width(m) > 0 ? translation_width(m) : kOneColumn; // a word or a phrase: 920
    return {std::min(two ? kTwoColumnsWidth : one, std::max(1.0f, room_.w - 2 * kSideRoom)), h, ui::th().radius.window};
}

void Painter::draw_conversation(Model& m, const Box& panel, Reaction& r) {
    draw_conversation_row(m, panel, r);
    if (r.command) {
        return; // a new question: the model has moved on
    }
    const float h = std::max(0.0f, panel.h - row_h_); // under the row, however many lines its field has
    const bool two = panel_shown(m);
    const Box column{panel.x, panel.y + row_h_, two ? conversation_width(panel.w) : panel.w, h};
    cascade_base_ = ui_->renderer().opacity();
    if (!ui_->inert() && send_frame_.fly_alpha > 0.001f) {
        // Enter (MS 05): where the turn's bubble will have its first line — the window has not
        // grown over it yet, so the turn is not drawn; the turn says the real place once it is.
        question_at_ = {column.x + kPadX + 15, column.y + kHeader + kPadTop + kWho + 9 + 11, 0, 14.5f * 1.5f * ui_->text_scale()};
        question_drawn_ = true;
    }
    // Each column is a pane of its own: a selection stays in the one it started in.
    if (column.h > 1) {
        ui_->push_pane(id_of("pane:conversation"));
        draw_column(m, column, r);
        ui_->pop_pane();
    }
    if (two && column.h > 1 && !r.command) {
        const float dy = send_part(3);
        ui_->push_pane(id_of("pane:panel"));
        draw_panel(m, {column.x + column.w + 1, column.y + dy, std::max(0.0f, panel.x + panel.w - column.x - column.w - 1), h}, r);
        ui_->pop_pane();
    } else {
        panel_table_ = nullptr;
    }
    ui_->renderer().set_opacity(cascade_base_);
    if (!ui_->inert()) {
        draw_send_fly(); // Enter: the request's text on its way from the field into its bubble (MS 05)
    }
}

void Painter::cascade(int index) {
    ui_->renderer().set_opacity(cascade_base_ * motion_.part(mnow_, index));
}

auto Painter::send_part(int index) -> float {
    // MS 05: sent from the bar, the column's head and the data panel come in at their times; a
    // follow-up in an open conversation finds them there.
    const auto& sf = send_frame_;
    const bool comes = send_fresh_;
    const float alpha = !comes ? 1.0f : index == 0 ? sf.head : index == 3 ? sf.data : 1.0f;
    ui_->renderer().set_opacity(cascade_base_ * alpha);
    return comes && index == 3 ? sf.data_dy : 0.0f;
}

void Painter::draw_send_fly() {
    const auto& sf = send_frame_;
    if (sf.fly_alpha <= 0.001f || !gone_para_ || !question_drawn_ || gone_para_->lines().empty()) {
        return;
    }
    // From the field's first line (its size) to the bubble's (the question's own size): place,
    // line height and size all by the one spring; the bubble's own text takes over once it is there.
    const float scale = ui_->scale();
    const auto& sized = gone_para_->fonts().sized(0, gone_para_->px());
    const float to_zoom = question_ ? question_->px() / gone_para_->px() : ui::motion::send::kFlyScale;
    const float zoom = 1.0f + (to_zoom - 1.0f) * sf.fly;
    const auto mix = [&](float a, float b) { return a + (b - a) * sf.fly; };
    const float x = mix(send_from_.x, question_at_.x);
    const float top = mix(send_from_.y, question_at_.y);
    const float line_h = mix(kFieldLine, question_at_.h);
    const float ascent = sized.ascent / scale * zoom;
    const float descent = sized.descent / scale * zoom;
    auto& rr = ui_->renderer();
    const float o = rr.opacity();
    rr.set_opacity(o * sf.fly_alpha);
    const auto& lines = gone_para_->lines();
    const std::size_t shown = std::min<std::size_t>(lines.size(), kQuestionLines);
    float widest = 0;
    for (std::size_t i = 0; i < shown; ++i) {
        const float line_top = top + static_cast<float>(i) * line_h;
        ui_->draw_line_zoomed(*gone_para_, lines[i], {x, line_top + (line_h - ascent - descent) / 2 + ascent}, ui::th().text, zoom);
        widest = std::max(widest, lines[i].width / scale * zoom);
    }
    rr.set_opacity(o);
    add_target("send:fly", {x, top, widest, static_cast<float>(shown) * line_h});
}

void Painter::draw_conversation_row(Model& m, const Box& panel, Reaction& r) {
    std::array<char, 64> buf{};
    status_.clear();
    work::Tone tone = work::Tone::Accent;
    const bool asking = m.permission.has_value();
    const bool busy = working(m) && !asking;
    if (m.task != turn_task_) {
        turn_task_ = m.task;
        turn_seen_at_ = now_;
    }
    // The budget's "Go on?": the task waits for the user — not done, not working on its own.
    const bool waiting = busy && std::ranges::any_of(m.actions, [](const events::Action& a) { return a.id == "continue"; });
    if (asking) {
        status_ = "Needs approval";
        tone = work::Tone::Warn;
    } else if (m.query) { // the model's question waits for the user (phase 12 § 4)
        status_ = "Needs a decision";
        tone = work::Tone::Warn;
    } else if (waiting) {
        status_ = "Waiting for you";
        tone = work::Tone::Warn;
    } else if (busy) {
        const auto run = std::ranges::find(m.steps, events::StepState::Run, &events::Step::state);
        if (run != m.steps.end()) {
            auto [k, n] = std::pair{static_cast<std::size_t>(run - m.steps.begin()) + 1, m.steps.size()};
            if (m.checklist && !m.checklist->items.empty()) {
                std::tie(k, n) = plan_step(*m.checklist); // M from the plan (phase 12 § 3)
            }
            std::format_to(std::back_inserter(status_), "Step {} of {} \xC2\xB7 {}", k, n, clock_text(buf, std::max(0.0, now_ - m.started_at)));
        } else {
            status_ = m.step.empty() ? std::string_view{"Thinking\xE2\x80\xA6"} : std::string_view{m.step}; // what it does now
        }
    } else if (m.report && m.activity == Activity::Answering) {
        status_ = "Report ready";
        tone = work::Tone::Ok;
    } else if (m.activity == Activity::Done && m.failed) {
        status_ = "Failed";
        tone = work::Tone::Crit;
    } else if ((m.activity == Activity::Answering || m.activity == Activity::Done) &&
               std::ranges::any_of(m.refusals, &events::Refusal::whole)) { // files refused on purpose (phase 10, screen 25)
        const auto n = std::ranges::count_if(m.refusals, &events::Refusal::whole);
        std::format_to(std::back_inserter(status_), "{} {} rejected", n, n == 1 ? "file" : "files");
        tone = work::Tone::Warn;
    } else if (m.activity == Activity::Answering || m.activity == Activity::Done) {
        const double took = m.ended_at >= 0 ? m.ended_at - m.started_at : 0.0;
        std::format_to(std::back_inserter(status_), "Done \xC2\xB7 {:.1f} s", std::max(0.0, took));
        tone = work::Tone::Ok;
    }
    std::array<RowChip, 2> chips{};
    std::size_t n = 0;
    if (!status_.empty()) {
        chips[n++] = {status_, tone, "status"};
    }
    if (m.unsafe) {
        chips[n++] = {kUnsafe, work::Tone::Warn, "unsafe"};
    }
    const bool word = asking && (m.permission->advanced || (m.permanent && m.permission->offers_permanent));
    const bool own_answer = m.query && m.answering_other; // the question's "Other": the field is the answer
    const std::string_view placeholder = word         ? std::string_view{"Type ALLOW to allow it, or say what to change\xE2\x80\xA6"}
                                         : asking     ? std::string_view{"Or tell me what to change\xE2\x80\xA6"}
                                         : own_answer ? std::string_view{"Your own answer\xE2\x80\xA6"}
                                         : busy       ? kRefine
                                                      : kFollowUp;
    draw_row(m, panel, r, placeholder, wt::type::placeholder_small, std::span{chips.data(), n},
             busy && !own_answer ? RowButton::Stop : RowButton::Send, false, false, 0, has_detail(m));
    { // .rprog on the bar's bottom edge while the agent works; the work done, it goes (MS 06)
        const float inset = shape_.radius * 0.6f;
        auto& renderer = ui_->renderer();
        renderer.push_clip({panel.x + inset, panel.y, panel.w - 2 * inset, panel.h});
        draw_rail(m, {panel.x, panel.y + row_h_ - wt::bar::progress, panel.w, wt::bar::progress}, busy);
        renderer.pop_clip();
    }
}

void Painter::draw_column(Model& m, const Box& column, Reaction& r) {
    auto& renderer = ui_->renderer();
    // The header: CONVERSATION · its first question, History and New chat on the right.
    const Box header{column.x, column.y + send_part(0), column.w, kHeader};
    add_target("conv:header", header);
    renderer.rect({header.x, header.y + kHeader - 1, header.w, 1}, {.fill = ui::th().line});
    const float ib = 28.0f;
    const Box new_box{header.x + header.w - 12 - ib, header.y + (kHeader - ib) / 2, ib, ib};
    const Box history_box{new_box.x - 6 - ib, new_box.y, ib, ib};
    if (work::icon_button(*ui_, id_of("new_chat"), new_box, kIconNewChat) && !r.command) {
        history_open_ = false;
        conv_scroll_ = {};
        if (auto cancel = new_chat(m)) {
            r.command = std::move(cancel);
        }
        return;
    }
    add_target("new_chat", new_box);
    if (work::icon_button(*ui_, id_of("history"), history_box, kIconHistory)) {
        history_open_ = !history_open_;
    }
    add_target("history", history_box);
    history_button_ = history_box;
    const float lx = header.x + 20;
    const float lw = work::caption(*ui_, {lx, header.y, 140, kHeader}, Face::Semibold, 11.0f, "CONVERSATION", ui::th().text_3);
    const std::string_view name = m.session.empty() ? std::string_view{m.question} : std::string_view{m.session.back().question};
    const float dot = work::caption(*ui_, {lx + lw + 6, header.y, 20, kHeader}, Face::Text, 13.0f, "\xC2\xB7", ui::th().text_4);
    // Phase 12 § 10: past half the history's limit, how full it is, left of the buttons.
    float right_edge = history_box.x - 12;
    if (m.context > 50) {
        std::array<char, 64> cb{};
        const auto said = text(cb, "Context {} %", m.context);
        const float cw = work::text_width(*ui_, Face::Text, 12.0f, said);
        const Box context{right_edge - cw, header.y + (kHeader - 18) / 2, cw, 18};
        work::caption(*ui_, context, Face::Text, 12.0f, said, m.context >= 80 ? ui::th().warn : ui::th().text_3);
        add_target("conv:context", context);
        right_edge = context.x - 12;
    }
    work::caption(*ui_, {lx + lw + 6 + dot + 6, header.y, std::max(0.0f, right_edge - (lx + lw + 6 + dot + 6)), kHeader},
                Face::Text, 13.0f, name, ui::th().text_2);

    // The turns, scrolled on their own.
    const Box area{column.x, column.y + kHeader, column.w, std::max(0.0f, column.h - kHeader)};
    card_room_ = std::max(0.0f, area.h - 24); // a question's card is never taller: past it, its text scrolls in it
    conv_view_ = area;
    const float w = std::min(column.w - 2 * kPadX, (m.word || m.phrase) ? kCardContent : kContentMax);
    const float x = column.x + kPadX;
    Reaction none;
    float content = 0;
    if (!m.question.empty()) {
        content += draw_turn(m, x, 0, w, area, true, none);
    }
    content += draw_earlier(m, column.x, 0, column.w, area, true, none);
    redraw_due_ = redraw_due_ || std::abs(content - conv_content_) > 0.5f;
    conv_content_ = content;
    if (m.task != follow_task_) { // a new turn: follow its running step again
        follow_task_ = m.task;
        conv_follow_ = true;
    }
    if (const auto& in = ui_->input(); in.wheel != 0 && inside(area, in.mouse_x, in.mouse_y)) {
        conv_follow_ = false;
    }
    if (m.task != answer_h_task_) { // another turn's answer: its height as it is (draw_turn's spring)
        answer_h_task_ = m.task;
        anim_.place(kAnswerHeightTrack, doc_ && !m.answer.empty() ? doc_->height() : 0.0f);
        // Its tail too: a turn whole when first seen (the history) has it open; one beginning, shut.
        const bool whole = !working(m) && shown_bytes_ == m.answer.size() && !m.streaming;
        tail_at_ = whole ? now_ - 10.0 : -1.0e9;
        anim_.place(kTailShareTrack, whole ? 1.0f : 0.0f);
        anim_.place(kTailAlphaTrack, whole ? 1.0f : 0.0f);
        anim_.place(kNoActionsTrack, whole && m.steps.empty() && m.data.empty() && !m.answer.empty() && !m.word && !m.phrase && !m.text_translation ? 1.0f : 0.0f);
    }
    const bool wheeled = ui_->input().wheel != 0 && inside(area, ui_->input().mouse_x, ui_->input().mouse_y);
    conv_scroll_.foot = work::corner_foot(bottom_corners(area).br);
    const Box at = work::scroll_view(*ui_, conv_scroll_, area, content);
    if (conv_scroll_.moved_by_user) {
        conv_follow_ = false; // its bar dragged or clicked, a selection scrolling it: as the wheel (final review I4)
    } else if (!conv_follow_ && !wheeled && conv_scroll_.offset >= std::max(0.0f, content - area.h) - 1.0f) {
        conv_follow_ = true; // back at the bottom by the user's own hand: it follows again (MS 07)
    }
    float y = at.y;
    answer_bottom_ = -1.0e9f;
    if (!m.question.empty()) {
        y += draw_turn(m, x, y + send_part(1), w, area, false, r);
    }
    const float turn_bottom = y;
    if (!r.command) {
        (void)draw_earlier(m, column.x, y + send_part(2), column.w, area, false, r);
    }
    ui_->renderer().set_opacity(cascade_base_);
    if (r.page > 0) { // a page link of the answer: that page of its document in the panel (phase 10)
        show_page(m, r.page);
        r.page = 0;
        redraw_due_ = true;
    }
    // MS 07: the scroll follows the text on smooth — to kFollowTail px under its last line — only
    // while the user is at the bottom; scrolled up, the view stays and "New" says there is more.
    if (working(m) || stream_.flowing() || now_ - last_token_at_ < kTurnLiveFor) {
        conv_live_at_ = now_;
    }
    const bool live = now_ - conv_live_at_ < kTurnLiveFor && !m.answer.empty() && !ui_->inert();
    const float under = std::max(answer_bottom_ + ui::motion::stream::kFollowTail, turn_bottom) - (area.y + area.h);
    // (Not while the window still grows to its height: its view is small only for that moment.)
    const bool grown = std::abs(glass_.h - target_.height) < 0.5f;
    if (live && conv_follow_ && send_.done() && grown && under > 0.5f && !conv_scroll_.moved_by_user) {
        work::scroll_to(conv_scroll_, conv_scroll_.offset + under);
    }
    if (conv_follow_ || under <= 8.0f) {
        conv_new_ = false;
    } else if (live) {
        conv_new_ = true;
    }
    work::end_scroll(*ui_, conv_scroll_, area, work::surface(*ui_), bottom_corners(area));
    if (const float shown = anim_.to(kConvNewTrack, conv_new_ ? 1.0f : 0.0f, ui::motion::kQuick); shown > 0.004f && !ui_->inert()) {
        // "New" (MS 07, not in the mockups' screens): a pill at the view's bottom; a click goes down
        // to the text and follows it again.
        constexpr std::string_view kNew = "New";
        const float tw = work::text_width(*ui_, Face::Semibold, 12.5f, kNew);
        const Box pill{area.x + (area.w - (12 + 12 + 6 + tw + 14)) / 2, area.y + area.h - 28 - 12, 12 + 12 + 6 + tw + 14, 28};
        auto& rr = ui_->renderer();
        const float o = rr.opacity();
        rr.set_opacity(o * shown);
        rr.rect({pill.x, pill.y, pill.w, pill.h},
                {.fill = ui::th().pop, .radius = {14, 14, 14, 14}, .border = ui::th().line_2, .border_width = 1});
        ui_->draw_icon(work::kIconChevronDown, 12.0f, {pill.x + 12 + 6, pill.y + pill.h / 2}, ui::th().accent);
        work::caption(*ui_, {pill.x + 12 + 12 + 6, pill.y, tw + 1, pill.h}, Face::Semibold, 12.5f, kNew, ui::th().text);
        rr.set_opacity(o);
        if (conv_new_) {
            add_target("conv:new", pill);
            if (ui_->interact(id_of("conv:new"), pill).clicked) {
                conv_follow_ = true;
                conv_new_ = false;
                work::scroll_to(conv_scroll_, conv_scroll_.offset + under);
            }
        }
    }
    if (history_open_) {
        draw_history(m, column, r);
    }
}

namespace {
const ui::Id kGoingChipAlpha = id_of("attach-going-alpha");
const ui::Id kGoingChipWidth = id_of("attach-going-width");
constexpr std::size_t kGoingChipIndex = 9999; // its widgets' ids: no live chip's
constexpr double kChipContentAfter = std::chrono::duration<double>(ui::motion::field::kChipContentAt).count();
constexpr double kChipWidthBackAfter = std::chrono::duration<double>(ui::motion::field::kChipWidthBackAt).count();
} // namespace

auto Painter::attach_row(const Model& m, View v) const -> float {
    // The last chip removed keeps the row while it fades (micro); then the row goes with the window's motion.
    const bool going = going_chip_.has_value() && now_ - going_chip_at_ < kChipWidthBackAfter;
    const bool any = !m.attachments.empty() || !m.attach_note.empty() || going;
    return any && !m.key_mode && (v == View::Input || conversation_shown(m, v)) ? 64.0f : 0.0f;
}

// The attachments' row (design/ui_remastered screen 17): chips 46 high from x 84 (under the text
// after the drop), 8 apart, at most 300 wide (a refused one 340): a thumbnail or the file's icon,
// its name and what tells it is the right file, the cross. Past the row's edge it scrolls with the
// wheel and "+N" says how many are beyond; only a chip in sight takes a click.
void Painter::draw_attach_row(Model& m, const Box& row) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    constexpr float kLeft = 84, kTop = 14, kH = 46, kGap = 8, kRight = 22;
    constexpr float kPadL = 7, kThumb = 32, kInner = 10, kX = 24, kPadR = 8;
    const ui::render::Color kCrit = ui::th().crit;
    const Box strip{row.x + kLeft, row.y + kTop, std::max(0.0f, row.w - kLeft - kRight), kH};
    const auto width_of = [&](const Attachment& a) {
        const float cap = (a.refused.empty() ? 300.0f : 340.0f) - (kPadL + kThumb + kInner + 4 + kX + kPadR);
        const float tw = std::max(work::text_width(*ui_, Face::Semibold, 13.0f, a.label), work::text_width(*ui_, Face::Text, 11.5f, a.meta));
        return kPadL + kThumb + kInner + std::min(tw, cap) + 4 + kX + kPadR;
    };
    float all = 0;
    for (const auto& a : m.attachments) {
        all += width_of(a) + kGap;
    }
    if (const auto& in = ui_->input(); in.wheel != 0 && inside(strip, in.mouse_x, in.mouse_y)) {
        attach_scroll_ -= in.wheel * 40.0f;
    }
    attach_scroll_ = std::clamp(attach_scroll_, 0.0f, std::max(0.0f, all - kGap - strip.w));
    renderer.push_clip({strip.x, row.y, strip.w, row.h}); // its "Ctrl+V" mark sits 9 above it
    float x = strip.x - attach_scroll_;
    std::size_t beyond = 0;
    const float base = renderer.opacity();
    enum class Did : std::uint8_t { None, Remove, Open };
    // One chip: `shell` its box now (its width grows in and goes, MS § 7), `chip` the box at its own
    // width — its content is laid out in that and cut by the shell; `content` its content's opacity.
    const auto draw_chip = [&](const Attachment& a, std::size_t i, const Box& shell, const Box& chip, bool in_sight, float content) -> Did {
        const bool bad = !a.refused.empty();
        // Its cross first: a press on it is the cross's, not the chip's (the first under the mouse takes it).
        const Box cross{chip.x + chip.w - kPadR - kX, chip.y + (kH - kX) / 2, kX, kX};
        const auto xid = id_of("attach-remove", static_cast<std::uint32_t>(i));
        const auto xhit = in_sight ? ui_->interact(xid, cross) : ui::Interaction{}; // the keyboard reaches it too (phase 10)
        const auto id = id_of("attach-chip", static_cast<std::uint32_t>(i));
        const auto hit = in_sight ? ui_->interact(id, chip, false) : ui::Interaction{};
        const float h = ui_->hover(id, hit.hovered); // the chip under the mouse (120 / 180 ms)
        const float r12 = std::min(12.0f, shell.w / 2);
        renderer.rect({shell.x, shell.y, shell.w, shell.h},
                      {.fill = bad ? ui::th().crit_soft : blend(ui::th().w2, ui::th().w3, h),
                       .radius = {r12, r12, r12, r12},
                       .border = bad ? ui::th().crit_line : blend(ui::th().line, ui::th().line_2, h),
                       .border_width = 1});
        if (in_sight) {
            add_target(text(buf, "attach:{}", i), shell);
        }
        const float o = renderer.opacity();
        renderer.push_clip({shell.x, row.y, shell.w, row.h}); // its content inside it as it grows
        renderer.set_opacity(o * content);
        const Box mark{chip.x + kPadL, chip.y + (kH - kThumb) / 2, kThumb, kThumb};
        if (bad) { // a lock: it is not sent
            renderer.rect({mark.x, mark.y, mark.w, mark.h}, {.fill = ui::th().crit_soft, .radius = {7, 7, 7, 7}});
            ui_->draw_icon(kIconLock, 16.0f, {mark.x + kThumb / 2, mark.y + kThumb / 2}, kCrit);
            if (in_sight) {
                add_target(text(buf, "attach:{}:locked", i), mark);
            }
        } else if (const auto* t = a.thumb ? picture_texture(*a.thumb) : nullptr) {
            renderer.rect({mark.x, mark.y, mark.w, mark.h}, {.fill = ui::th().w2, .radius = {7, 7, 7, 7}});
            renderer.image({mark.x, mark.y, mark.w, mark.h}, {}, t->id(), {255, 255, 255, 255});
            renderer.rect({mark.x, mark.y, mark.w, mark.h}, {.fill = {}, .radius = {7, 7, 7, 7}, .border = ui::th().line_2, .border_width = 1});
        } else {
            const auto dot = a.label.find_last_of('.');
            work::type_icon(*ui_, {mark.x + 4, mark.y + 2, 24, 28},
                            !a.text.empty()          ? std::string_view{"txt"} // a long paste (phase 10, screen 31)
                            : dot == std::string::npos ? std::string_view{"file"}
                                                       : std::string_view{a.label}.substr(dot + 1));
        }
        if (!a.text.empty() && hit.hovered) { // its card: the text's start (drawn over all, draw_paste_preview)
            paste_preview_ = i;
            paste_chip_ = chip;
        }
        const float tx = mark.x + kThumb + kInner;
        const float tw = chip.x + chip.w - kPadR - kX - 4 - tx;
        const float nh = ui_->line_height(Face::Semibold, 13.0f);
        const float mh = ui_->line_height(Face::Text, 11.5f);
        const float ty = chip.y + (kH - nh - 1 - mh) / 2;
        const float nw = work::caption(*ui_, {tx, ty, tw, nh}, Face::Semibold, 13.0f, a.label,
                                     bad ? ui::th().crit : ui::th().text);
        if (bad) { // struck through
            renderer.rect({tx, ty + nh / 2, std::min(nw, tw), 1}, {.fill = ui::th().crit_line});
        }
        work::caption(*ui_, {tx, ty + nh + 1, tw, mh}, Face::Text, 11.5f, a.meta,
                    bad ? ui::th().crit : ui::th().text_3);
        if (bad && hit.hovered) { // why, when the mouse rests on it (asked only then)
            ui_->tooltip(id, chip, "This file won't be sent",
                         a.refused == "secrets" ? "Looks like a file with access keys: .env, *.pem, id_rsa, *.kdbx. The agent doesn't "
                                                  "read its content \xE2\x80\x94 only its name."
                                                : std::string_view{a.refused});
        }
        // Its cross: a circle of its own, lit under the mouse, pressed smaller.
        const float xh = ui_->hover(xid, xhit.hovered);
        const float s = 1.0f - (1.0f - ui::motion::micro::kPress) * ui_->press(xid, xhit.held); // MS 14: one scale
        if (xh > 0.01f) {
            const ui::render::Color lit = with_alpha(bad ? ui::th().crit_soft_hi : ui::th().w3, xh);
            const float d = kX * s;
            renderer.rect({cross.x + (kX - d) / 2, cross.y + (kX - d) / 2, d, d}, {.fill = lit, .radius = {d / 2, d / 2, d / 2, d / 2}});
        }
        ui_->draw_icon(work::kIconClose, 10.0f * s, {cross.x + kX / 2, cross.y + kX / 2}, xh > 0.5f || xhit.focus_ring ? ui::th().text : ui::th().text_3);
        work::focus_ring(*ui_, xid, cross, kX / 2, xhit.focus_ring);
        if (in_sight) {
            add_target(text(buf, "attach:{}:remove", i), cross);
        }
        // Pasted: "Ctrl+V" over it for 2 s.
        // It comes on quick and goes on micro (the map's unnamed: it was there, then not, in a frame).
        const bool pasted_now = now_ - a.pasted_at < std::chrono::duration<double>(ui::motion::hold::kPasted).count();
        if (const float shown = std::clamp(ui_->fade(id ^ 0x5061737465ull, pasted_now), 0.0f, 1.0f); shown > 0.004f) {
            const float bw = work::text_width(*ui_, Face::Semibold, 10.0f, "Ctrl+V") + 12;
            const Box badge{chip.x + 8, chip.y - 9, bw, 16};
            const float before = renderer.opacity();
            renderer.set_opacity(before * shown);
            renderer.rect({badge.x, badge.y, badge.w, badge.h}, {.fill = ui::th().accent, .radius = {8, 8, 8, 8}});
            work::caption(*ui_, {badge.x + 6, badge.y, bw - 6, badge.h}, Face::Semibold, 10.0f, "Ctrl+V", ui::th().on_accent);
            renderer.set_opacity(before);
            add_target(text(buf, "attach:{}:pasted", i), badge);
        }
        if (pasted_now) {
            redraw_due_ = true; // until its two seconds are over
        }
        renderer.set_opacity(o);
        renderer.pop_clip();
        if (xhit.clicked) {
            return Did::Remove;
        }
        if (hit.clicked && !a.text.empty()) { // a pasted text: into the field, the editor open
            return Did::Open;
        }
        return Did::None;
    };

    // The chip just removed (MS § 7): it fades on micro where it was, then its width goes on smooth
    // — the chips after it slide along by themselves.
    float going_gap = 0, going_alpha = 0;
    if (going_chip_) {
        const bool holds = now_ - going_chip_at_ < kChipWidthBackAfter;
        going_alpha = anim_.to(kGoingChipAlpha, 0.0f, ui::motion::kMicro);
        const float left = anim_.to(kGoingChipWidth, holds ? 1.0f : 0.0f, ui::motion::kSmooth);
        if (!holds && left < 0.002f) {
            going_chip_.reset();
        } else {
            going_gap = (going_chip_w_ + kGap) * left;
        }
    }
    for (std::size_t i = 0; i <= m.attachments.size(); ++i) {
        if (going_chip_ && i == std::min(going_chip_index_, m.attachments.size())) {
            if (going_alpha > 0.001f) {
                const auto keep = target_count_;
                const bool was_inert = ui_->inert();
                const std::size_t was_preview = paste_preview_;
                ui_->set_inert(true);
                renderer.set_opacity(base * going_alpha);
                const Box gone{x, strip.y, going_chip_w_, kH};
                (void)draw_chip(*going_chip_, kGoingChipIndex, gone, gone, false, 1.0f);
                renderer.set_opacity(base);
                ui_->set_inert(was_inert);
                paste_preview_ = was_preview;
                target_count_ = keep;
            }
            x += going_gap;
        }
        if (i == m.attachments.size()) {
            break;
        }
        const auto& a = m.attachments[i];
        // New in the row: its width comes from nothing on smooth, its content at +60 ms on quick.
        const std::uint64_t key = (core::hash::fnv1a64(a.path, core::hash::fnv1a64(a.label)) ^ std::bit_cast<std::uint64_t>(a.pasted_at)) | 1u;
        auto seen = std::ranges::find(chip_seen_, key, &std::pair<std::uint64_t, double>::first);
        const bool fresh = seen == chip_seen_.end();
        if (fresh) {
            seen = chip_seen_.begin() + static_cast<std::ptrdiff_t>(chip_seen_next_++ % chip_seen_.size());
            *seen = {key, now_};
        }
        const double age = now_ - seen->second;
        const float grown = anim_.to(id_of("attach-width", key), 1.0f, ui::motion::kSmooth, fresh ? std::optional{0.0f} : std::nullopt);
        const float content = age >= 1.0             ? 1.0f
                              : age >= kChipContentAfter ? anim_.to(id_of("attach-content", key), 1.0f, ui::motion::kQuick, 0.0f)
                                                         : 0.0f;
        const float own = width_of(a);
        const Box shell{x, strip.y, own * grown, kH};
        const Box chip{x, strip.y, own, kH};
        x += (own + kGap) * grown;
        const bool in_sight = shell.x >= strip.x - 0.5f && shell.x + shell.w <= strip.x + strip.w + 0.5f;
        beyond += shell.x + shell.w > strip.x + strip.w + 0.5f ? 1 : 0;
        if (shell.x + shell.w < strip.x || shell.x > strip.x + strip.w) {
            continue;
        }
        const Did did = draw_chip(a, i, shell, chip, in_sight, content);
        if (did == Did::Remove) {
            renderer.pop_clip();
            going_chip_index_ = i;
            going_chip_w_ = shell.w;
            going_chip_at_ = now_;
            anim_.place(kGoingChipAlpha, 1.0f);
            anim_.place(kGoingChipWidth, 1.0f);
            seen->first = 0; // attached again, it comes in again
            going_chip_ = std::move(m.attachments[i]); // as it was; the model's is erased next
            remove_attachment(m, i);
            redraw_due_ = true;
            return; // the rest is drawn next frame
        }
        if (did == Did::Open) {
            renderer.pop_clip();
            open_pasted(m, i);
            paste_preview_ = static_cast<std::size_t>(-1);
            redraw_due_ = true;
            return;
        }
    }
    renderer.pop_clip();
    if (beyond > 0) { // how many are past the edge
        const auto more = text(buf, "+{}", beyond);
        const float mw = work::text_width(*ui_, Face::Semibold, 12.5f, more) + 16;
        const Box b{strip.x + strip.w - mw, strip.y + (kH - 24) / 2, mw, 24};
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = ui::th().pop, .radius = ui::corners(ui::th().radius.label)});
        work::caption(*ui_, {b.x + 8, b.y, mw - 8, b.h}, Face::Semibold, 12.5f, more, ui::th().text_2);
        add_target("attach:more", b);
    }
    if (!m.attach_note.empty()) {
        const float nw = work::text_width(*ui_, Face::Text, 11.5f, m.attach_note);
        work::caption(*ui_, {std::min(x, strip.x + strip.w - nw), row.y + row.h - 14, nw, 12}, Face::Text, 11.5f, m.attach_note,
                    ui::th().text_3);
    }
}

// Files over the window (design/ui_remastered screen 17, the drop): a dashed accent zone inside the
// glass, its icon, "Release to attach" and what it takes.
void Painter::draw_drop_zone(const Box& glass) {
    auto& renderer = ui_->renderer();
    const Box z{glass.x + 10, glass.y + 10, glass.w - 20, glass.h - 20};
    add_target("dropzone", z);
    const ui::render::Color kDash = ui::th().accent;
    // Over the line's own content (the mockup shows only the zone): the surface, then its tint.
    renderer.rect({z.x, z.y, z.w, z.h}, {.fill = ui::th().surface, .radius = {14, 14, 14, 14}});
    renderer.rect({z.x, z.y, z.w, z.h}, {.fill = ui::th().accent_soft, .radius = {14, 14, 14, 14}});
    // Dashes 6 long, 4 apart, 1.5 thick, along the straight part of each edge.
    for (float x = z.x + 14; x + 6 <= z.x + z.w - 14; x += 10) {
        renderer.rect({x, z.y, 6, 1.5f}, {.fill = kDash});
        renderer.rect({x, z.y + z.h - 1.5f, 6, 1.5f}, {.fill = kDash});
    }
    for (float y = z.y + 14; y + 6 <= z.y + z.h - 14; y += 10) {
        renderer.rect({z.x, y, 1.5f, 6}, {.fill = kDash});
        renderer.rect({z.x + z.w - 1.5f, y, 1.5f, 6}, {.fill = kDash});
    }
    constexpr std::string_view kTitle = "Release to attach";
    constexpr std::string_view kSub = "Photos and screenshots \xC2\xB7 PDF \xC2\xB7 Word \xC2\xB7 Excel \xC2\xB7 PowerPoint \xC2\xB7 CSV, TXT, MD";
    const float th = ui_->line_height(Face::Display, 20.0f);
    const float sh = ui_->line_height(Face::Text, 13.0f);
    const float tw = std::max(work::text_width(*ui_, Face::Display, 20.0f, kTitle), work::text_width(*ui_, Face::Text, 13.0f, kSub));
    const float w = std::min(46 + 16 + tw, z.w - 24);
    const float x = z.x + (z.w - w) / 2;
    const float cy = z.y + z.h / 2;
    renderer.rect({x, cy - 23, 46, 46}, {.fill = ui::th().accent_soft_hi, .radius = {12, 12, 12, 12}});
    ui_->draw_icon(kIconAttach, 22.0f, {x + 23, cy}, ui::th().accent);
    work::caption(*ui_, {x + 62, cy - (th + sh) / 2, w - 62, th}, Face::Display, 20.0f, kTitle, ui::th().text);
    work::caption(*ui_, {x + 62, cy - (th + sh) / 2 + th, w - 62, sh}, Face::Text, 13.0f, kSub, ui::th().text_2);
}

auto Painter::picture_texture(const events::Picture& p) -> const ui::render::Texture* {
    for (auto& t : picture_textures_) {
        if (t.picture == p.id) { // not the pixels' address: a new picture may get a freed one
            t.drawn = picture_frame_;
            return &t.texture;
        }
    }
    auto made = ui::render::Texture::rgba(p.thumb_w, p.thumb_h, p.thumb_rgba);
    if (!made) {
        return nullptr;
    }
    if (picture_textures_.size() >= 24) { // the one drawn longest ago gives way
        const auto oldest = std::ranges::min_element(picture_textures_, {}, &PictureTexture::drawn);
        *oldest = PictureTexture{.picture = p.id, .texture = std::move(*made), .drawn = picture_frame_};
        return &oldest->texture;
    }
    picture_textures_.push_back({.picture = p.id, .texture = std::move(*made), .drawn = picture_frame_});
    return &picture_textures_.back().texture;
}

namespace {

constexpr float kSeen = 26.0f;    // .tmb
constexpr float kSeenGap = 4.0f;
constexpr char32_t kIconZoom = 0xE71E;    // the magnifier over a thumbnail under the mouse
constexpr char32_t kIconPicture = 0xEB9F; // flash's pill: answered for a picture
constexpr char32_t kIconPc = 0xE7F4;      // done on this PC, nothing sent (phase 10)
constexpr char32_t kIconText = 0xE8C1;    // a picture's text (phase 10)
constexpr char32_t kIconFolder = 0xE838;  // Show in folder (phase 10)
constexpr char32_t kIconPage = 0xE8A5;    // a window read as text (phase 13a)
constexpr char32_t kIconFolderShut = 0xE8B7; // a folder the answer names
constexpr char32_t kIconDocument = 0xE8A5;   // a file the answer names

auto count_seen(std::span<const events::Picture> pictures) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(pictures, [](const events::Picture& p) { return !p.mine; }));
}

// The part of a thumbnail that covers a box of the ratio `aspect` (cropped in its middle).
auto cover(const events::Picture& p, float aspect) -> ui::render::Uv {
    const float tr = p.thumb_h > 0 ? static_cast<float>(p.thumb_w) / static_cast<float>(p.thumb_h) : aspect;
    if (tr > aspect) {
        const float k = aspect / tr;
        return {(1 - k) / 2, 0, (1 + k) / 2, 1};
    }
    const float k = tr / aspect;
    return {0, (1 - k) / 2, 1, (1 + k) / 2};
}

} // namespace

auto Painter::seen_width(std::span<const events::Picture> pictures) -> float {
    const auto n = count_seen(pictures);
    return n == 0 ? 0.0f : static_cast<float>(n) * (kSeen + kSeenGap) - kSeenGap;
}

auto Painter::draw_mine(std::span<const events::Picture> pictures, float x, float y, float w, bool measure, Reaction& r) -> float {
    constexpr float kW = 120.0f, kH = 84.0f, kGap = 8.0f;
    if (std::ranges::none_of(pictures, &events::Picture::mine)) {
        return 0.0f;
    }
    if (measure) {
        return kH;
    }
    ++picture_frame_;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    float px = x;
    std::size_t k = 0;
    for (const auto& p : pictures) {
        if (!p.mine) {
            continue;
        }
        if (px + kW > x + w) { // no room: how many more
            work::caption(*ui_, {px, y, 60, kH}, Face::Text, 13.0f, text(buf, "+{}", std::ranges::count_if(pictures, &events::Picture::mine) - k),
                        ui::th().text_3);
            break;
        }
        const Box box{px, y, kW, kH};
        add_target(text(buf, "turn:mine:{}", k), box);
        renderer.rect({box.x, box.y, box.w, box.h}, {.fill = ui::th().w2, .radius = {10, 10, 10, 10}});
        if (const auto* tex = picture_texture(p)) {
            renderer.image({box.x, box.y, box.w, box.h}, cover(p, kW / kH), tex->id(), {255, 255, 255, 255});
        }
        renderer.rect({box.x, box.y, box.w, box.h}, {.fill = {}, .radius = {10, 10, 10, 10}, .border = ui::th().line_2, .border_width = 1});
        if (!p.label.empty()) { // its name, bottom left
            const float nw = std::min(work::text_width(*ui_, Face::Text, 11.0f, p.label), kW - 24);
            const float nh = ui_->line_height(Face::Text, 11.0f) + 2;
            renderer.rect({box.x + 6, box.y + kH - 5 - nh, nw + 12, nh}, {.fill = ui::kPictureScrim, .radius = {5, 5, 5, 5}});
            work::caption(*ui_, {box.x + 12, box.y + kH - 5 - nh, nw, nh}, Face::Text, 11.0f, p.label, ui::kOnPicture);
        }
        if (ui_->interact(id_of("mine", static_cast<std::uint32_t>(k)), box, false).clicked) {
            (picture_file(p.path) ? r.open_file : r.not_a_picture) = p.path; // not a picture by its name: never run, a card (phase 10)
        }
        px += kW + kGap;
        ++k;
    }
    return kH;
}

auto Painter::draw_seen(std::span<const events::Picture> pictures, float x, float cy, bool measure, Reaction& r) -> float {
    const float width = seen_width(pictures);
    if (measure || width <= 0) {
        return width;
    }
    ++picture_frame_;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    std::array<char, 96> tip{};
    std::array<char, 64> sub{};
    float px = x;
    std::size_t k = 0;
    for (const auto& p : pictures) {
        if (p.mine) {
            continue;
        }
        const Box box{px, cy - kSeen / 2, kSeen, kSeen};
        px += kSeen + kSeenGap;
        add_target(text(buf, "turn:seen:{}", k), box);
        const auto id = id_of("seen", static_cast<std::uint32_t>(k));
        ++k;
        const auto hit = ui_->interact(id, box); // the keyboard reaches it too (phase 10)
        const float h = ui_->hover(id, hit.hovered);
        const float s = 1.0f - (1.0f - ui::motion::micro::kPress) * ui_->press(id, hit.held); // pressed: .96 (MS 14)
        const Box b{box.x + kSeen * (1 - s) / 2, box.y + kSeen * (1 - s) / 2, kSeen * s, kSeen * s};
        if (h > 0.01f) { // the accent's 2 px ring
            renderer.rect({b.x - 2, b.y - 2, b.w + 4, b.h + 4},
                          {.fill = {}, .radius = {8, 8, 8, 8}, .border = with_alpha(ui::th().accent, h), .border_width = 2});
        }
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = ui::th().w2, .radius = {6, 6, 6, 6}});
        if (const auto* tex = picture_texture(p)) {
            renderer.image({b.x, b.y, b.w, b.h}, cover(p, 1.0f), tex->id(), {255, 255, 255, 255});
        }
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = {}, .radius = {6, 6, 6, 6}, .border = ui::th().line_2, .border_width = 1});
        if (h > 0.01f) { // a magnifier on it, dimmed by 45 %
            renderer.rect({b.x, b.y, b.w, b.h}, {.fill = faint(ui::kPictureScrim, h), .radius = {6, 6, 6, 6}});
            ui_->draw_icon(kIconZoom, 14.0f, {b.x + b.w / 2, b.y + b.h / 2}, with_alpha(ui::kOnPicture, h));
        }
        const bool file = picture_file(p.path);
        if (hit.hovered) { // "Open arch.png" · "1600 × 1120 · PNG"; a screen or a pasted one is only named
            const auto r1 = std::format_to_n(tip.data(), tip.size(), "{}{}", file ? "Open " : "", p.label);
            const int sw = p.source_w > 0 ? p.source_w : p.width;
            const int sh = p.source_h > 0 ? p.source_h : p.height;
            const auto r2 = p.format.empty() ? std::format_to_n(sub.data(), sub.size(), "{} \xC3\x97 {}", sw, sh)
                                             : std::format_to_n(sub.data(), sub.size(), "{} \xC3\x97 {} \xC2\xB7 {}", sw, sh, p.format);
            ui_->tooltip(id, box, {tip.data(), static_cast<std::size_t>(r1.out - tip.data())},
                         {sub.data(), static_cast<std::size_t>(r2.out - sub.data())});
        }
        work::focus_ring(*ui_, id, b, 6, hit.focus_ring);
        if (hit.clicked && !p.path.empty()) {
            (file ? r.open_file : r.not_a_picture) = p.path; // not a picture by its name: never run, a card (phase 10)
        }
    }
    return width;
}

void Painter::refuse_picture(Model& m, Reaction& r) {
    if (r.not_a_picture.empty()) {
        return;
    }
    const auto at = r.not_a_picture.find_last_of("\\/");
    const std::string name = at == std::string::npos ? r.not_a_picture : r.not_a_picture.substr(at + 1);
    const auto title = "Thumbnail not opened: " + name;
    if (std::ranges::any_of(m.refusals, [&](const events::Refusal& c) { return c.title == title; })) {
        return; // one card, however often it is clicked
    }
    const auto ext_of = [](std::string_view n) {
        const auto dot = n.rfind('.');
        std::string e{dot == std::string_view::npos ? std::string_view{} : n.substr(dot)};
        std::ranges::transform(e, e.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        return e;
    };
    const auto ext = ext_of(name);
    const bool program = program_file(name);
    const auto stem = std::string_view{name}.substr(0, name.size() - ext.size());
    const bool disguised = program && picture_file(stem);
    m.refusals.push_back({.kind = "not_an_image",
                          .label = "Not an image",
                          .title = title,
                          .line = disguised ? "a program disguised as a photo" : program ? "a program, not a picture" : "not a picture by its name",
                          .advice = std::string{"Thumbnails open only image files: .png .jpg .jpeg .gif .bmp .tiff .webp. "} +
                                    (program ? "This one is a program, so it was not run." : "This one was not opened."),
                          .whole = false});
    redraw_due_ = true;
}

auto Painter::draw_created(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    if (m.created.empty() || shown_bytes_ != m.answer.size()) {
        return 0;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    float y = y0;
    for (std::size_t i = 0; i < m.created.size(); ++i) {
        const auto& c = m.created[i];
        const float h = c.kept.empty() ? 98.0f : 126.0f;
        if (!measure) {
            const Box card{x, y, w, h};
            const bool on = c.preview != 0 && m.active_set == c.preview;
            // Its buttons first: they take their clicks before the card.
            const auto open_size = work::button_size(*ui_, "Open", true);
            const Box open{x + 62, y + 60, open_size.w, open_size.h};
            const auto folder_size = work::button_size(*ui_, "Show in folder", true);
            const Box folder{open.x + open.w + 8, open.y, folder_size.w + 20, folder_size.h};
            add_target(text(buf, "file:{}:open", i), open);
            if (work::button(*ui_, id_of("file:open", i), open, "Open", work::ButtonStyle::Primary, true)) {
                r.open_created = c.path;
            }
            add_target(text(buf, "file:{}:folder", i), folder);
            const auto fhit = ui_->interact(id_of("file:folder", i), folder, false);
            const auto hit = ui_->interact(id_of("file", i), card, false);
            renderer.rect({card.x, card.y, card.w, card.h},
                          {.fill = on ? ui::th().accent_soft : under_mouse(ui::th().card, lit(id_of("file", i), hit.hovered)),
                           .radius = ui::card_corners(), .border = on ? ui::th().accent_line : ui::th().line, .border_width = 1});
            // The file: a page with its type's mark.
            const Box doc{card.x + 16, card.y + 14, 30, 38};
            renderer.rect({doc.x, doc.y, doc.w, doc.h}, {.fill = ui::th().f_page, .radius = {4, 9, 4, 4}, .border = ui::th().line_2, .border_width = 1});
            const auto mark = work::tag_ink(c.type); // as every file row's mark: its ground and its letters' ink
            const float tw = work::text_width(*ui_, Face::Semibold, 9.0f, c.type) + 8;
            renderer.rect({doc.x - 4, doc.y + 20, tw, 13}, {.fill = mark.ground, .radius = {3, 3, 3, 3}});
            work::caption(*ui_, {doc.x, doc.y + 20, tw - 4, 13}, Face::Semibold, 9.0f, c.type, mark.ink);
            work::caption(*ui_, {card.x + 62, card.y + 14, card.w - 74, 20}, Face::Semibold, 13.5f, c.name, ui::th().text);
            work::caption(*ui_, {card.x + 62, card.y + 34, card.w - 74, 17}, Face::Text, 12.0f, text(buf, "{} \xC2\xB7 {}", c.size, c.folder),
                        ui::th().text_3);
            // Show in folder: Explorer with the file selected.
            renderer.rect({folder.x, folder.y, folder.w, folder.h}, {.fill = blend(ui::th().w2, ui::th().w3, lit(id_of("file:folder", i), fhit.hovered)),
                                                                     .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
            ui_->draw_icon(kIconFolder, 12.0f, {folder.x + 10 + 6, folder.y + folder.h / 2}, ui::th().text_2);
            work::caption(*ui_, {folder.x + 10 + 12 + 6, folder.y, folder.w - 34, folder.h}, Face::Semibold, 12.0f, "Show in folder", ui::th().text_2);
            if (fhit.clicked) {
                r.show_in_folder = c.path;
            }
            // What it kept.
            float kx = card.x + 62;
            for (std::size_t k = 0; k < c.kept.size(); ++k) {
                const float kw = work::text_width(*ui_, Face::Text, 11.0f, c.kept[k]) + 16;
                if (kx + kw > card.x + card.w - 12) {
                    break;
                }
                const Box chip{kx, card.y + 96, kw, 20};
                renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {6, 6, 6, 6}});
                work::caption(*ui_, {chip.x + 8, chip.y, kw - 8, chip.h}, Face::Text, 11.0f, c.kept[k], ui::th().text_3);
                add_target(text(buf, "file:{}:kept:{}", i, k), chip);
                kx += kw + 6;
            }
            add_target(text(buf, "file:{}", i), card);
            if (hit.clicked && !fhit.clicked && c.preview != 0) {
                show_set(m, c.preview);
                redraw_due_ = true;
            }
        }
        y += h + 8;
    }
    return y - y0;
}

auto Painter::draw_places(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    if (m.places.empty() || m.streaming || shown_bytes_ != m.answer.size()) {
        return 0;
    }
    constexpr float kRow = 52;
    constexpr float kName = 50; // the name's left, after its icon
    constexpr double kCopiedFor = std::chrono::duration<double>(ui::motion::hold::kCopied).count();
    std::array<char, 64> buf{};
    const auto open_size = work::button_size(*ui_, "Open", true);
    const auto folder_size = work::button_size(*ui_, "Show in folder", true);
    const auto copy_size = work::button_size(*ui_, "Copy path", true);
    const float buttons = open_size.w + 6 + folder_size.w + 6 + copy_size.w; // ~260: the panel is never under 800 wide
    float y = y0;
    for (std::size_t i = 0; i < m.places.size(); ++i) {
        const auto& p = m.places[i];
        if (!measure) {
            auto& renderer = ui_->renderer();
            const Box row{x, y, w, kRow};
            const float by = y + (kRow - open_size.h) / 2;
            const float bx = x + w - 10 - buttons;
            const Box open{bx, by, open_size.w, open_size.h};
            const Box folder{open.x + open.w + 6, by, folder_size.w, folder_size.h};
            const Box copy{folder.x + folder.w + 6, by, copy_size.w, copy_size.h};
            renderer.rect({row.x, row.y, row.w, row.h}, {.fill = ui::th().card, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
            ui_->draw_icon(p.folder ? kIconFolderShut : kIconDocument, 16.0f, {x + 12 + 14, y + kRow / 2}, ui::th().text_2);
            const float room = bx - 10 - (x + kName);
            work::caption(*ui_, {x + kName, y + 8, room, 20}, Face::Semibold, 13.5f, p.name, ui::th().text);
            work::caption(*ui_, {x + kName, y + 27, room, 17}, Face::Text, 12.0f, p.where, ui::th().text_3);
            add_target(text(buf, "place:{}:open", i), open);
            if (work::button(*ui_, id_of("place:open", i), open, "Open", work::ButtonStyle::Primary, true)) {
                r.open_place = p.path;
            }
            add_target(text(buf, "place:{}:folder", i), folder);
            if (work::button(*ui_, id_of("place:folder", i), folder, "Show in folder", work::ButtonStyle::Normal, true)) {
                r.show_place = p.path;
            }
            add_target(text(buf, "place:{}:copy", i), copy);
            const bool copied = place_copied_ == i && now_ - place_copied_at_ < kCopiedFor;
            // "Copy path" and "Copied" cross on quick (the map's unnamed: the word changed in a frame):
            // the one that goes is a picture under the one that is pressed.
            const float turned = std::clamp(anim_.to(id_of("place:copied", i), copied ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f);
            const float copy_opacity = renderer.opacity();
            if (turned > 0.004f && turned < 0.996f) {
                const bool was_inert = ui_->inert();
                renderer.set_opacity(copy_opacity * (copied ? 1.0f - turned : turned));
                ui_->set_inert(true);
                (void)work::button(*ui_, id_of("place:copy:going", i), copy, copied ? "Copy path" : "Copied", work::ButtonStyle::Normal, true);
                ui_->set_inert(was_inert);
            }
            renderer.set_opacity(copy_opacity * (copied ? turned : 1.0f - turned));
            const bool copy_pressed = work::button(*ui_, id_of("place:copy", i), copy, copied ? "Copied" : "Copy path", work::ButtonStyle::Normal, true);
            renderer.set_opacity(copy_opacity);
            if (copy_pressed) {
                r.clipboard = p.path;
                place_copied_ = i;
                place_copied_at_ = now_;
                ui_->request_frame();
            }
            if (copied) {
                ui_->request_frame(); // "Copied" goes back to "Copy path"
            }
            add_target(text(buf, "place:{}", i), row);
        }
        y += kRow + 8;
    }
    return y - y0;
}

namespace {

constexpr float kRefusalSafe = 34;
constexpr char32_t kIconShield = 0xEA18;
constexpr char32_t kIconWarning = 0xE7BA;

} // namespace

auto Painter::draw_looked(Model& m, float x, float y0, float w, bool measure) -> float {
    // design/ui_system .looked: from 48 high, a 22 icon, a 64 x 40 thumbnail (radius 6); --read at
    // 6 % with a 26 % ring; a sensitive window grey (3.5 %, --line2) with a lock.
    constexpr float kLine = 52.0f, kGap = 8.0f, kThumbW = 64.0f, kThumbH = 40.0f;
    if (m.looked.empty()) {
        return 0.0f;
    }
    // MS § 7 "Looked at": a line that comes while its task works — its room on smooth, its words
    // on quick from kWordsAt, up from kLift px, its thumbnail on quick from kThumbAt. A line that
    // was there when its turn was first drawn is as it is.
    namespace lm = ui::motion::looked;
    if (looked_task_ != m.task) {
        looked_task_ = m.task;
        looked_seen_ = working(m) ? 0 : m.looked.size();
        looked_at_.fill(-1.0e9);
    }
    for (; looked_seen_ < m.looked.size(); ++looked_seen_) {
        if (looked_seen_ < looked_at_.size()) {
            looked_at_[looked_seen_] = reduced_motion_ ? -1.0e9 : mnow_;
        }
    }
    struct Coming {
        float room = 1, words = 1, lift = 0, thumb = 1;
    };
    const auto coming = [&](std::size_t i) {
        const double age = i < looked_at_.size() ? mnow_ - looked_at_[i] : 1.0e9;
        if (age > 5.0) {
            return Coming{};
        }
        const auto seconds = [](std::chrono::milliseconds ms) { return std::chrono::duration<double>(ms).count(); };
        const ui::Id id = id_of("looked:coming", (m.task * 8 + i) * 4); // four tracks a line: a line's are never its neighbour's
        const bool words_due = age >= seconds(lm::kWordsAt) - 1e-9;
        const bool thumb_due = age >= seconds(lm::kThumbAt) - 1e-9;
        Coming c;
        c.room = std::clamp(anim_.to(id, 1.0f, ui::motion::kSmooth, 0.0f), 0.0f, 1.0f);
        c.words = std::clamp(anim_.to(id + 1, words_due ? 1.0f : 0.0f, ui::motion::kQuick, 0.0f), 0.0f, 1.0f);
        c.lift = anim_.to(id + 2, words_due ? 0.0f : lm::kLift, ui::motion::kBase, lm::kLift);
        c.thumb = std::clamp(anim_.to(id + 3, thumb_due ? 1.0f : 0.0f, ui::motion::kQuick, 0.0f), 0.0f, 1.0f);
        if (!thumb_due) {
            redraw_due_ = true; // their turns come on frames
        }
        return c;
    };
    float total = 4.0f;
    for (std::size_t i = 0; i < m.looked.size(); ++i) {
        total += (kLine + kGap) * coming(i).room;
    }
    if (measure) {
        return total;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    // The picture that went to the model: the newest one the tool labelled "window".
    const auto shot = std::ranges::find_if(m.pictures.rbegin(), m.pictures.rend(), [](const events::Picture& p) { return !p.mine && p.label == "window"; });
    float y = y0;
    const float looked_opacity = renderer.opacity();
    for (std::size_t i = 0; i < m.looked.size(); ++i) {
        const auto& l = m.looked[i];
        const Coming in = coming(i);
        const Box room{x, y, w, kLine * in.room}; // as much of the line as there is now
        const float step = (kLine + kGap) * in.room;
        add_target(text(buf, "looked:{}", i), room);
        if (room.h <= 1.0f) {
            y += step;
            continue;
        }
        const Box line{x, y + in.lift, w, kLine}; // its words' place: kLift px under, coming up
        const Color tint = ui::th().w2;
        const Color ring = ui::th().line;
        const float corner = std::min(12.0f, room.h / 2);
        renderer.rect({room.x, room.y, room.w, room.h}, {.fill = tint, .radius = {corner, corner, corner, corner}, .border = ring, .border_width = 1});
        renderer.push_clip({room.x, room.y, room.w, room.h});
        renderer.set_opacity(looked_opacity * in.words);
        const float cy = line.y + line.h / 2;
        float tx = line.x + 14;
        if (l.sensitive) {
            const Box lock{tx, cy - 11, 22, 22};
            ui_->draw_icon(kIconLock, 16.0f, {lock.x + 11, cy}, ui::th().text_2);
            add_target(text(buf, "looked:{}:lock", i), lock);
            tx += 22 + 12;
            work::caption(*ui_, {tx, line.y + 8, line.w - (tx - line.x) - 14, 18}, Face::Semibold, 13.0f, "Sensitive window \xE2\x80\x94 not looked at",
                        ui::th().text);
            std::array<char, 256> why{};
            const auto said = std::format_to_n(why.data(), why.size(), "Not looked at or read: {}", l.reason.empty() ? "it holds sensitive information" : l.reason);
            work::caption(*ui_, {tx, line.y + 27, line.w - (tx - line.x) - 14, 17}, Face::Text, 12.0f,
                        {why.data(), static_cast<std::size_t>(said.out - why.data())}, ui::th().text_3);
        } else {
            ui_->draw_icon(work::kIconEye, 16.0f, {tx + 11, cy}, ui::th().text_2);
            tx += 22 + 12;
            const Box thumb{line.x + line.w - 14 - kThumbW, cy - kThumbH / 2, kThumbW, kThumbH};
            renderer.set_opacity(looked_opacity * in.thumb); // its thumbnail: its own turn, a little later
            if (in.thumb <= 0.01f) {
                // not yet
            } else if (l.picture && shot != m.pictures.rend()) {
                renderer.rect({thumb.x, thumb.y, thumb.w, thumb.h}, {.fill = ui::th().w2, .radius = {6, 6, 6, 6}});
                if (const auto* tex = picture_texture(*shot)) {
                    renderer.image({thumb.x, thumb.y, thumb.w, thumb.h}, cover(*shot, kThumbW / kThumbH), tex->id(), {255, 255, 255, 255});
                }
                renderer.rect({thumb.x, thumb.y, thumb.w, thumb.h}, {.fill = {}, .radius = {6, 6, 6, 6}, .border = ui::th().line_2, .border_width = 1});
                add_target(text(buf, "looked:{}:thumb", i), thumb);
            } else { // read as text: a page instead of the picture
                renderer.rect({thumb.x, thumb.y, thumb.w, thumb.h}, {.fill = ui::th().w2, .radius = {6, 6, 6, 6}});
                ui_->draw_icon(kIconPage, 16.0f, {thumb.x + thumb.w / 2, cy}, ui::th().text_2);
                add_target(text(buf, "looked:{}:text", i), thumb);
            }
            renderer.set_opacity(looked_opacity * in.words);
            const float tw = std::max(0.0f, thumb.x - 12 - tx);
            const float lw = work::caption(*ui_, {tx, line.y + 8, 80, 18}, Face::Semibold, 13.0f, "Looked at", ui::th().text);
            std::array<char, 512> what{};
            const auto named = l.title.empty() || l.program.empty()
                                   ? std::format_to_n(what.data(), what.size(), "{}{}", l.program, l.title)
                                   : std::format_to_n(what.data(), what.size(), "{} \xE2\x80\x94 {}", l.program, l.title);
            work::caption(*ui_, {tx + lw + 6, line.y + 8, std::max(0.0f, tw - lw - 6), 18}, Face::Text, 13.0f,
                        {what.data(), static_cast<std::size_t>(named.out - what.data())}, ui::th().text_2);
            work::caption(*ui_, {tx, line.y + 27, tw, 17}, Face::Text, 12.0f,
                        l.picture ? "A picture of your window went to the model" : "Read your window's text: its buttons, fields and labels",
                        ui::th().text_3);
        }
        renderer.set_opacity(looked_opacity);
        renderer.pop_clip();
        y += step;
    }
    return total;
}

auto Painter::draw_refusals(Model& m, float x, float y0, float w, bool measure) -> float {
    if (m.refusals.empty() || working(m)) {
        return 0;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    float y = y0;
    // Nothing broke: what was not done to the refused files, and what was read.
    safe_note_ = "No file was opened in full";
    if (std::ranges::none_of(m.steps, [](const events::Step& s) { return s.tool == "shell.run"; })) {
        safe_note_ += " and nothing was run";
    }
    safe_note_ += '.';
    bool first = true;
    for (const auto& s : m.steps) {
        if (!s.file.empty()) {
            safe_note_ += first ? " I read only " : ", ";
            safe_note_ += s.file;
            first = false;
        }
    }
    if (!first) {
        safe_note_ += '.';
    }
    if (!measure) {
        const Box safe{x, y, w, kRefusalSafe};
        renderer.rect({safe.x, safe.y, safe.w, safe.h},
                      {.fill = ui::th().ok_soft, .radius = {10, 10, 10, 10}, .border = ui::th().ok_line, .border_width = 1});
        ui_->draw_icon(kIconShield, 12.0f, {safe.x + 14 + 6, safe.y + safe.h / 2}, ui::th().ok);
        const float bw = work::caption(*ui_, {safe.x + 34, safe.y, 120, safe.h}, Face::Semibold, 12.5f, "Nothing broke.", ui::th().text);
        work::caption(*ui_, {safe.x + 34 + bw + 5, safe.y, std::max(0.0f, safe.w - 34 - bw - 17), safe.h}, Face::Text, 12.5f, safe_note_,
                    ui::th().text_2);
        add_target("refusal:safe", safe);
    }
    y += kRefusalSafe + 10;
    const float lh = ui_->line_height(Face::Text, 12.5f) * 1.35f;
    for (std::size_t i = 0; i < m.refusals.size(); ++i) {
        const auto& c = m.refusals[i];
        const auto [a, b] = wrap2(Face::Text, 12.5f, c.advice, w - 52 - 16);
        const float h = 74 + lh * (b.empty() ? 1.0f : 2.0f) + 12;
        if (!measure) {
            const Box card{x, y, w, h};
            renderer.rect({card.x, card.y, card.w, card.h},
                          {.fill = ui::th().card, .radius = ui::card_corners(), .border = ui::th().crit_line, .border_width = 1});
            renderer.rect({card.x + 14, card.y + 12, 26, 26}, {.fill = ui::th().crit_soft, .radius = {8, 8, 8, 8}});
            ui_->draw_icon(kIconWarning, 13.0f, {card.x + 27, card.y + 25}, ui::th().crit);
            const float tx = card.x + 52;
            const float pw = work::text_width(*ui_, Face::Semibold, 11.0f, c.label) + 14;
            renderer.rect({tx, card.y + 12, pw, 18}, {.fill = ui::th().crit_soft, .radius = {6, 6, 6, 6}});
            work::caption(*ui_, {tx + 7, card.y + 12, pw - 7, 18}, Face::Semibold, 11.0f, c.label, ui::th().crit);
            work::caption(*ui_, {tx, card.y + 34, card.w - 52 - 16, 20}, Face::Semibold, 13.5f, c.title, ui::th().text);
            work::caption(*ui_, {tx, card.y + 54, card.w - 52 - 16, 16}, Face::Mono, 11.5f, c.line, ui::th().text_3);
            work::caption(*ui_, {tx, card.y + 74, card.w - 52 - 16, lh}, Face::Text, 12.5f, a, ui::th().text_2);
            if (!b.empty()) {
                work::caption(*ui_, {tx, card.y + 74 + lh, card.w - 52 - 16, lh}, Face::Text, 12.5f, b, ui::th().text_2);
            }
            add_target(text(buf, "refusal:{}", i), card);
        }
        y += h + 8;
    }
    return y - y0 + 4;
}
auto Painter::draw_turn(Model& m, float x, float y0, float w, const Box& visible, bool measure, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    const float scale = ui_->scale();
    const float ts = ui_->text_scale();
    std::array<char, 64> buf{};
    float y = y0 + kPadTop;
    const bool busy = working(m);
    if (!measure) {
        ui_->order_base(1ull << 40); // the turn's texts' keys (final review C1): its answer's blocks take theirs under it
    }

    // Who asked, and the question in its bubble.
    const auto& sf = send_frame_; // Enter's scene (MS 05): at rest but for a moment after a send
    const float turn_opacity = measure ? 1.0f : renderer.opacity();
    const auto who = [&](bool agent, std::string_view name) {
        if (!measure) {
            renderer.set_opacity(turn_opacity * sf.who);
            if (agent) { // the mini orb, 30 (margin -4)
                // The model that made it: its Spent says; still working, the one it was sent to (asked_model).
                avatar_image({x - 4, y - 4, 30, 30}, model_index(m.spent ? std::string_view{m.spent->model} : m.asked_model));
                add_target("turn:avatar", {x - 4, y - 4, 30, 30});
            } else {
                renderer.rect({x, y, kWho, kWho}, {.fill = ui::th().w3, .radius = {11, 11, 11, 11}});
                const float iw = work::text_width(*ui_, Face::Semibold, 11.0f, "Y");
                work::caption(*ui_, {x + (kWho - iw) / 2, y, iw + 1, kWho}, Face::Semibold, 11.0f, "Y", ui::th().text_2);
            }
            const float nw = work::caption(*ui_, {x + kWho + 9, y, 120, kWho}, Face::Semibold, 13.0f, name, ui::th().text_2);
            work::caption(*ui_, {x + kWho + 9 + nw + 8, y, 80, kWho}, Face::Text, 12.0f, m.time, ui::th().text_4);
            renderer.set_opacity(turn_opacity);
        }
        y += kWho + 9;
    };
    y += draw_switched(m.switched_to, x, y, w, measure); // stage 4c: a model the user changed to
    who(false, "You");
    const float bubble_text = 14.5f * ts;
    if (!question_ || question_text_ != m.question || question_scale_ != scale * ts) {
        question_.emplace(fonts_->text, bubble_text * scale);
        question_->set_text(m.question);
        question_text_ = m.question;
        question_scale_ = scale * ts;
    }
    question_->set_width((w - 28 - 2) * scale);
    // A long one (screen 30): four lines, the last cut with "…", and Show more; Show less once open.
    const std::size_t all = question_->lines().size();
    const bool folds = all > kQuestionLines;
    const bool opened = folds && question_open_ == m.task;
    const std::size_t shown = folds && !opened ? kQuestionLines : all;
    question_link_ = !folds ? std::string_view{} : opened ? std::string_view{"Show less"} : std::string_view{"Show more"};
    const float link_w = folds ? 12 + 5 + work::text_width(*ui_, Face::Text, 12.5f, question_link_) : 0.0f;
    float qw = link_w;
    for (std::size_t i = 0; i < shown; ++i) {
        qw = std::max(qw, question_->lines()[i].width / scale);
    }
    const float q_line = 14.5f * 1.5f * ts;
    const float link_h = folds ? 6 + 18 : 0.0f;
    // Show more / Show less (the map's unnamed): its room — the lines it shows, and its width — goes
    // on smooth; while it does, every line that fits is drawn, cut by the bubble.
    const float lines_now = folds ? anim_.to(id_of("question:lines", m.task), static_cast<float>(shown), ui::motion::kSmooth) : static_cast<float>(shown);
    const bool unfolding = folds && std::abs(lines_now - static_cast<float>(shown)) > 0.01f;
    if (folds) {
        qw = anim_.to(id_of("question:width", m.task), qw, ui::motion::kSmooth);
    }
    const std::size_t drawn = unfolding ? std::min(all, static_cast<std::size_t>(std::ceil(std::max(lines_now, static_cast<float>(kQuestionLines))))) : shown;
    const Box bubble{x, y, qw + 28 + 2, lines_now * q_line + 20 + 2 + link_h};
    if (!measure) {
        add_target("turn:question", bubble);
        // MS 05: its ground comes under the text; the text itself is the flying layer's until that
        // has landed (the front layer's only: a layer fading out has no flight).
        renderer.set_opacity(turn_opacity * sf.bubble);
        renderer.rect({bubble.x, bubble.y, bubble.w, bubble.h},
                      {.fill = ui::th().you, .radius = ui::card_corners(), .border = ui::th().you_line, .border_width = 1});
        const bool flies = gone_para_.has_value() && !ui_->inert();
        // Where it lands: here, once the turn stands unscrolled (while the window is still small
        // the turn is scrolled to its running step — the flight keeps to the place it will have).
        if (flies && conv_scroll_.offset < 0.5f) {
            question_at_ = {bubble.x + 15, bubble.y + 11, qw, q_line};
            question_drawn_ = true;
        }
        renderer.set_opacity(turn_opacity * (flies ? 1.0f - sf.fly_alpha : 1.0f));
        const auto& sized = fonts_->text.sized(0, bubble_text * scale);
        const float ellipsis = work::text_width(*ui_, Face::Text, bubble_text, "\xE2\x80\xA6");
        const auto question_key = ui_->next_key(); // its lines: one text
        if (unfolding) {
            renderer.push_clip({bubble.x, bubble.y, bubble.w, 11 + lines_now * q_line});
        }
        for (std::size_t i = 0; i < drawn; ++i) {
            const float top = bubble.y + 11 + static_cast<float>(i) * q_line;
            const float base = top + (q_line - (sized.ascent + sized.descent) / scale) / 2 + sized.ascent / scale;
            const bool cut = folds && !opened && !unfolding && i + 1 == shown; // the last shown: room for "…" after it
            const float room = qw - ellipsis;
            if (cut) {
                renderer.push_clip({bubble.x + 15, top, room, q_line});
            }
            ui_->draw_line(*question_, question_->lines()[i], {bubble.x + 15, base}, ui::th().text, {.key = question_key});
            if (cut) {
                renderer.pop_clip();
                const float end = std::min(question_->lines()[i].width / scale, room);
                work::caption(*ui_, {bubble.x + 15 + end, top, ellipsis + 2, q_line}, Face::Text, bubble_text, "\xE2\x80\xA6", ui::th().text);
            }
        }
        if (unfolding) {
            renderer.pop_clip();
        }
        if (folds) {
            const Box link{bubble.x + 15, bubble.y + 11 + lines_now * q_line + 6, link_w, 18};
            const auto in = ui_->interact(id_of("turn:question:more", 0), link, false);
            const Color link_ink = blend(ui::th().accent, ui::th().text, lit(id_of("turn:question:more", 0), in.hovered));
            ui_->draw_icon(opened ? widgets::kIconChevronUp : work::kIconChevronDown, 12.0f, {link.x + 6, link.y + link.h / 2}, link_ink);
            work::caption(*ui_, {link.x + 12 + 5, link.y, link.w - 17, link.h}, Face::Text, 12.5f, question_link_, link_ink);
            add_target("turn:question:more", link);
            if (in.clicked) {
                question_open_ = opened ? 0 : m.task;
                redraw_due_ = true; // the turn is measured again
            }
        }
        renderer.set_opacity(turn_opacity);
    }
    y += bubble.h + 18;
    // The user's own pictures under their question (phase 10, screen 18).
    if (const float mh = draw_mine(m.pictures, x, y - 8, w, measure, r); mh > 0) {
        y += mh + 10;
    }

    // Stage 4c (screen 05): where it went instead of the model chosen, over the agent's words.
    y += draw_routes(m.routes, x, y, std::min(w, kContentMax), measure);
    // The agent: its actions, its answer.
    who(true, "Agent");
    if (m.checklist && !m.checklist->items.empty()) { // the plan over the steps (phase 12 § 3)
        y += draw_checklist(*m.checklist, x, y, w, measure) + 10;
    }
    // MS 06: the live block (Working · step k of n · 0:07, then each step) and, the work done, its
    // fold into its line — the list goes (micro), the height follows (smooth, +40 ms), "Working"
    // gives way to the line's words. The height is one spring: what is under the block moves with it.
    const float full_h = kActionsLine + 8 + static_cast<float>(m.steps.size()) * (kActionRow + 2) - 2 + 10;
    float act_h = busy ? full_h : kActionsLine;
    ActionsFold fold{.list = busy ? 1.0f : 0.0f, .live_words = busy ? 1.0f : 0.0f, .folded_words = busy ? 0.0f : 1.0f};
    if (!m.steps.empty()) {
        if (m.task != actions_task_) { // another turn's block: as it is
            actions_task_ = m.task;
            actions_busy_ = busy;
            actions_done_at_ = -1.0e9;
            anim_.place(kActHeightTrack, act_h);
            anim_.place(kActListTrack, fold.list);
            anim_.place(kActLiveWordsTrack, fold.live_words);
            anim_.place(kActFoldedWordsTrack, fold.folded_words);
        }
        if (actions_busy_ && !busy) {
            actions_done_at_ = now_;
        }
        actions_busy_ = busy;
        const double since = now_ - actions_done_at_;
        act_h = anim_.to(kActHeightTrack, busy || since < kFoldAfter ? full_h : kActionsLine, ui::motion::kSmooth);
        fold.list = anim_.to(kActListTrack, busy ? 1.0f : 0.0f, ui::motion::kMicro);
        fold.live_words = anim_.to(kActLiveWordsTrack, busy || since < kFoldOldWordsAfter ? 1.0f : 0.0f, ui::motion::kQuick);
        fold.folded_words = anim_.to(kActFoldedWordsTrack, !busy && since >= kFoldWordsAfter ? 1.0f : 0.0f, ui::motion::kBase);
        if (!busy && since < kFoldWordsAfter) {
            redraw_due_ = true; // the cues come on frames
        }
        if (!measure) {
            actions_fold_ = fold;
            add_target("actions:block", {x, y, w, act_h});
        }
    }
    // The live block's own parts in `block`: its head at `words`, its rows at `list`.
    const auto live_parts = [&](const Box& block, float words, float list) {
        const float was = renderer.opacity();
        renderer.set_opacity(was * words);
        work::spinner(*ui_, {block.x + 12 + 7.5f, block.y + kActionsLine / 2}, 15.0f, ui::th().accent);
        const float tw = work::caption(*ui_, {block.x + 42, block.y, 90, kActionsLine}, Face::Semibold, 13.0f, "Working", ui::th().text);
        const auto run = std::ranges::find(m.steps, events::StepState::Run, &events::Step::state);
        auto [k, n] = std::pair{run == m.steps.end() ? m.steps.size() : static_cast<std::size_t>(run - m.steps.begin()) + 1, m.steps.size()};
        if (m.checklist && !m.checklist->items.empty()) {
            std::tie(k, n) = plan_step(*m.checklist); // M from the plan
        }
        const float sw = work::caption(*ui_, {block.x + 42 + tw + 10, block.y, 140, kActionsLine}, Face::Text, 13.0f,
                                     text(buf, "step {} of {}", k, n), ui::th().text_3);
        work::caption(*ui_, {block.x + 42 + tw + 10 + sw + 10, block.y, 60, kActionsLine}, Face::Text, 13.0f,
                    clock_text(buf, std::max(0.0, (busy || m.ended_at < 0 ? now_ : m.ended_at) - m.started_at)), ui::th().text_3);
        renderer.set_opacity(was * list);
        renderer.rect({block.x + 1, block.y + kActionsLine - 1, block.w - 2, 1}, {.fill = ui::th().line});
        float ry = block.y + kActionsLine + 8;
        for (const auto& s : m.steps) {
            const Box row{block.x + 12, ry, block.w - 24, kActionRow};
            // The running step stays in view until the wheel takes over (conv_follow_).
            // (Not while Enter's scene opens the window: the view is small only for that moment,
            // and scrolling to the step moved the bubble the request's text flies to — MS 05.)
            if (s.state == events::StepState::Run && conv_follow_ && send_.done() && row.y + row.h > visible.y + visible.h &&
                !conv_scroll_.gliding) {
                work::scroll_to(conv_scroll_, conv_scroll_.offset + (row.y + row.h + 12 - (visible.y + visible.h)));
            }
            draw_step_row(s, row, "actions:step");
            ry += kActionRow + 2;
        }
        renderer.set_opacity(was);
    };
    if (!m.steps.empty() && busy) {
        const Box block{x, y, w, act_h};
        if (!measure) {
            // MS 05: after Enter it opens on smooth — its height's share and its opacity; its room
            // is its whole height from the start (the window is laid out once).
            const float open_h = block.h * sf.working;
            renderer.set_opacity(turn_opacity * sf.working);
            renderer.rect({block.x, block.y, block.w, open_h},
                          {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
            renderer.push_clip({block.x, block.y, block.w, open_h});
            live_parts(block, 1.0f, 1.0f);
            renderer.pop_clip();
            renderer.set_opacity(turn_opacity);
        }
        y += block.h + 12;
    } else if (!m.steps.empty()) {
        // Folded: ✓ N actions · their names · the time.
        const Box line{x, y, w, kActionsLine};
        const bool folding = act_h > kActionsLine + 0.5f || fold.live_words > 0.004f || fold.list > 0.004f;
        if (!measure) {
            add_target("actions", line);
            // The matrix (screen 26): under the mouse .06 and a .16 ring (its click is taken last, after its thumbnails).
            const auto& in = ui_->input();
            const float lh = ui_->hover(id_of("actions:hover"), in.mouse_x >= line.x && in.mouse_x < line.x + line.w && in.mouse_y >= line.y &&
                                                                     in.mouse_y < line.y + line.h);
            renderer.rect({line.x, line.y, line.w, std::max(line.h, act_h)},
                          {.fill = lerp(ui::th().w1, ui::th().w2, lh), .radius = {10, 10, 10, 10},
                           .border = lerp(ui::th().line, ui::th().line_2, lh), .border_width = 1});
            if (folding) { // the live block's parts going, inside the shrinking block
                renderer.push_clip({line.x, line.y, line.w, std::max(line.h, act_h)});
                live_parts({line.x, line.y, line.w, act_h}, fold.live_words, fold.list);
                renderer.pop_clip();
            }
            renderer.set_opacity(turn_opacity * fold.folded_words); // the line's own words come (base, +90 ms)
            const float cy = line.y + line.h / 2;
            // A step that failed: a red cross and how many, never the green check (phase 10); one
            // refused on purpose says "rejected", one that left a part out how much it read.
            const auto failed = std::ranges::count_if(m.steps, [](const events::Step& s) { return s.state == events::StepState::Failed && !s.rejected; });
            const auto rejected = std::ranges::count_if(m.steps, &events::Step::rejected);
            if (failed + rejected > 0) {
                renderer.rect({line.x + 12, cy - 9, 18, 18}, {.fill = with_alpha(ui::th().crit, 0.16f), .radius = {9, 9, 9, 9}});
                ui_->draw_icon(work::kIconClose, 10.0f, {line.x + 21, cy}, ui::th().crit);
            } else {
                renderer.rect({line.x + 12, cy - 9, 18, 18}, {.fill = ui::th().ok_soft, .radius = {9, 9, 9, 9}});
                ui_->draw_icon(kIconCheck, 11.0f, {line.x + 21, cy}, ui::th().ok);
            }
            float cw = work::caption(*ui_, {line.x + 40, line.y, 120, line.h}, Face::Semibold, 13.0f,
                                   text(buf, "{} {}", m.steps.size(), m.steps.size() == 1 ? "action" : "actions"), ui::th().text);
            if (failed > 0) {
                const auto said = text(buf, "\xC2\xB7 {} failed", failed);
                const float fw = work::caption(*ui_, {line.x + 40 + cw + 8, line.y, 120, line.h}, Face::Text, 13.0f, said, ui::th().crit);
                add_target("actions:failed", {line.x + 40 + cw + 8, line.y, fw, line.h});
                cw += 8 + fw;
            }
            if (rejected > 0) {
                const auto said = text(buf, "{} rejected", rejected);
                const float fw = work::caption(*ui_, {line.x + 40 + cw + 8, line.y, 120, line.h}, Face::Text, 13.0f, said, ui::th().crit);
                add_target("actions:rejected", {line.x + 40 + cw + 8, line.y, fw, line.h});
                cw += 8 + fw;
            }
            if (const auto part = std::ranges::find_if(m.steps, [](const events::Step& s) { return !s.skipped.empty(); }); part != m.steps.end()) {
                const auto n = std::ranges::count_if(m.steps, [](const events::Step& s) { return !s.skipped.empty(); });
                const auto said = n == 1 ? text(buf, "\xC2\xB7 1 read with {}", part->skipped) : text(buf, "\xC2\xB7 {} read with parts left out", n);
                const float fw = work::caption(*ui_, {line.x + 40 + cw + 6, line.y, 220, line.h}, Face::Text, 13.0f, said, ui::th().text_3);
                add_target("actions:skipped", {line.x + 40 + cw + 6, line.y, fw, line.h});
                cw += 6 + fw;
            }
            // Done on this PC (phase 10, screens 20-21): a pill that keeps its size; the names give way.
            if (std::ranges::any_of(m.steps, [](const events::Step& s) { return !s.local.empty(); })) {
                constexpr std::string_view kHere = "On this PC";
                const float lw = work::text_width(*ui_, Face::Text, 11.5f, kHere);
                const Box pill{line.x + 40 + cw + 10, cy - 10, 8 + 12 + 5 + lw + 9, 20};
                renderer.rect({pill.x, pill.y, pill.w, pill.h},
                              {.fill = ui::th().ok_soft, .radius = {10, 10, 10, 10}, .border = ui::th().ok_line,
                               .border_width = 1});
                ui_->draw_icon(kIconPc, 12.0f, {pill.x + 8 + 6, cy}, ui::th().ok);
                work::caption(*ui_, {pill.x + 8 + 12 + 5, pill.y, lw + 1, pill.h}, Face::Text, 11.5f, kHere, ui::th().ok);
                add_target("actions:local", pill);
                cw += 10 + pill.w;
            }
            joined_.clear();
            for (const auto& s : m.steps) {
                if (!joined_.empty()) {
                    joined_ += " \xC2\xB7 ";
                }
                joined_ += s.title;
            }
            const double took = m.ended_at >= 0 ? m.ended_at - m.started_at : 0.0;
            const auto time = text(buf, "{:.1f} s", std::max(0.0, took));
            const float tw = work::text_width(*ui_, Face::Text, 13.0f, time);
            const float right = line.x + line.w - 12 - 15 - 10;
            work::caption(*ui_, {right - tw, line.y, tw, line.h}, Face::Text, 13.0f, time, ui::th().text_3);
            ui_->draw_icon(work::kIconChevronRight, 15.0f, {line.x + line.w - 12 - 7.5f, cy}, ui::th().text_3);
            // What the model looked at, before the time (phase 10): they take their clicks before the row.
            const float sw = seen_width(m.pictures);
            const float sx = right - tw - 10 - sw;
            (void)draw_seen(m.pictures, sx, cy, false, r);
            const float nx = line.x + 40 + cw + 10;
            work::caption(*ui_, {nx, line.y, std::max(0.0f, (sw > 0 ? sx - 10 : right - tw - 10) - nx), line.h}, Face::Text, 13.0f, joined_,
                        ui::th().text_3);
            renderer.set_opacity(turn_opacity);
            if (ui_->interact(id_of("actions"), line, false).clicked) {
                show_activity(m, m.task); // what it ran, in the panel
            }
        }
        y += std::max(kActionsLine, act_h) + 12;
    }
    // "No actions": said once the work has ended with none. Its room opens on smooth, its words
    // with it — it stood there in a frame and pushed the answer down a line (MS § 2: no jumps).
    const bool no_actions = m.steps.empty() && !busy && m.data.empty() && !m.answer.empty() && !m.word && !m.phrase && !m.text_translation;
    if (const float said = anim_.to(kNoActionsTrack, no_actions ? 1.0f : 0.0f, ui::motion::kSmooth); said > 0.004f && m.steps.empty()) {
        if (!measure) {
            renderer.set_opacity(turn_opacity * said);
            ui_->draw_icon(kIconBook, 13.0f, {x + 6.5f, y + 9}, ui::th().text_4);
            work::caption(*ui_, {x + 20, y, w - 20, 18}, Face::Text, 12.5f, "No actions \xE2\x80\x94 answered from knowledge", ui::th().text_4);
            renderer.set_opacity(turn_opacity);
        }
        y += (18 + 12) * said;
    }
    if (m.steps.empty() || busy) { // no row of actions (yet): what the model looked at on a row of its own
        if (const float sw = draw_seen(m.pictures, x, y + kSeen / 2, measure, r); sw > 0) {
            y += kSeen + 12;
        }
    }
    refuse_picture(m, r);
    y += draw_looked(m, x, y, w, measure); // the user's window, looked at or not (phase 13a)
    // What the tools refused (phase 10, screen 25): that nothing broke, then a card each.
    y += draw_refusals(m, x, y, w, measure);
    y += draw_question_card(m, x, y, w, measure, r); // the model's question (phase 12 § 4)
    if (r.command) {
        return y - y0;
    }
    if (m.activity == Activity::Thinking && m.answer.empty() && !m.permission && !m.query) {
        // Phase 12 § 9: how long it has thought; past limits.think_seconds, "Answer now".
        if (m.thinking_since) {
            const double thought = std::max(0.0, now_ - *m.thinking_since);
            const auto size = work::button_size(*ui_, "Answer now");
            const float row = std::max(22.0f, size.h);
            if (!measure) {
                const float tw = work::caption(*ui_, {x, y, 72, row}, Face::Text, 12.5f, "Thinking \xC2\xB7", ui::th().text_3);
                const Box clock{x + tw + 4, y, 44, row};
                work::caption(*ui_, clock, Face::Text, 12.5f, clock_text(buf, thought), ui::th().text_3);
                add_target("thinking:clock", clock);
                if (thought >= m.think_seconds && !m.answer_pressed && !r.command) {
                    const Box button{clock.x + clock.w + 8, y + (row - size.h) / 2, size.w, size.h};
                    add_target("answer_now", button);
                    if (work::button(*ui_, id_of("answer_now"), button, "Answer now")) {
                        r.command = answer_now(m);
                    }
                }
            }
            y += row + 8;
        }
    }
    // The skeleton (MS § 7): three bars with a light running over them while it thinks — there only
    // while the answer is waited for; when it begins they go on quick and give their room back.
    {
        const bool waits = m.activity == Activity::Thinking && m.answer.empty() && !m.permission && !m.query;
        const float bars = std::clamp(anim_.to(id_of("turn:skeleton", m.task), waits ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f);
        if (bars > 0.004f) {
            if (!measure) {
                renderer.push_clip({x, y, w, (50 + 12) * bars});
                for (int i = 0; i < 3; ++i) {
                    const float share = i == 0 ? 0.92f : i == 1 ? 0.84f : 0.58f;
                    work::skeleton_bar(*ui_, {x, y + static_cast<float>(i) * 20, w * share, 10}, bars, waits);
                }
                renderer.pop_clip();
            }
            y += (50 + 12) * bars;
        }
    }

    // A translation card: the model's show.* call, drawn.
    if (m.word) {
        y += draw_word_card(m, x, y, w, measure, r);
    } else if (m.phrase) {
        y += draw_phrase_card(m, x, y, w, measure, r);
    } else if (m.text_translation) {
        y += draw_text_turn(m, x, y, w, measure, r);
    }
    if (r.command) {
        return y - y0;
    }
    // The detailed mode: the plan, its command under the mouse lit.
    if (m.expanded && m.plan) {
        y += measure ? 14 + 22 + static_cast<float>(m.plan->items.size()) * 23.0f + 10 + 8 : draw_plan(m, x, y, w);
    }

    // The answer.
    if (doc_ && !m.answer.empty()) {
        doc_->set_width(std::min(w, 62 * work::text_width(*ui_, Face::Text, theme::type::prose, "0")), w);
        // MS 07: it grows by its lines on quick — what is under it moves with the spring, a new
        // line is uncovered by it, never there in a frame.
        const float doc_h = doc_->height();
        const float h = anim_.to(kAnswerHeightTrack, doc_h, ui::motion::kQuick);
        if (!measure) {
            add_target("answer", {x, y, w, h});
            answer_bottom_ = y + h;
            const std::size_t first = tables_drawn_;
            renderer.push_clip({visible.x, y, visible.w, std::max(0.0f, h)});
            draw_markdown({x, y, w, doc_h}, visible, m.streaming, r);
            renderer.pop_clip();
            open_answer_table(m, m.data, first);
        }
        y += h + 10;
        // What was done on this PC, said quietly under it — unless a picture went to the model in
        // this turn, when "not sent anywhere" would not be true (phase 10, screens 20-21).
        const auto local = std::ranges::find_if(m.steps, [](const events::Step& s) { return !s.local.empty(); });
        if (local != m.steps.end() && m.pictures.empty() && !m.streaming) {
            const auto [first, second] = wrap2(Face::Text, 12.0f, local->local, w - 20);
            const float lh = ui_->line_height(Face::Text, 12.0f) * 1.3f;
            const float nh = lh * (second.empty() ? 1.0f : 2.0f);
            if (!measure) {
                ui_->draw_icon(kIconPc, 11.0f, {x + 6, y + lh / 2}, ui::th().text_4);
                work::caption(*ui_, {x + 20, y, w - 20, lh}, Face::Text, 12.0f, first, ui::th().text_4);
                if (!second.empty()) {
                    work::caption(*ui_, {x + 20, y + lh, w - 20, lh}, Face::Text, 12.0f, second, ui::th().text_4);
                }
                add_target("answer:local", {x, y, w, nh});
            }
            y += nh + 10;
        }
    }
    // MS 07: what follows the answer — the places, the files made, the buttons, the cost, the hints
    // — opens after its last character: its height's share on smooth from +220 ms, its opacity on
    // base 60 ms later. A card of the policy among them stands as it is, at once.
    const bool ended = shown_bytes_ == m.answer.size() && !m.streaming;
    if (measure) { // once a frame (the measuring pass comes first)
        if (!ended) {
            tail_at_ = -1.0e9;
        } else if (tail_at_ < -1.0e8) {
            tail_at_ = now_;
        }
    }
    const double tail_since = ended ? now_ - tail_at_ : -1.0;
    const bool tail_still = m.permission.has_value();
    if (tail_still) { // at once: it is asked now, not after an answer
        anim_.place(kTailShareTrack, 1.0f);
        anim_.place(kTailAlphaTrack, 1.0f);
    }
    TailMotion tail;
    tail.share = anim_.to(kTailShareTrack, tail_still || (ended && tail_since >= kTailAfter) ? 1.0f : 0.0f, ui::motion::kSmooth);
    tail.alpha = anim_.to(kTailAlphaTrack, tail_still || (ended && tail_since >= kTailAfter + kTailAlphaAfter) ? 1.0f : 0.0f, ui::motion::kBase);
    if (ended && tail_since < kTailAfter + kTailAlphaAfter) {
        redraw_due_ = true; // its cues come on frames
    }
    const float tail_from = y;
    // Whatever way this function is left (a button pressed returns from inside), the clip and the
    // opacity are put back.
    struct TailGuard {
        ui::render::Renderer& renderer;
        float opacity;
        bool on;
        ~TailGuard() {
            if (on) {
                renderer.pop_clip();
                renderer.set_opacity(opacity);
            }
        }
    } tail_guard{renderer, turn_opacity, !measure};
    if (!measure) {
        tail_motion_ = tail;
        renderer.push_clip({visible.x, tail_from, visible.w, std::max(0.0f, tail_full_ * tail.share)});
        renderer.set_opacity(turn_opacity * tail.alpha);
    }
    // The files and folders the answer names, then the files this turn made (phase 10, screen 24).
    y += draw_places(m, x, y, w, measure, r);
    y += draw_created(m, x, y, w, measure, r);
    // A question of the policy: its card here, its objects in the panel.
    if (m.permission) {
        y += approval_card(m, *m.permission, false, x, y, w, measure, r);
        if (r.command) {
            return y - y0;
        }
    } else if (m.answered) { // answered (MS 11): its card shrinking into the summary row that stays in the turn
        y += approval_card(m, m.answered->asked, true, x, y, w, measure, r);
    }
    // The report's advice: what can be done, each a button for the agent.
    if (m.report && all_shown_now(m)) {
        for (std::size_t i = 0; i < m.report->advice.size(); ++i) {
            const auto& a = m.report->advice[i];
            const Box row{x, y, w, 44};
            if (!measure) {
                add_target(text(buf, "advice:{}", i), row);
                const auto hit = ui_->interact(id_of("advice", i), row, false);
                renderer.rect({row.x, row.y, row.w, row.h},
                              {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("advice", i), hit.hovered)), .radius = {10, 10, 10, 10},
                               .border = ui::th().line, .border_width = 1});
                const float gw = work::text_width(*ui_, Face::Semibold, 13.0f, a.gain);
                work::caption(*ui_, {row.x + row.w - 14 - gw, row.y, gw, row.h}, Face::Semibold, 13.0f, a.gain, ui::th().ok);
                const float room = std::max(0.0f, row.w - 14 - gw - 12 - 14);
                const float tw = work::caption(*ui_, {row.x + 14, row.y, room, row.h}, Face::Semibold, 13.5f, a.title, ui::th().text);
                work::caption(*ui_, {row.x + 14 + tw + 10, row.y, std::max(0.0f, room - tw - 10), row.h}, Face::Text, 12.5f, a.sub,
                            ui::th().text_3);
                if (hit.clicked && !r.command) {
                    const std::string action = a.action;
                    r.command = press(m, action);
                    return y + 44 - y0;
                }
            }
            y += 44 + 8;
        }
    }
    // Its buttons once it is all shown, and what it cost.
    const bool all_shown = shown_bytes_ == m.answer.size();
    if (all_shown && !m.actions.empty()) {
        float bx = x;
        float bh = 0;
        for (std::size_t i = 0; i < m.actions.size(); ++i) {
            const auto& a = m.actions[i];
            const auto size = work::button_size(*ui_, a.label);
            bh = std::max(bh, size.h);
            if (!measure) {
                const Box box{bx, y, size.w, size.h};
                add_target(text(buf, "action:{}", a.id), box);
                if (work::button(*ui_, id_of("action:", core::hash::fnv1a64(a.id, 0)), box, a.label,
                                 a.primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal) &&
                    !r.command) {
                    const std::string id = a.id;
                    r.command = press(m, id);
                    return y + bh - y0;
                }
            }
            bx += size.w + 8;
        }
        y += bh + 10;
    }
    // Stage 4c (screen 06): a provider's failure is its card, the buttons in it.
    if (m.activity == Activity::Done && m.failed && !m.card.title.empty()) {
        y += draw_failure_card(m, x, y, w, measure, r);
        if (r.command) {
            return y - y0;
        }
    } else if (m.activity == Activity::Done && !m.done_title.empty()) {
        // The result: done (a title and its line) or failed, and its buttons.
        const float rh = 24.0f;
        if (!measure) {
            add_target("turn:result", {x, y, w, rh});
            const float cy = y + rh / 2;
            if (m.failed) {
                renderer.rect({x + 4, cy - 5, 10, 10}, {.fill = ui::th().crit, .radius = {5, 5, 5, 5}});
            } else {
                renderer.rect({x, cy - 9, 18, 18}, {.fill = ui::th().ok_soft, .radius = {9, 9, 9, 9}});
                ui_->draw_icon(kIconCheck, 11.0f, {x + 9, cy}, ui::th().ok);
            }
            const float tw = work::caption(*ui_, {x + 26, y, w - 26, rh}, Face::Semibold, 14.0f, m.done_title,
                                         m.failed ? ui::th().crit : ui::th().text);
            if (!m.done_detail.empty()) {
                work::caption(*ui_, {x + 26 + tw + 8, y, std::max(0.0f, w - 34 - tw), rh}, Face::Text, 13.0f, m.done_detail,
                            ui::th().text_3);
            }
        }
        y += rh + 8;
        if (!m.done_actions.empty()) {
            float bx = x;
            float bh = 0;
            for (std::size_t i = 0; i < m.done_actions.size(); ++i) {
                const auto& a = m.done_actions[i];
                const auto size = work::button_size(*ui_, a.label);
                bh = std::max(bh, size.h);
                if (!measure) {
                    const Box box{bx, y, size.w, size.h};
                    add_target(text(buf, "action:{}", a.id), box);
                    if (work::button(*ui_, id_of("done:", core::hash::fnv1a64(a.id, 0)), box, a.label,
                                     a.primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal) &&
                        !r.command) {
                        const std::string id = a.id;
                        r.command = press(m, id);
                        return y + bh - y0;
                    }
                }
                bx += size.w + 8;
            }
            y += bh + 10;
        }
    }
    // Its data: a card per set, the one in the panel marked.
    for (const auto id : m.data) {
        const auto* set = set_of(m, id);
        if (set == nullptr || std::ranges::any_of(m.created, [&](const events::Created& c) { return c.preview == id; })) {
            continue; // a created file's preview: its file card shows it
        }
        y += 8;
        const Box whole{x, y, w, kCard};
        if (!measure) {
            add_target(text(buf, "card:{}", id), whole);
            const bool on = m.active_set == id;
            // The matrix (screen 26): under the mouse .065 and a .16 ring, its arrow 2 px right and
            // white; pressed .99 and .03; the keyboard's ring.
            const auto hit = ui_->interact(id_of("card", id), whole);
            if (hit.clicked) {
                show_set(m, id);
            }
            const float ch = ui_->hover(id_of("card", id), hit.hovered);
            const float cp = ui_->press(id_of("card", id), hit.held);
            const Box card = work::squeezed(whole, 1.0f - (1.0f - ui::motion::micro::kCardPress) * cp); // a wide card: .98
            const Color rest = on ? ui::th().accent_soft : ui::th().card;
            renderer.rect({card.x, card.y, card.w, card.h},
                          {.fill = on ? rest : under_mouse(rest, ch * (1.0f - 0.5f * cp)),
                           .radius = ui::card_corners(),
                           .border = on ? ui::th().accent_line : lerp(ui::th().line, ui::th().line_2, ch),
                           .border_width = 1});
            work::focus_ring(*ui_, id_of("card", id), card, 12, hit.focus_ring);
            renderer.rect({card.x + 12, card.y + 10, 34, 34}, {.fill = with_alpha(ui::th().accent, 0.12f), .radius = {9, 9, 9, 9}});
            ui_->draw_icon(set->kind == events::SetKind::Text ? kIconText : set->kind == events::SetKind::Converted ? kIconPicture : kIconTable, 17.0f,
                           {card.x + 12 + 17, card.y + 27}, ui::th().accent);
            const float tx = card.x + 12 + 34 + 12;
            float right = card.x + card.w - 12;
            if (on) {
                const float bw = work::text_width(*ui_, Face::Semibold, 11.5f, "In panel") + 18;
                const Box badge{right - bw, card.y + (kCard - 20) / 2, bw, 20};
                renderer.rect({badge.x, badge.y, badge.w, badge.h}, {.fill = ui::th().accent, .radius = {10, 10, 10, 10}});
                work::caption(*ui_, {badge.x + 9, badge.y, bw - 9, badge.h}, Face::Semibold, 11.5f, "In panel", ui::th().on_accent);
                right = badge.x - 10;
            } else if (!set->complete) {
                work::spinner(*ui_, {right - 7, card.y + kCard / 2}, 13.0f, ui::th().accent);
                right -= 24;
            } else {
                ui_->draw_icon(work::kIconChevronRight, 16.0f, {right - 8 + 2 * ch, card.y + kCard / 2}, lerp(ui::th().text_3, ui::th().text, ch));
                right -= 26;
            }
            work::caption(*ui_, {tx, card.y + 9, std::max(0.0f, right - tx), 18}, Face::Semibold, 13.5f, set->title, ui::th().text);
            const std::string_view sub = set->complete ? std::string_view{set->footer}
                                                       : text(buf, "loading \xC2\xB7 {} so far", set->rows.rows());
            work::caption(*ui_, {tx, card.y + 28, std::max(0.0f, right - tx), 17}, Face::Text, 12.5f, sub, ui::th().text_3);
        }
        y += kCard;
    }
    if (all_shown && m.spent && !m.streaming) {
        // Screen 05: who answered and what it cost, small and quiet — the monogram and full name of the
        // model that answered, the turn's time, its price, what came from the cache. Made when it
        // changes, never in a frame.
        if (cost_usd_ != m.spent->usd || cost_.empty() || cost_model_ != m.spent->model) {
            const double took = m.ended_at >= 0 ? m.ended_at - m.started_at : 0.0;
            cost_ = std::format("{:.1f} s \xC2\xB7 {}", took, spent_text(*m.spent));
            cost_usd_ = m.spent->usd;
            cost_model_ = m.spent->model;
        }
        y += 8;
        if (!measure) {
            float cx = x;
            if (const auto* answered = ai::catalog::model(m.spent->model)) {
                const auto& by = ai::catalog::provider_of(*answered);
                constexpr float kMono = 16;
                const float nw = work::text_width(*ui_, Face::Semibold, 11.5f, answered->full_name);
                const Box badge{x, y - 1, 3 + kMono + 6 + nw + 9, 18};
                renderer.rect({badge.x, badge.y, badge.w, badge.h},
                              {.fill = ui::th().w2, .radius = {9, 9, 9, 9}, .border = ui::th().line, .border_width = 1});
                const Box mono{badge.x + 2, badge.y + 1, kMono, kMono};
                renderer.rect({mono.x, mono.y, mono.w, mono.h}, {.fill = ui::th().w3, .radius = {8, 8, 8, 8}});
                const float mw = work::text_width(*ui_, Face::Semibold, 8.5f, by.monogram);
                work::caption(*ui_, {mono.x + (kMono - mw) / 2, mono.y, mw + 1, kMono}, Face::Semibold, 8.5f, by.monogram, ui::th().text_2);
                work::caption(*ui_, {mono.x + kMono + 6, badge.y, nw + 1, badge.h}, Face::Semibold, 11.5f, answered->full_name, ui::th().text_2);
                add_target("answer:model", badge);
                // Another model than the one chosen: says both; why is the turn's route note (stage 4c).
                if (const auto* chosen = ai::catalog::model(m.spent->chosen);
                    chosen != nullptr && chosen != answered && ui_->interact(id_of("answer-model"), badge, false).hovered) {
                    std::array<char, 120> head{}; // on the stack: a frame under the mouse allocates nothing
                    const auto h = std::format_to_n(head.data(), head.size(), "Answered by {}, not {}", answered->short_name, chosen->short_name);
                    const std::string_view why = m.routes.empty() ? std::string_view{"Another model than the one chosen made it."}
                                                                  : std::string_view{m.routes.back()};
                    ui_->tooltip(id_of("answer-model"), badge, std::string_view{head.data(), static_cast<std::size_t>(h.out - head.data())}, why);
                }
                cx = badge.x + badge.w + 8;
            }
            const Box cost{cx, y, std::max(0.0f, x + w - cx), 16};
            add_target("answer:cost", cost);
            work::caption(*ui_, cost, Face::Text, 11.5f, cost_, ui::th().text_4);
        }
        y += 16;
    }
    // What the turn remembered (phase 12 § 6): a quiet line each, with Undo.
    // MS § 7 "Rules and memory": a line new in a live turn is lit green for kRememberedLit, then goes
    // pale on gentle; its Undo lets it go on micro, then closes its room on smooth (the model has
    // it no more: its words are kept here, copied once).
    const float lh = 20.0f;
    const float note_room = gone_note_n_ > 0 ? anim_.to(kGoneNoteRoomTrack, now_ - gone_note_at_ < kNoteCloseAfter ? 1.0f : 0.0f, ui::motion::kSmooth) : 0.0f;
    const float note_alpha = gone_note_n_ > 0 ? anim_.to(kGoneNoteAlphaTrack, 0.0f, ui::motion::kMicro) : 0.0f;
    if (measure && gone_note_n_ > 0 && now_ - gone_note_at_ >= kNoteCloseAfter && note_room <= 0.002f) {
        gone_note_n_ = 0; // closed
    }
    const auto gone_note = [&](std::size_t i) { // the line Undo took, where it stood (before line `i`)
        if (gone_note_n_ == 0 || gone_note_place_ != i) {
            return;
        }
        const float gap = gone_note_place_ == 0 ? 10.0f : 2.0f;
        if (!measure && note_alpha > 0.004f) {
            const float was = renderer.opacity();
            renderer.set_opacity(was * note_alpha);
            const float lw = work::caption(*ui_, {x, y + gap, w, lh}, Face::Text, 12.0f, "Remembered:", ui::th().text_3);
            work::caption(*ui_, {x + lw + 4, y + gap, std::max(0.0f, w - lw - 4), lh}, Face::Text, 12.0f,
                        std::string_view{gone_note_.data(), gone_note_n_}, ui::th().text_2);
            renderer.set_opacity(was);
        }
        y += (gap + lh) * note_room;
    };
    for (std::size_t i = 0; i < m.remembered.size(); ++i) {
        gone_note(i);
        const auto& said = m.remembered[i];
        y += i == 0 && !(gone_note_n_ > 0 && gone_note_place_ == 0) ? 10.0f : 2.0f;
        // When it was first seen: in a live turn, now; in one already whole (the history), long ago.
        auto seen = std::ranges::find(notes_seen_, said.id, &std::pair<std::uint64_t, double>::first);
        if (seen == notes_seen_.end()) {
            seen = notes_seen_.begin() + static_cast<std::ptrdiff_t>(notes_seen_next_++ % notes_seen_.size());
            *seen = {said.id, now_ - tail_at_ < kTurnLiveFor + 1.0 || working(m) ? now_ : -1.0e9};
        }
        if (!measure) {
            const auto undo_w = work::text_width(*ui_, Face::Semibold, 12.0f, "Undo");
            const float tw = std::max(0.0f, w - undo_w - 12);
            // Its green: from when it is in sight (the tail opens after the answer), for kRememberedLit.
            const double lit_for = now_ - std::max(seen->second, tail_at_ + kTailAfter);
            const float lit = anim_.to(id_of("remembered:lit", said.id), lit_for < kRememberedLit ? 1.0f : 0.0f, ui::motion::kGentle);
            if (lit > 0.004f) {
                const float lw0 = work::text_width(*ui_, Face::Text, 12.0f, "Remembered:");
                const float cw0 = std::min(work::text_width(*ui_, Face::Text, 12.0f, said.text), std::max(0.0f, tw - lw0 - 4));
                renderer.rect({x - 6, y, lw0 + 4 + cw0 + 12 + undo_w + 12, lh},
                              {.fill = with_alpha(ui::th().ok, 0.12f * lit), .radius = {6, 6, 6, 6}});
                if (lit_for < kRememberedLit) {
                    redraw_due_ = true; // its 4 s end on a frame
                }
            }
            const float lw = work::caption(*ui_, {x, y, tw, lh}, Face::Text, 12.0f, "Remembered:", ui::th().text_3);
            const float cw = work::caption(*ui_, {x + lw + 4, y, std::max(0.0f, tw - lw - 4), lh}, Face::Text, 12.0f, said.text, ui::th().text_2);
            const Box line{x, y, lw + 4 + cw, lh};
            add_target(text(buf, "remembered:{}", said.id), line);
            const Box undo{line.x + line.w + 12, y, undo_w, lh};
            add_target(text(buf, "remembered:{}:undo", said.id), undo);
            const auto hit = ui_->interact(id_of("remembered", said.id), undo);
            work::caption(*ui_, undo, Face::Semibold, 12.0f, "Undo",
                          blend(ui::th().accent, ui::th().text, this->lit(id_of("remembered", said.id), hit.hovered))); // (a local `lit` above)
            if (hit.clicked && !r.command) {
                gone_note_n_ = std::min(said.text.size(), gone_note_.size());
                while (gone_note_n_ > 0 && gone_note_n_ < said.text.size() && (static_cast<unsigned char>(said.text[gone_note_n_]) & 0xC0) == 0x80) {
                    --gone_note_n_; // never half a character
                }
                std::copy_n(said.text.data(), gone_note_n_, gone_note_.data());
                gone_note_place_ = i;
                gone_note_at_ = now_;
                anim_.place(kGoneNoteAlphaTrack, 1.0f);
                anim_.place(kGoneNoteRoomTrack, 1.0f);
                r.command = forget_remembered(m, said.id, said.private_window); // `said` is gone after this
                return y + lh - y0;
            }
        }
        y += lh;
    }
    gone_note(m.remembered.size());
    // The model's chips: what to ask next.
    if (all_shown && !m.suggestions.empty()) {
        y += 16;
        float cx = x;
        float row_y = y;
        const float ch = ui_->line_height(Face::Text, 12.5f) + 12;
        for (std::size_t i = 0; i < m.suggestions.size(); ++i) {
            const auto& s = m.suggestions[i];
            const float cw = work::text_width(*ui_, Face::Text, 12.5f, s) + 22;
            if (cx > x && cx + cw > x + w) {
                cx = x;
                row_y += ch + 7;
            }
            if (!measure) {
                const Box whole{cx, row_y, std::min(cw, w), ch};
                add_target(text(buf, "suggest:{}", i), whole);
                // MS § 7 "Suggestions after an answer": with the tail, one kChipStagger after another, on quick.
                const double chip_due = kTailAfter + kTailAlphaAfter +
                                        std::chrono::duration<double>(ui::motion::stream::kChipStagger).count() *
                                            static_cast<double>(std::min(i, ui::motion::kStaggerMaxItems));
                // (First drawn when the tail is already open: in a turn that just ended it comes from
                // nothing; in one long over — the history, the sheet opened again — it is there.)
                const std::optional<float> chip_from = ended && tail_since < 5.0 ? std::optional<float>{0.0f} : std::nullopt;
                const float chip_in = std::clamp(anim_.to(id_of("suggest:in", i), tail_still || (ended && tail_since >= chip_due) ? 1.0f : 0.0f,
                                                          ui::motion::kQuick, chip_from),
                                                 0.0f, 1.0f);
                if (ended && tail_since < chip_due) {
                    redraw_due_ = true; // its turn comes on a frame
                }
                const float chip_opacity = renderer.opacity();
                renderer.set_opacity(chip_opacity * chip_in);
                suggest_in_[std::min(i, suggest_in_.size() - 1)] = chip_in;
                // The matrix (screen 26): under the mouse .09, a .18 ring and white; pressed .97 and .05.
                const auto hit = ui_->interact(id_of("suggest", i), whole);
                const float sh = ui_->hover(id_of("suggest", i), hit.hovered);
                const float sp = ui_->press(id_of("suggest", i), hit.held);
                const Box chip = work::squeezed(whole, 1.0f - (1.0f - ui::motion::micro::kPress) * sp);
                renderer.rect({chip.x, chip.y, chip.w, chip.h},
                              {.fill = lerp(lerp(ui::th().w2, ui::th().w3, sh), ui::th().w1, sp),
                               .radius = {chip.h / 2, chip.h / 2, chip.h / 2, chip.h / 2},
                               .border = lerp(ui::th().line, ui::th().line_2, sh),
                               .border_width = 1});
                work::caption(*ui_, {chip.x + 11, chip.y, chip.w - 22, chip.h}, Face::Text, 12.5f, s, lerp(ui::th().text_2, ui::th().text, sh));
                work::focus_ring(*ui_, id_of("suggest", i), chip, chip.h / 2, hit.focus_ring);
                renderer.set_opacity(chip_opacity);
                if (hit.clicked && !r.command) {
                    m.input.text = s;
                    m.input.caret = m.input.text.size();
                    m.input.anchor = Edit::kNoAnchor; // a new text: no selection of the old one
                    r.command = submit(m);
                    return row_y + ch - y0;
                }
            }
            cx += cw + 7;
        }
        y = row_y + ch;
    }
    // The tail's room is its share (MS 07): what is under the turn moves with it.
    if (measure) {
        tail_full_ = y - tail_from;
    }
    y = tail_from + (y - tail_from) * tail.share;
    return y + kPadBottom - y0;
}

auto Painter::earlier_doc(const Turn& t, float width) -> answer::Doc& {
    auto it = std::ranges::find(earlier_docs_, t.task, &EarlierDoc::task);
    if (it == earlier_docs_.end()) {
        earlier_docs_.push_back({t.task, answer::Doc{*fonts_, theme::type::prose * ui_->text_scale(), ui_->scale()}});
        it = earlier_docs_.end() - 1;
    }
    it->doc.sync(t.answer, true);
    it->doc.set_width(std::min(width, 62 * work::text_width(*ui_, Face::Text, theme::type::prose, "0")), width);
    return it->doc;
}

auto Painter::draw_earlier(Model& m, float x0, float y0, float w0, const Box& visible, bool measure, Reaction& r) -> float {
    if (m.session.empty()) {
        return 0;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    float y = y0;
    if (!measure) {
        renderer.rect({x0, y, w0, 1}, {.fill = ui::th().line});
        work::caption(*ui_, {x0 + kPadX, y + 16, w0 - 2 * kPadX, 14}, Face::Semibold, 11.0f, "EARLIER IN THIS SESSION",
                    ui::th().text_3);
    }
    y += 16 + 14 + 8;
    const float w = std::min(w0 - 2 * kPadX, kContentMax);
    for (std::size_t i = 0; i < m.session.size(); ++i) {
        const auto& t = m.session[i];
        const Box row{x0, y, w0, kEarlierRow};
        // MS 10: its answer's room on smooth, the answer 80 ms later from above, the chevron turning.
        const std::uint64_t fold_key = id_of("earlier:fold", t.task);
        const auto fold = disclosed(fold_key, !t.collapsed);
        if (!measure && row.y + row.h >= visible.y && row.y <= visible.y + visible.h) {
            ui_->order_base((static_cast<std::uint64_t>(i) + 2) << 40); // after the turn in front, each its own
            add_target(text(buf, "earlier:{}", i), row);
            const auto hit = ui_->interact(id_of("earlier", t.task), row, false);
            if (const float light = lit(id_of("earlier", t.task), hit.hovered); light > 0.004f) {
                renderer.rect({row.x, row.y, row.w, row.h}, {.fill = faint(ui::th().w2, light)});
            }
            if (i > 0) {
                renderer.rect({row.x + kPadX, row.y, row.w - 2 * kPadX, 1}, {.fill = ui::th().line});
            }
            const float rx = row.x + kPadX;
            work::caption(*ui_, {rx, row.y, 40, row.h}, Face::Text, 12.0f, t.time, ui::th().text_4);
            // The result chip: its first data set and its rows, or "Text".
            std::string_view result = "Text";
            if (!t.data.empty()) {
                if (const auto* set = set_of(m, t.data.front())) {
                    const auto count = set->text_read                            ? set->text_read->lines.size()
                                       : set->converted                          ? set->converted->rows.size()
                                       : set->kind == events::SetKind::Preview ? std::size_t{1}
                                                                                 : set->rows.rows();
                    result = text(buf, "{} \xC2\xB7 {}", kind_name(set->kind), count);
                }
            }
            const float rw = work::text_width(*ui_, Face::Text, 12.0f, result) + 16;
            const float right = row.x + row.w - kPadX - 14 - 10;
            const Box chip{right - rw, row.y + (row.h - 22) / 2, rw, 22};
            renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w2, .radius = {7, 7, 7, 7}});
            work::caption(*ui_, {chip.x + 8, chip.y, rw - 8, chip.h}, Face::Text, 12.0f, result, ui::th().text_3);
            work::chevron(*ui_, {row.x + row.w - kPadX - 7, row.y + row.h / 2}, 14.0f, fold.chevron, ui::th().text_3);
            work::caption(*ui_, {rx + 50, row.y, std::max(0.0f, chip.x - 10 - rx - 50), row.h}, Face::Text, 13.5f, t.question,
                        ui::th().text_2);
            if (hit.clicked && !r.command) {
                open_turn(m, i);
                disclose(fold_key, !m.session[i].collapsed);
            }
        }
        y += kEarlierRow;
        if (fold.room > 0.001f && i < m.session.size()) {
            auto& doc = earlier_doc(t, w);
            const float h = doc.height();
            const float shown = (h + 16) * fold.room;
            if (!measure) {
                // The answer in the room it has now; one closing takes no click and names no target.
                const bool open = !t.collapsed;
                const float opacity = renderer.opacity();
                const bool was_inert = ui_->inert();
                const auto targets_kept = target_count_;
                renderer.push_clip({x0, y, w0, shown});
                renderer.set_opacity(opacity * fold.body_alpha);
                ui_->set_inert(was_inert || !open);
                const Box answer{x0 + kPadX, y + 4 + fold.body_dy, w, h};
                add_target(text(buf, "earlier:{}:answer", i), answer);
                const std::size_t first = tables_drawn_;
                draw_markdown(doc, {}, answer, visible, false, r);
                open_answer_table(m, t.data, first);
                ui_->set_inert(was_inert);
                renderer.set_opacity(opacity);
                renderer.pop_clip();
                if (!open) {
                    target_count_ = targets_kept;
                }
            }
            y += shown;
        }
    }
    return y - y0 + 14;
}

void Painter::draw_history(Model& m, const Box& column, Reaction& r) {
    (void)r;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const std::size_t n = std::min(m.recent.size(), kHistoryRows);
    const float w = std::min(360.0f, column.w - 24);
    const Box list{column.x + column.w - 12 - w, column.y + kHeader - 4, w, static_cast<float>(n) * 32 + 12 + (n == 0 ? 32 : 0)};
    work::pop_surface(*ui_, list, 12);
    if (n == 0) {
        work::caption(*ui_, {list.x + 14, list.y + 6, list.w - 28, 32}, Face::Text, 13.0f, "No questions yet", ui::th().text_3);
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto& q = m.recent[m.recent.size() - 1 - i]; // newest first
        const Box row{list.x + 6, list.y + 6 + static_cast<float>(i) * 32, list.w - 12, 32};
        add_target(text(buf, "history:{}", i), row);
        const auto hit = ui_->interact(id_of("history", i), row, false);
        if (const float light = lit(id_of("history", i), hit.hovered); light > 0.004f) {
            renderer.rect({row.x, row.y, row.w, row.h}, {.fill = faint(ui::th().w2, light), .radius = {8, 8, 8, 8}});
        }
        work::caption(*ui_, {row.x + 10, row.y, row.w - 20, row.h}, Face::Text, 13.0f, q, ui::th().text_2);
        if (hit.clicked) {
            m.input.text = q;
            m.input.caret = m.input.text.size();
            m.input.anchor = Edit::kNoAnchor; // a new text: no selection of the old one
            history_open_ = false;
        }
    }
    // A click elsewhere closes it.
    const auto& in = ui_->input();
    if (in.mouse_pressed && !inside(list, in.mouse_x, in.mouse_y) && !inside(history_button_, in.mouse_x, in.mouse_y)) {
        history_open_ = false;
    }
}

} // namespace app::sheet
