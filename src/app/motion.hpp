#pragma once

// app::motion — the agent window's motion. Since the motion rework (task 6) the summon and the
// dismiss are design/ui_motion's scenes 01, 02, 03 and 15 (app::scenes::Sheet: springs on the
// designer's tokens): the curtain opens over the content, which comes down inside it; Esc fades
// the content and closes the curtain in one spring; a new event turns round from where it is.
// WindowMotion steps the scene by the time between the frames that ask (at most a frame's limit)
// and gives the painter a MotionFrame. The window hides when the close has settled.
// The fold to the handle is app::scenes::Fold. Here too: the model picker's parts coming one after
// another as it opens from the bar.

#include "app/scenes/sheet.hpp"

#include <cstdint>

namespace app::motion {

struct MotionFrame {
    float orb_scale = 0;     // the drop's size and opacity (MS 01: .72 -> 1 from +30 ms)
    float orb_alpha = 0;
    float hint_alpha = 0;    // the field's placeholder and its shift (MS 01: 10 -> 0 px)
    float hint_x = 0;
    float button_scale = 1;
    float button_alpha = 0;  // the model's pill and the button
    float content_alpha = 1; // an expanded window's columns (MS 03 fades them first)
    float field_alpha = 1;   // the bar's row (MS 03: 40 ms after the columns)
    float content_dy = 0;    // the content's shift inside the curtain (MS 01: -16 -> 0)
    float height = 0;        // the curtain: the panel's visible height (logical px)
    float layout_h = 0;      // the height its content is laid out at (MS § 5.4)
    bool window_hidden = true;
};

class WindowMotion {
public:
    // Opens to the bar (`bar_h`) on a window `width` wide; from closed, or turning a close round.
    void open(double now, float width, float bar_h);
    // The panel grows (or shrinks) to `to_h`: only its bottom moves. `parts`: the content's parts
    // come one after another from now (the model picker opening from the bar: part()).
    void expand(double now, float to_h, bool parts = false);
    void close(double now);
    // Phase 10: open at once and at rest, or closed at once — the bar folding into the handle (or
    // back) is the motion then (app::scenes::Fold).
    void open_at_rest(double now, float width, float bar_h);
    // After an install: open at once, the row's content coming (app::scenes::Sheet::arrive).
    void arrive(double now, float bar_h);
    void cut() noexcept { sheet_.snap_closed(); }
    // Windows' animation effects off (MS § 3): geometry at once, only opacity moves.
    void reduced(bool on) noexcept { sheet_.reduced(on); }

    // This frame's values: the scene stepped to `now` first.
    [[nodiscard]] auto at(double now) -> MotionFrame;
    // The opacity of part `index` (0 the first) of a content asked to come by its parts: each on
    // kQuick, model::kPartStagger after the one before; they stand still. 1 when none was asked.
    [[nodiscard]] auto part(double now, int index) const -> float;
    [[nodiscard]] auto settled(double now) const -> bool;

private:
    // Steps the scene by the time since the last frame asked (a frame's limit at most).
    void advance(double now);

    scenes::Sheet sheet_;
    double last_ = -1.0e9;
    double parts_at_ = -1.0e9; // when the parts were asked to come
};

} // namespace app::motion
