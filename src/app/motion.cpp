#include "app/motion.hpp"

#include "ui/motion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace app::motion {
namespace {

constexpr double kPartStagger = std::chrono::duration<double>(ui::motion::model::kPartStagger).count();
constexpr int kParts = 4; // the picker's: the cards and the three under them

} // namespace

void WindowMotion::advance(double now) {
    if (last_ > -1.0e8 && now > last_) {
        sheet_.step(static_cast<float>(now - last_)); // the scene holds a step to a frame's limit
    }
    last_ = std::max(last_, now);
}

void WindowMotion::open(double now, float /*width*/, float bar_h) {
    advance(now);
    sheet_.summon(bar_h);
}

void WindowMotion::open_at_rest(double now, float /*width*/, float bar_h) {
    advance(now);
    sheet_.snap_open(bar_h, bar_h);
}

void WindowMotion::arrive(double now, float bar_h) {
    advance(now);
    sheet_.arrive(bar_h);
}

void WindowMotion::expand(double now, float to_h, bool parts) {
    advance(now);
    if (parts) {
        parts_at_ = now;
    }
    sheet_.expand(to_h);
}

void WindowMotion::close(double now) {
    advance(now);
    sheet_.dismiss();
}

auto WindowMotion::at(double now) -> MotionFrame {
    advance(now);
    const auto s = sheet_.frame();
    return {.orb_scale = s.orb_scale,
            .orb_alpha = s.orb_alpha,
            .hint_alpha = s.hint_alpha,
            .hint_x = s.hint_dx,
            .button_scale = s.button_scale,
            .button_alpha = s.row_alpha,
            .content_alpha = s.columns_alpha,
            .field_alpha = s.field_alpha,
            .content_dy = s.content_dy,
            .height = s.clip_h,
            .layout_h = s.layout_h,
            .window_hidden = s.hidden};
}

auto WindowMotion::part(double now, int index) const -> float {
    if (parts_at_ < -1.0e8) {
        return 1.0f;
    }
    // Opacity only: it moves with Windows' animation effects off too (MS § 3).
    return ui::motion::response(ui::motion::kQuick, now - parts_at_ - kPartStagger * index);
}

auto WindowMotion::settled(double now) const -> bool {
    return sheet_.settled() && now - parts_at_ >= kPartStagger * (kParts - 1) + ui::motion::settle_seconds(ui::motion::kQuick);
}

} // namespace app::motion
