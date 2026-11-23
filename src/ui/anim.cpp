#include "ui/anim.hpp"

#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>

namespace ui::anim {

namespace {

constexpr std::size_t kSlots = 2 * Animator::kCapacity; // a power of two: the hash is masked
constexpr double kKeep = 1.0;                         // seconds a track outlives its last ask
constexpr std::uint64_t kKeepFrames = 60;             // ...and frames drawn meanwhile

auto home(Id id) noexcept -> std::size_t {
    std::uint64_t h = id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<std::size_t>(h) & (kSlots - 1);
}

// 0 marks an empty slot: an id of 0 is kept as another.
auto key(Id id) noexcept -> Id {
    return id == 0 ? 0x9E3779B97F4A7C15ull : id;
}

} // namespace

Animator::Animator() : table_(kSlots) {}

void Animator::begin_frame(double now_seconds) {
    now_ = now_seconds;
    ++frame_;
    active_ = false;
}

auto Animator::slot(Id id) noexcept -> Track* {
    for (std::size_t i = home(id), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
        if (table_[i].id == id || table_[i].id == 0) {
            return &table_[i];
        }
    }
    return nullptr;
}

// Linear probing without tombstones: what follows a removed slot in its run moves back.
void Animator::remove(std::size_t index) noexcept {
    std::size_t hole = index;
    table_[hole] = Track{};
    for (std::size_t i = (hole + 1) & (kSlots - 1); table_[i].id != 0; i = (i + 1) & (kSlots - 1)) {
        const std::size_t want = home(table_[i].id);
        // Moves back when its home is not between the hole (exclusive) and it (inclusive), cyclically.
        const bool stays = hole <= i ? (hole < want && want <= i) : (hole < want || want <= i);
        if (!stays) {
            table_[hole] = table_[i];
            table_[i] = Track{};
            hole = i;
        }
    }
    --count_;
}

auto Animator::find_or_add(Id id, bool& added) noexcept -> Track* {
    id = key(id);
    added = false;
    Track* t = slot(id);
    if (t != nullptr && t->id == id) {
        return t;
    }
    if (count_ >= kCapacity) { // full: the longest unasked-for one makes room
        std::size_t oldest = kSlots;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (table_[i].id != 0 && (oldest == kSlots || table_[i].seen < table_[oldest].seen)) {
                oldest = i;
            }
        }
        remove(oldest);
        t = slot(id);
    }
    t->id = id;
    ++count_;
    added = true;
    return t;
}

void Animator::advance(Track& t) const {
    if (!t.moving) {
        t.time = now_;
        return;
    }
    motion::Spring s{.x = t.value, .v = t.velocity, .target = t.target, .tok = t.token, .range = t.range};
    s.step(static_cast<float>(now_ - t.time)); // at most a frame's limit: after a pause it goes on
    t.value = s.x;
    t.velocity = s.v;
    if (s.x == s.target && s.v == 0.0f) {
        t.moving = false;
    }
    t.time = now_;
}

auto Animator::to(Id id, float target, motion::MotionToken token, std::optional<float> from) -> float {
    bool added = false;
    Track& t = *find_or_add(id, added);
    t.seen = now_;
    t.seen_frame = frame_;
    if (added) {
        t.value = from.value_or(target);
        t.velocity = 0;
        t.target = target;
        t.token = token;
        t.time = now_;
        t.moving = t.value != target;
        t.range = std::max(1.0f, std::abs(target - t.value));
        active_ = active_ || t.moving;
        return t.value;
    }
    t.token = token;
    advance(t);
    if (target != t.target) { // a new target: from where it is, at the speed it has
        t.target = target;
        t.moving = true;
        t.range = std::max(1.0f, std::abs(target - t.value)); // it rests within 0.1 % of this way
    }
    active_ = active_ || t.moving;
    return t.value;
}

auto Animator::geometry(Id id, float target, motion::MotionToken token, std::optional<float> from) -> float {
    if (!reduced_) {
        return to(id, target, token, from);
    }
    bool added = false;
    Track& t = *find_or_add(id, added);
    t.seen = now_;
    t.seen_frame = frame_;
    if (added) {
        t.start = now_;
    }
    t.value = t.target = target; // there at once, at rest
    t.velocity = 0;
    t.token = token;
    t.time = now_;
    t.moving = false;
    t.range = 1.0f;
    return target;
}

void Animator::place(Id id, float value) noexcept {
    bool added = false;
    Track& t = *find_or_add(id, added);
    t.seen = now_;
    t.seen_frame = frame_;
    t.value = t.target = value;
    t.velocity = 0;
    t.start = t.time = now_;
    t.moving = false;
}

auto Animator::since(Id id) noexcept -> double {
    bool added = false;
    Track& t = *find_or_add(id, added);
    t.seen = now_;
    t.seen_frame = frame_;
    if (added) {
        t.start = now_; // born
        t.moving = false;
    }
    return now_ - t.start;
}

void Animator::restart(Id id) noexcept {
    bool added = false;
    Track& t = *find_or_add(id, added);
    t.seen = now_;
    t.seen_frame = frame_;
    t.start = now_;
    t.moving = false;
}

void Animator::end_frame() {
    for (std::size_t i = 0; i < kSlots;) {
        // Forgotten when not asked for a second and for kKeepFrames frames: a pause with no frames
        // (the handle at rest draws none) is not a second unasked — its next change goes on from there.
        if (table_[i].id != 0 && now_ - table_[i].seen > kKeep && frame_ - table_[i].seen_frame > kKeepFrames) {
            remove(i); // a later slot may move into i: look at it again
        } else {
            ++i;
        }
    }
}

} // namespace ui::anim
