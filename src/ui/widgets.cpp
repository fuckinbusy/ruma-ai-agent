#include "ui/widgets.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ui::widgets {
namespace {

using layout::Box;
using render::Color;

constexpr Id kHoverSalt = 0x9E3779B97F4A7C15ull; // fade id of a widget's hover highlight

auto rect(const Box& b) -> render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

auto lerp(Color a, Color b, float t) -> Color {
    const auto mix = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(
            std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

auto all(float r) -> render::Corners {
    return {r, r, r, r};
}

// Phase in [0, 1) of a cycle of `period` seconds.
auto phase(double now, float period) -> float {
    const double cycles = now / static_cast<double>(period);
    return static_cast<float>(cycles - std::floor(cycles));
}

auto hover(Ui& ui, Id id, bool on) -> float {
    return ui.hover(id ^ kHoverSalt, on); // in 120 ms, out in 180 (phase 10, screens 26–27)
}

// Keyboard focus: a 2 px ring of the accent, 2 px off the control — after Tab only
// (Interaction::focus_ring), never after a mouse click.
void focus_ring(Ui& ui, const Box& box, float radius) {
    ui.renderer().rect({box.x - 4, box.y - 4, box.w + 8, box.h + 8}, {.fill = {}, .radius = all(radius + 4), .border = th().accent, .border_width = 2});
}

// The resting strip's own colours (the old mockup's): the strip goes with stage 3 of the UI update (the
// handle takes its place), so it gets no tokens.
constexpr Color kStripLeft{124, 92, 255, 230};      // color-ok: the resting strip, removed in stage 3
constexpr Color kStripRight{34, 211, 238, 230};     // color-ok: the resting strip, removed in stage 3
constexpr Color kStripMiddle{255, 255, 255, 255};   // color-ok: the resting strip, removed in stage 3
constexpr Color kStripGlow{170, 160, 255, 204};     // color-ok: the resting strip, removed in stage 3
constexpr Color kStripGlowWide{124, 92, 255, 128};  // color-ok: the resting strip, removed in stage 3

} // namespace

void handle(Ui& ui, const Box& box, float radius, float highlight, float glow) {
    auto& r = ui.renderer();
    r.shadow(rect(box), radius, 40.0f, with_alpha(kStripGlowWide, glow));
    r.shadow(rect(box), radius, 18.0f, with_alpha(kStripGlow, glow));
    // border-radius 0 0 r r: hanging from the screen edge, like the sheet it grows into.
    // The two gradients meet on a whole physical pixel: at a fractional edge both anti-alias and
    // the background shows through a one-pixel seam (a thin line ran along the
    // strip with the drifting white).
    const float s = ui.scale();
    const float meet = std::round((box.x + box.w * std::clamp(highlight, 0.0f, 1.0f)) * s) / s;
    const float left = std::clamp(meet - box.x, 0.0f, box.w);
    r.rect({box.x, box.y, left, box.h}, {.fill = kStripLeft, .fill_to = kStripMiddle,
                                         .radius = {0, 0, 0, radius}});
    r.rect({box.x + left, box.y, box.w - left, box.h},
           {.fill = kStripMiddle, .fill_to = kStripRight, .radius = {0, 0, radius, 0}});
}

void panel(Ui& ui, const Box& box, float radius) {
    auto& r = ui.renderer();
    // box-shadow 0 24px 60px -16px: offset down, shrunk by the negative spread.
    const float spread = -theme::sheet::shadow_spread;
    r.shadow({box.x + spread, box.y + theme::sheet::shadow_y + spread, box.w - 2 * spread,
              box.h - 2 * spread},
             std::max(radius - spread, 0.0f), theme::sheet::shadow_blur, th().shadow[0].color);
    // The top edge sits on the screen edge: its border is pushed just above it.
    r.rect({box.x, box.y - 1, box.w, box.h + 1},
           {.fill = th().surface, .radius = {0, 0, radius, radius}, .border = ui::th().line,
            .border_width = 1});
}

auto send_button(Ui& ui, Id id, const Box& box) -> bool {
    const auto hit = ui.interact(id, box);
    const float h = hover(ui, id, hit.hovered);
    auto& r = ui.renderer();
    // box-shadow 0 4px 14px -4px
    r.shadow({box.x + 4, box.y + 8, box.w - 8, box.h - 8}, (box.w - 8) / 2, 14.0f,
             th().accent_line);
    r.rect(rect(box), {.fill = lerp(th().send.top, th().send.bottom, 0.0f),
                       .fill_to = lerp(th().send.bottom, th().send.top, h * 0.3f),
                       .gradient_angle = std::numbers::pi_v<float> / 2,
                       .radius = all(box.w / 2)});
    ui.draw_icon(kIconUp, theme::button::send_icon, {box.x + box.w / 2, box.y + box.h / 2},
                 th().on_accent);
    if (hit.focus_ring) {
        focus_ring(ui, box, box.w / 2);
    }
    return hit.clicked;
}

auto icon_button(Ui& ui, Id id, const Box& box, char32_t icon) -> bool {
    const auto hit = ui.interact(id, box);
    const float h = hover(ui, id, hit.hovered);
    if (h > 0) {
        ui.renderer().rect(rect(box), {.fill = with_alpha(th().w3, h),
                                       .radius = all(theme::button::icon_radius)});
    }
    ui.draw_icon(icon, theme::type::icon, {box.x + box.w / 2, box.y + box.h / 2},
                 lerp(ui::th().text_2, ui::th().text, h));
    if (hit.focus_ring) {
        focus_ring(ui, box, theme::button::icon_radius);
    }
    return hit.clicked;
}

auto pill_size(Ui& ui, std::string_view label) -> layout::Size {
    const float text = ui.shape(Face::Semibold, label, theme::type::button).width / ui.scale();
    return {2 * theme::button::pill_pad_x + text + 2,
            2 * theme::button::pill_pad_y + ui.line_height(Face::Semibold, theme::type::button) + 2};
}

auto pill_button(Ui& ui, Id id, const Box& box, std::string_view label, bool primary) -> bool {
    const auto hit = ui.interact(id, box);
    const float h = hover(ui, id, hit.hovered);
    auto& r = ui.renderer();
    const float radius = box.h / 2;
    if (primary) {
        r.rect(rect(box), {.fill = th().send.top,
                           .fill_to = lerp(th().send.bottom, th().send.top, h * 0.5f),
                           .gradient_angle = std::numbers::pi_v<float> / 2,
                           .radius = all(radius)});
    } else {
        r.rect(rect(box), {.fill = lerp(th().w2, th().w4, h), .radius = all(radius),
                           .border = ui::th().line_2, .border_width = 1});
    }
    const auto& shaped = ui.shape(Face::Semibold, label, theme::type::button);
    const float width = shaped.width / ui.scale();
    ui.draw_text(Face::Semibold, shaped, theme::type::button,
                 {box.x + (box.w - width) / 2,
                  ui.centred_baseline(Face::Semibold, theme::type::button, box.y, box.h)},
                 primary ? th().on_accent : ui::th().text);
    if (hit.focus_ring) {
        focus_ring(ui, box, radius);
    }
    return hit.clicked;
}

auto chip_size(Ui& ui, std::string_view label) -> layout::Size {
    const float text = ui.shape(Face::Text, label, theme::type::chip).width / ui.scale();
    return {2 * theme::chip::pad_x + theme::chip::dot + theme::chip::gap + text,
            2 * theme::chip::pad_y + ui.line_height(Face::Text, theme::type::chip)};
}

void chip(Ui& ui, const Box& box, std::string_view label) {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = th().w2, .radius = all(box.h / 2)});
    const float d = theme::chip::dot;
    const render::Rect dot{box.x + theme::chip::pad_x, box.y + (box.h - d) / 2, d, d};
    r.shadow(dot, d / 2, theme::chip::dot_glow, th().accent_line);
    r.rect(dot, {.fill = th().accent, .radius = all(d / 2)});
    const auto& shaped = ui.shape(Face::Text, label, theme::type::chip);
    ui.draw_text(Face::Text, shaped, theme::type::chip,
                 {dot.x + d + theme::chip::gap,
                  ui.centred_baseline(Face::Text, theme::type::chip, box.y, box.h)},
                 ui::th().text_2);
}

void ok_badge(Ui& ui, const Box& box) {
    ui.renderer().rect(rect(box), {.fill = th().ok_soft, .radius = all(box.w / 2), .border = th().ok_line, .border_width = 1});
    ui.draw_icon(kIconCheck, box.w * 0.55f, {box.x + box.w / 2, box.y + box.h / 2}, ui::th().ok);
}

auto field(Ui& ui, Id id, const Box& box, const Box& hit_box, std::string_view placeholder) -> Interaction {
    const auto hit = ui.interact(id, hit_box);
    const float h = hover(ui, id, hit.hovered || hit.focus_ring);
    ui.renderer().rect(rect(box), {.fill = th().w2, .radius = all(box.h / 2),
                                   .border = lerp(ui::th().line, ui::th().line_2, h),
                                   .border_width = 1});
    const auto& shaped = ui.shape(Face::Text, placeholder, theme::type::placeholder);
    ui.draw_text(Face::Text, shaped, theme::type::placeholder,
                 {box.x + theme::field::pad_left,
                  ui.centred_baseline(Face::Text, theme::type::placeholder, box.y, box.h)},
                 ui::th().text_3);
    return hit;
}

void bubble(Ui& ui, const Box& box) {
    const float r = theme::bubble::radius;
    ui.renderer().rect(rect(box), {.fill = th().you,
                                   .radius = {r, r, theme::bubble::tail_radius, r},
                                   .border = th().you_line, .border_width = 1});
}

void caret(Ui& ui, const Box& box, double since, bool stream) {
    // No frame request: the shell wakes at each flip (Painter::next_caret_flip), not 60 a second.
    constexpr float kBlink = std::chrono::duration<float>(motion::caret::kBlink).count();
    if (phase(ui.now() - since, kBlink) >= 0.5f) { // steps(2): off half the time
        return;
    }
    if (stream) {
        ui.renderer().rect(rect(box), {.fill = th().text_2, .radius = all(theme::caret::stream_radius)});
    } else {
        ui.renderer().rect(rect(box), {.fill = th().text, .radius = all(box.w / 2)});
    }
}

namespace {

struct StreamFade {
    const StreamTimes* times;
    const text::Shaped* shaped;
    const StreamStyle* style;
    double now;
};

// MS § 6 "Streaming": each character comes in over kCharIn, smoothstep.
constexpr double kCharIn = std::chrono::duration<double>(motion::stream::kCharIn).count();
auto stream_color(const void* context, std::uint32_t glyph, float) -> Color {
    const auto& s = *static_cast<const StreamFade*>(context);
    const auto& g = s.shaped->glyphs[glyph];
    const double age = s.now - s.times->time_of(g.cluster);
    const auto k = static_cast<float>(std::clamp(age / kCharIn, 0.0, 1.0));
    return with_alpha(s.style->colors[std::min<std::size_t>(g.face, s.style->colors.size() - 1)], k * k * (3.0f - 2.0f * k));
}

} // namespace

auto StreamTimes::time_of(std::uint32_t byte) const noexcept -> double {
    const auto it = std::upper_bound(marks_.begin(), marks_.end(), byte,
                                     [](std::uint32_t b, const Mark& m) { return b < m.end; });
    return it == marks_.end() ? -1.0e9 : it->time;
}

auto answer_style() noexcept -> StreamStyle {
    // What is read is `text` whatever its weight; a link is the accent. Made each time it is asked for:
    // the theme can change.
    return {{th().text, th().text, th().text, th().text, th().text, th().accent}, theme::type::prose_line};
}

void stream_text(Ui& ui, const Box& box, const text::Paragraph& paragraph,
                 const StreamTimes& times, bool with_caret, const StreamStyle& style) {
    const float scale = ui.scale();
    const auto& sized = paragraph.fonts().sized(0, paragraph.px());
    const float ascent = sized.ascent / scale;
    const float descent = sized.descent / scale;
    const float line = paragraph.px() / scale * style.line;
    const auto baseline_of = [&](std::size_t i) {
        return box.y + static_cast<float>(i) * line + (line - (ascent + descent)) / 2 + ascent;
    };

    const StreamFade fade{&times, &paragraph.shaped(), &style, ui.now()};
    const auto& lines = paragraph.lines();
    const TextKey key = ui.next_key(); // the paragraph's lines: one text in the frame's map
    const auto text = paragraph.text();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text::draw(ui.renderer(), ui.atlas(), paragraph.faces(), paragraph.shaped(),
                   lines[i].first_glyph, lines[i].end_glyph, paragraph.px(),
                   {box.x, baseline_of(i)}, stream_color, &fade, scale);
        TextRun run;
        run.box = {box.x, baseline_of(i) - ascent - (line - (ascent + descent)) / 2, lines[i].width / scale, line};
        run.baseline = baseline_of(i);
        run.first_byte = lines[i].first_byte;
        run.shaped = &paragraph.shaped();
        run.first_glyph = lines[i].first_glyph;
        run.end_glyph = lines[i].end_glyph;
        run.soft_break_after = i + 1 < lines.size() &&
                               text.substr(lines[i].end_byte, lines[i + 1].first_byte - lines[i].end_byte).find('\n') == std::string_view::npos;
        // A line keeps the spaces it hangs (wrapped, before its line end, at the text's end): copied,
        // the text is its source byte for byte.
        auto end = static_cast<std::size_t>(lines[i].end_byte);
        while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
            ++end;
        }
        ui.note_text(run, text.substr(lines[i].first_byte, end - lines[i].first_byte), {}, {.key = key});
    }
    if (!paragraph.text().empty() &&
        ui.now() - times.time_of(static_cast<std::uint32_t>(paragraph.text().size() - 1)) <
            kCharIn) {
        ui.request_frame(); // a token is still fading in
    }
    if (with_caret) {
        const std::size_t last = lines.empty() ? 0 : lines.size() - 1;
        const float x = box.x + (lines.empty() ? 0.0f : lines.back().width / scale) + 2;
        const float base = baseline_of(last);
        // .scaret: 8x14, vertical-align -2px (its bottom 2 px below the baseline)
        caret(ui, {x, base + 2 - theme::caret::stream_height, theme::caret::stream_width,
                   theme::caret::stream_height},
              0.0, true);
    }
}

} // namespace ui::widgets
