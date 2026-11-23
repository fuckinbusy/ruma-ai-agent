#pragma once

// app::scenes::Fold — the window folding into the handle at the screen's edge and coming out of it
// (UI update, stage 3; the motion is the motion rework's scenes 12 and 13, design/ui_motion/MOTION_SPEC.md).
// One morph spring is the shape: 0 the window, 1 the handle — the painter puts width, height, radius
// and the orb's place and size between the two by it. Turned round mid-way (Ctrl+Space as it folds)
// the spring keeps its value and speed. The contents cross at the tables' times.

#include "ui/motion.hpp"

namespace app::scenes {

struct FoldFrame {
    float shape = 0;         // 0 the window, 1 the handle (morph: ~1 % past either end)
    float window_alpha = 1;  // the bar's row (and, folding, everything of the window)
    float columns_alpha = 1; // the window's content under the row
    float columns_dy = 0;    // coming back, the columns come down from kColumnsLift
    float handle_alpha = 0;  // the sign's own look: over the last fifth of the shape's way
    float orb_alpha = 1;     // the orb flies with the shape and is gone by half the way
};

class Fold {
public:
    // From the window as it is to the handle.
    void fold();
    // Back to the window, from wherever the shape is.
    void unfold();
    // At once: the handle (the start of an open), or the window.
    void snap_handle();
    void snap_window();

    void step(float dt);
    [[nodiscard]] auto frame() const -> FoldFrame;
    // Every part at rest.
    [[nodiscard]] auto done() const -> bool;
    // Going to the handle (the last call was fold()).
    [[nodiscard]] auto folding() const noexcept -> bool { return folding_; }
    // Unfolding, the shape is within a pixel a side of the window (`range`: the larger of the two
    // ends' differences, px) and slow, its content there: the window's own drawing may take over.
    [[nodiscard]] auto near_window(float range) const -> bool;

private:
    ui::motion::Spring shape_{.tok = ui::motion::kMorph};
    ui::motion::Spring window_{.x = 1, .v = 0, .target = 1};
    ui::motion::Spring columns_{.x = 1, .v = 0, .target = 1};
    ui::motion::Spring dy_{.range = 10.0f};
    ui::motion::Timeline line_;
    bool folding_ = false;
};

} // namespace app::scenes
