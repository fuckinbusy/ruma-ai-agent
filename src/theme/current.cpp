#include "theme/current.hpp"

#include "theme/contrast.hpp"

namespace theme {
namespace {

// The UI thread's: the theme painted with, and a change's two ends while one is going.
struct State {
    Theme now = dark();
    Theme from = dark();
    Theme to = dark();
    bool changing = false;
};

auto state() noexcept -> State& {
    static State s;
    return s;
}

auto same(Color a, Color b) noexcept -> bool {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// The same theme: its name and every number of it.
auto same(const Theme& a, const Theme& b) noexcept -> bool {
    if (a.name != b.name || a.light != b.light) {
        return false;
    }
    for (const ColorToken& t : color_tokens()) {
        if (!same(a.*t.field, b.*t.field)) {
            return false;
        }
    }
    const auto same_shadow = [](const Shadow& x, const Shadow& y) {
        return x.y == y.y && x.blur == y.blur && x.spread == y.spread && same(x.color, y.color);
    };
    return same(a.send.top, b.send.top) && same(a.send.bottom, b.send.bottom) && same_shadow(a.shadow[0], b.shadow[0]) &&
           same_shadow(a.shadow[1], b.shadow[1]) && same_shadow(a.pop_shadow[0], b.pop_shadow[0]) &&
           same_shadow(a.pop_shadow[1], b.pop_shadow[1]) && same(a.orb.l1, b.orb.l1) && same(a.orb.l2, b.orb.l2) && same(a.orb.l3, b.orb.l3) &&
           same(a.orb.tint, b.orb.tint) && same(a.orb.glow, b.orb.glow) && a.orb.amb == b.orb.amb && a.radius.window == b.radius.window &&
           a.radius.card == b.radius.card && a.radius.button == b.radius.button && a.radius.label == b.radius.label;
}

} // namespace

auto current() noexcept -> const Theme& {
    return state().now;
}

void set(const Theme& th) noexcept {
    State& s = state();
    s.now = th;
    s.changing = false;
}

void begin_change(const Theme& to) noexcept {
    State& s = state();
    // Windows says "the theme changed" in bursts: the theme it already is, or already goes to, is no
    // new change — it does not start over.
    if (s.changing ? same(s.to, to) : same(s.now, to)) {
        return;
    }
    s.from = s.now; // the colours on the screen now: nothing jumps
    s.to = to;
    s.changing = true;
}

void progress(float t) noexcept {
    State& s = state();
    if (!s.changing) {
        return;
    }
    if (t >= 1.0f) {
        s.now = s.to;
        s.changing = false;
    } else {
        s.now = mix(s.from, s.to, t);
    }
}

auto changing() noexcept -> bool {
    return state().changing;
}

auto target() noexcept -> const Theme& {
    const State& s = state();
    return s.changing ? s.to : s.now;
}

} // namespace theme
