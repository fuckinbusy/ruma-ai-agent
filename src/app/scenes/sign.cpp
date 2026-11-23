#include "app/scenes/sign.hpp"

#include <algorithm>
#include <chrono>

namespace app::scenes {

namespace m = ui::motion;
namespace s = ui::motion::sign;

namespace {

constexpr double kCollapseAfter = std::chrono::duration<double>(s::kCollapseAfter).count();

auto fill_of(const SignTarget& t) -> float {
    return t.working && t.progress >= 0 ? std::clamp(t.progress, 0.0f, 1.0f) : 0.0f;
}

} // namespace

void Sign::set(const SignTarget& to, double seconds) {
    if (to.working != to_.working) {
        width_.set(to.working ? s::kWorkWidth : s::kWidth, m::kBase);
        if (!to.working) {
            fill_.set(0.0f, m::kSmooth); // the work is over: the fill goes with the wide bar
        }
    }
    if (to.working && fill_of(to) > fill_.target) { // only grows while the work goes
        fill_.set(fill_of(to), m::kSmooth);
    }
    if (to.dot != to_.dot) {
        if (to.dot && dot_.x < 0.01f) { // not there before: it grows in
            dot_scale_.snap(s::kDotFrom);
            dot_scale_.set(1.0f, m::kBase);
        }
        dot_.set(to.dot ? 1.0f : 0.0f, m::kQuick);
    }
    if (to.warn != to_.warn) {
        warn_.set(to.warn ? 1.0f : 0.0f, m::kSmooth);
    }
    if (to.hovered) {
        capsule_width_ = to.capsule_width;
        if (!open_) {
            open_ = true;
            capsule_.set(1.0f, m::kMorph);
        }
    } else if (to_.hovered) {
        left_at_ = seconds; // the capsule stays kCollapseAfter more
    }
    to_ = to;
}

void Sign::snap(const SignTarget& to) {
    to_ = to;
    width_.snap(to.working ? s::kWorkWidth : s::kWidth);
    fill_.snap(fill_of(to));
    dot_.snap(to.dot ? 1.0f : 0.0f);
    dot_scale_.snap(1.0f);
    warn_.snap(to.warn ? 1.0f : 0.0f);
    open_ = to.hovered;
    capsule_width_ = to.capsule_width;
    capsule_.snap(open_ ? 1.0f : 0.0f);
    words_.snap(open_ ? 1.0f : 0.0f);
    left_at_ = -1.0e9;
}

void Sign::step(float dt, double seconds) {
    for (m::Spring* sp : {&width_, &capsule_, &words_, &fill_, &dot_, &dot_scale_, &warn_}) {
        sp->step(dt);
    }
    // Opening: the words once the shape is past half its way. (Closing: they went first.)
    if (open_ && words_.target < 1.0f && capsule_.x >= s::kWordsAfterShare) {
        words_.set(1.0f, m::kQuick);
    }
    // The mouse has been away long enough: from the next frame the capsule goes back.
    if (open_ && !to_.hovered && seconds - left_at_ >= kCollapseAfter) {
        open_ = false;
        capsule_.set(0.0f, m::kMorph);
        words_.set(0.0f, m::kQuick);
    }
}

void Sign::settle() {
    for (m::Spring* sp : {&width_, &capsule_, &fill_, &dot_, &dot_scale_, &warn_}) {
        sp->snap(sp->target);
    }
    words_.snap(capsule_.x >= s::kWordsAfterShare ? 1.0f : 0.0f); // the words go with the shape
}

auto Sign::frame() const -> SignFrame {
    const float c = capsule_.x;
    const float base = width_.x;
    return {.width = base + (capsule_width_ - base) * c,
            .height = s::kHeight + (s::kCapsuleHeight - s::kHeight) * c,
            .radius = s::kRadius + (s::kCapsuleHeight / 2 - s::kRadius) * c,
            .capsule = c,
            .words = std::clamp(words_.x, 0.0f, 1.0f),
            .fill = std::clamp(fill_.x, 0.0f, 1.0f),
            .dot = std::clamp(dot_.x, 0.0f, 1.0f),
            .dot_scale = dot_scale_.x,
            .warn = std::clamp(warn_.x, 0.0f, 1.0f)};
}

auto Sign::done() const -> bool {
    if (open_ && !to_.hovered) {
        return false; // its collapse is still to come
    }
    for (const m::Spring* sp : {&width_, &capsule_, &words_, &fill_, &dot_, &dot_scale_, &warn_}) {
        if (sp->x != sp->target || sp->v != 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace app::scenes
