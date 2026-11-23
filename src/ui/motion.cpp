#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ui::motion {

void Spring::snap(float value) noexcept {
    x = target = value;
    v = 0;
}

void Spring::set(float to, MotionToken token) noexcept {
    target = to;
    tok = token;
}

// The exact solution of x'' = -ω²(x - target) - 2ζω x' from (x, v) after dt (MS § 4).
void Spring::step(float dt) noexcept {
    dt = std::clamp(dt, 0.0f, kMaxFrameDt);
    if (dt <= 0) {
        return;
    }
    const float w = 2.0f * std::numbers::pi_v<float> / tok.response;
    const float z = tok.damping;
    const float d = x - target;
    if (z >= 1.0f) { // critically damped
        const float e = std::exp(-w * dt);
        const float a = v + w * d;
        x = target + (d + a * dt) * e;
        v = (v - w * a * dt) * e;
    } else { // under-damped: kMorph only
        const float wd = w * std::sqrt(1.0f - z * z);
        const float e = std::exp(-z * w * dt);
        const float c = std::cos(wd * dt);
        const float s = std::sin(wd * dt);
        const float b = (v + z * w * d) / wd;
        const float p = d * c + b * s;
        x = target + e * p;
        v = e * (-z * w * p + (-d * wd * s + b * wd * c));
    }
    if (settled(range)) {
        x = target;
        v = 0;
    }
}

auto Spring::settled(float span) const noexcept -> bool {
    return std::fabs(x - target) < span * 0.001f && std::fabs(v) < span * 0.01f;
}

auto response(MotionToken token, double seconds) noexcept -> float {
    if (seconds <= 0) {
        return 0.0f;
    }
    if (seconds >= settle_seconds(token)) {
        return 1.0f;
    }
    const double w = 2.0 * std::numbers::pi / token.response;
    const double z = token.damping;
    if (z >= 1.0) { // 1 - (1 + wt) e^{-wt}
        return static_cast<float>(1.0 - (1.0 + w * seconds) * std::exp(-w * seconds));
    }
    const double wd = w * std::sqrt(1.0 - z * z);
    return static_cast<float>(1.0 - std::exp(-z * w * seconds) * (std::cos(wd * seconds) + z * w / wd * std::sin(wd * seconds)));
}

auto settle_seconds(MotionToken token) noexcept -> double {
    // Where Spring::settled(1) first holds: within 0.1 % of the way and under 1 % of it a second.
    // Critically damped that is w t = 11.0 (the speed's bound is the later one); kMorph, ringing
    // down faster than it creeps, is there by w t = 9.3 (both measured against a Spring stepped:
    // tests/ui/motion_test.cpp).
    return (token.damping >= 1.0f ? 11.0 : 9.3) / (2.0 * std::numbers::pi) * static_cast<double>(token.response);
}

void Timeline::cue(std::chrono::milliseconds at, Spring& spring, float to, MotionToken token) noexcept {
    if (count_ >= kMaxCues) {
        return;
    }
    cues_[count_++] = Cue{.at = static_cast<double>(at.count()) / 1000.0, .spring = &spring, .to = to, .token = token, .fired = false};
}

// Cues at 0 ms are set here, at once: a target another call gives right after the start (the sheet
// opening taller than its bar) is then not undone by the next step.
void Timeline::start() noexcept {
    t_ = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        Cue& c = cues_[i];
        c.fired = c.at <= 0;
        if (c.fired) {
            c.spring->set(c.to, c.token);
        }
    }
}

void Timeline::clear() noexcept {
    count_ = 0;
    t_ = 0;
}

void Timeline::step(float dt) noexcept {
    t_ += std::clamp(dt, 0.0f, kMaxFrameDt);
    for (std::size_t i = 0; i < count_; ++i) {
        Cue& c = cues_[i];
        if (!c.fired && c.at <= t_ + 1e-9) {
            c.spring->set(c.to, c.token);
            c.fired = true;
        }
    }
}

auto Timeline::done() const noexcept -> bool {
    for (std::size_t i = 0; i < count_; ++i) {
        if (!cues_[i].fired) {
            return false;
        }
    }
    return true;
}

} // namespace ui::motion
