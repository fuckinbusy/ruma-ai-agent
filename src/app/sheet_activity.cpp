// The Activity tab (phase 8, docs/specs/phase-8-ui-remaster.md § 5.4; mockup-spec § 3, Activity
// log): what the turn ran — a summary (how many, how long, read-only or how many changes, Copy
// all), then each call, the failed ones first: its number, title and purpose, its time and exit
// code, its command, its output folded (twelve lines when opened; Show all).

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <numeric>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet/activity")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

constexpr float kPadX = 22.0f;
constexpr float kSummary = 44.0f;
constexpr float kHeader = 46.0f;
constexpr float kCommand = 35.0f;
constexpr float kToggle = 26.0f;
constexpr float kOutHeader = 32.0f;
constexpr float kLine = 12.0f * 1.6f; // mono 12/1.6
constexpr float kMore = 24.0f;
constexpr std::size_t kFirstLines = 12;
constexpr float kIndent = 50.0f;      // the command block's margin 0 14 10 50

// Its words in full (responsive text): they wrap, the entry grows.
auto title_style() -> work::TextBlockStyle {
    return {.face = Face::Semibold, .size = 13.5f, .color = ui::th().text, .line = 1.0f, .min_line_h = 19.0f};
}
auto purpose_style() -> work::TextBlockStyle {
    return {.face = Face::Text, .size = 12.5f, .color = ui::th().text_3, .line = 1.0f, .min_line_h = 16.0f};
}
auto command_style() -> work::TextBlockStyle {
    return {.face = Face::Mono, .size = 12.5f, .color = ui::th().text, .min_line_h = 20.0f};
}
auto summary_style() -> work::TextBlockStyle {
    return {.face = Face::Text, .size = 13.0f, .color = ui::th().text, .min_line_h = 20.0f};
}

constexpr char32_t kIconShield = 0xEA18;

auto failed(const events::ActivityEntry& e) -> bool {
    return !e.internal && e.exit_code != 0;
}

} // namespace

auto Painter::output_of(std::uint64_t task, const events::ActivityEntry& e) -> Output& {
    auto it = std::ranges::find_if(outputs_, [&](const Output& o) { return o.task == task && o.number == e.number; });
    if (it == outputs_.end()) {
        outputs_.push_back({.task = task, .number = e.number, .size = static_cast<std::size_t>(-1)});
        it = outputs_.end() - 1;
    }
    if (it->size != e.output.size()) { // its lines, found once (and again only if it grew)
        it->size = e.output.size();
        it->starts.clear();
        for (std::size_t i = 0; i < e.output.size(); i = e.output.find('\n', i) == std::string::npos ? e.output.size()
                                                                                                         : e.output.find('\n', i) + 1) {
            it->starts.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return *it;
}

auto Painter::header_pills(const events::ActivityEntry& e) -> float {
    // What the header's right takes: "Inside the agent" or the time and the exit code, and their gaps.
    std::array<char, 64> buf{};
    if (e.internal) {
        return work::text_width(*ui_, Face::Text, 12.0f, "Inside the agent \xC2\xB7 no command") + 18 + 10;
    }
    const float code = work::text_width(*ui_, Face::Text, 12.0f, text(buf, "exit {}", e.exit_code)) + 18 + 10;
    return code + work::text_width(*ui_, Face::Text, 12.5f, text(buf, "{:.2f} s", e.seconds)) + 10;
}

auto Painter::activity_entry(const events::ActivityEntry& e, std::uint64_t task, float x, float y0, float w, const Box& visible,
                             bool measure) -> float {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    float y = y0;
    const bool bad = failed(e);
    auto* out = e.output.empty() ? nullptr : &output_of(task, e);
    const std::size_t lines = out == nullptr ? 0 : out->starts.size();
    const std::size_t shown = out == nullptr || !out->open ? 0 : out->all ? lines : std::min(lines, kFirstLines);

    // The header's words in full (responsive text): the title and its purpose wrap left of the pills.
    const float pills = header_pills(e);
    const float tx = x + 14 + 24 + 12;
    const float room = std::max(0.0f, x + w - 14 - pills - tx);
    const float th = work::text_block_height(*ui_, e.title, title_style(), room);
    const float ph = e.purpose.empty() ? 0.0f : work::text_block_height(*ui_, e.purpose, purpose_style(), room);
    const float head = std::max(kHeader, 6 + th + ph + 5);
    // The command in full, in its block ("Run: Get-Process | …" cut).
    const float bw0 = std::max(0.0f, w - kIndent - 14);
    const float cw = std::max(0.0f, bw0 - 30 - work::text_width(*ui_, Face::Mono, 12.5f, "PS>"));
    const float ch = e.internal || e.command.empty() ? 0.0f : std::max(kCommand, work::text_block_height(*ui_, e.command, command_style(), cw) + 15);
    float h = head;
    if (ch > 0) {
        h += ch + 10;
    }
    if (out == nullptr && e.set != 0) {
        h += kToggle; // "Open table" alone in its row
    }
    if (out != nullptr) {
        h += kToggle;
        if (out->open) {
            h += kOutHeader + static_cast<float>(shown) * kLine + (lines > shown ? kMore : 0.0f) + 6 + 6;
        }
    }
    h += 6;
    const Box box{x, y, w, h};
    if (measure || box.y > visible.y + visible.h || box.y + box.h < visible.y) {
        return h;
    }
    add_target(text(buf, "activity:entry:{}", e.number), box);
    renderer.rect({box.x, box.y, box.w, box.h},
                  {.fill = e.internal ? Color{} : ui::th().w1, .radius = ui::card_corners(),
                   .border = e.internal ? ui::th().w1 : bad ? ui::th().crit_line : ui::th().line, .border_width = 1});

    // The header: the number, the title and its purpose; the time and how it ended.
    const Box badge{x + 14, y + 11, 24, 24};
    renderer.rect({badge.x, badge.y, badge.w, badge.h},
                  {.fill = bad ? ui::th().crit_soft : ui::th().w2, .radius = {7, 7, 7, 7}});
    const auto num = text(buf, "{}", e.number);
    const float nw = work::text_width(*ui_, Face::Semibold, 12.0f, num);
    work::caption(*ui_, {badge.x + (badge.w - nw) / 2, badge.y, nw + 1, badge.h}, Face::Semibold, 12.0f, num,
                bad ? ui::th().crit : ui::th().text_3);
    float right = x + w - 14;
    if (e.internal) {
        const std::string_view note = "Inside the agent \xC2\xB7 no command";
        const float pw = work::text_width(*ui_, Face::Text, 12.0f, note) + 18;
        const Box pill{right - pw, y + 12, pw, 22};
        add_target(text(buf, "activity:internal:{}", e.number), pill);
        renderer.rect({pill.x, pill.y, pill.w, pill.h}, {.fill = ui::th().w1, .radius = {11, 11, 11, 11}});
        work::caption(*ui_, {pill.x + 9, pill.y, pw - 9, pill.h}, Face::Text, 12.0f, note, ui::th().text_3);
        right = pill.x - 10;
    } else {
        const auto code = text(buf, "exit {}", e.exit_code);
        const float pw = work::text_width(*ui_, Face::Text, 12.0f, code) + 18;
        const Box pill{right - pw, y + 12, pw, 22};
        const Color tone = bad ? ui::th().crit : ui::th().ok;
        renderer.rect({pill.x, pill.y, pill.w, pill.h}, {.fill = bad ? ui::th().crit_soft : ui::th().ok_soft, .radius = {11, 11, 11, 11}});
        work::caption(*ui_, {pill.x + 9, pill.y, pw - 9, pill.h}, Face::Text, 12.0f, code, tone);
        right = pill.x - 10;
        const auto time = text(buf, "{:.2f} s", e.seconds);
        const float tw = work::text_width(*ui_, Face::Text, 12.5f, time);
        work::caption(*ui_, {right - tw, y + 12, tw, 22}, Face::Text, 12.5f, time, ui::th().text_3);
        right -= tw + 10;
    }
    (void)right;
    auto title_look = title_style();
    title_look.color = bad ? ui::th().crit : ui::th().text;
    // A title alone on one line sits in the header's middle, as it did.
    const float title_y = e.purpose.empty() && th <= title_style().min_line_h + 0.5f ? y + (kHeader - th) / 2 : y + 6;
    (void)card_text(text(buf, "activity:title:{}:", e.number), 0, {tx, title_y, room, 0}, e.title, title_look);
    if (!e.purpose.empty()) {
        (void)card_text(text(buf, "activity:purpose:{}:", e.number), 0, {tx, y + 6 + th, room, 0}, e.purpose, purpose_style());
    }
    y += head;

    // The command: "PS>" and its words, all of them.
    const float bx = x + kIndent;
    const float bw = bw0;
    if (ch > 0) {
        renderer.rect({bx, y, bw, ch}, {.fill = ui::th().sunk, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
        const float one = command_style().min_line_h;
        const float pw = work::text_width(*ui_, Face::Mono, 12.5f, "PS>"); // its own width: the command's start is the command's
        (void)work::caption(*ui_, {bx + 11, y + 7.5f, pw, one}, Face::Mono, 12.5f, "PS>", ui::th().accent);
        (void)card_text(text(buf, "activity:command:{}:", e.number), 0, {bx + 11 + pw + 8, y + 7.5f, cw, 0}, e.command, command_style());
        y += ch + 10;
    }

    // A step that made a set: "Open table" at its row's right (responsive text: a set replaced in the
    // panel's tabs by a later one is still reached from here). Before the row's toggle: it takes its click.
    float open_w = 0;
    if (e.set != 0) {
        const auto size = work::button_size(*ui_, "Open table", true);
        const Box open{bx + bw - size.w, y + (kToggle - size.h) / 2, size.w, size.h};
        add_target(text(buf, "activity:open:{}", e.number), open);
        if (work::button(*ui_, id_of("open", task * 1000 + static_cast<std::uint64_t>(e.number)), open, "Open table",
                         work::ButtonStyle::Normal, true)) {
            open_set_ = e.set;
        }
        open_w = size.w + 8;
        if (out == nullptr) {
            y += kToggle;
        }
    }
    // The output: folded, its lines counted; opened, twelve of them, or all.
    if (out != nullptr) {
        const Box toggle{bx, y, bw - open_w, kToggle};
        add_target(text(buf, "activity:output:{}", e.number), toggle);
        if (!out->open) {
            ui_->draw_icon(work::kIconChevronRight, 12.0f, {toggle.x + 6, toggle.y + kToggle / 2}, ui::th().text_3);
            work::caption(*ui_, {toggle.x + 18, toggle.y, bw - 18 - open_w, kToggle}, Face::Text, 12.5f,
                        text(buf, "Output \xC2\xB7 {} {}", lines, lines == 1 ? "line" : "lines"), ui::th().text_3);
        }
        if (ui_->interact(id_of("output", task * 1000 + static_cast<std::uint64_t>(e.number)), toggle, false).clicked) {
            out->open = !out->open;
        }
        y += kToggle;
        if (out->open) {
            const float body_h = static_cast<float>(shown) * kLine;
            const Box frame{bx, y - kToggle, bw, kOutHeader + body_h + (lines > shown ? kMore : 0.0f) + 6};
            renderer.rect({frame.x, frame.y, frame.w, frame.h}, {.fill = ui::th().sunk, .radius = {8, 8, 8, 8}});
            ui_->draw_icon(work::kIconChevronDown, 12.0f, {frame.x + 12, frame.y + kOutHeader / 2}, ui::th().text_3);
            work::caption(*ui_, {frame.x + 24, frame.y, bw - 30, kOutHeader}, Face::Text, 12.5f,
                        text(buf, "Output \xC2\xB7 {} {}", lines, lines == 1 ? "line" : "lines"), ui::th().text_2);
            const Box body{frame.x + 12, frame.y + kOutHeader, bw - 24, body_h};
            add_target(text(buf, "activity:output:{}:body", e.number), body);
            // Only the lines in view are drawn.
            const float top = std::max(body.y, visible.y);
            const float bottom = std::min(body.y + body.h, visible.y + visible.h);
            if (bottom > top) {
                const auto first = static_cast<std::size_t>((top - body.y) / kLine);
                const auto last = std::min(shown, static_cast<std::size_t>((bottom - body.y) / kLine) + 1);
                for (std::size_t i = first; i < last; ++i) {
                    const std::size_t from = out->starts[i];
                    std::size_t to = i + 1 < out->starts.size() ? out->starts[i + 1] : e.output.size();
                    while (to > from && (e.output[to - 1] == '\n' || e.output[to - 1] == '\r')) {
                        --to;
                    }
                    ++output_lines_drawn_;
                    work::caption(*ui_, {body.x, body.y + static_cast<float>(i) * kLine, body.w, kLine}, Face::Mono, 12.0f,
                                std::string_view{e.output}.substr(from, to - from), ui::th().text_2);
                }
            }
            if (lines > shown) {
                const Box more{body.x, body.y + body.h, body.w, kMore};
                add_target(text(buf, "activity:showall:{}", e.number), more);
                work::caption(*ui_, more, Face::Text, 12.5f,
                            text(buf, "\xE2\x80\xA6 {} more lines \xC2\xB7 Show all", lines - shown), ui::th().text_4);
                if (ui_->interact(id_of("all", task * 1000 + static_cast<std::uint64_t>(e.number)), more, false).clicked) {
                    out->all = true;
                }
            }
        }
    }
    return h;
}

void Painter::draw_activity(Model& m, std::span<const events::ActivityEntry> entries, const Box& box, Reaction& r) {
    (void)m;
    auto& renderer = ui_->renderer();
    const std::uint64_t task = m.activity_task;
    if (task != activity_scroll_task_) {
        activity_scroll_task_ = task;
        activity_scroll_ = {};
    }
    // The summary: how many, how long, what it changed.
    std::size_t changes = 0;
    bool internal = false;
    double seconds = 0;
    for (const auto& e : entries) {
        changes += e.changed ? 1 : 0;
        internal = internal || e.internal;
        seconds += e.seconds;
    }
    activity_summary_.clear();
    std::format_to(std::back_inserter(activity_summary_), "{} {} \xC2\xB7 {:.2f} s \xC2\xB7 ", entries.size(),
                   internal ? (entries.size() == 1 ? "step" : "steps") : (entries.size() == 1 ? "command" : "commands"), seconds);
    if (changes == 0) {
        activity_summary_ += "Read-only \xE2\x80\x94 nothing was changed on this PC";
    } else {
        std::format_to(std::back_inserter(activity_summary_), "{} {} on this PC", changes, changes == 1 ? "change" : "changes");
    }
    // The failed ones first, each group in its order.
    activity_order_.resize(entries.size());
    std::iota(activity_order_.begin(), activity_order_.end(), 0u);
    std::ranges::stable_partition(activity_order_, [&](std::uint32_t i) { return failed(entries[i]); });

    const float x = box.x + kPadX;
    const float w = std::max(0.0f, box.w - 2 * kPadX);
    // The summary in full beside Copy all: as tall as its lines.
    const float summary_w = std::max(0.0f, w - 40 - 10 - work::button_size(*ui_, "Copy all", true).w - 10);
    const float summary_text = work::text_block_height(*ui_, activity_summary_, summary_style(), summary_w);
    const float summary_h = std::max(kSummary, summary_text + 24);
    float content = 18 + summary_h + 12;
    for (const auto i : activity_order_) {
        content += activity_entry(entries[i], task, x, 0, w, box, true) + 10;
    }
    activity_scroll_.foot = work::corner_foot(bottom_corners(box).br);
    const Box at = work::scroll_view(*ui_, activity_scroll_, box, content);
    float y = at.y + 18;
    ui_->order_base(1u << 20); // the summary: before every entry's
    const Box summary{x, y, w, summary_h};
    add_target("activity:summary", summary);
    const Color tone = changes == 0 ? ui::th().ok : ui::th().warn;
    renderer.rect({summary.x, summary.y, summary.w, summary.h},
                  {.fill = changes == 0 ? ui::th().ok_soft : ui::th().warn_soft, .radius = ui::card_corners(),
                   .border = changes == 0 ? ui::th().ok_line : ui::th().warn_line, .border_width = 1});
    ui_->draw_icon(kIconShield, 17.0f, {summary.x + 14 + 8.5f, summary.y + summary_h / 2}, tone);
    const auto copy_size = work::button_size(*ui_, "Copy all", true);
    const Box copy{summary.x + summary.w - 10 - copy_size.w, summary.y + (summary_h - copy_size.h) / 2, copy_size.w, copy_size.h};
    add_target("activity:copy", copy);
    (void)work::text_block(*ui_, {summary.x + 40, summary.y + (summary_h - summary_text) / 2, summary_w, 0}, activity_summary_, summary_style());
    if (work::button(*ui_, id_of("copy", task), copy, "Copy all", work::ButtonStyle::Normal, true)) {
        panel_text_.clear();
        for (const auto& e : entries) {
            panel_text_ += e.internal ? "# " : "PS> ";
            panel_text_ += e.internal ? e.title : e.command;
            panel_text_ += '\n';
            panel_text_ += e.output;
            if (!e.output.empty() && e.output.back() != '\n') {
                panel_text_ += '\n';
            }
        }
        r.clipboard = panel_text_;
    }
    y += summary_h + 12;
    for (std::size_t k = 0; k < activity_order_.size(); ++k) {
        ui_->order_base((static_cast<std::uint64_t>(k) + 1) << 24); // its texts' keys, whether the ones above are drawn or not
        y += activity_entry(entries[activity_order_[k]], task, x, y, w, box, false) + 10;
    }
    work::end_scroll(*ui_, activity_scroll_, box, panel_color(), bottom_corners(box));
    if (open_set_ != 0) { // a step's "Open table": its set in the panel, its own tab active
        show_set(m, open_set_);
        open_set_ = 0;
        redraw_due_ = true;
    }
}

} // namespace app::sheet
