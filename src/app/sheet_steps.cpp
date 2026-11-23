// The steps screen of design/interaction-topbar (1_steps): every action of the task is a card
// under the bar — done ones one line with their result, the running one open with its progress,
// its command and its warnings, the waiting ones faint with what they will need. The summary of
// the task stands beside them.

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

auto id_of(std::string_view name) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet"));
}

auto id_of(std::string_view name, std::uint32_t n) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
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

auto tone_of(events::Tone t) -> work::Tone {
    switch (t) {
    case events::Tone::Accent: return work::Tone::Accent;
    case events::Tone::Ok: return work::Tone::Ok;
    case events::Tone::Warn: return work::Tone::Warn;
    case events::Tone::Crit: return work::Tone::Crit;
    case events::Tone::Plain: return work::Tone::Plain;
    }
    return work::Tone::Plain;
}

auto icon_of(events::Kind k) -> char32_t {
    switch (k) {
    case events::Kind::Read: return work::kIconSearch;
    case events::Kind::Change: return work::kIconMove;
    case events::Kind::Delete: return work::kIconTrash;
    case events::Kind::System: return work::kIconSystem;
    case events::Kind::Ok: return widgets::kIconCheck;
    }
    return work::kIconSearch;
}

auto badge_of(events::StepState s) -> work::Badge {
    switch (s) {
    case events::StepState::Done: return work::Badge::Done;
    case events::StepState::Run: return work::Badge::Run;
    case events::StepState::Wait: return work::Badge::Wait;
    case events::StepState::Failed:
    case events::StepState::Skipped: break;
    }
    return work::Badge::None;
}

// Text made each frame goes into a buffer on the stack: std::format allocates.
template <class... Args>
auto text(std::array<char, 48>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

constexpr float kSummaryWidth = 280.0f; // .cols 1fr 280px
constexpr float kHeadMin = wt::step::min_height;
constexpr float kMono = wt::type::mono;
constexpr float kMonoLine = kMono * 1.6f; // .cmd line-height 1.6

} // namespace

auto Painter::step_open(const events::Step& s) const -> bool {
    const bool flipped = std::ranges::find(toggled_, s.id) != toggled_.end();
    const bool open = s.state == events::StepState::Run;
    return open != flipped;
}

auto Painter::draw_step(const events::Step& s, float x, float y, float w, bool measure) -> float {
    auto& renderer = ui_->renderer();
    const float title_h = ui_->line_height(Face::Semibold, wt::type::step_title);
    const float sub_h = s.detail.empty() ? 0.0f : ui_->line_height(Face::Text, wt::type::step_sub) + 2;
    const float head = std::max(kHeadMin, 2 * wt::step::pad_y + title_h + sub_h);
    const bool open = !brief_ && step_open(s);

    // The open body: files, progress, the command, warnings (.sb, padding 0 14 14 56, gap 6).
    const float bx = x + wt::step::body_left;
    const float bw = std::max(0.0f, w - wt::step::body_left - wt::step::pad_x);
    float body = 0;
    std::size_t command_lines = 0;
    std::array<std::string_view, 8> lines{};
    if (open) {
        const auto add = [&](float h) { body += (body > 0 ? wt::step::body_gap : 0.0f) + h; };
        for (std::size_t i = 0; i < s.files.size(); ++i) {
            add(wt::file_row::height);
        }
        if (s.more_files > 0) {
            add(wt::file_row::height);
        }
        if (s.progress >= 0) {
            add(wt::progress::height);
            if (!s.progress_text.empty() || !s.eta.empty()) {
                add(ui_->line_height(Face::Text, wt::type::step_sub));
            }
        }
        if (!s.command.empty()) {
            // Wrapped by words inside the block (padding 8 96 8 11): "PS> " then the command.
            const float room = std::max(40.0f, bw - 11 - 96);
            std::string_view rest = s.command;
            float first_indent = work::text_width(*ui_, Face::Mono, kMono, "PS> ");
            while (!rest.empty() && command_lines < lines.size()) {
                const float avail = room - (command_lines == 0 ? first_indent : 0.0f);
                std::size_t cut = rest.size();
                if (work::text_width(*ui_, Face::Mono, kMono, rest) > avail) {
                    cut = 0;
                    for (std::size_t sp = rest.find(' '); sp != std::string_view::npos; sp = rest.find(' ', sp + 1)) {
                        if (work::text_width(*ui_, Face::Mono, kMono, rest.substr(0, sp)) > avail) {
                            break;
                        }
                        cut = sp;
                    }
                    if (cut == 0) {
                        cut = std::min(rest.size(), static_cast<std::size_t>(std::max(1.0f, avail / (kMono * 0.6f))));
                    }
                }
                lines[command_lines++] = rest.substr(0, cut);
                rest.remove_prefix(cut);
                while (!rest.empty() && rest.front() == ' ') {
                    rest.remove_prefix(1);
                }
                first_indent = 0;
            }
            add(16 + static_cast<float>(command_lines) * kMonoLine);
        }
        for (std::size_t i = 0; i < s.notes.size(); ++i) {
            add(ui_->line_height(Face::Text, wt::type::step_sub));
        }
        if (body > 0) {
            body += wt::step::pad_x; // the bottom padding
        }
    }
    const float h = head + body;
    if (measure) {
        return h;
    }

    // .step / .step.run / .step.wait
    const Color k = work::kind_color(kind_of(s.kind));
    const Box card{x, y, w, h};
    if (s.state == events::StepState::Run) {
        renderer.rect({x - 3, y - 3, w + 6, h + 6},
                      {.fill = {}, .radius = ui::corners(ui::th().radius.card + 3), .border = with_alpha(k, 0.08f), .border_width = 3});
        renderer.rect({x, y, w, h}, {.fill = ui::th().card, .radius = ui::card_corners(), .border = with_alpha(k, 0.42f),
                                     .border_width = 1});
    } else if (s.state == events::StepState::Wait) {
        renderer.rect({x, y, w, h}, {.fill = {}, .radius = ui::card_corners(), .border = ui::th().w1, .border_width = 1});
    } else {
        work::card(*ui_, card);
    }
    const float icon = wt::kind_icon::size;
    work::kind_icon(*ui_, {x + wt::step::pad_x, y + (head - icon) / 2, icon, icon}, kind_of(s.kind), icon_of(s.kind),
                    badge_of(s.state), id_of("step:badge", s.id * 4 + 1)); // MS 06: its badge turns, never switches
    // Right side: the result, a tag for a waiting step, a chevron where there is more to see.
    float right = x + w - wt::step::pad_x;
    const bool has_more = !s.files.empty() || !s.command.empty() || s.progress >= 0 || !s.notes.empty();
    if (has_more && s.state != events::StepState::Run && s.state != events::StepState::Wait) {
        ui_->draw_icon(open ? work::kIconChevronDown : work::kIconChevronRight, 11.0f, {right - 8, y + head / 2},
                       ui::th().text_3);
        right -= 16 + wt::step::gap;
    }
    if (!s.tag.empty()) {
        const auto t = work::tag_size(*ui_, s.tag);
        work::tag(*ui_, {right - t.w, y + (head - t.h) / 2, t.w, t.h}, s.tag, work::Kind::Change);
        right -= t.w + wt::step::gap;
    } else if (!s.result.empty()) {
        const float rw = work::text_width(*ui_, Face::Text, wt::type::step_result, s.result);
        work::caption(*ui_, {right - rw, y, rw, head}, Face::Text, wt::type::step_result, s.result, ui::th().text_3);
        right -= rw + wt::step::gap;
    }
    const float tx = x + wt::step::pad_x + icon + wt::step::gap;
    const float tw = std::max(0.0f, right - tx);
    const float top = y + (head - title_h - sub_h) / 2;
    const bool waiting = s.state == events::StepState::Wait;
    work::caption(*ui_, {tx, top, tw, title_h}, waiting ? Face::Text : Face::Semibold, wt::type::step_title, s.title,
                waiting ? ui::th().text_2 : ui::th().text);
    if (!s.detail.empty()) {
        work::caption(*ui_, {tx, top + title_h + 2, tw, sub_h - 2}, Face::Text, wt::type::step_sub, s.detail,
                    ui::th().text_3);
    }
    const Box header{x, y, w, head};
    const auto hit = ui_->interact(id_of("step", s.id), header, false);
    std::array<char, 48> buf{};
    add_target(text(buf, "step:{}", s.id), header);
    if (hit.clicked && has_more) {
        if (const auto it = std::ranges::find(toggled_, s.id); it != toggled_.end()) {
            toggled_.erase(it);
        } else {
            toggled_.push_back(s.id);
        }
    }
    if (!open || body <= 0) {
        return h;
    }

    float by = y + head;
    const auto next = [&](float row_h) {
        const float at = by;
        by += row_h + wt::step::body_gap;
        return at;
    };
    for (const auto& f : s.files) {
        work::file_row(*ui_, {bx, next(wt::file_row::height), bw, wt::file_row::height}, f.ext, f.name,
                       f.meta);
    }
    if (s.more_files > 0) {
        const float ry = next(wt::file_row::height);
        work::caption(*ui_, {bx + wt::file_row::pad_x, ry, bw, wt::file_row::height}, Face::Text, wt::type::file,
                    text(buf, "+ {} more files", s.more_files), ui::th().text_3);
    }
    if (s.progress >= 0) {
        work::progress_bar(*ui_, {bx, next(wt::progress::height), bw, wt::progress::height}, s.progress, k);
        if (!s.progress_text.empty() || !s.eta.empty()) {
            const float line = ui_->line_height(Face::Text, wt::type::step_sub);
            const float ry = next(line);
            work::caption(*ui_, {bx, ry, bw * 0.7f, line}, Face::Text, wt::type::step_sub, s.progress_text, ui::th().text_3);
            const float ew = work::text_width(*ui_, Face::Text, wt::type::step_sub, s.eta);
            work::caption(*ui_, {bx + bw - ew, ry, ew, line}, Face::Text, wt::type::step_sub, s.eta, ui::th().text_3);
        }
    }
    if (!s.command.empty()) {
        const float ch = 16 + static_cast<float>(command_lines) * kMonoLine;
        const Box block{bx, next(ch), bw, ch};
        renderer.rect({block.x, block.y, block.w, block.h},
                      {.fill = ui::th().sunk, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
        float lx = block.x + 11;
        for (std::size_t i = 0; i < command_lines; ++i) {
            const Box line{lx, block.y + 8 + static_cast<float>(i) * kMonoLine, block.w - 11 - 96, kMonoLine};
            float px = line.x;
            if (i == 0) {
                px += work::caption(*ui_, line, Face::Mono, kMono, "PS> ", ui::th().accent);
            }
            work::caption(*ui_, {px, line.y, line.x + line.w - px, line.h}, Face::Mono, kMono, lines[i], ui::th().text);
        }
        if (!s.command_label.empty()) {
            const float lw = work::text_width(*ui_, Face::Text, 10.0f, s.command_label);
            work::caption(*ui_, {block.x + block.w - 9 - lw, block.y + 6, lw, 14}, Face::Text, 10.0f, s.command_label,
                        ui::th().text_3);
        }
    }
    for (const auto& note : s.notes) {
        const float line = ui_->line_height(Face::Text, wt::type::step_sub);
        const float ny = next(line);
        ui_->draw_icon(work::kIconWarning, 12.0f, {bx + 7, ny + line / 2}, ui::th().warn);
        work::caption(*ui_, {bx + 22, ny, bw - 22, line}, Face::Text, wt::type::step_sub, note, ui::th().warn);
    }
    add_target(text(buf, "step:{}:body", s.id), {x, y + head, w, body});
    return h;
}

void Painter::draw_summary(const events::Summary& s, const Box& column) {
    add_target("summary", column);
    float y = column.y + 2;
    // .side h5: SUMMARY
    work::caption(*ui_, {column.x + 2, y, column.w, 16}, Face::Semibold, 12.0f, "SUMMARY", ui::th().text_3);
    y += 16 + 10;
    // .card.big: the ring, the value, what it is.
    const float big_h = 76 + 32;
    work::card(*ui_, {column.x, y, column.w, big_h});
    float vx = column.x + 16;
    if (s.fraction >= 0) {
        work::donut(*ui_, {column.x + 16, y + 16, 76, 76}, s.fraction);
        vx += 76 + 14;
    }
    const float value_h = ui_->line_height(Face::Display, 26.0f);
    const float label_h = ui_->line_height(Face::Text, 12.5f);
    const float vy = y + (big_h - value_h - 3 - label_h) / 2;
    const float vw = work::caption(*ui_, {vx, vy, column.x + column.w - 16 - vx, value_h}, Face::Display, 26.0f, s.value,
                                 ui::th().text);
    if (!s.of.empty()) {
        work::caption(*ui_, {vx + vw + 4, vy + 6, column.x + column.w - 16 - (vx + vw + 4), value_h - 6}, Face::Text, 13.0f,
                    s.of, ui::th().text_3);
    }
    work::caption(*ui_, {vx, vy + value_h + 3, column.x + column.w - 16 - vx, label_h}, Face::Text, 12.5f, s.label,
                ui::th().text_3);
    y += big_h + 6;
    const float row = ui_->line_height(Face::Text, wt::type::kv) + 2 * wt::kv::pad_y;
    for (const auto& f : s.facts) {
        work::kv_row(*ui_, {column.x + 4, y, column.w - 8, row}, f.key, f.value, tone_of(f.tone));
        y += row;
    }
}

void Painter::draw_steps(Model& m, const Box& panel, Reaction& r) {
    if (steps_task_ != m.task) {
        steps_task_ = m.task;
        toggled_.clear();
        steps_scroll_ = 0;
        steps_follow_ = true;
    }
    // The row: what step this is, the stop button, the progress line.
    std::size_t at = 0;
    for (std::size_t i = 0; i < m.steps.size(); ++i) {
        if (m.steps[i].state != events::StepState::Wait) {
            at = i + 1;
        }
    }
    std::array<char, 48> buf{};
    const std::string_view label = text(buf, "Step {} of {}", std::max<std::size_t>(at, 1), m.steps.size());
    const bool working = sheet::working(m);
    std::array<RowChip, 2> chips{{{label, work::Tone::Accent, "chip"}, {"Unsafe mode", work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, "Refine or add to the task\xE2\x80\xA6", wt::type::placeholder_small,
             std::span{chips.data(), m.unsafe ? 2u : 1u}, working ? RowButton::Stop : RowButton::Send, false, false, 0, has_detail(m));
    if (working) { // the rail runs only while the agent works (design/ui_remastered README)
        const float inset = shape_.radius * 0.6f;
        ui_->renderer().push_clip({panel.x + inset, panel.y, panel.w - 2 * inset, panel.h});
        work::bar_progress(*ui_, {panel.x, panel.y + row_h_ - wt::bar::progress, panel.w, wt::bar::progress},
                           m.progress);
        ui_->renderer().pop_clip();
    }
    if (m.steps.empty()) {
        return;
    }

    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_ + wt::body::pad_top, panel.w - 2 * wt::body::pad_x,
                   std::max(0.0f, panel.h - row_h_ - wt::body::pad_top)};
    if (body.h <= 0 || body.w <= 0) {
        return;
    }
    const bool side = m.summary.has_value() && body.w > kSummaryWidth + 300;
    const Box left{body.x, body.y, side ? body.w - kSummaryWidth - wt::body::column_gap : body.w, body.h};
    if (side) {
        draw_summary(*m.summary, {left.x + left.w + wt::body::column_gap, body.y, kSummaryWidth, body.h});
    }

    // The question and the agent's words over the cards.
    const float scale = ui_->scale();
    const float ts = ui_->text_scale();
    constexpr float kBubblePadX = 14.0f;
    constexpr float kBubblePadY = 9.0f;
    if (!question_ || question_text_ != m.question || question_scale_ != scale * ts) {
        question_.emplace(fonts_->text, wt::type::bubble * ts * scale);
        question_->set_text(m.question);
        question_text_ = m.question;
        question_scale_ = scale * ts;
    }
    question_->set_width((left.w * theme::bubble::max_width - 2 * kBubblePadX - 2) * scale);
    float question_w = 0;
    for (const auto& l : question_->lines()) {
        question_w = std::max(question_w, l.width / scale);
    }
    const float q_line = ui_->line_height(Face::Text, wt::type::bubble);
    const float q_w = question_w + 2 * kBubblePadX + 2;
    const float q_h = static_cast<float>(question_->lines().size()) * q_line + 2 * kBubblePadY + 2;
    doc_->set_width(left.w);
    const float line = theme::type::prose * ts * theme::type::prose_line;
    const float words = doc_->height() + 8;
    const std::array<std::string_view, 2> modes{"Brief", "Detailed"};
    const auto seg = work::segmented_size(*ui_, modes);
    const float header = seg.h + 10;

    // Measured first: the cards' heights decide the scroll.
    float content = q_h + theme::bubble::margin_bottom + words + 6 + header;
    float run_top = -1;
    float run_bottom = -1;
    for (const auto& s : m.steps) {
        const float h = draw_step(s, 0, 0, left.w, true);
        if (s.state == events::StepState::Run) {
            run_top = content;
            run_bottom = content + h;
        }
        content += h + wt::step::list_gap;
    }
    const auto& in = ui_->input();
    if (in.wheel != 0 && in.mouse_x >= left.x && in.mouse_x < left.x + left.w && in.mouse_y >= left.y &&
        in.mouse_y < left.y + left.h) {
        steps_scroll_ -= in.wheel * line;
        steps_follow_ = false;
    }
    const float max_scroll = std::max(0.0f, content - left.h);
    if (steps_follow_ && run_bottom > 0) {
        if (run_bottom - steps_scroll_ > left.h) {
            steps_scroll_ = run_bottom - left.h + 4;
        }
        if (run_top < steps_scroll_) {
            steps_scroll_ = run_top;
        }
    }
    steps_scroll_ = std::clamp(steps_scroll_, 0.0f, max_scroll);

    auto& renderer = ui_->renderer();
    renderer.push_clip({left.x - 4, left.y, left.w + 8, left.h});
    float y = left.y - steps_scroll_;
    const Box bubble{left.x + left.w - q_w, y, q_w, q_h};
    work::bubble(*ui_, bubble);
    const auto& q_sized = fonts_->text.sized(0, wt::type::bubble * ts * scale);
    const auto question_key = ui_->next_key(); // its lines: one text
    for (std::size_t i = 0; i < question_->lines().size(); ++i) {
        const float top = bubble.y + 1 + kBubblePadY + static_cast<float>(i) * q_line;
        const float base = top + (q_line - (q_sized.ascent + q_sized.descent) / scale) / 2 + q_sized.ascent / scale;
        ui_->draw_line(*question_, question_->lines()[i], {bubble.x + 1 + kBubblePadX, base}, ui::th().text, {.key = question_key});
    }
    y += q_h + theme::bubble::margin_bottom;
    draw_markdown({left.x, y, left.w, words}, left, m.streaming, r);
    y += words;
    y += 6;
    // "Progress", and Brief / Detailed on the right.
    work::caption(*ui_, {left.x + 2, y, 200, seg.h}, Face::Text, 12.0f, "Progress", ui::th().text_3);
    const Box seg_box{left.x + left.w - seg.w - 2, y, seg.w, seg.h};
    const int chosen = work::segmented(*ui_, id_of("steps:mode"), seg_box, modes, brief_ ? 0 : 1);
    {
        const float w0 = 2 * wt::segment::item_pad_x + work::text_width(*ui_, Face::Text, wt::type::segment, modes[0]);
        add_target("brief", {seg_box.x, seg_box.y, w0 + wt::segment::pad, seg_box.h});
        add_target("detailed", {seg_box.x + w0 + wt::segment::pad + 2, seg_box.y, seg_box.w - w0 - wt::segment::pad - 2, seg_box.h});
    }
    brief_ = chosen == 0;
    y += header;
    for (const auto& s : m.steps) {
        y += draw_step(s, left.x, y, left.w, false) + wt::step::list_gap;
    }
    renderer.pop_clip();
}

} // namespace app::sheet
