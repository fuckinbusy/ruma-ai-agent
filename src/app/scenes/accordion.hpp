#pragma once

// app::scenes::Accordion — something opening under its row and closing (motion rework, task 14;
// design/ui_motion/MOTION_SPEC.md scene 10). Opening: the room grows on smooth, the chevron turns
// on base, the body comes 80 ms later, from 6 px above. Closing: the body goes first, on micro,
// then the room closes — an empty frame never hangs. Turned round mid-way, every part goes on
// from where it is.

#include "ui/motion.hpp"

namespace app::scenes {

struct AccordionFrame {
    float room = 0;       // 0 shut .. 1 the body's whole height
    float body_alpha = 0; // the body's opacity
    float body_dy = 0;    // the body's shift, px (from -kBodyLift coming, to -kBodyDrop going)
    float chevron = 0;    // degrees: 0 shut, 90 open
    float ground = 0;     // the opened row's ground and bar
};

class Accordion {
public:
    void open();
    void close();
    // At once: open or shut, at rest.
    void snap(bool open);

    void step(float dt);
    [[nodiscard]] auto frame() const -> AccordionFrame;
    // Every part at rest.
    [[nodiscard]] auto done() const -> bool;
    // Going to open (the last call was open(), or snap(true)).
    [[nodiscard]] auto opening() const noexcept -> bool { return opening_; }

private:
    ui::motion::Spring room_{};
    ui::motion::Spring alpha_{};
    ui::motion::Spring dy_{.range = ui::motion::accordion::kBodyLift};
    ui::motion::Spring chevron_{.range = 90.0f};
    ui::motion::Spring ground_{};
    // Its own clock for what starts late (the body coming, the room closing): no timeline here —
    // one holds pointers to its springs, and these scenes live in a vector and move with it.
    double t_ = 0;
    bool body_due_ = false;    // opening: the body's turn has not come yet
    bool chevron_due_ = false; // closing: the chevron's
    bool room_due_ = false;    // closing: the room's
    bool opening_ = false;
};

} // namespace app::scenes
