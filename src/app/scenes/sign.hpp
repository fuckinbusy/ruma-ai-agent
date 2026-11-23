#pragma once

// app::scenes::Sign — the hidden agent's sign in motion (UI update, stage 3; design/ui_presence,
// variant A · Handle, its README's table). The painter gives it what the sign says now (the
// state, the progress, whether its dot is there), whether the mouse is in its zone and how wide the
// capsule's words are; it gives back the shape and the shares of this frame. Its numbers are
// ui::motion::sign's. No colours here: the painter mixes them by `warn` and `fill`.

#include "ui/motion.hpp"

namespace app::scenes {

struct SignTarget {
    bool working = false;  // the bar is kWorkWidth wide, its fill shows the progress
    float progress = -1;   // 0..1; -1: no fraction known (no fill)
    bool dot = false;      // done, a decision, a failure
    bool warn = false;     // a decision: the bar in the warn's colour
    bool hovered = false;  // the mouse in the sign's zone, or on the capsule
    float capsule_width = 0; // the capsule's width for its words
};

struct SignFrame {
    float width = 0;       // the body: the bar, on its way to the capsule
    float height = 0;
    float radius = 0;
    float capsule = 0;     // 0 the handle .. 1 the capsule (morph: ~1 % past either end)
    float words = 0;       // the capsule's content, 0..1
    float fill = 0;        // the share of the bar in the accent, 0..1
    float dot = 0;         // the dot's opacity
    float dot_scale = 1;   // …and its size's share
    float warn = 0;        // 0 the bar's own colour .. 1 the warn's
};

class Sign {
public:
    // What it goes to from now on; the same target again changes nothing. `seconds`: the motion
    // clock (the collapse counts from the mouse's leaving on it).
    void set(const SignTarget& to, double seconds);
    // There at once (the first frame; Windows' animation effects off).
    void snap(const SignTarget& to);
    void step(float dt, double seconds);
    // Every part at its end at once (Windows' animation effects off). What waits still waits: the
    // capsule stays kCollapseAfter after the mouse — a hold is not motion.
    void settle();
    [[nodiscard]] auto frame() const -> SignFrame;
    // Every part at rest and no collapse waiting.
    [[nodiscard]] auto done() const -> bool;

private:
    ui::motion::Spring width_{.x = ui::motion::sign::kWidth, .v = 0, .target = ui::motion::sign::kWidth, .tok = ui::motion::kBase, .range = 28.0f};
    ui::motion::Spring capsule_{.tok = ui::motion::kMorph};
    ui::motion::Spring words_{.tok = ui::motion::kQuick};
    ui::motion::Spring fill_{.tok = ui::motion::kSmooth};
    ui::motion::Spring dot_{.tok = ui::motion::kQuick};
    ui::motion::Spring dot_scale_{.x = 1, .v = 0, .target = 1, .tok = ui::motion::kBase};
    ui::motion::Spring warn_{.tok = ui::motion::kSmooth};
    SignTarget to_{};
    float capsule_width_ = 0; // the capsule's width while it is, or was last, open
    double left_at_ = -1.0e9; // when the mouse left the zone; the capsule goes kCollapseAfter later
    bool open_ = false;       // the capsule is asked open (or its collapse is still waiting)
};

} // namespace app::scenes
