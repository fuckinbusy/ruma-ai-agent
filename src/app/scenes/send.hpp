#pragma once

// app::scenes::Send — Enter (motion rework, task 9; design/ui_motion/MOTION_SPEC.md scene 05
// "Send: the request flies into the conversation"). The request's text is never gone: it flies from the field
// into its bubble while the window grows; the arrow turns into "stop", the rail and the status come,
// the conversation's parts come in at the table's times. Springs on the designer's tokens, their
// targets set at ui/motion_tokens.hpp's send:: times, stepped by a frame's dt. At rest (no send, or
// one ended) every part is where a conversation has it. It draws nothing: the painter reads frame().

#include "ui/motion.hpp"

namespace app::scenes {

struct SendFrame {
    float press = 1;     // the round button's scale
    float arrow = 0;     // the arrow's opacity
    float stop = 1;      // "stop": its opacity; its scale kStopScaleFrom .. 1 by it
    float ring = 1;      // its turning ring
    float hint = 1;      // the new placeholder
    float rail = 1;      // the progress rail
    float chip = 1;      // the status chip, and how far right of its place it is
    float chip_dx = 0;
    float fly = 1;       // the request's text: 0 in the field .. 1 in its bubble
    float fly_alpha = 0; // the flying layer over the bubble's own text (which has 1 - this)
    float head = 1;      // the column's head
    float bubble = 1;    // the bubble's ground and ring
    float who = 1;       // "You" and "Agent"
    float working = 1;   // the "Working" block: its height's share and its opacity
    float data = 1;      // the data panel, and how far below its place it is
    float data_dy = 0;
};

class Send {
public:
    // Enter: from the field as it is.
    void start();
    // At once at rest (another view, a turn that needs none of it).
    void snap();
    // Windows' animation effects off (MS § 3): nothing travels, opacities on kQuick.
    void reduced(bool on) noexcept { reduced_ = on; }

    void step(float dt);
    [[nodiscard]] auto frame() const -> SendFrame;
    [[nodiscard]] auto done() const -> bool;
    // Seconds since start() (for what waits its time outside: the window's growth from kGrowAt).
    [[nodiscard]] auto elapsed() const noexcept -> double { return line_.elapsed(); }

private:
    using Spring = ui::motion::Spring;
    Spring press_{.x = 1, .v = 0, .target = 1};
    Spring arrow_{};
    Spring stop_{.x = 1, .v = 0, .target = 1};
    Spring ring_{.x = 1, .v = 0, .target = 1};
    Spring hint_{.x = 1, .v = 0, .target = 1};
    Spring rail_{.x = 1, .v = 0, .target = 1};
    Spring chip_{.x = 1, .v = 0, .target = 1};
    Spring chip_dx_{.range = ui::motion::send::kChipShift};
    Spring fly_{.x = 1, .v = 0, .target = 1};
    Spring fly_alpha_{};
    Spring head_{.x = 1, .v = 0, .target = 1};
    Spring bubble_{.x = 1, .v = 0, .target = 1};
    Spring who_{.x = 1, .v = 0, .target = 1};
    Spring working_{.x = 1, .v = 0, .target = 1};
    Spring data_{.x = 1, .v = 0, .target = 1};
    Spring data_dy_{.range = ui::motion::send::kDataDrop};
    ui::motion::Timeline line_;
    bool reduced_ = false;
};

} // namespace app::scenes
