#include "app/sheet_handle.hpp"

#include "ui/motion_tokens.hpp"
#include "ui/orb.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <cmath>

namespace app::sheet {

namespace s = ui::motion::sign;
namespace work = ui::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

namespace {

// The capsule's own measures (design/ui_presence, screens 3 and 4): the orb at its left end, the
// word, the line, the shortcut's key at its right end.
constexpr float kPad = 8.0f;
constexpr float kOrb = 26.0f;
constexpr float kGap = 8.0f;
constexpr float kText = 12.5f;
constexpr float kLineMost = 240.0f; // a longer line is cut with an ellipsis
constexpr float kKeyText = 11.0f;
constexpr float kKeyPad = 7.0f;
constexpr float kKeyHeight = 20.0f;
constexpr float kKeyGap = 12.0f;

// `a` to `b` by `t`, channel by channel (alpha too).
auto blend(Color a, Color b, float t) noexcept -> Color {
    const auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}

auto all(float r) noexcept -> ui::render::Corners {
    return {r, r, r, r};
}

auto key_width(ui::Ui& ui, std::string_view shortcut) -> float {
    return work::text_width(ui, Face::Semibold, kKeyText, shortcut) + 2 * kKeyPad;
}

} // namespace

auto sign_zone(float centre_x, float top) noexcept -> Box {
    return {centre_x - s::kZoneWidth / 2, top, s::kZoneWidth, s::kZoneHeight};
}

auto sign_body(const Box& zone, const scenes::SignFrame& f) noexcept -> Box {
    return {zone.x + (zone.w - f.width) / 2, zone.y + s::kTop, f.width, f.height};
}

auto sign_dot(const Box& zone, const scenes::SignFrame& f) noexcept -> Box {
    const Box body = sign_body(zone, f);
    return {body.x + body.w + s::kDotGap, body.y + body.h / 2 - s::kDot / 2, s::kDot, s::kDot};
}

auto sign_capsule_width(ui::Ui& ui, std::string_view word, std::string_view line, std::string_view shortcut) -> float {
    float w = kPad + kOrb + kGap;
    if (!word.empty()) {
        w += work::text_width(ui, Face::Semibold, kText, word) + kGap;
    }
    w += std::min(work::text_width(ui, Face::Text, kText, line), kLineMost);
    w += kKeyGap + key_width(ui, shortcut) + kPad;
    return std::clamp(w, s::kCapsuleMin, s::kCapsuleMost);
}

auto sign_key(const Box& zone, const scenes::SignFrame& f, ui::Ui& ui, std::string_view shortcut) -> Box {
    const Box body = sign_body(zone, f);
    const float w = key_width(ui, shortcut);
    return {body.x + body.w - kPad - w, body.y + (body.h - kKeyHeight) / 2, w, kKeyHeight};
}

void draw_sign(ui::Ui& ui, const Box& zone, const SignLook& look) {
    if (look.theme == nullptr || look.opacity <= 0.001f) {
        return;
    }
    const auto& th = *look.theme;
    const auto& f = look.frame;
    auto& r = ui.renderer();
    const float was = r.opacity();
    const float whole = was * std::clamp(look.opacity, 0.0f, 1.0f);
    const float capsule = std::clamp(f.capsule, 0.0f, 1.0f); // the share of the capsule's look on the one shape
    const Box body = sign_body(zone, f);

    if (capsule < 0.999f) { // the handle's look, going as the capsule comes
        r.set_opacity(whole * (1.0f - capsule));
        // The bar in its halo: seen on any window and wallpaper.
        // (A ring round the bar, not a ground under it: the bar is see-through and must keep its own tone.)
        r.rect({body.x - s::kHalo, body.y - s::kHalo, body.w + 2 * s::kHalo, body.h + 2 * s::kHalo},
               {.fill = {}, .radius = all(f.radius + s::kHalo), .border = th.sign_halo, .border_width = s::kHalo});
        // A decision turns it to the warn; the warn is a solid colour, the bar's own is see-through.
        r.rect({body.x, body.y, body.w, body.h}, {.fill = blend(th.sign_bar, th.warn, f.warn), .radius = all(f.radius)});
        if (f.fill > 0.001f) { // the work done so far, in the accent — no pulse
            const float w = std::max(body.w * std::min(f.fill, 1.0f), std::min(body.w, 2 * f.radius));
            r.rect({body.x, body.y, w, body.h}, {.fill = th.accent, .radius = all(f.radius)});
        }
        if (f.dot > 0.001f) { // the state's dot, with a halo of its own
            const Box full = sign_dot(zone, f);
            const float d = s::kDot * std::clamp(f.dot_scale, 0.0f, 1.2f);
            const Box dot{full.x + (full.w - d) / 2, full.y + (full.h - d) / 2, d, d};
            r.set_opacity(whole * (1.0f - capsule) * f.dot);
            r.rect({dot.x - s::kHalo, dot.y - s::kHalo, d + 2 * s::kHalo, d + 2 * s::kHalo},
                   {.fill = {}, .radius = all(d / 2 + s::kHalo), .border = th.sign_halo, .border_width = s::kHalo});
            r.rect({dot.x, dot.y, d, d}, {.fill = look.dot, .radius = all(d / 2)});
        }
    }

    if (capsule > 0.001f) { // the capsule's look on the same shape: a pop's surface and line
        // No shadow: the window's region is the capsule itself (what is beside it is the desktop's to
        // click), so a shadow could only show in the wedges at its round ends — cut square.
        r.set_opacity(whole * capsule);
        r.rect({body.x, body.y, body.w, body.h}, {.fill = th.pop, .radius = all(f.radius), .border = th.line_2, .border_width = 1});
        if (f.words > 0.001f) { // its content: past half the shape's way, on quick
            r.set_opacity(whole * capsule * f.words);
            r.push_clip({body.x, body.y, body.w, body.h});
            float x = body.x + kPad;
            if (look.orb != 0) {
                r.image({x, body.y + (body.h - kOrb) / 2, kOrb, kOrb}, ui::Orb::uv(), look.orb, ui::kOnPicture);
            }
            x += kOrb + kGap;
            const float line_h = ui.line_height(Face::Text, kText);
            const float ty = body.y + (body.h - line_h) / 2;
            const Box key = sign_key(zone, f, ui, look.shortcut);
            if (!look.word.empty()) {
                x += work::caption(ui, {x, ty, std::max(0.0f, key.x - kKeyGap - x), line_h}, Face::Semibold, kText, look.word, look.word_color) + kGap;
            }
            (void)work::caption(ui, {x, ty, std::max(0.0f, key.x - kKeyGap - x), line_h}, Face::Text, kText, look.line, th.text_2);
            r.rect({key.x, key.y, key.w, key.h}, {.fill = th.w2, .radius = all(6), .border = th.line, .border_width = 1});
            const float key_h = ui.line_height(Face::Semibold, kKeyText);
            (void)work::caption(ui, {key.x + kKeyPad, key.y + (key.h - key_h) / 2, key.w - kKeyPad, key_h}, Face::Semibold, kKeyText, look.shortcut,
                                th.text_3);
            r.pop_clip();
        }
    }
    r.set_opacity(was);
}

} // namespace app::sheet
