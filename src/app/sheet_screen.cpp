// The screen question (phase 10, design/ui_remastered screen 19). A screenshot is a risk class of
// its own and is never pre-approved: the card says what the model would see, Don't send or Send
// screenshot, and that it asks every time. Its panel shows exactly what would go, larger: from
// where, another monitor, all of them or a window instead (each taken again before any answer),
// Refresh after closing what should not be seen, and where the agent's own window is (not in it).

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using Color = ui::render::Color;

constexpr char32_t kIconMonitor = 0xE7F4;
constexpr char32_t kIconRefresh = 0xE72C;
constexpr char32_t kIconHidden = 0xED1A;

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

// `s` cut into lines that fit `width`, at most `max` of them (the last cut with "…" by the label).
auto lines_of(ui::Ui& ui, Face face, float size, std::string_view s, float width, std::size_t max) -> std::array<std::string_view, 4> {
    std::array<std::string_view, 4> out{};
    max = std::min(max, out.size());
    for (std::size_t n = 0; n < max && !s.empty(); ++n) {
        if (n + 1 == max || work::text_width(ui, face, size, s) <= width) {
            out[n] = s;
            break;
        }
        std::size_t cut = std::string_view::npos;
        for (auto i = s.find(' '); i != std::string_view::npos; i = s.find(' ', i + 1)) {
            if (work::text_width(ui, face, size, s.substr(0, i)) > width) {
                break;
            }
            cut = i;
        }
        if (cut == std::string_view::npos) {
            out[n] = s;
            break;
        }
        out[n] = s.substr(0, cut);
        s.remove_prefix(cut + 1);
    }
    return out;
}

auto count_lines(const std::array<std::string_view, 4>& lines) -> float {
    return static_cast<float>(std::ranges::count_if(lines, [](std::string_view l) { return !l.empty(); }));
}

constexpr std::string_view kWhat = "The model will see everything on it: open windows, notifications, file names. The agent window is not captured.";
constexpr std::string_view kNever = "A screenshot is never pre-approved \xE2\x80\x94 I'll ask every time. There is no \xE2\x80\x9C" "don't ask again\xE2\x80\x9D for the screen.";

} // namespace

auto Painter::screen_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    constexpr float kPad = 16;
    const float iw = w - 2 * kPad;
    const float rh = ui_->line_height(Face::Semibold, 11.5f) + 4;
    const float th = ui_->line_height(Face::Display, 15.5f);
    const float ph = ui_->line_height(Face::Text, 13.5f) * 1.1f;
    const float nh = ui_->line_height(Face::Text, 12.5f) * 1.1f;
    const auto what = lines_of(*ui_, Face::Text, 13.5f, kWhat, iw, 4);
    const auto never = lines_of(*ui_, Face::Text, 12.5f, kNever, iw - 24, 3);
    const auto bs = work::button_size(*ui_, "Send screenshot");
    const float h = 14 + rh + 9 + th + 8 + count_lines(what) * ph + 12 + bs.h + 12 + count_lines(never) * nh + 16;
    if (measure) {
        return h + 12;
    }
    const Box card{x, y0, w, h};
    renderer.rect({card.x, card.y, card.w, card.h},
                  {.fill = ui::th().warn_soft, .radius = {14, 14, 14, 14}, .border = ui::th().warn_line,
                   .border_width = 1});
    add_target("card", card);
    float y = card.y + 14;
    // Its risk class, as wide as its words.
    constexpr std::string_view kRisk = "Risk class: screen";
    const float rw = 9 + 12 + 6 + work::text_width(*ui_, Face::Semibold, 11.5f, kRisk) + 9;
    const Box risk{card.x + kPad, y, rw, rh};
    renderer.rect({risk.x, risk.y, risk.w, risk.h}, {.fill = ui::th().warn_soft_hi, .radius = {rh / 2, rh / 2, rh / 2, rh / 2}});
    ui_->draw_icon(work::kIconEye, 12.0f, {risk.x + 9 + 6, risk.y + rh / 2}, ui::th().warn);
    work::caption(*ui_, {risk.x + 9 + 12 + 6, risk.y, rw - 27, rh}, Face::Semibold, 11.5f, kRisk, ui::th().warn);
    add_target("card:risk", risk);
    y += rh + 9;
    ui_->draw_icon(kIconMonitor, 18.0f, {card.x + kPad + 9, y + th / 2}, ui::th().warn);
    work::caption(*ui_, {card.x + kPad + 26, y, iw - 26, th}, Face::Display, 15.5f, "Send a screenshot?", ui::th().text);
    y += th + 8;
    for (const auto line : what) {
        if (!line.empty()) {
            work::caption(*ui_, {card.x + kPad, y, iw, ph}, Face::Text, 13.5f, line, ui::th().text_2);
            y += ph;
        }
    }
    y += 12;
    // Don't send on the left, Send screenshot on the right; no "don't ask again", ever.
    const auto ds = work::button_size(*ui_, "Don't send");
    const Box deny{card.x + kPad, y, ds.w, ds.h};
    const Box allow{card.x + card.w - kPad - bs.w, y, bs.w, bs.h};
    add_target("action:deny", deny);
    add_target("action:allow", allow);
    if (work::button(*ui_, id_of("action:deny"), deny, "Don't send") && !r.command) {
        r.command = answer(m, events::Choice::Deny);
        return h + 12;
    }
    if (work::button(*ui_, id_of("action:allow"), allow, "Send screenshot", work::ButtonStyle::Primary, false, !m.retaking) && !r.command) {
        r.command = answer(m, events::Choice::Allow);
    }
    y += bs.h + 12;
    const float ny = y;
    ui_->draw_icon(work::kIconShield, 15.0f, {card.x + kPad + 7.5f, y + nh / 2}, ui::th().warn);
    for (const auto line : never) {
        if (!line.empty()) {
            work::caption(*ui_, {card.x + kPad + 24, y, iw - 24, nh}, Face::Text, 12.5f, line, ui::th().text_3);
            y += nh;
        }
    }
    add_target("card:never", {card.x + kPad, ny, iw, y - ny});
    return h + 12;
}

void Painter::draw_capture_panel(Model& m, const Box& panel, Reaction& r) {
    auto& renderer = ui_->renderer();
    const auto& c = *m.permission->capture;
    std::array<char, 96> buf{};
    add_target("panel", panel);
    // The window grows to show its picture whole (432 high), as far as it can.
    const float content = 44 + 18 + ui_->line_height(Face::Display, 18.0f) + 8 + 24 + 14 + 30 + 14 + 432 + 12 + 18 + 20;
    redraw_due_ = redraw_due_ || std::abs(content - panel_content_) > 0.5f;
    panel_content_ = content;
    panel_ground(panel);
    renderer.rect({panel.x - 1, panel.y, 1, panel.h}, {.fill = ui::th().line});
    const float x = panel.x + 22;
    const float w = std::max(0.0f, panel.w - 44);

    // Its one tab.
    constexpr float kTabs = 44;
    const float tw = work::text_width(*ui_, Face::Semibold, 13.0f, "Screenshot");
    const Box tab{x, panel.y, 18 + 8 + tw + 8 + 20, kTabs};
    // The one tab is the current one: in the accent, as the data panel's tabs are.
    ui_->draw_icon(kIconMonitor, 15.0f, {tab.x + 9, tab.y + kTabs / 2}, ui::th().accent);
    work::caption(*ui_, {tab.x + 26, tab.y, tw, kTabs}, Face::Semibold, 13.0f, "Screenshot", ui::th().accent);
    renderer.rect({tab.x + 26 + tw + 8, tab.y + kTabs / 2 - 9, 20, 18}, {.fill = ui::th().accent_soft_hi, .radius = {9, 9, 9, 9}});
    work::caption(*ui_, {tab.x + 26 + tw + 14, tab.y, 12, kTabs}, Face::Semibold, 11.5f, "1", ui::th().accent);
    renderer.rect({tab.x - 10, tab.y + kTabs - 2, tab.w + 20, 2}, {.fill = ui::th().accent});
    renderer.rect({panel.x, panel.y + kTabs - 1, panel.w, 1}, {.fill = ui::th().line});
    add_target("panel:tab:screenshot", tab);
    float y = panel.y + kTabs + 18;

    // What will be sent, from where, when.
    const float hh = ui_->line_height(Face::Display, 18.0f);
    work::caption(*ui_, {x, y, w, hh}, Face::Display, 18.0f, "What will be sent", ui::th().text);
    y += hh + 8;
    const auto src = text(buf, "screen.capture \xC2\xB7 {}", c.source);
    const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, src) + 9 + 13 + 6 + 9, w * 0.7f);
    const Box chip{x, y, sw, 24};
    renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
    ui_->draw_icon(kIconMonitor, 13.0f, {chip.x + 9 + 6.5f, chip.y + 12}, ui::th().text_2);
    work::caption(*ui_, {chip.x + 9 + 13 + 6, chip.y, sw - 37, 24}, Face::Mono, 12.0f, src, ui::th().text_2);
    add_target("capture:source", chip);
    std::array<char, 64> when{};
    work::caption(*ui_, {chip.x + chip.w + 10, y, std::max(0.0f, w - sw - 10), 24}, Face::Text, 12.5f, text(when, "{} \xC2\xB7 not sent yet", c.taken_at),
                ui::th().text_3);
    y += 24 + 14;

    // Monitor 1 | Monitor 2 | All monitors | Window ⌄, and Refresh.
    const float sh = 30;
    float sx = x + 3;
    const float seg_y = y;
    std::array<char, 32> label{};
    std::array<char, 32> name{};
    const auto segment = [&](std::string_view target, std::string_view words, bool on) -> bool {
        const float lw = work::text_width(*ui_, Face::Text, 12.5f, words) + 22;
        const Box b{sx, seg_y + 3, lw, sh - 6};
        sx += lw + 2;
        const auto hit = ui_->interact(id_of(target), b, false);
        const float hv = ui_->hover(id_of(target), hit.hovered);
        if (on) { // the chosen one's ground slides to it on base, its words turn on quick (the map's unnamed: segments)
            const Box ground = work::chosen_ground(*ui_, id_of("capture:what"), x, b);
            renderer.rect({ground.x, ground.y, ground.w, ground.h}, {.fill = ui::th().w3, .radius = {7, 7, 7, 7}});
        } else if (hv > 0.01f) {
            renderer.rect({b.x, b.y, b.w, b.h}, {.fill = faint(ui::th().w1, hv), .radius = {7, 7, 7, 7}});
        }
        work::caption(*ui_, {b.x + 11, b.y, lw - 11, b.h}, Face::Text, 12.5f, words,
                      blend(ui::th().text_3, ui::th().text, work::chosen(*ui_, id_of(target), on)));
        add_target(target, b);
        return hit.clicked;
    };
    const float seg_x = x;
    // The group's own background first (drawn under the segments: measured from their words).
    float group = 6;
    const int shown_monitors = std::min(c.monitors, 4);
    for (int i = 1; i <= shown_monitors; ++i) {
        group += work::text_width(*ui_, Face::Text, 12.5f, text(label, "Monitor {}", i)) + 24;
    }
    group += work::text_width(*ui_, Face::Text, 12.5f, "All monitors") + 24 + work::text_width(*ui_, Face::Text, 12.5f, "Window    ") + 24;
    renderer.rect({seg_x, seg_y, group, sh}, {.fill = ui::th().w1, .radius = {9, 9, 9, 9}, .border = ui::th().line, .border_width = 1});
    std::optional<events::Command> want;
    const bool window_on = c.source != "All monitors" && !c.source.starts_with("Monitor ");
    for (int i = 1; i <= shown_monitors; ++i) {
        const auto words = text(label, "Monitor {}", i);
        if (segment(text(name, "capture:monitor:{}", i), words, c.source == words) && !want) {
            want = retake(m, events::Answer::Take::Monitor, i, {});
        }
    }
    if (segment("capture:all", "All monitors", c.source == "All monitors") && !want) {
        want = retake(m, events::Answer::Take::All, 0, {});
    }
    const float window_x = sx;
    if (segment("capture:window", "Window    ", window_on)) { // room for its chevron
        capture_menu_ = !capture_menu_;
    }
    ui_->draw_icon(work::kIconChevronDown, 9.0f, {sx - 12, seg_y + sh / 2}, window_on ? ui::th().text : ui::th().text_3);
    // Refresh: the same again, after closing what should not be seen.
    const Box refresh{seg_x + group + 10, seg_y - 1, 32, 32};
    renderer.rect({refresh.x, refresh.y, refresh.w, refresh.h}, {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
    if (work::icon_button(*ui_, id_of("capture:refresh"), refresh, kIconRefresh) && !want) {
        want = retake(m, events::Answer::Take::Same, 0, {});
    }
    add_target("capture:refresh", refresh);
    y += sh + 14;

    // The picture: exactly what goes, whole, the agent's place outlined on it.
    const float room = std::max(120.0f, panel.y + panel.h - y - 60);
    const float ph = std::min(432.0f, room);
    const Box frame{x, y, w, ph};
    renderer.rect({frame.x, frame.y, frame.w, frame.h}, {.fill = ui::th().panel, .radius = ui::card_corners()});
    const auto& pic = c.preview;
    Box shot = frame;
    if (pic.thumb_w > 0 && pic.thumb_h > 0) {
        const float k = std::min(frame.w / static_cast<float>(pic.thumb_w), frame.h / static_cast<float>(pic.thumb_h));
        shot = {frame.x + (frame.w - pic.thumb_w * k) / 2, frame.y + (frame.h - pic.thumb_h * k) / 2, pic.thumb_w * k, pic.thumb_h * k};
        if (const auto* t = picture_texture(pic)) {
            renderer.image({shot.x, shot.y, shot.w, shot.h}, {}, t->id(), {255, 255, 255, 255});
        }
    }
    renderer.rect({frame.x, frame.y, frame.w, frame.h}, {.fill = {}, .radius = ui::card_corners(), .border = ui::th().line_2, .border_width = 1});
    add_target("capture:preview", shot);
    if (c.hole_w > 0 && c.hole_h > 0) {
        const Box hole{shot.x + shot.w * c.hole_x, shot.y + shot.h * c.hole_y, shot.w * c.hole_w, std::max(18.0f, shot.h * c.hole_h)};
        for (float hx = hole.x; hx + 5 <= hole.x + hole.w; hx += 9) { // its bottom, dashed
            renderer.rect({hx, hole.y + hole.h - 1.5f, 5, 1.5f}, {.fill = ui::kOnPicture});
        }
        for (float hy = hole.y; hy + 5 <= hole.y + hole.h; hy += 9) { // its sides; never its top (the sheet hangs from the edge)
            renderer.rect({hole.x, hy, 1.5f, 5}, {.fill = ui::kOnPicture});
            renderer.rect({hole.x + hole.w - 1.5f, hy, 1.5f, 5}, {.fill = ui::kOnPicture});
        }
        constexpr std::string_view kHole = "Agent window \xE2\x80\x94 not captured";
        const float lw = work::text_width(*ui_, Face::Text, 11.5f, kHole) + 16 + 12 + 6;
        const float lh = ui_->line_height(Face::Text, 11.5f) + 4;
        const Box tag{hole.x + (hole.w - lw) / 2, hole.y + (hole.h - lh) / 2, lw, lh};
        renderer.rect({tag.x, tag.y, tag.w, tag.h}, {.fill = ui::kPictureScrim, .radius = {6, 6, 6, 6}});
        ui_->draw_icon(kIconHidden, 12.0f, {tag.x + 8 + 6, tag.y + lh / 2}, ui::kOnPicture);
        work::caption(*ui_, {tag.x + 8 + 12 + 6, tag.y, lw - 26, lh}, Face::Text, 11.5f, kHole, ui::kOnPicture);
        add_target("capture:hole", hole);
    }
    if (m.retaking) { // taken again: until it comes
        renderer.rect({frame.x, frame.y, frame.w, frame.h}, {.fill = ui::th().scrim, .radius = ui::card_corners()});
        work::spinner(*ui_, {frame.x + frame.w / 2, frame.y + frame.h / 2 - 12}, 20.0f, ui::th().accent);
        constexpr std::string_view kTaking = "Taking it again\xE2\x80\xA6";
        const float kw = work::text_width(*ui_, Face::Text, 13.0f, kTaking);
        work::caption(*ui_, {frame.x + (frame.w - kw) / 2, frame.y + frame.h / 2 + 4, kw, 20}, Face::Text, 13.0f, kTaking, ui::th().text_2);
        add_target("capture:taking", frame);
    }
    y += ph + 12;

    // Its sizes, and what to do before sending.
    const auto mb = static_cast<double>(c.bytes) / 1048576.0;
    std::array<char, 96> meta{};
    const auto said = c.format.empty() ? text(meta, "{} \xC3\x97 {} \xC2\xB7 {:.1f} MB \xC2\xB7 sent as {} \xC3\x97 {}", c.width, c.height, mb, c.sent_w, c.sent_h)
                                       : text(meta, "{} \xC3\x97 {} \xC2\xB7 {} \xC2\xB7 {:.1f} MB \xC2\xB7 sent as {} \xC3\x97 {}", c.width, c.height, c.format,
                                              mb, c.sent_w, c.sent_h);
    const float mw = work::caption(*ui_, {x, y, w, 18}, Face::Text, 12.5f, said, ui::th().text_3);
    add_target("capture:meta", {x, y, mw, 18});
    constexpr std::string_view kHint = "Close what you don't want to show, then press";
    const float hw = work::text_width(*ui_, Face::Text, 12.5f, kHint);
    if (mw + 24 + hw + 20 <= w) {
        work::caption(*ui_, {x + w - hw - 18, y, hw, 18}, Face::Text, 12.5f, kHint, ui::th().text_3);
        ui_->draw_icon(kIconRefresh, 13.0f, {x + w - 6.5f, y + 9}, ui::th().text_3);
    }

    // The windows to take instead, under its segment.
    if (capture_menu_) {
        const float rowh = 30;
        const float mw2 = 300;
        const Box menu{window_x, seg_y + sh + 4, mw2, rowh * static_cast<float>(std::min<std::size_t>(c.windows.size(), 10)) + 8};
        renderer.rect({menu.x, menu.y, menu.w, menu.h}, {.fill = ui::th().pop, .radius = {9, 9, 9, 9}, .border = ui::th().w2, .border_width = 1});
        for (std::size_t i = 0; i < std::min<std::size_t>(c.windows.size(), 10); ++i) {
            const Box row{menu.x + 4, menu.y + 4 + rowh * static_cast<float>(i), menu.w - 8, rowh};
            const auto hit = ui_->interact(id_of("capture:window-row", i), row, false);
            if (const float light = lit(id_of("capture:window-row", i), hit.hovered); light > 0.004f) {
                renderer.rect({row.x, row.y, row.w, row.h}, {.fill = faint(ui::th().w1, light), .radius = {6, 6, 6, 6}});
            }
            work::caption(*ui_, {row.x + 10, row.y, row.w - 20, row.h}, Face::Text, 12.5f, c.windows[i], ui::th().text);
            add_target(text(name, "capture:window:{}", i), row);
            if (hit.clicked && !want) {
                capture_menu_ = false;
                want = retake(m, events::Answer::Take::Window, 0, c.windows[i]);
            }
        }
    }
    if (want && !r.command) {
        capture_menu_ = false;
        r.command = std::move(want);
    }
}

} // namespace app::sheet
