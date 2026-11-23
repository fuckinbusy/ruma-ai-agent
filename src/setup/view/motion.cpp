#include "setup/view/motion.hpp"

#include "ui/motion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace setup::view {
namespace {

namespace mt = ::ui::motion;
namespace tok = ::ui::motion::installer;

auto seconds(std::chrono::milliseconds ms) -> double {
    return std::chrono::duration<double>(ms).count();
}

// A value on its way from `a` to `b` by a spring's share `x` — exactly `b` when the spring rests.
auto lerp(float a, float b, float x) -> float {
    return x == 1.0f ? b : a + (b - a) * x;
}

auto lerp(const ::ui::layout::Box& a, const ::ui::layout::Box& b, float x) -> ::ui::layout::Box {
    return {lerp(a.x, b.x, x), lerp(a.y, b.y, x), lerp(a.w, b.w, x), lerp(a.h, b.h, x)};
}

// A ring from `r0` to `r1` on smooth from `at`: shown on quick, going on gentle.
auto wave(double t, double at, float r0, float r1) -> Wave {
    if (t < at) {
        return {r0, 0.0f};
    }
    return {lerp(r0, r1, mt::response(mt::kSmooth, t - at)),
            0.9f * mt::response(mt::kQuick, t - at) * (1.0f - mt::response(mt::kGentle, t - at))};
}

// How long the rim's light runs along the launch's edge before it goes.
constexpr double kLaunchRim = std::chrono::duration<double>(::ui::motion::installer_hold::kRimRun).count();
// The orb's shader in the appearance: its swell begins to ebb this long after it began.
constexpr double kSwellEbb = std::chrono::duration<double>(::ui::motion::installer_hold::kSwellEbb).count();

} // namespace

auto intro_at(double t, const ::ui::layout::Box& w, bool reduced) -> IntroFrame {
    IntroFrame f;
    f.centre = {w.x + w.w / 2, w.y + w.h / 2};
    const ::ui::render::Point orb_place{f.centre.x, w.y + kWindowOrbY};
    if (reduced) { // nothing moves: the window, the orb and the content come on quick
        const float a = mt::response(mt::kQuick, t);
        f.window = w;
        f.window_radius = kWindowRadius;
        f.window_alpha = a;
        f.orb_alpha = a;
        f.orb_centre = orb_place;
        f.orb_body = kOrbBody;
        for (auto& part : f.parts) {
            part = {a, 0.0f};
        }
        return f;
    }
    // The orb: from kOrbFrom of its size on smooth, shown on quick — never from nothing, no breath.
    f.orb_alpha = mt::response(mt::kQuick, t);
    f.orb_body = kOrbBody * lerp(tok::kOrbFrom, 1.0f, mt::response(mt::kSmooth, t));
    // Its shader on the orb token: a swell that ebbs, the energy it came with dying away.
    const float swell = mt::response(mt::kOrb, t) * (1.0f - mt::response(mt::kOrb, t - kSwellEbb));
    f.orb_spike = 0.05f + 0.95f * swell;
    f.orb_hue = 0.32f * swell;
    f.orb_energy = 0.9f * (1.0f - mt::response(mt::kOrb, t));

    f.waves = {wave(t, seconds(tok::kWave1At), 90, 520), wave(t, seconds(tok::kWave2At), 95, 360)};

    // The membrane: width, height and radius by one morph spring from its round seed; the orb goes
    // to its place in the window with it.
    const double grown = t - seconds(tok::kMembraneAt);
    const float m = mt::response(mt::kMorph, grown);
    const float ww = lerp(tok::kSeed, w.w, m);
    const float wh = lerp(tok::kSeed, w.h, m);
    f.window = {f.centre.x - ww / 2, f.centre.y - wh / 2, ww, wh}; // it grows about its own middle
    f.window_radius = lerp(tok::kSeed / 2, kWindowRadius, m);
    f.window_alpha = mt::response(mt::kQuick, grown);
    f.orb_centre = {f.centre.x, lerp(f.centre.y, orb_place.y, m)};
    f.chrome = 1.0f - mt::response(mt::kGentle, t - seconds(tok::kChromeAt));
    f.rim_alpha = mt::response(mt::kQuick, grown) * (1.0f - mt::response(mt::kQuick, t - seconds(tok::kRimOutAt)));
    f.rim_degrees = static_cast<float>(std::max(0.0, grown) * static_cast<double>(::ui::motion::installer_hold::kRimTurnAppear));

    // The content: its parts one stagger after another, each on quick, up from kContentLift on base.
    for (std::size_t k = 0; k < f.parts.size(); ++k) {
        const double since = t - seconds(tok::kContentAt) - static_cast<double>(k) * seconds(tok::kContentStagger);
        f.parts[k] = {mt::response(mt::kQuick, since), tok::kContentLift * (1.0f - mt::response(mt::kBase, since))};
    }
    return f;
}

auto launch_at(double t, const LaunchFrom& from, const ::ui::layout::Box& bar, bool reduced) -> LaunchFrame {
    LaunchFrame f;
    const ::ui::render::Point slot = agent_bar_orb(bar);
    if (reduced) { // nothing moves: the agent's bar and its orb come on quick where they will stand
        const float a = mt::response(mt::kQuick, t);
        f.content_alpha = 0;
        f.window = bar;
        f.radius_top = 0;
        f.radius_bottom = ::ui::th().radius.window; // the agent's window, as its theme rounds it
        f.window_alpha = a;
        f.orb_alpha = a;
        f.orb_centre = slot;
        f.orb_zoom = 1.0f;
        f.orb_quad = kBarOrbQuad;
        return f;
    }
    f.content_alpha = 1.0f - mt::response(mt::kQuick, t);
    // One morph spring for the shape and for the orb's flight: the window becomes the agent's bar
    // (hung from the monitor's top: its top corners go square), the orb comes to its place in it.
    const float m = mt::response(mt::kMorph, t - seconds(tok::kShapeAt));
    const float within = std::clamp(m, 0.0f, 1.0f); // what must not go past its end
    f.window = lerp(from.window, bar, m);
    f.radius_top = lerp(kWindowRadius, 0.0f, within);
    f.radius_bottom = lerp(kWindowRadius, ::ui::th().radius.window, within);
    f.window_alpha = 1.0f;
    f.rim_alpha = mt::response(mt::kQuick, t) * (1.0f - mt::response(mt::kQuick, t - kLaunchRim));
    f.rim_degrees = static_cast<float>(-t * static_cast<double>(::ui::motion::installer_hold::kRimTurnLaunch));
    f.orb_centre = {lerp(from.orb.x, slot.x, m), lerp(from.orb.y, slot.y, m)};
    f.orb_zoom = lerp(kOrbZoom, 1.0f, within);
    f.orb_quad = lerp(from.orb_body * kOrbZoom / kBodyShare, kBarOrbQuad, within);
    f.orb_alpha = 1.0f;
    return f; // (its shader stays the idle orb's all the way: orb_spike, orb_energy as they are)
}

auto intro_end(bool reduced) -> double {
    if (reduced) {
        return mt::settle_seconds(mt::kQuick);
    }
    const double content = seconds(tok::kContentAt) + static_cast<double>(tok::kContentParts - 1) * seconds(tok::kContentStagger) +
                           mt::settle_seconds(mt::kBase);
    return std::max({content, seconds(tok::kChromeAt) + mt::settle_seconds(mt::kGentle),
                     seconds(tok::kMembraneAt) + mt::settle_seconds(mt::kMorph), seconds(tok::kRimOutAt) + mt::settle_seconds(mt::kQuick),
                     seconds(tok::kWave2At) + mt::settle_seconds(mt::kGentle)});
}

auto launch_end(bool reduced) -> double {
    return reduced ? mt::settle_seconds(mt::kQuick) : seconds(tok::kShapeAt) + mt::settle_seconds(mt::kMorph);
}

auto intro_done(double t, bool reduced) -> bool {
    return t >= intro_end(reduced) - 1e-9;
}

auto launch_done(double t, bool reduced) -> bool {
    return t >= launch_end(reduced) - 1e-9;
}

} // namespace setup::view
