#include "app/scenes/accordion.hpp"

#include <algorithm>
#include <chrono>

namespace app::scenes {

namespace m = ui::motion;

namespace {

auto seconds(std::chrono::milliseconds ms) -> double {
    return std::chrono::duration<double>(ms).count();
}

} // namespace

void Accordion::open() {
    opening_ = true;
    if (alpha_.x < 0.01f) {
        dy_.snap(-m::accordion::kBodyLift); // unseen: it will come down from above
    }
    t_ = 0;
    chevron_due_ = room_due_ = false;
    chevron_.set(90, m::kBase);
    room_.set(1, m::kSmooth);
    ground_.set(1, m::kSmooth);
    body_due_ = true;
}

void Accordion::close() {
    opening_ = false;
    t_ = 0;
    body_due_ = false;
    alpha_.set(0, m::kMicro);
    dy_.set(-m::accordion::kBodyDrop, m::kMicro);
    chevron_due_ = true;
    room_due_ = true;
}

void Accordion::snap(bool open) {
    opening_ = open;
    body_due_ = chevron_due_ = room_due_ = false;
    room_.snap(open ? 1.0f : 0.0f);
    alpha_.snap(open ? 1.0f : 0.0f);
    dy_.snap(open ? 0.0f : -m::accordion::kBodyLift);
    chevron_.snap(open ? 90.0f : 0.0f);
    ground_.snap(open ? 1.0f : 0.0f);
}

void Accordion::step(float dt) {
    t_ += std::clamp(dt, 0.0f, m::kMaxFrameDt);
    const auto due = [&](std::chrono::milliseconds at) { return seconds(at) <= t_ + 1e-9; };
    if (body_due_ && due(m::accordion::kBodyInAt)) {
        alpha_.set(1, m::kBase);
        dy_.set(0, m::kSmooth);
        body_due_ = false;
    }
    if (chevron_due_ && due(m::accordion::kChevronBackAt)) {
        chevron_.set(0, m::kBase);
        chevron_due_ = false;
    }
    if (room_due_ && due(m::accordion::kRoomCloseAt)) {
        room_.set(0, m::kSmooth);
        ground_.set(0, m::kSmooth);
        room_due_ = false;
    }
    for (m::Spring* s : {&room_, &alpha_, &dy_, &chevron_, &ground_}) {
        s->step(dt);
    }
}

auto Accordion::frame() const -> AccordionFrame {
    return {.room = std::clamp(room_.x, 0.0f, 1.0f),
            .body_alpha = std::clamp(alpha_.x, 0.0f, 1.0f),
            .body_dy = dy_.x,
            .chevron = std::clamp(chevron_.x, 0.0f, 90.0f),
            .ground = std::clamp(ground_.x, 0.0f, 1.0f)};
}

auto Accordion::done() const -> bool {
    if (body_due_ || chevron_due_ || room_due_) {
        return false;
    }
    for (const m::Spring* s : {&room_, &alpha_, &dy_, &chevron_, &ground_}) {
        if (s->x != s->target || s->v != 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace app::scenes
