#include "theme/contrast.hpp"

#include <algorithm>
#include <cmath>

namespace theme {
namespace {

auto lerp8(std::uint8_t a, std::uint8_t b, float t) -> std::uint8_t {
    return static_cast<std::uint8_t>(std::lround(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
}

auto lerp(float a, float b, float t) -> float {
    return a + (b - a) * t;
}

// An sRGB channel made linear (WCAG 2, "relative luminance").
auto linear(std::uint8_t channel) -> double {
    const double c = channel / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

auto over(Color top, Color ground) noexcept -> Color {
    const float a = static_cast<float>(top.a) / 255.0f;
    return {lerp8(ground.r, top.r, a), lerp8(ground.g, top.g, a), lerp8(ground.b, top.b, a), 255};
}

auto layer(Color top, Color under) noexcept -> Color {
    const float a_top = static_cast<float>(top.a) / 255.0f;
    const float a_under = static_cast<float>(under.a) / 255.0f;
    const float a = a_top + a_under * (1.0f - a_top);
    if (a <= 0.0f) {
        return {};
    }
    const auto ch = [&](std::uint8_t t, std::uint8_t u) {
        const float v = (static_cast<float>(t) * a_top + static_cast<float>(u) * a_under * (1.0f - a_top)) / a;
        return static_cast<std::uint8_t>(std::lround(v));
    };
    return {ch(top.r, under.r), ch(top.g, under.g), ch(top.b, under.b), static_cast<std::uint8_t>(std::lround(a * 255.0f))};
}

auto luminance(Color c) noexcept -> double {
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

auto contrast(Color text, Color ground) noexcept -> double {
    const double a = luminance(over(text, ground));
    const double b = luminance(ground);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

auto mix(Color a, Color b, float t) noexcept -> Color {
    t = std::clamp(t, 0.0f, 1.0f);
    return {lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t), lerp8(a.a, b.a, t)};
}

auto mix(const Theme& a, const Theme& b, float t) noexcept -> Theme {
    t = std::clamp(t, 0.0f, 1.0f);
    Theme out = t < 0.5f ? a : b; // the name, `light` and whatever is not a number: the nearer one's
    for (const ColorToken& token : color_tokens()) {
        out.*token.field = mix(a.*token.field, b.*token.field, t);
    }
    out.send = {mix(a.send.top, b.send.top, t), mix(a.send.bottom, b.send.bottom, t)};
    for (std::size_t i = 0; i < out.shadow.size(); ++i) {
        out.shadow[i] = {lerp(a.shadow[i].y, b.shadow[i].y, t), lerp(a.shadow[i].blur, b.shadow[i].blur, t),
                         lerp(a.shadow[i].spread, b.shadow[i].spread, t), mix(a.shadow[i].color, b.shadow[i].color, t)};
    }
    for (std::size_t i = 0; i < out.pop_shadow.size(); ++i) {
        out.pop_shadow[i] = {lerp(a.pop_shadow[i].y, b.pop_shadow[i].y, t), lerp(a.pop_shadow[i].blur, b.pop_shadow[i].blur, t),
                             lerp(a.pop_shadow[i].spread, b.pop_shadow[i].spread, t), mix(a.pop_shadow[i].color, b.pop_shadow[i].color, t)};
    }
    out.orb = {mix(a.orb.l1, b.orb.l1, t), mix(a.orb.l2, b.orb.l2, t), mix(a.orb.l3, b.orb.l3, t), mix(a.orb.tint, b.orb.tint, t),
               mix(a.orb.glow, b.orb.glow, t), lerp(a.orb.amb, b.orb.amb, t)};
    out.radius = {lerp(a.radius.window, b.radius.window, t), lerp(a.radius.card, b.radius.card, t),
                  lerp(a.radius.button, b.radius.button, t), lerp(a.radius.label, b.radius.label, t)};
    return out;
}

} // namespace theme
