#pragma once

// app::scenes::Approval — a question asked and answered (motion rework, task 15;
// design/ui_motion/MOTION_SPEC.md scene 11). Asked: the screen around dims, the card's room grows,
// its contents come a little later from below. Answered: the contents go, the card shrinks into
// its summary row and turns from yellow to green, the dim goes, the summary's words come. Answered
// while it still comes, every part goes on from where it is.

#include "ui/motion.hpp"

namespace app::scenes {

struct ApprovalFrame {
    float dim = 0;      // the screen around: 0 clear .. 1 dimmed
    float room = 0;     // the card's room: 0 none .. 1 its whole height
    float contents = 0; // the card's contents' opacity
    float lift = 0;     // their shift down, px (kLift → 0 coming)
    float shrink = 0;   // 0 the card .. 1 the summary row
    float tone = 0;     // 0 the question's colour .. 1 the answer's
    float summary = 0;  // the summary's words' opacity
    float status = 0;   // 0 nothing, 1 "Needs approval", 2 done
};

class Approval {
public:
    // The question comes (MS 11, +0).
    void ask();
    // Its answer was given: from wherever the card is.
    void answer();
    // The question is gone with nothing to show for it (withdrawn, the sheet closed): the dim goes
    // on gentle, the rest at once.
    void leave();
    // With Windows' animation effects off (MS § 3): the geometry at its place at once — the card's
    // room, its shrinking, the shift — and only opacity moves, on kQuick: the dim, the words.
    void ask_reduced();
    void answer_reduced();
    void leave_reduced();
    // At once: asked and at rest; answered and at rest (the summary row); nothing.
    void snap_asked();
    void snap_answered();
    void snap_idle();

    void step(float dt);
    [[nodiscard]] auto frame() const -> ApprovalFrame;
    // Every part at rest.
    [[nodiscard]] auto done() const -> bool;
    // Seconds since the last ask(), answer() or leave().
    [[nodiscard]] auto elapsed() const noexcept -> double { return line_.elapsed(); }

private:
    ui::motion::Spring dim_{.tok = ui::motion::kGentle};
    ui::motion::Spring room_{};
    ui::motion::Spring contents_{};
    ui::motion::Spring lift_{.range = ui::motion::approval::kLift};
    ui::motion::Spring shrink_{};
    ui::motion::Spring tone_{};
    ui::motion::Spring summary_{};
    ui::motion::Spring status_{};
    ui::motion::Timeline line_; // holds pointers to the springs above: the scene is never moved
};

} // namespace app::scenes
