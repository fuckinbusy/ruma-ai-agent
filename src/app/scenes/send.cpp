#include "app/scenes/send.hpp"

#include <algorithm>

namespace app::scenes {

namespace m = ui::motion;
using std::chrono::milliseconds;

void Send::start() {
    line_.clear();
    // Where Enter finds it: the arrow, the text in the field, nothing of the conversation yet.
    arrow_.snap(1);
    stop_.snap(0);
    ring_.snap(0);
    hint_.snap(0);
    rail_.snap(0);
    chip_.snap(0);
    head_.snap(0);
    bubble_.snap(0);
    who_.snap(0);
    data_.snap(0);
    if (reduced_) { // nothing travels: the text is in its bubble, the heights and shifts at rest
        press_.snap(1);
        fly_.snap(1);
        fly_alpha_.snap(0);
        chip_dx_.snap(0);
        working_.snap(1);
        data_dy_.snap(0);
        for (m::Spring* s : {&stop_, &ring_, &hint_, &rail_, &chip_, &head_, &bubble_, &who_, &data_}) {
            line_.cue(milliseconds{0}, *s, 1, m::kQuick);
        }
        line_.cue(milliseconds{0}, arrow_, 0, m::kQuick);
        line_.start();
        return;
    }
    press_.snap(1);
    fly_.snap(0);
    fly_alpha_.snap(1);
    chip_dx_.snap(m::send::kChipShift);
    working_.snap(0);
    data_dy_.snap(m::send::kDataDrop);
    line_.cue(milliseconds{0}, press_, m::send::kPress, m::kMicro);
    line_.cue(m::send::kPressBackAt, press_, 1, m::kBase);
    line_.cue(m::send::kArrowAt, arrow_, 0, m::kMicro);
    line_.cue(m::send::kFlyAt, fly_, 1, m::kSmooth);
    line_.cue(m::send::kStopAt, stop_, 1, m::kBase);
    line_.cue(m::send::kRingAt, ring_, 1, m::kSmooth);
    line_.cue(m::send::kHintAt, hint_, 1, m::kQuick);
    line_.cue(m::send::kRailAt, rail_, 1, m::kSmooth);
    line_.cue(m::send::kRailAt, chip_, 1, m::kQuick);
    line_.cue(m::send::kRailAt, chip_dx_, 0, m::kBase);
    line_.cue(m::send::kHintAt, head_, 1, m::kBase);
    line_.cue(m::send::kBubbleAt, bubble_, 1, m::kQuick);
    line_.cue(m::send::kBubbleAt, who_, 1, m::kQuick);
    line_.cue(m::send::kFlyOutAt, fly_alpha_, 0, m::kQuick);
    line_.cue(m::send::kWorkingAt, working_, 1, m::kSmooth);
    line_.cue(m::send::kDataAt, data_, 1, m::kBase);
    line_.cue(m::send::kDataAt, data_dy_, 0, m::kSmooth);
    line_.start();
}

void Send::snap() {
    line_.clear();
    arrow_.snap(0);
    fly_alpha_.snap(0);
    chip_dx_.snap(0);
    data_dy_.snap(0);
    for (m::Spring* s : {&press_, &stop_, &ring_, &hint_, &rail_, &chip_, &fly_, &head_, &bubble_, &who_, &working_, &data_}) {
        s->snap(1);
    }
}

void Send::step(float dt) {
    line_.step(dt);
    for (m::Spring* s : {&press_, &arrow_, &stop_, &ring_, &hint_, &rail_, &chip_, &chip_dx_, &fly_, &fly_alpha_, &head_, &bubble_, &who_,
                         &working_, &data_, &data_dy_}) {
        s->step(dt);
    }
}

auto Send::frame() const -> SendFrame {
    const auto unit = [](const m::Spring& s) { return std::clamp(s.x, 0.0f, 1.0f); };
    return {.press = press_.x,
            .arrow = unit(arrow_),
            .stop = unit(stop_),
            .ring = unit(ring_),
            .hint = unit(hint_),
            .rail = unit(rail_),
            .chip = unit(chip_),
            .chip_dx = chip_dx_.x,
            .fly = unit(fly_),
            .fly_alpha = unit(fly_alpha_),
            .head = unit(head_),
            .bubble = unit(bubble_),
            .who = unit(who_),
            .working = unit(working_),
            .data = unit(data_),
            .data_dy = data_dy_.x};
}

auto Send::done() const -> bool {
    if (!line_.done()) {
        return false;
    }
    for (const m::Spring* s : {&press_, &arrow_, &stop_, &ring_, &hint_, &rail_, &chip_, &chip_dx_, &fly_, &fly_alpha_, &head_, &bubble_,
                               &who_, &working_, &data_, &data_dy_}) {
        if (s->x != s->target || s->v != 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace app::scenes
