#pragma once

// ui::anim — the values of immediate-mode widgets. Each frame a widget asks the Animator "the
// value of id heading to target"; a changed target goes on from wherever the value is, at the
// speed it has. Every value is a ui::motion::Spring on a token of ui/motion_tokens.hpp — there is
// no other kind of motion. While any value moves, active() asks the frame scheduler for another
// frame.

#include "ui/motion_tokens.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ui::anim {

using Id = std::uint64_t;

// The values of immediate-mode widgets, by id (motion rework, task 2): each a ui::motion::Spring on
// a token. A track lives a second and 60 frames after it was last asked for (a pause with no frames
// forgets nothing) — one frame without it (a branch
// drawn every other frame) neither forgets nor restarts it. Tracks sit in one table made at
// construction: asking for new ids while scrolling allocates nothing.
class Animator {
public:
    // Tracks at most; asking for more retires the longest unasked-for one.
    static constexpr std::size_t kCapacity = 4096;

    Animator();

    void begin_frame(double now_seconds);
    // The value of `id` this frame, heading to `target` on `token`. The first time `id` is asked
    // for it starts at `from` (at `target` without one); a changed target keeps value and speed.
    auto to(Id id, float target, motion::MotionToken token, std::optional<float> from = std::nullopt) -> float;
    // A size or a place: as to(), but with Windows' animation effects off (set_reduced) it is at its
    // target at once (MOTION_SPEC § 3: geometry at once; an opacity or a colour still crosses on
    // quick through to()).
    auto geometry(Id id, float target, motion::MotionToken token, std::optional<float> from = std::nullopt) -> float;
    void set_reduced(bool on) noexcept { reduced_ = on; }
    [[nodiscard]] auto reduced() const noexcept -> bool { return reduced_; }
    // Puts `id` at `value`, at rest: the next ask goes on from there (known or not). For a value that
    // was driven by something else meanwhile — a shape the window's motion moved.
    void place(Id id, float value) noexcept;
    // Seconds since `id` was first asked for (or restarted): a stateless widget's own clock for its
    // cues ("60 ms after it came", "each 30 ms apart"). It is no motion: nothing asks for frames.
    auto since(Id id) noexcept -> double;
    // `id`'s clock from now (an event the cues count from).
    void restart(Id id) noexcept;
    // Forgets values not asked for in the last second.
    void end_frame();

    // True when a value asked for in the last frame is still moving.
    [[nodiscard]] auto active() const noexcept -> bool { return active_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return count_; }

private:
    struct Track {
        Id id = 0;            // 0: an empty slot
        float target = 0;
        float value = 0;
        float velocity = 0;
        double start = 0;     // since()'s zero: first asked for, or restarted
        double time = 0;      // when value was last advanced
        double seen = 0;      // when it was last asked for
        std::uint64_t seen_frame = 0; // in which frame
        float range = 1;      // a token track's way (at least 1): it rests within 0.1 % of it (MS § 4)
        motion::MotionToken token;
        bool moving = false;
    };
    // Its slot, or an empty one where it would go (nullptr: the table is full).
    auto slot(Id id) noexcept -> Track*;
    auto find_or_add(Id id, bool& added) noexcept -> Track*;
    void remove(std::size_t index) noexcept;
    void advance(Track& track) const;

    std::vector<Track> table_; // 2 × kCapacity slots, open addressing, made once
    std::size_t count_ = 0;
    double now_ = 0;
    std::uint64_t frame_ = 0; // frames begun: a pause with none forgets nothing
    bool active_ = false;
    bool reduced_ = false; // Windows' animation effects off: geometry() at once
};

} // namespace ui::anim
