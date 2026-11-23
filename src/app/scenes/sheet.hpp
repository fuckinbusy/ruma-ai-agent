#pragma once

// app::scenes::Sheet — the sheet's summon and dismiss (motion rework, task 6;
// design/ui_motion/MOTION_SPEC.md scenes 01, 02, 03, 15). Springs on the designer's tokens, their
// targets set at the tables' times (ui/motion_tokens.hpp: summon::, dismiss::), stepped by a frame's
// dt. A new event retargets what moves — value and speed kept (scene 15). It draws nothing: the
// painter reads frame().

#include "ui/motion.hpp"

namespace app::scenes {

// What the painter draws this frame (logical pixels; opacities and scales 0..1).
struct SheetFrame {
    float clip_h = 0;        // the curtain: the sheet's visible height
    float layout_h = 0;      // the height its content is laid out at (the largest it opened to)
    float content_dy = 0;    // the content's shift inside the curtain
    float orb_alpha = 0;
    float orb_scale = 1;
    float hint_alpha = 0;    // the field's placeholder and caret
    float hint_dx = 0;
    float row_alpha = 0;     // the model's pill and the button
    float button_scale = 1;
    float columns_alpha = 1; // an expanded window's content under the row
    float field_alpha = 1;   // the bar's row as a whole
    bool hidden = true;      // closed and at rest: the window may hide
};

class Sheet {
public:
    // MS 01 (and 15's reopen): the curtain to `bar_h`, the content down, the orb, the hint, the row.
    void summon(float bar_h);
    // MS 02 from the bar, 03 from a taller window; during a summon, MS 15: turned round at once.
    void dismiss();
    // The window grows or shrinks to `to_h` (the curtain on kSmooth; the layout at the larger).
    void expand(float to_h);
    // At once: open at `h` (its bar `bar_h` high), at rest (a fold's end) — or closed.
    void snap_open(float h, float bar_h);
    void snap_closed();
    // After an install (docs/specs/motion-rework.md § 3.8): the installer's window has become this
    // bar and is held under it, so the bar and its orb are there at once — nothing of scene 01's
    // curtain is played — and the row's own content (the hint, the buttons) comes on quick.
    void arrive(float bar_h);
    // Windows' animation effects off (MS § 3): geometry at its target at once, opacity on kQuick.
    void reduced(bool on);

    void step(float dt);
    [[nodiscard]] auto frame() const -> SheetFrame;
    [[nodiscard]] auto settled() const -> bool;

private:
    enum class Phase { Closed, Summoning, Open, Closing };
    void snap_geometry();
    [[nodiscard]] auto at_(std::chrono::milliseconds at) const -> std::chrono::milliseconds;
    [[nodiscard]] auto all_still() const -> bool;

    // At rest closed, as scene 01 starts (ui/motion_tokens.hpp, summon::).
    // Ranges (Spring::range): the curtain's pixels in hundreds, the shifts' in their amplitudes.
    ui::motion::Spring clip_{.range = 100.0f};
    ui::motion::Spring dy_{.x = ui::motion::summon::kContentLift, .v = 0, .target = ui::motion::summon::kContentLift, .range = 16.0f};
    ui::motion::Spring orb_a_{};
    ui::motion::Spring orb_s_{.x = ui::motion::summon::kOrbScaleFrom, .v = 0, .target = ui::motion::summon::kOrbScaleFrom};
    ui::motion::Spring hint_a_{};
    ui::motion::Spring hint_x_{.x = ui::motion::summon::kHintShift, .v = 0, .target = ui::motion::summon::kHintShift, .range = 10.0f};
    ui::motion::Spring row_a_{};
    ui::motion::Spring button_s_{.x = ui::motion::summon::kButtonScaleFrom, .v = 0, .target = ui::motion::summon::kButtonScaleFrom};
    ui::motion::Spring columns_a_{.x = 1, .v = 0, .target = 1};
    ui::motion::Spring field_a_{.x = 1, .v = 0, .target = 1};
    ui::motion::Timeline line_;
    Phase phase_ = Phase::Closed;
    float bar_h_ = 0;
    float layout_h_ = 0;
    bool reduced_ = false;
};

} // namespace app::scenes
