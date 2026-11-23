#pragma once

// ui::orb_look — the orb as the thinking level chosen (design/ui_model README): thinking is its energy — drops split off the
// core (Off 0, Low 2, High 4, Max 5) and a glow (0…3, a corona of sparks at 3). Its changes are
// springs on the tokens of ui/motion_tokens.hpp (motion rework, task 8; MOTION_SPEC § 7).

#include "ui/motion.hpp"

#include <array>
#include <compare>

namespace ui::orb_look {

struct Look {
    int model = 0;    // which model is chosen (its place in the catalog): a change sends one ring (the orb's light is the theme's)
    float drops = 2;  // uDrops
    float level = 1;  // uLevel
    auto operator==(const Look&) const -> bool = default;
};

// `model`: the chosen model's place in the catalog; `level`: 0 Off, 1 Low, 2 High, 3 Max (clamped).
[[nodiscard]] auto look_of(int model, int level) noexcept -> Look;

// How fast the orb moves at rest and while thinking, and how far its drops stretch then.
struct Energy {
    float rest_speed = 0.32f;
    float think_speed = 1.15f;
    float think_spike = 0.65f;
};
[[nodiscard]] auto energy_of(int level) noexcept -> Energy;
// Between two levels (a transition's fractional level): in between.
[[nodiscard]] auto energy_at(float level) noexcept -> Energy;

// A frame of a change: the look and a model change's ring (its scale of the orb and its alpha; alpha
// 0: none).
struct Frame {
    Look look;
    float ring = 0;
    float ring_alpha = 0;
};

// MOTION_SPEC § 7 "Model chip Flash ↔ Pro" and "Thinking level": the glow on kOrb; a
// model change sends one ring, 1 → kRingTo of the orb on kSmooth, its alpha kRingAlpha →
// 0 on kGentle; thinking up — the drops one by one kBarUpStep apart, each on kBase; down — from the
// last, kBarDownStep apart, each on kQuick. With Windows' animations off (§ 3): no ring, no turns,
// the look crosses on kQuick. A change during another goes on from where it is; a model change and
// the drops never restart each other. Moved by the frame's time: a stall
// moves it one frame's limit (§ 5.1).
class Motion {
public:
    void snap(Look look) noexcept;
    // Where it goes from `now` on. A target it already has changes nothing.
    void set(Look to, double now, bool reduced) noexcept;
    // Moves to `now` and says the frame.
    [[nodiscard]] auto at(double now) noexcept -> Frame;
    [[nodiscard]] auto moving() const noexcept -> bool;
    [[nodiscard]] auto target() const noexcept -> const Look& { return to_; }

private:
    static constexpr std::size_t kDrops = 5;
    struct Drop {
        motion::Spring there; // 0 gone .. 1 split off
        float then = 0;       // where it goes at its turn
        motion::MotionToken token;
        double due = 0;
        bool waits = false;
        [[nodiscard]] auto wanted() const noexcept -> float { return waits ? then : there.target; }
    };
    void step(double now) noexcept;

    motion::Spring level_;
    motion::Spring ring_;
    motion::Spring ring_alpha_;
    std::array<Drop, kDrops> drops_{};
    Look to_;
    double last_ = -1.0e9;
};

} // namespace ui::orb_look
