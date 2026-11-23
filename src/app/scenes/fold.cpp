#include "app/scenes/fold.hpp"

#include <algorithm>
#include <cmath>

namespace app::scenes {

namespace m = ui::motion;
using std::chrono::milliseconds;

void Fold::fold() {
    folding_ = true;
    line_.clear();
    line_.cue(milliseconds{0}, window_, 0, m::kQuick);
    line_.cue(milliseconds{0}, columns_, 0, m::kQuick);
    line_.cue(m::sign::kFoldShapeAt, shape_, 1, m::kMorph);
    line_.start();
}

void Fold::unfold() {
    folding_ = false;
    if (columns_.x < 0.01f) {
        dy_.snap(m::sign::kColumnsLift); // unseen: the columns will come down from above
    }
    line_.clear();
    line_.cue(m::sign::kUnfoldShapeAt, shape_, 0, m::kMorph);
    line_.cue(m::sign::kUnfoldBarAt, window_, 1, m::kBase);
    line_.cue(m::sign::kUnfoldColumnsAt, columns_, 1, m::kBase);
    line_.cue(m::sign::kUnfoldColumnsAt, dy_, 0, m::kSmooth);
    line_.start();
}

void Fold::snap_handle() {
    folding_ = true;
    line_.clear();
    shape_.snap(1);
    window_.snap(0);
    columns_.snap(0);
    dy_.snap(m::sign::kColumnsLift);
}

void Fold::snap_window() {
    folding_ = false;
    line_.clear();
    shape_.snap(0);
    window_.snap(1);
    columns_.snap(1);
    dy_.snap(0);
}

void Fold::step(float dt) {
    line_.step(dt);
    for (m::Spring* s : {&shape_, &window_, &columns_, &dy_}) {
        s->step(dt);
    }
}

auto Fold::frame() const -> FoldFrame {
    return {.shape = shape_.x,
            .window_alpha = std::clamp(window_.x, 0.0f, 1.0f),
            .columns_alpha = std::clamp(columns_.x, 0.0f, 1.0f),
            .columns_dy = dy_.x,
            // The sign's look over the last fifth of the shape's way, the orb gone by half of it.
            .handle_alpha = std::clamp(1.0f - (1.0f - shape_.x) * 5.0f, 0.0f, 1.0f),
            .orb_alpha = std::clamp(1.0f - 2.0f * shape_.x, 0.0f, 1.0f)};
}

auto Fold::near_window(float range) const -> bool {
    return !folding_ && line_.done() && std::abs(shape_.x) * range < 2.0f && std::abs(shape_.v) * range < 60.0f && window_.x > 0.98f &&
           columns_.x > 0.98f && std::abs(dy_.x) < 1.0f; // (2 px of a centred shape: one a side)
}

auto Fold::done() const -> bool {
    if (!line_.done()) {
        return false;
    }
    for (const m::Spring* s : {&shape_, &window_, &columns_, &dy_}) {
        if (s->x != s->target || s->v != 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace app::scenes
