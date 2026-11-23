#include "app/scenes/sheet.hpp"

#include <algorithm>

namespace app::scenes {

namespace m = ui::motion;
using std::chrono::milliseconds;

void Sheet::summon(float bar_h) {
    bar_h_ = bar_h;
    if (phase_ == Phase::Closed && settled()) {
        // Unseen while closed (no opacity): scene 01's start values. The content's shift is not:
        // it comes down from wherever the last close left it (MS 15's reopen: from -12).
        // Opacities too: a bar's close (MS 02) takes them to 0, an expanded one's (MS 03) leaves them.
        orb_a_.snap(0);
        hint_a_.snap(0);
        row_a_.snap(0);
        orb_s_.snap(m::summon::kOrbScaleFrom);
        hint_x_.snap(m::summon::kHintShift);
        button_s_.snap(m::summon::kButtonScaleFrom);
        columns_a_.snap(1);
        field_a_.snap(1);
        layout_h_ = bar_h;
    }
    clip_.range = std::max({m::kHeightRange, bar_h, layout_h_});
    line_.clear();
    line_.cue(milliseconds{0}, clip_, bar_h, m::kBase);
    line_.cue(milliseconds{0}, dy_, 0, m::kSmooth);
    line_.cue(milliseconds{0}, columns_a_, 1, m::kQuick);
    line_.cue(milliseconds{0}, field_a_, 1, m::kQuick);
    line_.cue(at_(m::summon::kOrbAt), orb_a_, 1, m::kQuick);
    line_.cue(at_(m::summon::kOrbAt), orb_s_, 1, m::kBase);
    line_.cue(at_(m::summon::kHintAt), hint_a_, 1, m::kQuick);
    line_.cue(at_(m::summon::kHintAt), hint_x_, 0, m::kBase);
    line_.cue(at_(m::summon::kRowAt), row_a_, 1, m::kQuick);
    line_.cue(at_(m::summon::kRowAt), button_s_, 1, m::kBase);
    line_.start();
    phase_ = Phase::Summoning;
}

void Sheet::dismiss() {
    if (phase_ == Phase::Closed || phase_ == Phase::Closing) {
        return;
    }
    const bool mid_summon = phase_ == Phase::Summoning && !settled();
    layout_h_ = std::max({layout_h_, clip_.x, clip_.target});
    clip_.range = std::max(m::kHeightRange, layout_h_);
    line_.clear();
    if (mid_summon) { // MS 15: everything turned round at once, from where and how fast it is
        line_.cue(milliseconds{0}, clip_, 0, m::kBase);
        line_.cue(milliseconds{0}, dy_, m::dismiss::kBarContentDrop, m::kBase);
        line_.cue(milliseconds{0}, orb_a_, 0, m::kQuick);
        line_.cue(milliseconds{0}, hint_a_, 0, m::kMicro);
        line_.cue(milliseconds{0}, row_a_, 0, m::kMicro);
    } else if (clip_.target > bar_h_ + 1.0f) { // MS 03: the columns, then the row; one spring to nothing
        line_.cue(milliseconds{0}, columns_a_, 0, m::kQuick);
        line_.cue(at_(m::dismiss::kRowAt), field_a_, 0, m::kQuick);
        line_.cue(at_(m::dismiss::kCurtainAt), clip_, 0, m::kSmooth);
        line_.cue(at_(m::dismiss::kCurtainAt), dy_, m::dismiss::kExpandedContentDrop, m::kSmooth);
    } else { // MS 02: the content goes first, the curtain takes what is left
        line_.cue(milliseconds{0}, hint_a_, 0, m::kMicro);
        line_.cue(milliseconds{0}, row_a_, 0, m::kMicro);
        line_.cue(milliseconds{0}, orb_a_, 0, m::kQuick);
        line_.cue(milliseconds{0}, orb_s_, m::dismiss::kOrbScaleTo, m::kBase);
        line_.cue(at_(m::dismiss::kCurtainAt), clip_, 0, m::kBase);
        line_.cue(at_(m::dismiss::kCurtainAt), dy_, m::dismiss::kBarContentDrop, m::kBase);
    }
    line_.start();
    phase_ = Phase::Closing;
}

void Sheet::expand(float to_h) {
    if (phase_ == Phase::Closed || phase_ == Phase::Closing) {
        return;
    }
    layout_h_ = std::max(layout_h_, to_h);
    clip_.range = std::max(m::kHeightRange, layout_h_);
    clip_.set(to_h, m::kSmooth);
}

void Sheet::snap_open(float h, float bar_h) {
    line_.clear();
    bar_h_ = bar_h;
    layout_h_ = h;
    clip_.range = std::max(m::kHeightRange, h);
    clip_.snap(h);
    dy_.snap(0);
    orb_a_.snap(1);
    orb_s_.snap(1);
    hint_a_.snap(1);
    hint_x_.snap(0);
    row_a_.snap(1);
    button_s_.snap(1);
    columns_a_.snap(1);
    field_a_.snap(1);
    phase_ = Phase::Open;
}

void Sheet::arrive(float bar_h) {
    snap_open(bar_h, bar_h);
    hint_a_.snap(0);
    hint_x_.snap(reduced_ ? 0.0f : m::summon::kHintShift);
    row_a_.snap(0);
    button_s_.snap(reduced_ ? 1.0f : m::summon::kButtonScaleFrom);
    line_.cue(milliseconds{0}, hint_a_, 1, m::kQuick);
    line_.cue(milliseconds{0}, hint_x_, 0, m::kBase);
    line_.cue(milliseconds{0}, row_a_, 1, m::kQuick);
    line_.cue(milliseconds{0}, button_s_, 1, m::kBase);
    line_.start();
}

void Sheet::snap_closed() {
    line_.clear();
    clip_.snap(0);
    dy_.snap(m::summon::kContentLift);
    orb_a_.snap(0);
    orb_s_.snap(m::summon::kOrbScaleFrom);
    hint_a_.snap(0);
    hint_x_.snap(m::summon::kHintShift);
    row_a_.snap(0);
    button_s_.snap(m::summon::kButtonScaleFrom);
    columns_a_.snap(1);
    field_a_.snap(1);
    phase_ = Phase::Closed;
}

void Sheet::reduced(bool on) {
    reduced_ = on;
    if (on) {
        snap_geometry();
    }
}

// A geometry cue's time: at once with Windows' animations off (MS § 3), else the table's.
auto Sheet::at_(std::chrono::milliseconds at) const -> std::chrono::milliseconds {
    return reduced_ ? std::chrono::milliseconds{0} : at;
}

// MS § 3: with Windows' animations off nothing travels; only opacity moves.
void Sheet::snap_geometry() {
    for (m::Spring* s : {&clip_, &dy_, &orb_s_, &hint_x_, &button_s_}) {
        s->snap(s->target);
    }
}

void Sheet::step(float dt) {
    line_.step(dt);
    if (reduced_) {
        snap_geometry();
    }
    for (m::Spring* s : {&clip_, &dy_, &orb_a_, &orb_s_, &hint_a_, &hint_x_, &row_a_, &button_s_, &columns_a_, &field_a_}) {
        s->step(dt);
    }
    if (line_.done() && all_still()) {
        if (phase_ == Phase::Closing) {
            phase_ = Phase::Closed;
        } else if (phase_ == Phase::Summoning) {
            phase_ = Phase::Open;
        }
    }
    if (phase_ == Phase::Open && clip_.x == clip_.target) {
        layout_h_ = clip_.target; // the window shrank and settled: lay out at what it is now
    }
}

auto Sheet::all_still() const -> bool {
    for (const m::Spring* s : {&clip_, &dy_, &orb_a_, &orb_s_, &hint_a_, &hint_x_, &row_a_, &button_s_, &columns_a_, &field_a_}) {
        if (s->x != s->target || s->v != 0.0f) {
            return false;
        }
    }
    return true;
}

auto Sheet::settled() const -> bool {
    return line_.done() && all_still();
}

auto Sheet::frame() const -> SheetFrame {
    return {.clip_h = std::max(0.0f, clip_.x),
            .layout_h = std::max(layout_h_, clip_.x),
            .content_dy = dy_.x,
            .orb_alpha = std::clamp(orb_a_.x, 0.0f, 1.0f),
            .orb_scale = orb_s_.x,
            .hint_alpha = std::clamp(hint_a_.x, 0.0f, 1.0f),
            .hint_dx = hint_x_.x,
            .row_alpha = std::clamp(row_a_.x, 0.0f, 1.0f),
            .button_scale = button_s_.x,
            .columns_alpha = std::clamp(columns_a_.x, 0.0f, 1.0f),
            .field_alpha = std::clamp(field_a_.x, 0.0f, 1.0f),
            .hidden = phase_ == Phase::Closed};
}

} // namespace app::scenes
