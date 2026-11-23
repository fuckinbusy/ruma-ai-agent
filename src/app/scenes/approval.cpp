#include "app/scenes/approval.hpp"

#include <algorithm>

namespace app::scenes {

namespace m = ui::motion;
using std::chrono::milliseconds;

void Approval::ask() {
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    if (contents_.x < 0.01f) {
        lift_.snap(m::approval::kLift); // unseen: they will come up from below
    }
    line_.clear();
    line_.cue(milliseconds{0}, dim_, 1, m::kGentle);
    line_.cue(m::approval::kCardAt, room_, 1, m::kSmooth);
    line_.cue(m::approval::kCardAt, status_, 1, m::kBase);
    line_.cue(m::approval::kContentsAt, contents_, 1, m::kBase);
    line_.cue(m::approval::kContentsAt, lift_, 0, m::kBase);
    line_.start();
}

void Approval::answer() {
    line_.clear();
    line_.cue(milliseconds{0}, contents_, 0, m::kQuick);
    line_.cue(milliseconds{0}, room_, 1, m::kSmooth); // answered as it came: its room is the summary's now
    line_.cue(m::approval::kShrinkAt, shrink_, 1, m::kSmooth);
    line_.cue(m::approval::kShrinkAt, tone_, 1, m::kSmooth);
    line_.cue(m::approval::kDimOutAt, dim_, 0, m::kGentle);
    line_.cue(m::approval::kDimOutAt, status_, 2, m::kBase);
    line_.cue(m::approval::kSummaryAt, summary_, 1, m::kBase);
    line_.start();
}

void Approval::leave() {
    line_.clear();
    room_.snap(0);
    contents_.snap(0);
    lift_.snap(m::approval::kLift);
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    status_.snap(0);
    line_.cue(milliseconds{0}, dim_, 0, m::kGentle);
    line_.start();
}

void Approval::ask_reduced() {
    line_.clear();
    room_.snap(1);
    lift_.snap(0);
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    status_.snap(1);
    line_.cue(milliseconds{0}, dim_, 1, m::kQuick);
    line_.cue(milliseconds{0}, contents_, 1, m::kQuick);
    line_.start();
}

void Approval::answer_reduced() {
    line_.clear();
    room_.snap(1);
    lift_.snap(0);
    shrink_.snap(1);
    contents_.snap(0); // the card is its summary row at once: its contents have no room to fade in
    status_.snap(2);
    line_.cue(milliseconds{0}, dim_, 0, m::kQuick);
    line_.cue(milliseconds{0}, tone_, 1, m::kQuick);
    line_.cue(milliseconds{0}, summary_, 1, m::kQuick);
    line_.start();
}

void Approval::leave_reduced() {
    line_.clear();
    room_.snap(0);
    contents_.snap(0);
    lift_.snap(m::approval::kLift);
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    status_.snap(0);
    line_.cue(milliseconds{0}, dim_, 0, m::kQuick);
    line_.start();
}

void Approval::snap_asked() {
    line_.clear();
    dim_.snap(1);
    room_.snap(1);
    contents_.snap(1);
    lift_.snap(0);
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    status_.snap(1);
}

void Approval::snap_answered() {
    line_.clear();
    dim_.snap(0);
    room_.snap(1);
    contents_.snap(0);
    lift_.snap(0);
    shrink_.snap(1);
    tone_.snap(1);
    summary_.snap(1);
    status_.snap(2);
}

void Approval::snap_idle() {
    line_.clear();
    dim_.snap(0);
    room_.snap(0);
    contents_.snap(0);
    lift_.snap(m::approval::kLift);
    shrink_.snap(0);
    tone_.snap(0);
    summary_.snap(0);
    status_.snap(0);
}

void Approval::step(float dt) {
    line_.step(dt);
    for (m::Spring* s : {&dim_, &room_, &contents_, &lift_, &shrink_, &tone_, &summary_, &status_}) {
        s->step(dt);
    }
}

auto Approval::frame() const -> ApprovalFrame {
    return {.dim = std::clamp(dim_.x, 0.0f, 1.0f),
            .room = std::clamp(room_.x, 0.0f, 1.0f),
            .contents = std::clamp(contents_.x, 0.0f, 1.0f),
            .lift = lift_.x,
            .shrink = std::clamp(shrink_.x, 0.0f, 1.0f),
            .tone = std::clamp(tone_.x, 0.0f, 1.0f),
            .summary = std::clamp(summary_.x, 0.0f, 1.0f),
            .status = status_.x};
}

auto Approval::done() const -> bool {
    if (!line_.done()) {
        return false;
    }
    for (const m::Spring* s : {&dim_, &room_, &contents_, &lift_, &shrink_, &tone_, &summary_, &status_}) {
        if (s->x != s->target || s->v != 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace app::scenes
