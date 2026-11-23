#include "ui/work.hpp"

#include "theme/contrast.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "core/hash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <numbers>

namespace ui::work {
namespace {

using layout::Box;
using render::Color;
namespace wt = theme::work;

constexpr Id kHoverSalt = 0xC2B2AE3D27D4EB4Full;
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

auto rect(const Box& b) -> render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto all(float r) -> render::Corners {
    return {r, r, r, r};
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

auto lerp(Color a, Color b, float t) -> Color {
    const auto mix = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

auto hover(Ui& ui, Id id, bool on) -> float {
    return ui.hover(id ^ kHoverSalt, on); // in 120 ms, out in 180 (screens 26–27)
}

} // namespace

void focus_ring(Ui& ui, Id id, const Box& box, float radius, bool on) {
    // A 2 px ring of the accent, 2 px off the control: what is under it shows in the gap — for the
    // keyboard only (Interaction::focus_ring).
    // MS 14: it comes on base from kFocusFrom of its size — it does not flash on — and goes on micro.
    const float shown =
        std::clamp(ui.animator().to(id ^ 0x466F637573ull, on ? 1.0f : 0.0f, on ? motion::kBase : motion::kMicro, 0.0f), 0.0f, 1.0f);
    if (shown <= 0.004f) {
        return;
    }
    auto& r = ui.renderer();
    const float k = motion::micro::kFocusFrom - (motion::micro::kFocusFrom - 1.0f) * shown;
    const Box at{box.x + box.w * (1 - k) / 2, box.y + box.h * (1 - k) / 2, box.w * k, box.h * k};
    const float was = r.opacity();
    r.set_opacity(was * shown);
    r.rect({at.x - 4, at.y - 4, at.w + 8, at.h + 8}, {.fill = {}, .radius = all(radius + 4), .border = ui::th().accent, .border_width = 2});
    r.set_opacity(was);
}

namespace {

// The byte length of the longest prefix of `text` (on a character boundary) whose width plus
// the ellipsis' fits `room`. Shapes are cached, so a steady frame finds them all again.
auto fitting_prefix(Ui& ui, Face face, float size, std::string_view text, float room) -> std::size_t {
    std::size_t lo = 0;
    std::size_t hi = text.size();
    while (lo < hi) {
        std::size_t mid = (lo + hi + 1) / 2;
        while (mid > lo && mid < text.size() && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80) {
            --mid; // a character boundary (the end of the text is one)
        }
        if (mid == lo) {
            break;
        }
        if (text_width(ui, face, size, text.substr(0, mid)) <= room) {
            lo = mid;
        } else {
            hi = mid - 1;
            while (hi > lo && hi < text.size() && (static_cast<unsigned char>(text[hi]) & 0xC0) == 0x80) {
                --hi;
            }
        }
    }
    return lo;
}

// Every colour here is a token of the theme of the frame (ui::th()), by the designer's rules: the ink
// ladder w1..w4 for what lies under the mouse, is pressed, is chosen, or is a track; a soft ground, a
// ground under the mouse and a line for each coloured token; colour for risk, not for category.

// CSS brightness(): each channel times `k`.
auto brightness(Color c, float k) -> Color {
    const auto ch = [k](std::uint8_t v) { return static_cast<std::uint8_t>(std::clamp(std::lround(static_cast<float>(v) * k), 0L, 255L)); };
    return {ch(c.r), ch(c.g), ch(c.b), c.a};
}

// One layer of a CSS box-shadow under `box`: its spread grows or shrinks the box, its y moves it.
void shadow_layer(Ui& ui, const Box& box, float radius, const ::theme::Shadow& s) {
    ui.renderer().shadow({box.x - s.spread, box.y + s.y - s.spread, box.w + 2 * s.spread, box.h + 2 * s.spread},
                         std::max(radius + s.spread, 0.0f), s.blur, s.color);
}

// A step's badge is cut out of its icon by a ring of the surface; one that waits is a neutral disc.
auto badge_back() -> Color {
    return ui::th().surface;
}
auto badge_wait() -> Color {
    return ::theme::over(ui::th().c_rest, ui::th().surface);
}


} // namespace

auto kind_color(Kind k) noexcept -> Color {
    switch (k) {
    case Kind::Read: return ui::th().text_3; // the most frequent and the safest: neutral
    case Kind::Change: return ui::th().warn;
    case Kind::Delete: return ui::th().crit;
    case Kind::System: return ui::th().accent;
    case Kind::Ok: return ui::th().ok;
    }
    return ui::th().accent;
}

auto page_ink() noexcept -> Color {
    return ui::th().light ? ui::th().text_4 : with_alpha(ui::th().on_accent, 0.5f);
}

// A type's mark: a text document and a spreadsheet keep their own hue, softened; every other type
// is neutral — colour says risk, not a file's kind.
auto tag_ink(std::string_view tag) noexcept -> TagInk {
    if (tag == "DOCX" || tag == "DOC" || tag == "RTF" || tag == "ODT") {
        return {ui::th().f_doc, ui::th().on_accent};
    }
    if (tag == "XLSX" || tag == "XLS" || tag == "CSV" || tag == "ODS") {
        return {ui::th().f_sheet, ui::th().on_accent};
    }
    // Neutral: the page's own colour on a mark dark enough for it — at .7 of the dark ink the letters
    // read at 5:1 on a dark theme (at .5, the page's fold, they were at 3:1).
    return {ui::th().light ? ui::th().text_4 : with_alpha(ui::th().on_accent, 0.7f), ui::th().f_page};
}

auto kind_ground(Kind k) noexcept -> Color {
    switch (k) {
    case Kind::Read: return ui::th().w2;
    case Kind::Change: return ui::th().warn_soft;
    case Kind::Delete: return ui::th().crit_soft;
    case Kind::System: return ui::th().accent_soft;
    case Kind::Ok: return ui::th().ok_soft;
    }
    return ui::th().accent_soft;
}

void pop_surface(Ui& ui, const Box& box, float radius) {
    for (const auto& layer : ui::th().pop_shadow) {
        shadow_layer(ui, box, radius, layer);
    }
    ui.renderer().rect(rect(box), {.fill = ui::th().pop, .radius = all(radius), .border = ui::th().line_2, .border_width = 1});
}

auto tone_color(Tone t) noexcept -> Color {
    switch (t) {
    case Tone::Accent: return ui::th().accent;
    case Tone::Ok: return ui::th().ok;
    case Tone::Warn: return ui::th().warn;
    case Tone::Crit: return ui::th().crit;
    case Tone::Plain: return ui::th().text_2;
    }
    return ui::th().text_2;
}

auto text_width(Ui& ui, Face face, float size, std::string_view text) -> float {
    return ui.shape(face, text, size).width / ui.scale();
}

namespace {

auto pitch(Ui& ui, const TextBlockStyle& style) -> float {
    return std::max(style.min_line_h, style.size * ui.text_scale() * style.line);
}

auto measure_block(const void* context, float max_width) -> layout::Size {
    const auto& spec = *static_cast<const TextBlockSpec*>(context);
    const auto& p = spec.ui->paragraph(spec.style.face, spec.text, spec.style.size, max_width);
    float widest = 0;
    for (const auto& line : p.lines()) {
        widest = std::max(widest, line.width / spec.ui->scale());
    }
    return {max_width < layout::kUnbounded ? max_width : widest, text_block_height(*spec.ui, spec.text, spec.style, max_width)};
}

} // namespace

auto text_block_height(Ui& ui, std::string_view text, const TextBlockStyle& style, float width) -> float {
    if (text.empty()) {
        return 0;
    }
    const auto& p = ui.paragraph(style.face, text, style.size, width);
    return pitch(ui, style) * static_cast<float>(std::max<std::size_t>(p.lines().size(), 1));
}

auto text_block(Ui& ui, const Box& box, std::string_view text, const TextBlockStyle& style, const TextFrom& from) -> float {
    if (text.empty()) {
        return 0;
    }
    const auto& p = ui.paragraph(style.face, text, style.size, box.w);
    const float step = pitch(ui, style);
    const float ascent = ui.ascent(style.face, style.size);
    const float descent = ui.descent(style.face, style.size);
    const auto& lines = p.lines();
    TextFrom line_from = from; // one key for all its lines
    if (line_from.key.order == 0) {
        line_from.key = ui.next_key();
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float top = box.y + static_cast<float>(i) * step;
        const float baseline = top + (step - (ascent + descent)) / 2 + ascent;
        // Wrapped here, or broken by the text's own line end.
        line_from.soft_break_after = i + 1 < lines.size() &&
                                     text.substr(lines[i].end_byte, lines[i + 1].first_byte - lines[i].end_byte).find('\n') == std::string_view::npos;
        ui.draw_line(p, lines[i], {box.x, baseline}, style.color, line_from);
    }
    return step * static_cast<float>(std::max<std::size_t>(lines.size(), 1));
}

auto text_measure(const TextBlockSpec& spec) -> layout::Measure {
    std::uint64_t key = core::hash::fnv1a64(spec.text);
    key = key * 1099511628211ull ^ static_cast<std::uint64_t>(spec.style.face);
    key = key * 1099511628211ull ^ std::bit_cast<std::uint32_t>(spec.style.size * spec.ui->text_scale());
    key = key * 1099511628211ull ^ std::bit_cast<std::uint32_t>(spec.style.line);
    key = key * 1099511628211ull ^ std::bit_cast<std::uint32_t>(spec.style.min_line_h);
    key = key * 1099511628211ull ^ std::bit_cast<std::uint32_t>(spec.ui->scale());
    return {&measure_block, &spec, key == 0 ? 1 : key};
}

namespace {

struct OneLine {
    float width = 0;
    bool cut = false;
};

// A label's line: drawn whole, or cut with "…"; put into the frame's text map either way — cut, it
// is `reachable` when its caller shows the whole text elsewhere (a caption's tooltip).
auto one_line(Ui& ui, const Box& box, Face face, float size, std::string_view text, Color c, bool reachable) -> OneLine {
    const float baseline = ui.centred_baseline(face, size, box.y, box.h);
    const auto& whole = ui.shape(face, text, size);
    const float width = whole.width / ui.scale();
    TextRun run;
    run.box = box;
    run.baseline = baseline;
    // A box made from the text's own width loses a bit or two to the sums on the way ((10 + w) - 10
    // at 125 %): the tongue's "0:00" came out "0:...". A hundredth of a pixel.
    if (width <= box.w + 0.01f) {
        ui.draw_text(face, whole, size, {box.x, baseline}, c);
        run.shaped = &whole;
        run.end_glyph = static_cast<std::uint32_t>(whole.glyphs.size());
        ui.note_text(run, text, {}, {});
        return {width, false};
    }
    const float dots = text_width(ui, face, size, kEllipsis);
    const std::size_t n = fitting_prefix(ui, face, size, text, std::max(box.w - dots, 0.0f));
    const auto& head = ui.shape(face, text.substr(0, n), size);
    const float head_w = head.width / ui.scale();
    ui.draw_text(face, head, size, {box.x, baseline}, c);
    ui.draw_text(face, ui.shape(face, kEllipsis, size), size, {box.x + head_w, baseline}, c);
    run.shaped = &head;
    run.end_glyph = static_cast<std::uint32_t>(head.glyphs.size());
    run.cut = true;
    run.reachable = reachable;
    ui.note_text(run, text.substr(0, n), text, {});
    return {head_w + dots, true};
}

} // namespace

auto caption(Ui& ui, const Box& box, Face face, float size, std::string_view text, Color c) -> float {
    const auto drawn = one_line(ui, box, face, size, text, c, true);
    if (drawn.cut) {
        // Its whole text under the mouse (the id: the text and its place).
        const Id id = core::hash::fnv1a64(text) * 1099511628211ull ^ (static_cast<Id>(std::bit_cast<std::uint32_t>(box.x)) << 32 |
                                                                     std::bit_cast<std::uint32_t>(box.y));
        const auto& in = ui.input();
        if (in.mouse_x >= box.x && in.mouse_x < box.x + box.w && in.mouse_y >= box.y && in.mouse_y < box.y + box.h) {
            ui.tooltip(id, box, text);
        }
    }
    return drawn.width;
}

auto label(Ui& ui, const Box& box, Face face, float size, std::string_view text, Color c) -> float {
    return one_line(ui, box, face, size, text, c, false).width;
}

void kind_icon(Ui& ui, const Box& box, Kind kind, char32_t icon, Badge badge, Id id) {
    auto& r = ui.renderer();
    const Color k = kind_color(kind);
    r.rect(rect(box), {.fill = kind_ground(kind), .radius = all(wt::kind_icon::radius)});
    ui.draw_icon(icon, wt::kind_icon::icon * box.w / wt::kind_icon::size, {box.x + box.w / 2, box.y + box.h / 2}, k);
    if (badge == Badge::None) {
        return;
    }
    const float d = wt::kind_icon::badge;
    const float off = wt::kind_icon::badge_offset;
    const Box b{box.x + box.w - d - off, box.y + box.h - d - off, d, d};
    // The badge cut out of the icon: a 2 px ring of the surface.
    r.rect({b.x - 2, b.y - 2, d + 4, d + 4}, {.fill = badge_back(), .radius = all(d / 2 + 2)});
    const float cx = b.x + d / 2;
    const float cy = b.y + d / 2;
    if (id != 0) {
        // MS 06 (a step): it wakes from its dot on quick; done, the turning ring goes as the green
        // circle comes on base (from kCircleFrom of its size) and the check draws itself on smooth,
        // once the circle is as far as base takes it in kCheckAt (kCheckAfterShare of its way).
        auto& anim = ui.animator();
        const Id own = id * 1099511628211ull; // its tracks: never a neighbour's (ids next to each other)
        const float run = anim.to(own + 0x4B49430, badge == Badge::Wait ? 0.0f : 1.0f, motion::kQuick);
        const float done = anim.to(own + 0x4B49431, badge == Badge::Done ? 1.0f : 0.0f, motion::kBase);
        const float check = anim.to(own + 0x4B49432, badge == Badge::Done && done >= motion::steps::kCheckAfterShare ? 1.0f : 0.0f, motion::kSmooth);
        const float o = r.opacity();
        if (run < 0.996f) {
            r.set_opacity(o * (1.0f - run));
            r.rect(rect(b), {.fill = badge_wait(), .radius = all(d / 2)});
            r.rect({cx - 2, cy - 2, 4, 4}, {.fill = badge_back(), .radius = all(2)});
        }
        if (const float spin = run * (1.0f - done); spin > 0.004f) {
            r.set_opacity(o * spin);
            r.rect(rect(b), {.fill = badge_back(), .radius = all(d / 2)});
            const float from = static_cast<float>(std::fmod(ui.now() / 0.9, 1.0)) * kTau;
            r.arc({cx, cy}, 4.2f, 1.6f, 0.0f, kTau, ui::th().w4);
            r.arc({cx, cy}, 4.2f, 1.6f, from, std::min(from + kTau / 4, kTau), ui::th().text);
            if (from + kTau / 4 > kTau) {
                r.arc({cx, cy}, 4.2f, 1.6f, 0.0f, from + kTau / 4 - kTau, ui::th().text);
            }
            ui.request_frame();
        }
        if (done > 0.004f) {
            r.set_opacity(o * done);
            const float s = d * (motion::steps::kCircleFrom + (1.0f - motion::steps::kCircleFrom) * done);
            r.rect({cx - s / 2, cy - s / 2, s, s}, {.fill = ui::th().ok, .radius = all(s / 2)});
            const float u = d / 14.0f; // the check in a 14 px badge
            const std::array<render::Point, 3> mark{{{cx - 3.0f * u, cy + 0.2f * u}, {cx - 1.0f * u, cy + 2.2f * u}, {cx + 3.2f * u, cy - 2.2f * u}}};
            r.stroke(mark, 1.6f * u, ui::th().on_accent, check);
        }
        r.set_opacity(o);
        return;
    }
    switch (badge) {
    case Badge::Done:
        r.rect(rect(b), {.fill = ui::th().ok, .radius = all(d / 2)});
        ui.draw_icon(widgets::kIconCheck, d * 0.6f, {cx, cy}, ui::th().on_accent);
        break;
    case Badge::Run: {
        // A 10 px ring, 1.6 px, a quarter of it in the text colour, turning once in .9 s.
        r.rect(rect(b), {.fill = badge_back(), .radius = all(d / 2)});
        const float t = static_cast<float>(std::fmod(ui.now() / 0.9, 1.0)) * kTau;
        r.arc({cx, cy}, 4.2f, 1.6f, 0.0f, kTau, ui::th().w4);
        const float from = std::fmod(t, kTau);
        r.arc({cx, cy}, 4.2f, 1.6f, from, std::min(from + kTau / 4, kTau), ui::th().text);
        if (from + kTau / 4 > kTau) {
            r.arc({cx, cy}, 4.2f, 1.6f, 0.0f, from + kTau / 4 - kTau, ui::th().text);
        }
        ui.request_frame();
        break;
    }
    case Badge::Wait:
        r.rect(rect(b), {.fill = badge_wait(), .radius = all(d / 2)});
        r.rect({cx - 2, cy - 2, 4, 4}, {.fill = badge_back(), .radius = all(2)});
        break;
    case Badge::None: break;
    }
}

auto chip_size(Ui& ui, std::string_view text) -> layout::Size {
    const float w = text_width(ui, Face::Text, wt::type::chip, text);
    return {2 * wt::chip::pad_x + wt::chip::dot + wt::chip::gap + w,
            2 * wt::chip::pad_y + ui.line_height(Face::Text, wt::type::chip)};
}

void chip(Ui& ui, const Box& box, std::string_view text, Tone tone) {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = ui::th().w2, .radius = all(box.h / 2), .border = ui::th().line, .border_width = 1});
    float x = box.x + wt::chip::pad_x;
    if (tone != Tone::Plain) {
        const float d = wt::chip::dot;
        const render::Rect dot{x, box.y + (box.h - d) / 2, d, d};
        const Color c = tone_color(tone);
        r.shadow(dot, d / 2, 8.0f, c); // box-shadow 0 0 8px
        r.rect(dot, {.fill = c, .radius = all(d / 2)});
        x += d + wt::chip::gap;
    }
    label(ui, {x, box.y, box.x + box.w - x, box.h}, Face::Text, wt::type::chip, text, ui::th().text_2);
}

auto button_size(Ui& ui, std::string_view text, bool small) -> layout::Size {
    const float size = small ? wt::type::button_small : wt::type::button;
    const float px = small ? wt::button::small_pad_x : wt::button::pad_x;
    const float py = small ? wt::button::small_pad_y : wt::button::pad_y;
    return {2 * px + text_width(ui, Face::Semibold, size, text), 2 * py + ui.line_height(Face::Semibold, size)};
}

auto squeezed(const Box& box, float k) -> Box {
    return {box.x + box.w * (1 - k) / 2, box.y + box.h * (1 - k) / 2, box.w * k, box.h * k};
}

auto button(Ui& ui, Id id, const Box& whole, std::string_view text, ButtonStyle style, bool small, bool enabled) -> bool {
    const Interaction hit = enabled ? ui.interact(id, whole) : Interaction{};
    // The matrix (screen 26): under the mouse lighter and ringed; pressed .96 (MS 14) and darker; disabled 36 %.
    const float h = hover(ui, id, hit.hovered);
    const float p = ui.press(id, hit.held);
    const Box box = squeezed(whole, 1.0f - (1.0f - motion::micro::kPress) * p);
    auto& r = ui.renderer();
    const float radius = ui::th().radius.button; // small or not: the designer's one radius
    const float size = small ? wt::type::button_small : wt::type::button;
    const float was = r.opacity();
    if (!enabled) {
        r.set_opacity(was * 0.36f);
    }
    const auto& th = ui::th();
    Color ink = th.text;
    switch (style) {
    case ButtonStyle::Primary: {
        // The one main button of a card is the send button's look: the send gradient, words in on_accent,
        // a shadow 0 4px 14px -6px of the accent's line; brighter by .06 under the mouse, .95 pressed.
        const float bright = 1.0f + 0.06f * h * (1 - p) - 0.05f * p;
        shadow_layer(ui, box, radius, {.y = 4, .blur = 14, .spread = -6, .color = with_alpha(th.accent_line, 1 - p)});
        r.rect(rect(box), {.fill = brightness(th.send.top, bright), .fill_to = brightness(th.send.bottom, bright),
                           .gradient_angle = std::numbers::pi_v<float> / 2, .radius = all(radius)});
        ink = th.on_accent;
        break;
    }
    case ButtonStyle::Crit:
        // A dangerous button stays soft (the designer: its words say the danger, not a shout of colour):
        // the soft ground, its line, the words in crit; under the mouse the stronger ground.
        r.rect(rect(box), {.fill = lerp(th.crit_soft, th.crit_soft_hi, h * (1 - p)), .radius = all(radius), .border = th.crit_line, .border_width = 1});
        ink = th.crit;
        break;
    case ButtonStyle::Normal:
        // w2 at rest, w3 and the stronger line under the mouse, w1 pressed.
        r.rect(rect(box), {.fill = lerp(lerp(th.w2, th.w3, h), th.w1, p), .radius = all(radius),
                           .border = lerp(th.line, th.line_2, h * (1 - p)), .border_width = 1});
        break;
    }
    const float w = text_width(ui, Face::Semibold, size, text);
    const bool selectable = ui.selectable(); // a button's word is not selected with the text around it
    ui.set_selectable(false);
    label(ui, {box.x + std::max((box.w - w) / 2, 0.0f), box.y, std::min(w, box.w), box.h}, Face::Semibold, size, text, ink);
    ui.set_selectable(selectable);
    if (!enabled) {
        r.set_opacity(was);
    }
    focus_ring(ui, id, box, radius, hit.focus_ring);
    return hit.clicked;
}

auto segmented_size(Ui& ui, std::span<const std::string_view> labels) -> layout::Size {
    float w = 2 * wt::segment::pad;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        w += 2 * wt::segment::item_pad_x + text_width(ui, Face::Text, wt::type::segment, labels[i]) + (i > 0 ? 2.0f : 0.0f);
    }
    return {w, 2 * wt::segment::pad + 2 * wt::segment::item_pad_y + ui.line_height(Face::Text, wt::type::segment)};
}

namespace {

// A list that opens over the rest (the map's unnamed: "N more", a dropdown): it comes on quick,
// kPopupLift px down to its place on base. Shut, it is gone at once — and asked each frame it is
// shut, so the next opening comes from nothing again.
struct Popup {
    float shown = 1;
    float lift = 0;
};

auto popup(Ui& ui, Id id, bool open) -> Popup {
    auto& anim = ui.animator();
    if (!open) {
        anim.place(id ^ 0x506F707570ull, 0.0f);
        anim.place(id ^ 0x506F7075704Cull, -motion::micro::kPopupLift);
        return {.shown = 0, .lift = 0};
    }
    return {.shown = std::clamp(anim.to(id ^ 0x506F707570ull, 1.0f, motion::kQuick, 0.0f), 0.0f, 1.0f),
            .lift = anim.geometry(id ^ 0x506F7075704Cull, 0.0f, motion::kBase, -motion::micro::kPopupLift)};
}

} // namespace

auto chosen_ground(Ui& ui, Id id, float origin_x, const Box& on) -> Box {
    auto& anim = ui.animator();
    const float x = anim.geometry(id ^ 0x47726F756E6458ull, on.x - origin_x, motion::kBase);
    const float w = anim.geometry(id ^ 0x47726F756E6457ull, on.w, motion::kBase);
    return {origin_x + x, on.y, w, on.h};
}

auto chosen(Ui& ui, Id id, bool on) -> float {
    return std::clamp(ui.animator().to(id ^ 0x43686F73656Eull, on ? 1.0f : 0.0f, motion::kQuick), 0.0f, 1.0f);
}

auto segmented(Ui& ui, Id id, const Box& box, std::span<const std::string_view> labels, int selected) -> int {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = ui::th().w1, .radius = all(wt::segment::radius)});
    float x = box.x + wt::segment::pad;
    int result = selected;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const float w = 2 * wt::segment::item_pad_x + text_width(ui, Face::Text, wt::type::segment, labels[i]);
        const Box item{x, box.y + wt::segment::pad, w, box.h - 2 * wt::segment::pad};
        const auto hit = ui.interact(id * 31 + i + 1, item);
        const bool on = static_cast<int>(i) == selected;
        if (on) { // one ground for the control: it slides to the segment chosen
            r.rect(rect(chosen_ground(ui, id, box.x, item)), {.fill = ui::th().w3, .radius = all(wt::segment::item_radius)});
        } else if (const float light = hover(ui, id * 31 + i + 1, hit.hovered); light > 0.004f) { // MS 14: on quick
            r.rect(rect(item), {.fill = with_alpha(ui::th().w1, light), .radius = all(wt::segment::item_radius)});
        }
        label(ui, {x + wt::segment::item_pad_x, item.y, w - 2 * wt::segment::item_pad_x, item.h}, Face::Text,
              wt::type::segment, labels[i], lerp(ui::th().text_3, ui::th().text, chosen(ui, id * 31 + i + 1, on)));
        if (hit.clicked) {
            result = static_cast<int>(i);
        }
        focus_ring(ui, id * 31 + i + 1, item, wt::segment::item_radius, hit.focus_ring);
        x += w + 2.0f;
    }
    return result;
}

auto checkbox(Ui& ui, Id id, const Box& box, std::string_view text, bool checked) -> bool {
    const auto hit = ui.interact(id, box);
    auto& r = ui.renderer();
    const float s = wt::checkbox::size;
    const Box mark{box.x, box.y + (box.h - s) / 2, s, s};
    if (checked) {
        r.rect(rect(mark), {.fill = ui::th().accent, .radius = all(wt::checkbox::radius)});
        ui.draw_icon(widgets::kIconCheck, s * 0.7f, {mark.x + s / 2, mark.y + s / 2}, ui::th().on_accent);
    } else {
        r.rect(rect(mark), {.fill = with_alpha(ui::th().w1, hover(ui, id, hit.hovered)), .radius = all(wt::checkbox::radius),
                            .border = ui::th().text_4, .border_width = 1.5f});
    }
    const float x = mark.x + s + wt::checkbox::gap;
    // Cut, its whole under the mouse (final review I8: "Don't ask again for <folder>" is a permission).
    caption(ui, {x, box.y, box.x + box.w - x, box.h}, Face::Text, wt::type::file, text, ui::th().text_2);
    focus_ring(ui, id, mark, wt::checkbox::radius, hit.focus_ring);
    return hit.clicked;
}

auto option_height(Ui& ui) -> float {
    return 2 * wt::option::pad_y + ui.line_height(Face::Text, wt::type::option) + 1 + ui.line_height(Face::Text, wt::type::step_sub);
}

auto option(Ui& ui, Id id, const Box& box, std::string_view title, std::string_view sub, bool on) -> bool {
    const auto hit = ui.interact(id, box);
    auto& r = ui.renderer();
    if (on) {
        r.rect(rect(box), {.fill = ui::th().w2, .radius = all(wt::option::radius), .border = ui::th().line, .border_width = 1});
    } else if (const float light = hover(ui, id, hit.hovered); light > 0.004f) { // MS 14: on quick
        r.rect(rect(box), {.fill = with_alpha(ui::th().w1, light), .radius = all(wt::option::radius)});
    }
    const float d = wt::option::radio;
    const Box radio{box.x + wt::option::pad_x, box.y + wt::option::pad_y + 1, d, d}; // margin-top 1px
    r.rect(rect(radio), {.fill = {}, .radius = all(d / 2), .border = on ? ui::th().accent : ui::th().text_4,
                         .border_width = on ? wt::option::radio_dot : 1.5f});
    const float x = radio.x + d + wt::option::gap;
    const float line = ui.line_height(Face::Text, wt::type::option);
    const float sub_line = ui.line_height(Face::Text, wt::type::step_sub);
    // The two lines in the middle of the frame, whatever the text size (the user's 1.071 pressed the
    // second to its bottom edge in a fixed 44 px, 2026-10-29). Centred by their line boxes, the letters
    // still sat low (the room above capitals is larger than the second line's below its baseline): the
    // pair goes up by half the second line's descent, measured even within 3 px at 100–150 %.
    const float top = box.y + std::max(0.0f, (box.h - (line + 1 + sub_line)) / 2 - ui.descent(Face::Text, wt::type::step_sub) / 2);
    label(ui, {x, top, box.x + box.w - x - wt::option::pad_x, line}, Face::Text, wt::type::option, title, ui::th().text);
    label(ui, {x, top + line + 1, box.x + box.w - x - wt::option::pad_x, sub_line}, Face::Text, wt::type::step_sub, sub,
          ui::th().text_3);
    focus_ring(ui, id, box, wt::option::radius, hit.focus_ring);
    return hit.clicked;
}

auto dropdown_size(Ui& ui, std::span<const std::string_view> items) -> layout::Size {
    float w = 0;
    for (const auto item : items) {
        w = std::max(w, text_width(ui, Face::Text, wt::type::segment, item));
    }
    return {2 * wt::dropdown::pad_x + w + 6 + 8, 2 * wt::dropdown::pad_y + ui.line_height(Face::Text, wt::type::segment)};
}

auto dropdown(Ui& ui, Id id, const Box& box, std::span<const std::string_view> items, int selected, bool& open, bool enabled)
    -> std::optional<int> {
    auto& r = ui.renderer();
    const Interaction hit = enabled ? ui.interact(id, box) : Interaction{};
    if (!enabled) {
        open = false;
    }
    const float h = hover(ui, id, hit.hovered || open);
    r.rect(rect(box), {.fill = lerp(ui::th().w2, ui::th().w3, h), .radius = all(wt::dropdown::radius),
                       .border = lerp(ui::th().line, ui::th().line_2, h), .border_width = 1});
    const std::string_view current =
        selected >= 0 && static_cast<std::size_t>(selected) < items.size() ? items[static_cast<std::size_t>(selected)] : "";
    const float chevron = 8.0f;
    label(ui, {box.x + wt::dropdown::pad_x, box.y, box.w - 2 * wt::dropdown::pad_x - chevron - 6, box.h}, Face::Text,
          wt::type::segment, current, enabled ? ui::th().text : ui::th().text_2);
    if (enabled) {
        ui.draw_icon(work::kIconChevronDown, 9.0f, {box.x + box.w - wt::dropdown::pad_x - chevron / 2, box.y + box.h / 2},
                     ui::th().text_2);
    }
    if (hit.clicked) {
        open = !open;
        return std::nullopt;
    }
    const Popup pop = popup(ui, id, open);
    if (!open) {
        return std::nullopt;
    }
    // The list, under the box: one row per item, the selected one marked.
    const float row = box.h;
    const Box list{box.x, box.y + box.h + 4 + pop.lift, box.w, row * static_cast<float>(items.size())};
    const float list_opacity = r.opacity();
    r.set_opacity(list_opacity * pop.shown);
    pop_surface(ui, list, wt::dropdown::radius);
    std::optional<int> picked;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Box item{list.x, list.y + row * static_cast<float>(i), list.w, row};
        const auto hit_item = ui.interact(id * 131 + i + 1, item);
        const float item_light = hover(ui, id * 131 + i + 1, hit_item.hovered); // MS 14: on quick
        if (item_light > 0.004f || static_cast<int>(i) == selected) {
            r.rect({item.x + 2, item.y + 2, item.w - 4, item.h - 4},
                   {.fill = static_cast<int>(i) == selected ? ui::th().w2 : with_alpha(ui::th().w1, item_light),
                    .radius = all(wt::dropdown::radius - 2)});
        }
        label(ui, {item.x + wt::dropdown::pad_x, item.y, item.w - 2 * wt::dropdown::pad_x, item.h}, Face::Text,
              wt::type::segment, items[i], ui::th().text);
        if (hit_item.clicked) {
            picked = static_cast<int>(i);
        }
    }
    r.set_opacity(list_opacity);
    if (picked) {
        open = false;
    }
    return picked;
}

auto context_menu(Ui& ui, Id id, render::Point at, std::span<const std::string_view> items, std::span<const bool> enabled, bool& open,
                  std::span<Box> boxes) -> std::optional<int> {
    const Popup pop = popup(ui, id, open && !items.empty());
    if (!open || items.empty()) {
        return std::nullopt;
    }
    auto& r = ui.renderer();
    const float row = 32.0f;
    float w = 0;
    for (const auto item : items) {
        w = std::max(w, text_width(ui, Face::Text, wt::type::segment, item));
    }
    w += 2 * wt::dropdown::pad_x + 24;
    const float h = row * static_cast<float>(items.size()) + 8;
    // Inside the window: to the left of the point, or above it, when there is no room.
    const float vw = r.logical_width();
    const float vh = r.logical_height();
    const float x = at.x + w + 4 > vw ? std::max(4.0f, at.x - w) : at.x;
    const float y = (at.y + h + 4 > vh ? std::max(4.0f, at.y - h) : at.y) + pop.lift;
    const Box list{x, y, w, h};
    const float list_opacity = r.opacity();
    r.set_opacity(list_opacity * pop.shown);
    pop_surface(ui, list, wt::dropdown::radius);
    // Over the rest (final review I1): a press on it is its, never the text's or a row's under it.
    ui.begin_overlay(list);
    const auto& press = ui.input();
    if (press.mouse_pressed && press.mouse_x >= list.x && press.mouse_x < list.x + list.w && press.mouse_y >= list.y &&
        press.mouse_y < list.y + list.h) {
        ui.claim_press();
    }
    const bool selectable = ui.selectable(); // a menu's words are not selected
    ui.set_selectable(false);
    std::optional<int> picked;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool on = i >= enabled.size() || enabled[i];
        const Box item{list.x + 4, list.y + 4 + row * static_cast<float>(i), list.w - 8, row};
        if (i < boxes.size()) {
            boxes[i] = item;
        }
        const auto hit = on ? ui.interact(id * 131 + i + 1, item, false) : Interaction{};
        if (const float light = hover(ui, id * 131 + i + 1, hit.hovered); light > 0.004f) { // MS 14: on quick
            r.rect(rect(item), {.fill = with_alpha(ui::th().w1, light), .radius = all(wt::dropdown::radius - 2)});
        }
        label(ui, {item.x + wt::dropdown::pad_x, item.y, item.w - 2 * wt::dropdown::pad_x, item.h}, Face::Text, wt::type::segment, items[i],
              on ? ui::th().text : ui::th().text_3);
        if (hit.clicked) {
            picked = static_cast<int>(i);
        }
    }
    ui.set_selectable(selectable);
    ui.end_overlay();
    r.set_opacity(list_opacity);
    const auto& in = ui.input();
    const bool outside = in.mouse_x < list.x || in.mouse_x >= list.x + list.w || in.mouse_y < list.y || in.mouse_y >= list.y + list.h;
    if (picked || ((in.mouse_pressed || in.right_pressed) && outside)) {
        open = false;
    }
    return picked;
}

auto toggle(Ui& ui, Id id, const Box& box, bool on) -> bool {
    const auto hit = ui.interact(id, box);
    auto& r = ui.renderer();
    const float k = wt::toggle::knob;
    const float inset = (box.h - k) / 2;
    if (on) {
        r.rect(rect(box), {.fill = ui::th().accent, .radius = all(box.h / 2)});
        r.rect({box.x + box.w - inset - k, box.y + inset, k, k}, {.fill = ui::th().on_accent, .radius = all(k / 2)});
    } else {
        r.rect(rect(box), {.fill = ui::th().w3, .radius = all(box.h / 2), .border = ui::th().line, .border_width = 1});
        r.rect({box.x + inset, box.y + inset, k, k}, {.fill = ui::th().text_2, .radius = all(k / 2)});
    }
    focus_ring(ui, id, box, box.h / 2, hit.focus_ring);
    return hit.clicked;
}

void progress_bar(Ui& ui, const Box& box, float fraction, Color c) {
    auto& r = ui.renderer();
    const float radius = std::min(wt::progress::radius, box.h / 2);
    r.rect(rect(box), {.fill = ui::th().w2, .radius = all(radius)});
    const float w = box.w * std::clamp(fraction, 0.0f, 1.0f);
    if (w > 0) {
        r.rect({box.x, box.y, w, box.h}, {.fill = c, .radius = all(radius)});
    }
}

void sparkline(Ui& ui, const Box& box, std::span<const float> values, Color c, std::optional<float> warn_line, float slide,
               bool rolling) {
    if (values.empty()) {
        return;
    }
    auto& r = ui.renderer();
    // As the mockup's SVG: y = h - 2 - v / 100 * (h - 4).
    const auto y_of = [&](float v) { return box.y + box.h - 2 - std::clamp(v, 0.0f, 100.0f) / 100.0f * (box.h - 4); };
    // Where point i is (MS § 7): at rest the points span the box. On its way in, the newest is
    // beyond the right edge by (1 - slide) of a step: a whole minute keeps its step and goes left
    // (its first point off the edge), one still filling keeps its first point and gathers itself.
    const float n = static_cast<float>(values.size());
    const float s = values.size() > 2 ? std::clamp(slide, 0.0f, 1.0f) : 1.0f;
    const bool rolls = rolling && values.size() > 2;
    const float step = values.size() < 2 ? 0.0f : rolls ? box.w / (n - 2) : box.w / (n - 2 + s);
    const float first = rolls ? -s * step : 0.0f; // point 0, from the box's left
    r.push_clip({box.x, box.y - 2, box.w, box.h + 4});
    if (warn_line) {
        const float y = y_of(*warn_line);
        for (float x = box.x; x < box.x + box.w; x += 6) { // stroke-dasharray 3 3
            r.rect({x, y - 0.5f, std::min(3.0f, box.x + box.w - x), 1.0f}, {.fill = ui::th().line_2});
        }
    }
    // The area under the line (.12 of the colour), one column per logical pixel.
    const Color area = with_alpha(c, 0.12f);
    for (float x = 0; x < box.w; x += 1.0f) {
        const float at = values.size() > 1 ? std::clamp((x - first) / step, 0.0f, n - 1) : 0.0f;
        const auto i = std::min(static_cast<std::size_t>(at), values.size() - 1);
        const auto j = std::min(i + 1, values.size() - 1);
        const float v = values[i] + (values[j] - values[i]) * (at - static_cast<float>(i));
        const float y = y_of(v);
        r.rect({box.x + x, y, 1.0f, box.y + box.h - y}, {.fill = area});
    }
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        r.segment({box.x + first + step * static_cast<float>(i), y_of(values[i])},
                  {box.x + first + step * static_cast<float>(i + 1), y_of(values[i + 1])}, 2.0f, c);
    }
    r.pop_clip();
}

void chevron(Ui& ui, render::Point centre, float size, float degrees, Color color) {
    // The icon font's › is not turned (a glyph's quad is cut by the clip as it lies): the mark is
    // drawn — two strokes meeting at its point, turned about its middle.
    const float a = degrees * 3.14159265f / 180.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    const float half_w = size * 0.16f;
    const float half_h = size * 0.32f;
    const auto at = [&](float x, float y) { return render::Point{centre.x + x * c - y * s, centre.y + x * s + y * c}; };
    const std::array<render::Point, 3> points{at(-half_w, -half_h), at(half_w, 0), at(-half_w, half_h)};
    ui.renderer().stroke(points, std::max(1.0f, size * 0.1f), color, 1.0f);
}

void donut(Ui& ui, const Box& box, float fraction) {
    auto& r = ui.renderer();
    const float size = std::min(box.w, box.h);
    const float radius = size * 15.0f / 36.0f;
    const float stroke = size * 4.0f / 36.0f;
    const render::Point c{box.x + box.w / 2, box.y + box.h / 2};
    r.arc(c, radius, stroke, 0.0f, kTau, ui::th().w2);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0) {
        r.arc(c, radius, stroke, 0.0f, f * kTau, ui::th().accent);
    }
}

void stacked_bar(Ui& ui, const Box& box, std::span<const Segment> segments, Id id) {
    auto& r = ui.renderer();
    const float radius = box.h / 2;
    r.rect(rect(box), {.fill = ui::th().w2, .radius = all(radius)});
    float x = box.x;
    // Its own clock (MS § 7): segment i's turn is i · kSegmentStagger after the bar was first drawn.
    const double age = id != 0 ? ui.animator().since(id) : 1.0e9;
    const double stagger = std::chrono::duration<double>(motion::panel::kSegmentStagger).count();
    for (std::size_t i = 0; i < segments.size(); ++i) {
        float share = std::clamp(segments[i].share, 0.0f, 1.0f);
        if (id != 0) {
            const bool due = age >= stagger * static_cast<double>(i);
            if (!due) {
                ui.request_frame(); // its turn comes on a frame
            }
            share = ui.animator().geometry(id * 1099511628211ull + 0x5342410 + i, due ? share : 0.0f, motion::kSmooth, 0.0f); // its own, not a neighbour's
        }
        const float w = box.w * share;
        if (w <= 0) {
            continue;
        }
        const float left = i == 0 ? radius : 0.0f;
        const float right = x + w >= box.x + box.w - 0.5f ? radius : 0.0f;
        r.rect({x, box.y, std::min(w, box.x + box.w - x), box.h},
               {.fill = segments[i].color, .radius = {left, right, right, left}});
        x += w + 2.0f; // gap:2px
    }
}

void file_row(Ui& ui, const Box& box, std::string_view ext, std::string_view name, std::string_view meta) {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = ui::th().w1, .radius = all(wt::file_row::radius)});
    const float iw = wt::file_row::icon_w;
    const float ih = wt::file_row::icon_h;
    const Box icon{box.x + wt::file_row::pad_x, box.y + (box.h - ih) / 2, iw, ih};
    r.rect(rect(icon), {.fill = ui::th().f_page, .radius = all(3), .border = ui::th().line_2, .border_width = 1});
    // .fic::before: the extension on its colour, overhanging the icon's left edge.
    const float badge_size = 6.5f;
    const float bw = text_width(ui, Face::Semibold, badge_size, ext) + 4;
    const float bh = ui.line_height(Face::Semibold, badge_size) + 1;
    const Box badge{icon.x - 3, icon.y + ih - 2 - bh, bw, bh};
    const TagInk mark = tag_ink(ext);
    r.rect(rect(badge), {.fill = mark.ground, .radius = all(2)});
    label(ui, {badge.x + 2, badge.y, bw - 2, bh}, Face::Semibold, badge_size, ext, mark.ink);
    const float meta_w = text_width(ui, Face::Text, wt::type::file, meta);
    const float right = box.x + box.w - wt::file_row::pad_x;
    label(ui, {right - meta_w, box.y, meta_w, box.h}, Face::Text, wt::type::file, meta, ui::th().text_3);
    const float x = icon.x + iw + wt::file_row::gap;
    caption(ui, {x, box.y, std::max(right - meta_w - wt::file_row::gap - x, 0.0f), box.h}, Face::Text, wt::type::file, name, // whole under the mouse
          ui::th().text);
}

auto tag_size(Ui& ui, std::string_view text) -> layout::Size {
    return {2 * wt::tag::pad_x + text_width(ui, Face::Text, wt::type::tag, text),
            2 * wt::tag::pad_y + ui.line_height(Face::Text, wt::type::tag)};
}

void tag(Ui& ui, const Box& box, std::string_view text, Kind kind) {
    const Color k = kind_color(kind);
    ui.renderer().rect(rect(box), {.fill = kind_ground(kind), .radius = all(ui::th().radius.label)}); // 99: a capsule
    label(ui, {box.x + wt::tag::pad_x, box.y, box.w - 2 * wt::tag::pad_x, box.h}, Face::Text, wt::type::tag, text, k);
}

void kv_row(Ui& ui, const Box& box, std::string_view key, std::string_view value, Tone tone) {
    ui.renderer().rect({box.x, box.y, box.w, 1}, {.fill = ui::th().line});
    const float vw = text_width(ui, Face::Text, wt::type::kv, value);
    label(ui, {box.x + 2, box.y, box.w - vw - 8, box.h}, Face::Text, wt::type::kv, key, ui::th().text);
    label(ui, {box.x + box.w - 2 - vw, box.y, vw, box.h}, Face::Text, wt::type::kv, value,
          tone == Tone::Plain ? ui::th().text_2 : tone_color(tone));
}

void card(Ui& ui, const Box& box) {
    ui.renderer().rect(rect(box), {.fill = ui::th().card, .radius = all(ui::th().radius.card), .border = ui::th().line, .border_width = 1});
}

auto surface(const Ui& ui, Color c) -> Color {
    c.a = ui.surface_alpha();
    return c;
}

auto sheet_shadow_reach() -> ShadowReach {
    ShadowReach reach;
    for (const auto& layer : ui::th().shadow) {
        const float out = layer.blur * 1.5f + 1.0f + layer.spread; // where the renderer's shadow ends (Renderer::shadow)
        reach.side = std::max(reach.side, out);
        reach.bottom = std::max(reach.bottom, out + layer.y);
    }
    return reach;
}

void sheet_shadow(Ui& ui, const Box& box, float radius) {
    for (const auto& layer : ui::th().shadow) {
        shadow_layer(ui, box, radius, layer);
    }
}

void sheet(Ui& ui, const Box& box, float radius, bool shadow) {
    auto& r = ui.renderer();
    if (shadow) { // the theme's two layers: a wide one and a near one (a light window is held by them)
        sheet_shadow(ui, box, radius);
    }
    // The top edge sits on the screen edge: its stroke is pushed just above it (inset strokes
    // on the left, the right and the bottom only).
    r.rect({box.x, box.y - 1, box.w, box.h + 1},
           {.fill = surface(ui, ui::th().surface_top), .fill_to = surface(ui, ui::th().surface),
            .gradient_angle = std::numbers::pi_v<float> / 2, .radius = {0, 0, radius, radius}, .border = ui::th().line, .border_width = 1});
}

auto send_button(Ui& ui, Id id, const Box& whole, bool enabled) -> bool {
    const auto hit = enabled ? ui.interact(id, whole) : Interaction{};
    const float h = hover(ui, id, hit.hovered);
    const float p = ui.press(id, hit.held);
    auto& r = ui.renderer();
    if (!enabled) { // nothing to send: a neutral disc at 36 %, the arrow faint
        const float was = r.opacity();
        r.set_opacity(was * 0.36f);
        r.rect(rect(whole), {.fill = ui::th().w4, .radius = all(whole.w / 2)});
        ui.draw_icon(widgets::kIconUp, wt::bar::send_icon, {whole.x + whole.w / 2, whole.y + whole.h / 2}, ui::th().text, whole.w / wt::bar::send);
        r.set_opacity(was);
        return false;
    }
    // The send gradient, a shadow 0 4px 14px -6px of the accent's line; under the mouse brighter by .06,
    // pressed .95 and .96 of its size (MS 14).
    const Box box = squeezed(whole, 1.0f - (1.0f - motion::micro::kPress) * p);
    const float bright = 1.0f + 0.06f * h * (1 - p) - 0.05f * p;
    shadow_layer(ui, box, box.w / 2, {.y = 4, .blur = 14, .spread = -6, .color = ui::th().accent_line});
    r.rect(rect(box), {.fill = brightness(ui::th().send.top, bright), .fill_to = brightness(ui::th().send.bottom, bright),
                       .gradient_angle = std::numbers::pi_v<float> / 2, .radius = all(box.w / 2)});
    // A smaller disc (springing in, app::motion) zooms the 44 px one's arrow: one glyph, no new sizes.
    ui.draw_icon(widgets::kIconUp, wt::bar::send_icon, {box.x + box.w / 2, box.y + box.h / 2}, ui::th().on_accent,
                 box.w / wt::bar::send);
    focus_ring(ui, id, box, box.w / 2, hit.focus_ring);
    return hit.clicked;
}

auto stop_button(Ui& ui, Id id, const Box& box, float ring_alpha) -> bool {
    const auto hit = ui.interact(id, box);
    const float h = hover(ui, id, hit.hovered);
    auto& r = ui.renderer();
    const render::Point c{box.x + box.w / 2, box.y + box.h / 2};
    // ::before: a 2 px ring 3 px outside the disc, a conic gradient from the turning angle —
    // nothing for 55 % of the turn, then up to the text colour; at .85 opacity, kTurn a turn (MS § 7: as every spinner, .9 s).
    const float ring = box.w / 2 + 3 - 1;
    const double turn_seconds = std::chrono::duration<double>(motion::spin::kTurn).count();
    const float turn = static_cast<float>(std::fmod(ui.now() / turn_seconds, 1.0)) * kTau;
    constexpr int kPieces = 18;
    for (int i = 0; i < kPieces; ++i) {
        const float t0 = 0.55f + 0.45f * static_cast<float>(i) / kPieces;
        const float t1 = 0.55f + 0.45f * static_cast<float>(i + 1) / kPieces;
        float from = std::fmod(turn + t0 * kTau, kTau);
        const float span = (t1 - t0) * kTau;
        const Color c_i = with_alpha(ui::th().text, 0.85f * ring_alpha * (t1 - 0.55f) / 0.45f);
        if (from + span <= kTau) {
            r.arc(c, ring, 2.0f, from, from + span, c_i);
        } else {
            r.arc(c, ring, 2.0f, from, kTau, c_i);
            r.arc(c, ring, 2.0f, 0.0f, from + span - kTau, c_i);
        }
    }
    ui.request_frame();
    r.rect(rect(box), {.fill = lerp(ui::th().w3, ui::th().w4, h), .radius = all(box.w / 2)});
    const float s = wt::bar::stop_square * box.w / wt::bar::stop;
    r.rect({c.x - s / 2, c.y - s / 2, s, s}, {.fill = ui::th().text, .radius = all(wt::bar::stop_square_radius)});
    focus_ring(ui, id, box, box.w / 2, hit.focus_ring);
    return hit.clicked;
}

auto icon_button(Ui& ui, Id id, const Box& whole, char32_t icon, bool filled, bool enabled) -> bool {
    const auto hit = enabled ? ui.interact(id, whole) : Interaction{};
    // Under the mouse w3 and the stronger line, the icon in the text colour; pressed .96 (MS 14) and w1.
    const float h = hover(ui, id, hit.hovered);
    const float p = ui.press(id, hit.held);
    const Box box = squeezed(whole, 1.0f - (1.0f - motion::micro::kPress) * p);
    auto& r = ui.renderer();
    const float was = r.opacity();
    if (!enabled) {
        r.set_opacity(was * 0.36f);
    }
    const Color back = filled ? ui::th().w2 : with_alpha(ui::th().w2, 0.0f);
    r.rect(rect(box), {.fill = lerp(lerp(back, ui::th().w3, h), ui::th().w1, p), .radius = all(wt::bar::icon_button_radius),
                       .border = with_alpha(ui::th().line_2, h * (1 - p)), .border_width = 1});
    ui.draw_icon(icon, wt::bar::icon * box.w / wt::bar::icon_button, {box.x + box.w / 2, box.y + box.h / 2},
                 lerp(ui::th().text_2, ui::th().text, std::max(h, p)));
    r.set_opacity(was);
    focus_ring(ui, id, box, wt::bar::icon_button_radius, hit.focus_ring);
    return hit.clicked;
}

void bar_progress(Ui& ui, const Box& box, float fraction) {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = ui::th().w2}); // a track
    float x0 = box.x;
    float w = 0;
    if (fraction < 0) {
        // .rprog.ind: a 30 % highlight from -30 % to 100 % in 1.6 s, easing in fast, out slow.
        const float t = static_cast<float>(std::fmod(ui.now() / 1.6, 1.0));
        const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        w = box.w * 0.3f;
        x0 = box.x - w + (box.w + w) * e;
        ui.request_frame();
    } else {
        w = box.w * std::clamp(fraction, 0.0f, 1.0f);
    }
    if (w <= 0) {
        return;
    }
    r.push_clip(rect(box));
    // From nothing through the accent's line to the accent, a glow of the accent's line: no white.
    r.shadow({x0, box.y, w, box.h}, 0, 10.0f, ui::th().accent_line);
    r.rect({x0, box.y, w * 0.7f, box.h}, {.fill = with_alpha(ui::th().accent_line, 0.0f), .fill_to = ui::th().accent_line});
    r.rect({x0 + w * 0.7f, box.y, w * 0.3f, box.h}, {.fill = ui::th().accent_line, .fill_to = ui::th().accent});
    r.pop_clip();
}

void bubble(Ui& ui, const Box& box) {
    // border-radius 14px 14px 4px 14px; the user's own message: its ground and its line.
    ui.renderer().rect(rect(box), {.fill = ui::th().you, .radius = {14, 14, 4, 14}, .border = ui::th().you_line,
                                   .border_width = 1});
}

} // namespace ui::work

namespace ui::work {

namespace {

constexpr float kWheelLine = 40.0f; // pixels a wheel line
constexpr float kScrollBar = 5.0f;  // its width, 4 px from the right
constexpr float kScrollBarInset = 4.0f;
constexpr float kMinThumb = 24.0f;
constexpr float kBarHit = 14.0f;    // the bar takes the mouse this far from the right
constexpr float kBarWide = 2.0f;    // under the mouse the thumb is this much wider
constexpr float kAutoScroll = 8.0f; // a selection past the view: pixels a second per pixel past it
constexpr float kFade = 80.0f;      // the bottom's fade into the surface

auto most_of(const Scroll& s, const Box& viewport) -> float {
    // Less than a pixel over is not something to scroll: a window that follows its content's height
    // is a fraction short of it, and a bar as tall as the view stood beside content that fits.
    const float over = s.content - viewport.h;
    return over < 1.0f ? 0.0f : over;
}

} // namespace

auto corner_foot(float radius) -> float {
    if (radius <= kScrollBarInset) {
        return 0.0f;
    }
    const float in = radius - kScrollBarInset; // the bar's outer edge, from the corner's centre
    return radius - std::sqrt(radius * radius - in * in) + 2.0f;
}

auto scroll_view(Ui& ui, Scroll& s, const Box& viewport, float content_height) -> Box {
    const double now = ui.now();
    const auto dt = static_cast<float>(s.at < 0 ? 0.0 : std::clamp(now - s.at, 0.0, 1.0));
    s.at = now;
    s.content = content_height;
    const float most = most_of(s, viewport);
    const auto& in = ui.input();
    const Id id = (static_cast<Id>(reinterpret_cast<std::uintptr_t>(&s)) * 1099511628211ull) | 1;
    s.moved_by_user = false;
    Interaction hit{};
    if (most > 0) {
        hit = ui.interact(id, {viewport.x + viewport.w - kBarHit, viewport.y, kBarHit, viewport.h}, false);
    }
    // Asked every frame, with or without a bar: its track is kept, not made again when one comes.
    s.lit = ui.hover(id, hit.hovered || hit.held);
    if (most > 0) {
        // The bar by the mouse (responsive text): its thumb dragged, its track clicked a page at a time.
        const float rail = std::max(0.0f, viewport.h - s.foot); // the bar's room: clear of a rounded corner
        const float thumb = std::clamp(rail * viewport.h / s.content, std::min(kMinThumb, rail), rail);
        const float track = rail - thumb;
        const float top = viewport.y + track * (s.offset / most);
        if (hit.held) {
            s.moved_by_user = true; // the user's own scroll: whoever follows something lets go (final review I4)
        }
        if (hit.held && in.mouse_pressed) {
            ui.claim_press();
            if (in.mouse_y >= top && in.mouse_y < top + thumb) {
                s.grab = in.mouse_y - top;
            } else {
                s.grab = -1;
                scroll_to(s, s.offset + (in.mouse_y < top ? -1.0f : 1.0f) * viewport.h * 0.9f);
            }
        }
        if (hit.held && s.grab >= 0 && track > 0) {
            s.offset = std::clamp((in.mouse_y - s.grab - viewport.y) / track * most, 0.0f, most);
            s.gliding = false;
        } else if (!hit.held) {
            s.grab = -1;
        }
    }
    // A selection dragged past the view's top or bottom: it scrolls, the faster the further.
    if (ui.selecting() && in.mouse_down && ui.pane() != 0 && ui.selection().pane() == ui.pane() && in.mouse_x >= viewport.x &&
        in.mouse_x < viewport.x + viewport.w) {
        const float below = in.mouse_y - (viewport.y + viewport.h);
        const float above = viewport.y - in.mouse_y;
        if (below > 0 || above > 0) {
            s.offset += (below > 0 ? below : -above) * kAutoScroll * dt;
            s.gliding = false;
            s.moved_by_user = true;
            ui.request_frame();
        }
    }
    if (const float wheel = ui.wheel_for(&s, viewport); wheel != 0) { // the innermost view under the mouse
        s.offset -= wheel * kWheelLine; // away from the user: back up the content
        s.gliding = false;
    }
    if (s.gliding) {
        s.glide.target = std::clamp(s.glide.target, 0.0f, most);
        s.glide.step(dt);
        s.offset = s.glide.x;
        if (s.glide.x == s.glide.target && s.glide.v == 0.0f) {
            s.offset = s.glide.target;
            s.gliding = false;
        } else {
            ui.request_frame();
        }
    }
    s.offset = std::clamp(s.offset, 0.0f, most);
    ui.renderer().push_clip(rect(viewport));
    ui.push_hit_clip(viewport);
    return {viewport.x, viewport.y - s.offset, viewport.w, content_height};
}

void end_scroll(Ui& ui, const Scroll& s, const Box& viewport, Color surface, render::Corners fade) {
    auto& r = ui.renderer();
    const float most = most_of(s, viewport);
    if (s.offset < most - 0.5f) {
        // More below: the bottom fades into the surface.
        const float h = std::min(kFade, viewport.h);
        Color clear = surface;
        clear.a = 0;
        r.rect({viewport.x, viewport.y + viewport.h - h, viewport.w, h},
               {.fill = clear, .fill_to = surface, .gradient_angle = std::numbers::pi_v<float> / 2, .radius = fade});
    }
    r.pop_clip();
    ui.pop_hit_clip();
    if (most > 0) {
        const float rail = std::max(0.0f, viewport.h - s.foot);
        const float thumb = std::clamp(rail * viewport.h / s.content, std::min(kMinThumb, rail), rail);
        const float y = viewport.y + (rail - thumb) * (s.offset / most);
        const float w = kScrollBar + kBarWide * s.lit; // wider under the mouse, as Windows 11's
        r.rect({viewport.x + viewport.w - kScrollBarInset - w, y, w, thumb}, {.fill = lerp(ui::th().scroll, ui::th().scroll_hi, s.lit), .radius = all(w / 2)});
    }
}

void spinner(Ui& ui, render::Point centre, float diameter, Color c) {
    auto& r = ui.renderer();
    const float radius = diameter / 2 - 1;
    const float from = static_cast<float>(std::fmod(ui.now() / std::chrono::duration<double>(motion::spin::kTurn).count(), 1.0)) * kTau;
    r.arc(centre, radius, 1.6f, 0.0f, kTau, ui::th().w4);
    r.arc(centre, radius, 1.6f, from, std::min(from + kTau / 4, kTau), c);
    if (from + kTau / 4 > kTau) {
        r.arc(centre, radius, 1.6f, 0.0f, from + kTau / 4 - kTau, c);
    }
    ui.request_frame();
}

namespace {

constexpr float kTabRow = 46.0f;
constexpr float kTabPad = 13.0f;
constexpr float kTabIcon = 15.0f;
constexpr float kTabGap = 8.0f;
constexpr float kTabText = 13.0f;
constexpr float kTabCount = 11.5f;
constexpr std::string_view kDots = "\xE2\x80\xA6"; // "6…" while it loads

// A tab's count as text ("9", or "6…" while loading) in `buf`.
auto count_text(const Tab& t, std::array<char, 16>& buf) -> std::string_view {
    if (t.count < 0) {
        return {};
    }
    char* end = std::to_chars(buf.data(), buf.data() + buf.size() - kDots.size(), t.count).ptr;
    if (t.loading) {
        end = std::copy(kDots.begin(), kDots.end(), end);
    }
    return {buf.data(), static_cast<std::size_t>(end - buf.data())};
}

auto tab_width(Ui& ui, const Tab& t, std::string_view count) -> float {
    float w = kTabPad + kTabIcon + kTabGap + text_width(ui, Face::Text, kTabText, t.label) + kTabPad;
    if (!count.empty()) {
        w += kTabGap + 7 + text_width(ui, Face::Text, kTabCount, count) + 7;
    }
    return w;
}

} // namespace

auto tabs(Ui& ui, Id id, const Box& box, std::span<const Tab> list, int& active, int pinned_right, std::span<Box> boxes,
          float reserve_right, TabsMore* more) -> Box {
    auto& r = ui.renderer();
    r.rect({box.x, box.y + kTabRow - 1, box.w, 1}, {.fill = ui::th().line});
    std::array<char, 16> buf{};
    // Fitted to the row (the last of five drawn over Activity): at their own
    // widths; else their labels share what is left, cut with "…"; else the counts of the tabs not
    // shown go; else the leftmost of those go under "N more" at the row's end (responsive text: none
    // is out of reach). No allocation: at most kFit tabs.
    constexpr std::size_t kFit = 64;
    const std::size_t n = std::min(list.size(), kFit);
    std::array<float, kFit> natural{}, fixed{}, label_w{};
    std::array<bool, kFit> counted{}, shown{};
    const float dots = text_width(ui, Face::Text, kTabText, kDots);
    for (std::size_t i = 0; i < n; ++i) {
        const auto count = count_text(list[i], buf);
        natural[i] = tab_width(ui, list[i], count);
        label_w[i] = text_width(ui, Face::Text, kTabText, list[i].label);
        fixed[i] = natural[i] - label_w[i]; // padding, icon and count
        counted[i] = !count.empty();
        shown[i] = true;
    }
    const bool has_pinned = pinned_right >= 0 && static_cast<std::size_t>(pinned_right) < n;
    const float pinned_w = has_pinned ? natural[static_cast<std::size_t>(pinned_right)] : 0.0f;
    float row = box.w - 8 - 12 - reserve_right - (has_pinned ? pinned_w + 8 : 0.0f);
    // "N more": its room, taken from the row once a tab has to go under it.
    std::array<char, 24> more_buf{};
    const auto more_text = [&](std::size_t hidden) {
        const auto r2 = std::format_to_n(more_buf.data(), more_buf.size(), "{} more", hidden);
        return std::string_view{more_buf.data(), static_cast<std::size_t>(r2.out - more_buf.data())};
    };
    const float more_w = more != nullptr ? text_width(ui, Face::Text, kTabText, more_text(n)) + 2 * kTabPad + kTabGap + 10 : 0.0f;
    const auto is_free = [&](std::size_t i) { return shown[i] && static_cast<int>(i) != pinned_right; };
    // The widths the free tabs take with labels at most `cap` wide (never under the dots).
    const auto total = [&](float cap) {
        float sum = 0;
        int k = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (is_free(i)) {
                sum += fixed[i] + std::max(std::min(label_w[i], cap), dots);
                ++k;
            }
        }
        return sum + 2.0f * static_cast<float>(std::max(0, k - 1));
    };
    float cap = 1.0e9f;
    if (total(cap) > row) {
        for (int stage = 0; stage < 2 && total(dots) > row; ++stage) {
            for (std::size_t i = 0; i < n; ++i) { // the counts of the tabs not shown go
                if (stage == 0 && is_free(i) && static_cast<int>(i) != active && counted[i]) {
                    const auto count = count_text(list[i], buf);
                    fixed[i] -= kTabGap + 7 + text_width(ui, Face::Text, kTabCount, count) + 7;
                    counted[i] = false;
                }
            }
            if (stage == 1) {
                row -= more_w; // the tabs that go are listed under "N more"
            }
            for (std::size_t i = 0; stage == 1 && i < n && total(dots) > row; ++i) {
                if (is_free(i) && static_cast<int>(i) != active) {
                    shown[i] = false; // the leftmost first
                }
            }
        }
        float lo = dots, hi = 0;
        for (std::size_t i = 0; i < n; ++i) {
            hi = std::max(hi, label_w[i]);
        }
        for (int step = 0; step < 24; ++step) { // the widest cap that fits
            const float mid = (lo + hi) / 2;
            (total(mid) <= row ? lo : hi) = mid;
        }
        cap = lo;
    }
    float x = box.x + 8;
    Box active_tab{};
    for (std::size_t i = 0; i < n; ++i) {
        const auto& t = list[i];
        const auto count = counted[i] ? count_text(t, buf) : std::string_view{};
        const bool pinned = static_cast<int>(i) == pinned_right;
        if (!shown[i]) {
            if (i < boxes.size()) {
                boxes[i] = {};
            }
            continue;
        }
        // The label's room as decided above, not what is left after subtracting the rest: at a
        // monitor's 125 or 150 % that came a fraction short and "D:" was drawn "…".
        const float room = pinned ? label_w[i] : std::max(std::min(label_w[i], cap), dots);
        const float w = pinned ? natural[i] : fixed[i] + room;
        const Box tab{pinned ? box.x + box.w - 12 - reserve_right - w : x, box.y, w, kTabRow};
        if (!pinned) {
            x += w + 2;
        }
        if (i < boxes.size()) {
            boxes[i] = tab;
        }
        const Id tab_id = id * 1099511628211ull + i + 1;
        const auto hit = ui.interact(tab_id, tab);
        if (hit.clicked) {
            active = static_cast<int>(i);
        }
        const bool on = static_cast<int>(i) == active;
        // Under the mouse w1 and, not the active one, a faint 2 px line; pressed w2.
        const float th = hover(ui, tab_id, hit.hovered);
        const float tp = ui.press(tab_id, hit.held);
        if (th > 0.01f || tp > 0.01f) {
            r.rect({tab.x, tab.y + 4, tab.w, kTabRow - 8},
                   {.fill = lerp(with_alpha(ui::th().w1, th), ui::th().w2, tp), .radius = all(8)});
            if (!on) {
                r.rect({tab.x + 10, tab.y + kTabRow - 1, tab.w - 20, 2}, {.fill = with_alpha(ui::th().line_2, th), .radius = all(1)});
            }
        }
        focus_ring(ui, tab_id, {tab.x, tab.y + 4, tab.w, kTabRow - 8}, 8, hit.focus_ring);
        // MS 09: a tab's words turn on quick as it becomes (or stops being) the active one.
        const float lit = ui.animator().to(id * 1099511628211ull + 0x554C10 + i, on ? 1.0f : 0.0f, motion::kQuick);
        // The current tab's words are the accent (the designer: what is current is the accent, not a brighter grey).
        const Color text = lerp(lerp(lerp(ui::th().text_3, ui::th().text_2, th), ui::th().text, tp), ui::th().accent, lit);
        if (on) {
            active_tab = tab;
        }
        const float cy = tab.y + kTabRow / 2;
        float tx = tab.x + kTabPad;
        if (t.loading) {
            spinner(ui, {tx + kTabIcon / 2, cy}, 13.0f, ui::th().accent);
        } else if (t.icon != 0) {
            ui.draw_icon(t.icon, kTabIcon, {tx + kTabIcon / 2, cy}, text);
        }
        tx += kTabIcon + kTabGap;
        const bool selectable = ui.selectable(); // a tab's word is not selected with the text
        ui.set_selectable(false);
        tx += caption(ui, {tx, tab.y, room, kTabRow}, Face::Text, kTabText, t.label, text);
        ui.set_selectable(selectable);
        if (!count.empty()) {
            const float cw = text_width(ui, Face::Text, kTabCount, count) + 14;
            const float ch = ui.line_height(Face::Text, kTabCount) + 2;
            const Box badge{tx + kTabGap, cy - ch / 2, cw, ch};
            r.rect(rect(badge), {.fill = lerp(ui::th().w2, ui::th().accent_soft, lit), .radius = all(ch / 2)});
            label(ui, {badge.x + 7, badge.y, cw - 7, ch}, Face::Text, kTabCount, count, lerp(ui::th().text_2, ui::th().accent, lit));
        }
    }
    // The active tab's underline: one line that goes under the new tab — its place and its width
    // on base (MS 09), never from one tab to the other in a frame.
    if (active_tab.w > 0) {
        auto& anim = ui.animator();
        const float ux = anim.geometry(id * 1099511628211ull + 0x554C01, active_tab.x + 10, motion::kBase);
        const float uw = anim.geometry(id * 1099511628211ull + 0x554C02, active_tab.w - 20, motion::kBase);
        r.rect({ux, box.y + kTabRow - 2, uw, 2}, {.fill = ui::th().accent, .radius = all(1)});
    }
    // The tabs with no room: under "N more", a list of them; one chosen is the active one.
    if (more != nullptr) {
        std::array<std::string_view, kFit> hidden{};
        std::array<int, kFit> index{};
        std::size_t k = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!shown[i]) {
                hidden[k] = list[i].label;
                index[k++] = static_cast<int>(i);
            }
        }
        more->button = {};
        for (auto& b : more->items) {
            b = {};
        }
        if (k == 0) {
            more->open = false;
        } else {
            const auto said = more_text(k);
            const float w = text_width(ui, Face::Text, kTabText, said) + 2 * kTabPad + kTabGap + 10;
            const Box button{x, box.y, w, kTabRow};
            more->button = button;
            const Id more_id = id * 1099511628211ull + 0x4D4F5245ull;
            const auto hit = ui.interact(more_id, button);
            if (hit.clicked) {
                more->open = !more->open;
            }
            const float th = hover(ui, more_id, hit.hovered || more->open);
            if (th > 0.01f) {
                r.rect({button.x, button.y + 4, button.w, kTabRow - 8}, {.fill = with_alpha(ui::th().w1, th), .radius = all(8)});
            }
            const bool selectable = ui.selectable();
            ui.set_selectable(false);
            const Color ink = lerp(ui::th().text_3, ui::th().text_2, th);
            label(ui, {button.x + kTabPad, button.y, w - 2 * kTabPad - 10, kTabRow}, Face::Text, kTabText, said, ink);
            ui.set_selectable(selectable);
            ui.draw_icon(kIconChevronDown, 9.0f, {button.x + w - kTabPad - 4, button.y + kTabRow / 2}, ink);
            more->list_id = more_id + 1;
            more->at = {button.x, button.y + kTabRow};
            more->hidden_count = std::min(k, more->hidden.size());
            std::copy_n(index.begin(), more->hidden_count, more->hidden.begin());
            if (!more->defer) {
                if (const auto picked = tabs_more_list(ui, list, *more)) {
                    active = *picked;
                }
            }
        }
    }
    return {box.x, box.y + kTabRow, box.w, std::max(0.0f, box.h - kTabRow)};
}

auto tabs_more_list(Ui& ui, std::span<const Tab> list, TabsMore& more) -> std::optional<int> {
    if (!more.open || more.hidden_count == 0) {
        return std::nullopt;
    }
    constexpr std::size_t kMost = 64;
    std::array<std::string_view, kMost> labels{};
    std::array<bool, kMost> on{};
    std::array<Box, kMost> item_boxes{};
    const std::size_t k = std::min(more.hidden_count, kMost);
    for (std::size_t j = 0; j < k; ++j) {
        const auto i = static_cast<std::size_t>(more.hidden[j]);
        labels[j] = i < list.size() ? list[i].label : std::string_view{};
        on[j] = true;
    }
    const auto picked = context_menu(ui, more.list_id, more.at, std::span{labels.data(), k}, std::span{on.data(), k}, more.open,
                                     std::span{item_boxes.data(), k});
    for (std::size_t j = 0; j < k; ++j) {
        const auto at = static_cast<std::size_t>(more.hidden[j]);
        if (at < more.items.size()) {
            more.items[at] = item_boxes[j];
        }
    }
    if (!picked) {
        return std::nullopt;
    }
    return more.hidden[static_cast<std::size_t>(*picked)];
}

void type_icon(Ui& ui, const Box& box, std::string_view type) {
    auto& r = ui.renderer();
    const float cx = box.x + box.w / 2;
    const float cy = box.y + box.h / 2;
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    if (type == "folder") {
        // 21 x 16: its tab, then the body — one fill, no gradient; on a light theme an inner line
        // (yellow is lost on paper without it).
        const float x0 = cx - 10.5f;
        const float y0 = cy - 8;
        r.rect({x0, y0, 9, 5}, {.fill = ui::th().f_folder, .radius = {2, 2, 0, 0}});
        r.rect({x0, y0 + 2, 21, 14}, {.fill = ui::th().f_folder, .radius = all(3), .border = ui::th().f_folder_line, .border_width = 1});
        return;
    }
    if (type.starts_with("process:")) {
        const auto name = type.substr(8);
        const Box square{cx - 11, cy - 11, 22, 22};
        r.rect(rect(square), {.fill = ui::th().w3, .radius = all(6)}); // an app's letter is neutral, as a model's monogram
        const char initial = upper(name.empty() ? '?' : name[0]);
        const std::string_view letter{&initial, 1};
        const float lw = text_width(ui, Face::Semibold, 11.0f, letter);
        label(ui, {cx - lw / 2, square.y, lw + 1, square.h}, Face::Semibold, 11.0f, letter, ui::th().text_2);
        return;
    }
    // A page 18 x 22, its top-right corner folded, the type's tag over its bottom-left edge.
    const float x0 = cx - 9;
    const float y0 = cy - 11;
    r.rect({x0, y0, 18, 22}, {.fill = ui::th().f_page, .radius = {3, 0, 3, 3}, .border = ui::th().line_2, .border_width = 1});
    r.rect({x0 + 12, y0, 6, 6}, {.fill = page_ink(), .radius = {0, 0, 0, 2}});
    std::array<char, 8> ext{};
    const std::size_t n = std::min(type.size(), ext.size());
    std::transform(type.begin(), type.begin() + static_cast<std::ptrdiff_t>(n), ext.begin(), upper);
    const std::string_view tag{ext.data(), n};
    if (tag.empty()) {
        return;
    }
    const TagInk mark = tag_ink(tag);
    constexpr float kTagText = 6.5f;
    const float tw = text_width(ui, Face::Semibold, kTagText, tag) + 4;
    const float th = ui.line_height(Face::Semibold, kTagText) + 1;
    const Box badge{x0 - 4, y0 + 22 - 3 - th, tw, th};
    r.rect(rect(badge), {.fill = mark.ground, .radius = all(2)});
    label(ui, {badge.x + 2, badge.y, tw - 2, th}, Face::Semibold, kTagText, tag, mark.ink);
}

namespace {

// The number `value` begins with and how many bytes it takes; nothing when it begins with none.
auto leading_number(std::string_view value) noexcept -> std::optional<std::pair<float, std::size_t>> {
    float number = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number, std::chars_format::fixed);
    if (ec != std::errc{} || end == value.data()) {
        return std::nullopt;
    }
    return std::pair{number, static_cast<std::size_t>(end - value.data())};
}

} // namespace

auto number_of(std::string_view value) noexcept -> std::optional<float> {
    const auto n = leading_number(value);
    return n ? std::optional{n->first} : std::nullopt;
}

auto counted(std::string_view value, float shown, std::span<char> out) noexcept -> std::string_view {
    const auto n = leading_number(value);
    if (!n) {
        return value;
    }
    const std::string_view digits = value.substr(0, n->second);
    const auto dot = digits.find('.');
    const int decimals = dot == std::string_view::npos ? 0 : static_cast<int>(digits.size() - dot - 1);
    const auto wrote = std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size()), "{:.{}f}{}", shown, decimals, value.substr(n->second));
    return static_cast<std::size_t>(wrote.size) > out.size() ? value : std::string_view{out.data(), static_cast<std::size_t>(wrote.size)};
}

void metric_tile(Ui& ui, const Box& box, const Metric& m, Id id) {
    auto& r = ui.renderer();
    r.rect(rect(box), {.fill = ui::th().card, .radius = all(ui::th().radius.card), .border = ui::th().line, .border_width = 1});
    const float x = box.x + 14;
    const float w = box.w - 28;
    // The header: the label, its state in a pill of its tone on the right — or, when both do not fit
    // ("Grap…" was seen beside "not measured"), the pill on a line of its own under it.
    const float hy = box.y + 11;
    const float hh = ui.line_height(Face::Text, 12.5f);
    // MS § 7: the state's colour turns on quick — through the tone between when it goes two steps.
    Color tone = tone_color(m.tone);
    if (id != 0) {
        constexpr std::array kTones{Tone::Ok, Tone::Warn, Tone::Crit};
        const auto found = std::ranges::find(kTones, m.tone);
        if (found != kTones.end()) {
            const float at = ui.animator().to(id * 1099511628211ull + 0x4D54542, static_cast<float>(found - kTones.begin()), motion::kQuick); // its own track
            const auto low = static_cast<std::size_t>(std::clamp(at, 0.0f, 1.999f));
            tone = lerp(tone_color(kTones[low]), tone_color(kTones[low + 1]), std::clamp(at - static_cast<float>(low), 0.0f, 1.0f));
        }
    }
    const float state_w = m.state.empty() ? 0.0f : text_width(ui, Face::Text, 12.0f, m.state) + 18;
    const bool apart = state_w > 0 && text_width(ui, Face::Text, 12.5f, m.label) + 6 + state_w > w;
    if (state_w > 0) {
        const Box pill{apart ? x : x + w - state_w, apart ? hy + hh + 3 : hy, std::min(state_w, w), hh};
        r.rect(rect(pill), {.fill = with_alpha(tone, 0.12f), .radius = all(hh / 2)});
        caption(ui, {pill.x + 9, pill.y, pill.w - 9, hh}, Face::Text, 12.0f, m.state, tone);
    }
    caption(ui, {x, hy, std::max(0.0f, apart || state_w == 0 ? w : w - state_w - 6), hh}, Face::Text, 12.5f, m.label, ui::th().text_2);
    // The value, 24 px Display, its unit small beside it (cut, whole under the mouse).
    const float vy = hy + hh + 2 + (apart ? hh + 3 : 0.0f);
    const float vh = 24.0f * 1.15f;
    // MS § 7: a new value is counted to — the number a spring on smooth, written rounded as the value is.
    std::array<char, 32> counting{};
    std::string_view value = m.value;
    if (const auto number = id != 0 ? number_of(m.value) : std::nullopt) {
        value = counted(m.value, ui.animator().to(id * 1099511628211ull + 0x4D54541, *number, motion::kSmooth), counting);
    }
    const float vw = caption(ui, {x, vy, w, vh}, Face::Display, 24.0f, value, ui::th().text);
    if (!m.unit.empty() && x + vw + 4 < x + w) {
        const float uh = ui.line_height(Face::Text, 12.5f);
        const float base = ui.centred_baseline(Face::Display, 24.0f, vy, vh);
        caption(ui, {x + vw + 4, base - ui.ascent(Face::Text, 12.5f), w - vw - 4, uh}, Face::Text, 12.5f, m.unit, ui::th().text_3);
    }
    float under = vy + vh;
    if (!m.sub.empty()) {
        const float sh = ui.line_height(Face::Text, 12.0f);
        caption(ui, {x, vy + vh + 6, w, sh}, Face::Text, 12.0f, m.sub, ui::th().text_3);
        under = vy + vh + 6 + sh;
    }
    if (!m.series.empty()) {
        // The last minute at the bottom; under a header of two lines it gives up what it must.
        const bool warn = m.tone == Tone::Warn || m.tone == Tone::Crit;
        const float bottom = box.y + box.h - 8;
        const float top = std::max(bottom - 30, under + 4);
        if (bottom - top >= 8) {
            sparkline(ui, {x, top, w, bottom - top}, m.series, warn ? ui::th().warn : ui::th().accent, m.line, m.slide, m.rolling);
        }
    }
}

void scroll_to(Scroll& s, float y) {
    s.glide.x = s.offset;
    s.glide.v = s.gliding ? s.glide.v : 0.0f; // turned round, it keeps its speed
    s.glide.target = y;
    s.gliding = true;
}

} // namespace ui::work
