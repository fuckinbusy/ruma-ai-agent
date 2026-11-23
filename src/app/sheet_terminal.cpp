// The detailed mode of design/interaction-topbar (7_chat_terminal): the bar widens into a
// workspace. On the left the conversation and the plan as mini steps (done ticked, the current
// one's ring turning); on the right the agent's processes as tabs, read only, their output inked
// as PowerShell, a cursor after the newest line; "Take over" gives the user a terminal of their
// own and holds the task. A command under the mouse lights its plan step.

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

// .term's inks.
auto ink_color(terminal::Ink ink) -> Color {
    switch (ink) {
    case terminal::Ink::Prompt: return ui::th().text_3; // the prompt: as a comment
    case terminal::Ink::Command: return ui::th().accent; // a command: as a keyword
    case terminal::Ink::String: return ui::th().text;
    case terminal::Ink::Dim: return ui::th().text_3;
    case terminal::Ink::Warn: return ui::th().warn;         // .w
    case terminal::Ink::Value: return ui::th().text;
    case terminal::Ink::Plain: break;
    }
    return ui::th().text;
}

constexpr float kLeft = 420.0f;   // .cols 420px 1fr
constexpr float kTabs = 42.0f;    // .tabs
constexpr float kFoot = 46.0f;    // .tfoot
constexpr float kMono = 13.0f;    // .term 13px/1.65
constexpr float kLine = 13.0f * 1.65f;

} // namespace

void Painter::draw_terminal(Model& m, const Box& panel, Reaction& r) {
    if (task_seen_ != m.task) {
        task_seen_ = m.task;
        task_seen_at_ = now_;
    }
    const int secs = static_cast<int>(now_ - task_seen_at_);
    const bool working = sheet::working(m);
    // "Working · 2:14": the chip keeps room for the time in figure spaces (as wide as digits), and
    // the time is drawn into it a character at a time — a new second shapes nothing new.
    constexpr std::string_view kWorking = "Working \xC2\xB7 \xE2\x80\x87\xE2\x80\x87\xE2\x80\x87\xE2\x80\x87\xE2\x80\x87";
    std::array<RowChip, 2> chips{{{working ? kWorking : std::string_view{"Done"}, working ? work::Tone::Accent : work::Tone::Ok, "chip"},
                                  {"Unsafe mode", work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, "Refine\xE2\x80\xA6", wt::type::placeholder_small, std::span{chips.data(), m.unsafe ? 2u : 1u},
             working ? RowButton::Stop : RowButton::Send, false, false, 0, true);
    if (working) {
        for (const auto& t : targets()) {
            if (t.name != "chip") {
                continue;
            }
            std::array<char, 16> clock{};
            const auto n = std::format_to_n(clock.data(), clock.size(), "{}:{:02}", std::min(secs / 60, 99), secs % 60);
            float x = t.box.x + t.box.w - wt::chip::pad_x;
            for (auto c = n.out; c != clock.data();) {
                --c;
                const std::string_view ch{c, 1};
                const float w = work::text_width(*ui_, Face::Text, wt::type::chip, ch);
                x -= w;
                work::caption(*ui_, {x, t.box.y, w + 1, t.box.h}, Face::Text, wt::type::chip, ch, ui::th().text_2);
            }
        }
    }
    if (!m.expanded || !has_detail(m)) {
        return; // folded back, or a new task took its place
    }
    auto& renderer = ui_->renderer();
    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_, panel.w - wt::body::pad_x,
                   std::max(0.0f, panel.h - row_h_)};
    if (body.w <= kLeft || body.h <= kTabs + kFoot) {
        return;
    }

    // Left: the question, the agent's words, the plan.
    const Box left{body.x, body.y + 20, kLeft - 24, body.h - 20};
    renderer.push_clip({left.x - 4, body.y, left.w + 8, body.h});
    float y = left.y + draw_question(m, left.x + left.w, left.y, left.w) + theme::bubble::margin_bottom;
    if (doc_) {
        doc_->set_width(left.w);
        const float words = doc_->height();
        draw_markdown({left.x, y, left.w, words}, {left.x, body.y, left.w, body.h}, m.streaming, r);
        y += words + 8;
        y += 6;
    }
    if (m.plan) {
        y += draw_plan(m, left.x, y, left.w);
    }
    renderer.pop_clip();

    // Right: the processes' tabs over the terminal.
    draw_terminal_pane(m, {body.x + kLeft, body.y, body.w - kLeft, body.h}, r);
}
auto Painter::draw_plan(Model& m, float x, float y, float w) -> float {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const Box left{x, y, w, 0};if (m.plan) {
    const auto& items = m.plan->items;
    constexpr float kStep = 23.0f; // .mini gap 7, 16 px rows
    const Box card{left.x, y, left.w, 14 + 22 + static_cast<float>(items.size()) * kStep + 10};
    work::card(*ui_, card);
    work::caption(*ui_, {card.x + 16, card.y + 12, card.w - 32, 16}, Face::Text, 12.0f, "Plan", ui::th().text_3);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        const Box row{card.x + 8, card.y + 36 + static_cast<float>(i) * kStep, card.w - 16, kStep};
        add_target(text(buf, "plan:{}", i), row);
        if (static_cast<int>(i) == m.hovered_step) {
            renderer.rect({row.x, row.y, row.w, row.h}, {.fill = ui::th().accent_soft, .radius = {6, 6, 6, 6}});
        }
        const float cx = row.x + 8 + 8;
        const float cy = row.y + row.h / 2;
        const float was = renderer.opacity();
        if (it.state == events::PlanState::Done) {
            renderer.rect({cx - 8, cy - 8, 16, 16}, {.fill = ui::th().ok, .radius = {8, 8, 8, 8}});
            ui_->draw_icon(widgets::kIconCheck, 9.0f, {cx, cy}, ui::th().on_accent);
        } else if (it.state == events::PlanState::Run) {
            renderer.arc({cx, cy}, 6.5f, 2.0f, 0, 6.2831853f, ui::th().w4);
            const float a = static_cast<float>(std::fmod(now_ * 4.0, 6.2831853));
            renderer.arc({cx, cy}, 6.5f, 2.0f, a, a + 1.6f, ui::th().accent);
        } else {
            renderer.set_opacity(was * 0.5f);
            renderer.arc({cx, cy}, 6.5f, 1.5f, 0, 6.2831853f, ui::th().w4);
        }
        const float tx = cx + 8 + 10;
        const Color tc = it.state == events::PlanState::Run ? ui::th().text : ui::th().text_2;
        const float tw = work::caption(*ui_, {tx, row.y, row.x + row.w - tx - 50, row.h}, Face::Text, 13.0f, it.title, tc);
        if (!it.note.empty()) {
            if (it.state == events::PlanState::Run) {
                const float nw = work::text_width(*ui_, Face::Text, 12.0f, it.note);
                work::caption(*ui_, {row.x + row.w - 8 - nw, row.y, nw, row.h}, Face::Text, 12.0f, it.note, ui::th().text_3);
            } else {
                const float nw = work::text_width(*ui_, Face::Mono, 12.0f, it.note) + 10;
                const Box pill{tx + tw + 8, cy - 9, nw, 18};
                renderer.rect({pill.x, pill.y, pill.w, pill.h}, {.fill = ui::th().w1, .radius = {4, 4, 4, 4}});
                work::caption(*ui_, {pill.x + 5, pill.y, nw - 10, pill.h}, Face::Mono, 12.0f, it.note, ui::th().text_2);
            }
        }
        renderer.set_opacity(was);
    }
}
    return 14 + 22 + static_cast<float>(m.plan->items.size()) * 23.0f + 10 + 8;
}

void Painter::draw_terminal_pane(Model& m, const Box& pane, Reaction& r) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const auto& in = ui_->input();    renderer.rect({pane.x - 1, pane.y, 1, pane.h}, {.fill = ui::th().line});
    renderer.rect({pane.x, pane.y, pane.w, pane.h}, {.fill = ui::th().sunk});
    m.tab_shown = std::min(m.tab_shown, m.tabs.empty() ? std::size_t{0} : m.tabs.size() - 1);
    float tx = pane.x + 12;
    for (std::size_t i = 0; i < m.tabs.size(); ++i) {
        const bool on = i == m.tab_shown;
        const float lw = work::text_width(*ui_, Face::Text, 12.5f, m.tabs[i].label);
        const float iw = i == 0 ? 21.0f : 0.0f;
        const Box tab{tx, pane.y + kTabs - 33, lw + iw + 28, 33};
        if (on) {
            renderer.rect({tab.x, tab.y, tab.w, tab.h}, {.fill = ui::th().w2, .radius = {8, 8, 0, 0}});
        }
        if (i == 0) {
            ui_->draw_icon(work::kIconTerminal, 13.0f, {tab.x + 14 + 7, tab.y + tab.h / 2}, on ? ui::th().text : ui::th().text_3);
        }
        work::caption(*ui_, {tab.x + 14 + iw, tab.y, lw + 2, tab.h}, Face::Text, 12.5f, m.tabs[i].label,
                    on ? ui::th().text : ui::th().text_3);
        if (ui_->interact(id_of("tab", i), tab).clicked) {
            m.tab_shown = i;
        }
        add_target(text(buf, "tab:{}", i), tab);
        tx += tab.w + 2;
    }
    renderer.rect({pane.x, pane.y + kTabs - 1, pane.w, 1}, {.fill = ui::th().line});
    // "read only" and Take over.
    const auto take = work::button_size(*ui_, "Take over", true);
    const Box take_box{pane.x + pane.w - 16 - take.w - 18, pane.y + (kTabs - take.h) / 2, take.w + 18, take.h};
    if (work::button(*ui_, id_of("take_over", 0), take_box, "", work::ButtonStyle::Normal, true) && !r.command) {
        r.command = press(m, "take_over");
    }
    ui_->draw_icon(work::kIconHand, 12.0f, {take_box.x + 14, take_box.y + take_box.h / 2}, ui::th().text);
    work::caption(*ui_, {take_box.x + 24, take_box.y, take_box.w - 28, take_box.h}, Face::Semibold, wt::type::button_small, "Take over",
                ui::th().text);
    add_target("take_over", take_box);
    const float ro = work::text_width(*ui_, Face::Text, 12.0f, "read only");
    work::caption(*ui_, {take_box.x - 8 - ro, pane.y, ro, kTabs}, Face::Text, 12.0f, "read only", ui::th().text_3);
    ui_->draw_icon(work::kIconEye, 12.0f, {take_box.x - 8 - ro - 12, pane.y + kTabs / 2}, ui::th().text_3);

    // The terminal: the newest lines that fit, inked as they came; the cursor after the last.
    const Box term{pane.x, pane.y + kTabs, pane.w, pane.h - kTabs - kFoot};
    add_target("terminal", term);
    m.hovered_step = -1;
    if (!m.tabs.empty()) {
        const auto& lines = m.tabs[m.tab_shown].buffer;
        const auto visible = static_cast<std::size_t>(std::max(0.0f, (term.h - 28) / kLine));
        if (in.wheel != 0 && in.mouse_x >= term.x && in.mouse_x < term.x + term.w && in.mouse_y >= term.y &&
            in.mouse_y < term.y + term.h) {
            terminal_scroll_ += in.wheel * 3;
        }
        const float most = static_cast<float>(lines.size() > visible ? lines.size() - visible : 0);
        terminal_scroll_ = std::clamp(terminal_scroll_, 0.0f, most);
        const std::size_t last = lines.size() - static_cast<std::size_t>(terminal_scroll_);
        const std::size_t first = last > visible ? last - visible : 0;
        renderer.push_clip({term.x, term.y, term.w, term.h});
        float ly = term.y + 14;
        for (std::size_t i = first; i < last; ++i) {
            const auto& line = lines.line(i);
            const Box row{term.x + 8, ly, term.w - 16, kLine};
            add_target(text(buf, "line:{}", i - first), row);
            const bool hover = in.mouse_x >= row.x && in.mouse_x < row.x + row.w && in.mouse_y >= row.y && in.mouse_y < row.y + row.h;
            if (hover && line.step >= 0) {
                m.hovered_step = line.step;
                renderer.rect({row.x, row.y, row.w, row.h}, {.fill = ui::th().accent_soft}); // .hlc
            }
            float x = term.x + 18;
            const float right = term.x + term.w - 18;
            for (const auto& t : line.tokens) {
                if (x >= right) {
                    break;
                }
                const std::string_view part{line.text.data() + t.begin, t.end - t.begin};
                const float room = right - x;
                x += work::caption(*ui_, {x, ly, room, kLine}, Face::Mono, kMono, part, ink_color(t.ink));
                if (work::text_width(*ui_, Face::Mono, kMono, part) > room + 0.01f) { // caption's own measure of "cut"
                    break; // cut with its "…": the line ends there, the next word adds no second one
                }
            }
            if (i + 1 == lines.size() && std::fmod(now_, 1.0) < 0.5) {
                renderer.rect({std::min(x + 2, right), ly + (kLine - 16) / 2, 8, 16}, {.fill = ui::th().text_2}); // .tcur
            }
            ly += kLine;
        }
        renderer.pop_clip();
    }

    // .tfoot
    const float fy = pane.y + pane.h - kFoot;
    renderer.rect({pane.x, fy, pane.w, 1}, {.fill = ui::th().line});
    work::caption(*ui_, {pane.x + 16, fy, pane.w / 2, kFoot}, Face::Text, 12.5f, m.agent_usage, ui::th().text_3);
    const std::string_view hint = "Hover a command to see why in the chat";
    const float hw = work::text_width(*ui_, Face::Text, 12.5f, hint);
    work::caption(*ui_, {pane.x + pane.w - 16 - hw, fy, hw, kFoot}, Face::Text, 12.5f, hint, ui::th().text_3);
}
} // namespace app::sheet
