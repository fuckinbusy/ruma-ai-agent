#pragma once

// setup::view::motion — the installer's appearance and its launch as pure functions of time, on
// the tokens of ui/motion_tokens.hpp (installer::): each value is a spring's way from rest
// (ui::motion::response) from its own start (design/ui_motion/MOTION_SPEC.md § 7 "Installer").
// There is only the window, never a dimmed desktop. The launch ends with the
// installer's window become the agent's bar, its orb in the bar's place: the installer holds that
// frame until the agent has shown the same bar of its own (docs/specs/motion-rework.md § 3.8) — no
// empty frame between the two. With Windows' animation effects off nothing moves: opacity on quick.
// Coordinates are the overlay's logical pixels; `window` is where the window stands.

#include "ui/layout.hpp"
#include "ui/motion_tokens.hpp"
#include "ui/render/renderer.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>

namespace setup::view {

// The share of the orb's texture its body takes at zoom 1 (ui::Orb's framing): a texture `q`
// across at zoom `z` shows a body q * kBodyShare / z across.
inline constexpr float kBodyShare = 0.32f;
// Where the orb stands in the installer's window.
inline constexpr float kWindowOrbY = 190.0f;
// The installer's orb: its body at scale 1 (README "1.0 ≈ 240 px"; this body is taller than wide)
// and its framing (ui::Orb::set_zoom): the body is 58 % of the texture.
inline constexpr float kOrbBody = 215.0f;
inline constexpr float kOrbZoom = 0.55f;
inline constexpr float kWindowRadius = 22.0f;

// The agent's bar as it opens after an install (app::sheet::shape_of(View::Input) hung from the
// monitor's top, checked by wa_app_tests): what the installer's window becomes.
inline constexpr float kBarWidth = 860.0f;
inline constexpr float kBarSide = 24.0f; // the room the agent leaves at a narrow monitor's sides
[[nodiscard]] inline auto agent_bar(float monitor_width) -> ::ui::layout::Box {
    const float w = std::max(1.0f, std::min(kBarWidth, monitor_width - 2 * kBarSide));
    return {(monitor_width - w) / 2, 0, w, ::ui::theme::work::bar::height};
}
// The orb in that bar: its middle and the size its texture is drawn at (the agent's zoom: 1).
[[nodiscard]] inline auto agent_bar_orb(const ::ui::layout::Box& bar) -> ::ui::render::Point {
    namespace b = ::ui::theme::work::bar;
    return {bar.x + b::pad_left + b::orb_margin + b::orb / 2, bar.y + bar.h / 2};
}
inline constexpr float kBarOrbQuad = ::ui::theme::work::bar::orb;

struct Wave {
    float radius = 0;
    float alpha = 0;
};

// One part of the content on its way in: its opacity and how far under its place it still is.
struct Part {
    float alpha = 0;
    float dy = 0;
};

struct IntroFrame {
    ::ui::render::Point centre;   // the window's middle: the rings
    std::array<Wave, 2> waves{};
    ::ui::render::Point orb_centre;
    float orb_body = 0;           // the body's size across
    float orb_alpha = 0;
    float orb_spike = 0.05f;
    float orb_energy = 0;
    float orb_hue = 0;
    ::ui::layout::Box window;     // the membrane
    float window_radius = 0;
    float window_alpha = 0;
    float chrome = 0;             // 1 a chrome mirror, 0 the dark glass
    float rim_alpha = 0;
    float rim_degrees = 0;        // where the highlight runs along the edge
    std::array<Part, ::ui::motion::installer::kContentParts> parts{}; // the first screen, top to bottom
};

struct LaunchFrom {
    ::ui::layout::Box window;
    ::ui::render::Point orb;      // the orb's centre on the Done screen
    float orb_body = 215.0f;
};

struct LaunchFrame {
    float content_alpha = 1;
    ::ui::layout::Box window;     // the window on its way to the agent's bar
    float radius_top = kWindowRadius;    // the bar hangs from the monitor's top: its top corners square
    float radius_bottom = kWindowRadius;
    float window_alpha = 1;
    float rim_alpha = 0;
    float rim_degrees = 0;
    ::ui::render::Point orb_centre;
    float orb_quad = 0;           // the texture's size as drawn
    float orb_zoom = 1;           // ui::Orb::set_zoom
    float orb_alpha = 1;
    float orb_spike = 0.05f;      // at rest all the way: the agent's idle orb
    float orb_energy = 0;
};

auto intro_at(double t, const ::ui::layout::Box& window, bool reduced = false) -> IntroFrame;
// `bar`: the agent's bar (agent_bar) the window becomes.
auto launch_at(double t, const LaunchFrom& from, const ::ui::layout::Box& bar, bool reduced = false) -> LaunchFrame;
// When each has come to rest.
[[nodiscard]] auto intro_end(bool reduced = false) -> double;
[[nodiscard]] auto launch_end(bool reduced = false) -> double;
[[nodiscard]] auto intro_done(double t, bool reduced) -> bool;
[[nodiscard]] auto launch_done(double t, bool reduced) -> bool;

// The launch's clock (the window's Launch and Handoff phases): the morph from the launch's start,
// then the wait for the agent's own bar from the handoff. The handoff keeps the morph's clock: the
// layer holds its last frame — the bar — until the agent's bar is on the screen (the user saw the
// window fold a second time when the handoff restarted it, 2026-10-16).
class LaunchClock {
public:
    void begin(double now) {
        began_ = now;
        handed_at_ = -1;
    }
    void hand_off(double now) { handed_at_ = now; }
    [[nodiscard]] auto handing_off() const -> bool { return handed_at_ >= 0; }
    // The morph's time to draw at `now`.
    [[nodiscard]] auto flight(double now, bool reduced = false) const -> double { return std::min(now - began_, launch_end(reduced)); }
    [[nodiscard]] auto flown(double now, bool reduced) const -> bool { return launch_done(now - began_, reduced); }
    // Seconds since the handoff; 0 before it.
    [[nodiscard]] auto waited(double now) const -> double { return handing_off() ? now - handed_at_ : 0; }

private:
    double began_ = 0;
    double handed_at_ = -1;
};

} // namespace setup::view
