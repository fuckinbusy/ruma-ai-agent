#pragma once

// ui::motion — the one motion model (motion rework; design/ui_motion/MOTION_SPEC.md § 4): a spring
// solved in closed form, exact for any time step, so a dropped frame neither explodes nor jerks
// and the move looks the same at 30 and at 240 Hz. A new target is set() again: value and velocity
// are kept, nothing starts over. The numbers live in motion_tokens.hpp.

#include "ui/motion_tokens.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace ui::motion {

// One animated value.
struct Spring {
    float x = 0;
    float v = 0;
    float target = 0;
    MotionToken tok = kBase;
    // The size of its moves (MS § 4: "range 1 for opacity; pass real range for px"): it rests on
    // its target within 0.1 % of this and under 1 % of it a second.
    float range = 1.0f;

    // At `value`, at rest.
    void snap(float value) noexcept;
    // Heads for `to` from wherever it is, at whatever speed it has.
    void set(float to, MotionToken token) noexcept;
    // Advances `dt` seconds (at most kMaxFrameDt). Settled for its `range`, it rests exactly on the
    // target.
    void step(float dt) noexcept;
    // Within 0.1 % of `span` of the target and slower than 1 % of it a second.
    [[nodiscard]] auto settled(float span) const noexcept -> bool;
};

// A spring's way from rest, as a number: how far (0..1) one of `token` is `seconds` after its
// target was set — exactly on it from settle_seconds() on. For what is drawn from a moment kept
// (a layer changed at, a row come at) and never turned round mid-way; what can be turned round is
// a Spring.
[[nodiscard]] auto response(MotionToken token, double seconds) noexcept -> float;
// When a spring of `token` going its whole way from rest rests on its target (Spring::settled).
[[nodiscard]] auto settle_seconds(MotionToken token) noexcept -> double;

// The time of motion drawn by its age — a layer's change, a part's turn, a row's coming: it runs with
// the frames' time, but a frame after a stall moves it kMaxFrameDt at most (MS § 5.1), so a stall
// never eats the middle of a change. Stamps and ages are both read from it.
class Clock {
public:
    // The clock at the frame of `now` (seconds); a time not after the last one moves it not at all.
    auto at(double now) noexcept -> double {
        if (last_ < -1.0e8) {
            time_ = now; // its first frame
        } else if (now > last_) {
            time_ += std::min(now - last_, static_cast<double>(kMaxFrameDt));
        }
        last_ = std::max(last_, now);
        return time_;
    }
    [[nodiscard]] auto now() const noexcept -> double { return time_; }

private:
    double last_ = -1.0e9;
    double time_ = 0;
};

// Delayed targets: "at +N ms set this spring's target" (the stagger in MS § 6's tables). A fixed
// number of cues; nothing allocated.
class Timeline {
public:
    static constexpr std::size_t kMaxCues = 32;

    // A cue at `at` after start(); more than kMaxCues are dropped.
    void cue(std::chrono::milliseconds at, Spring& spring, float to, MotionToken token) noexcept;
    // From zero; the cues given so far run again, those at 0 ms set at once.
    void start() noexcept;
    // Forgets every cue.
    void clear() noexcept;
    // Advances `dt` seconds (at most kMaxFrameDt) and sets every target whose time has come.
    void step(float dt) noexcept;

    [[nodiscard]] auto elapsed() const noexcept -> double { return t_; }
    // True when every cue has been set.
    [[nodiscard]] auto done() const noexcept -> bool;

private:
    struct Cue {
        double at = 0;
        Spring* spring = nullptr;
        float to = 0;
        MotionToken token;
        bool fired = false;
    };
    std::array<Cue, kMaxCues> cues_{};
    std::size_t count_ = 0;
    double t_ = 0;
};

} // namespace ui::motion
