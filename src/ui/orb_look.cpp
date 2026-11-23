#include "ui/orb_look.hpp"

#include "ui/motion_tokens.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace ui::orb_look {
namespace {

// The README's table: drops, speed at rest, speed while thinking, spike while thinking.
constexpr std::array<float, 4> kDropsOf{0.0f, 2.0f, 4.0f, 5.0f};
constexpr std::array<Energy, 4> kEnergy{{{0.12f, 0.70f, 0.30f}, {0.32f, 1.15f, 0.65f}, {0.50f, 1.60f, 0.95f}, {0.72f, 2.20f, 1.15f}}};

constexpr double kUpStep = std::chrono::duration<double>(motion::model::kBarUpStep).count();
constexpr double kDownStep = std::chrono::duration<double>(motion::model::kBarDownStep).count();

auto lerp(float a, float b, float k) -> float {
    return a + (b - a) * k;
}

auto still(const motion::Spring& s) -> bool {
    return s.x == s.target && s.v == 0.0f;
}

} // namespace

auto look_of(int model, int level) noexcept -> Look {
    const auto i = static_cast<std::size_t>(std::clamp(level, 0, 3));
    return {model, kDropsOf[i], static_cast<float>(i)};
}

auto energy_of(int level) noexcept -> Energy {
    return kEnergy[static_cast<std::size_t>(std::clamp(level, 0, 3))];
}

auto energy_at(float level) noexcept -> Energy {
    const float l = std::clamp(level, 0.0f, 3.0f);
    const int a = std::min(2, static_cast<int>(l));
    const float k = l - static_cast<float>(a);
    const Energy& x = kEnergy[static_cast<std::size_t>(a)];
    const Energy& y = kEnergy[static_cast<std::size_t>(a + 1)];
    return {lerp(x.rest_speed, y.rest_speed, k), lerp(x.think_speed, y.think_speed, k), lerp(x.think_spike, y.think_spike, k)};
}

void Motion::snap(Look look) noexcept {
    level_.snap(look.level);
    level_.range = 3.0f;
    ring_.snap(1.0f);
    ring_alpha_.snap(0.0f);
    for (std::size_t i = 0; i < kDrops; ++i) {
        drops_[i].there.snap(std::clamp(look.drops - static_cast<float>(i), 0.0f, 1.0f));
        drops_[i].waits = false;
    }
    to_ = look;
}

void Motion::step(double now) noexcept {
    if (now <= last_) {
        return;
    }
    const auto dt = static_cast<float>(now - last_);
    for (motion::Spring* s : {&level_, &ring_, &ring_alpha_}) {
        s->step(dt);
    }
    for (auto& d : drops_) {
        if (d.waits && now >= d.due) { // its turn: it moves from then, not from the frame before
            d.there.set(d.then, d.token);
            d.waits = false;
            d.there.step(static_cast<float>(now - std::max(last_, d.due)));
        } else {
            d.there.step(dt);
        }
    }
    last_ = now;
}

void Motion::set(Look to, double now, bool reduced) noexcept {
    if (to == to_) {
        return;
    }
    step(now); // as far as the old targets took it
    last_ = std::max(last_, now);
    if (to.model != to_.model) {
        if (!reduced) { // one ring: the model changed
            ring_.snap(1.0f);
            ring_.set(motion::orb::kRingTo, motion::kSmooth);
            ring_alpha_.snap(motion::orb::kRingAlpha);
            ring_alpha_.set(0.0f, motion::kGentle);
        }
    }
    if (to.level != to_.level) {
        level_.set(to.level, reduced ? motion::kQuick : motion::kOrb);
    }
    if (to.drops != to_.drops) {
        // Up: the lowest new drop first; down: the last one first. Each waits its turn.
        const bool up = to.drops > to_.drops;
        int turn = 0;
        for (std::size_t k = 0; k < kDrops; ++k) {
            const std::size_t i = up ? k : kDrops - 1 - k;
            auto& d = drops_[i];
            const float want = std::clamp(to.drops - static_cast<float>(i), 0.0f, 1.0f);
            if (want == d.wanted()) {
                continue;
            }
            d.then = want;
            d.token = reduced ? motion::kQuick : up ? motion::kBase : motion::kQuick;
            d.due = now + (reduced ? 0.0 : turn * (up ? kUpStep : kDownStep));
            d.waits = true;
            ++turn;
            if (d.due <= now) {
                d.there.set(d.then, d.token);
                d.waits = false;
            }
        }
    }
    to_ = to;
}

auto Motion::at(double now) noexcept -> Frame {
    step(now);
    Frame f;
    f.look.model = to_.model;
    f.look.level = level_.x;
    f.look.drops = 0;
    for (const auto& d : drops_) {
        f.look.drops += d.there.x;
    }
    f.ring = ring_.x;
    f.ring_alpha = ring_alpha_.x;
    return f;
}

auto Motion::moving() const noexcept -> bool {
    return !still(level_) || !still(ring_alpha_) ||
           std::ranges::any_of(drops_, [](const Drop& d) { return d.waits || !still(d.there); });
}

} // namespace ui::orb_look
