#pragma once

// app::scenes::Stream — the answer's smoother (motion rework, task 11;
// design/ui_motion/MOTION_SPEC.md § 6 "Streaming", scene 07). Tokens come in bursts; the text comes
// out evenly: rate = clamp(backlog · kRatePerBacklog, kRateMin, kRateMax) characters a second, and
// only whole words — an unfinished word waits for its space (it may still grow and take another
// line). A script without spaces (CJK) flows character by character; a word longer than kLongWord
// bytes is not held. It owns no text: the caller gives what has arrived each frame.

#include <cstddef>
#include <string_view>

namespace app::scenes {

class Stream {
public:
    // A word this long with no end in sight (a link, a hash) flows anyway.
    static constexpr std::size_t kLongWord = 64;

    // `text`: all that has arrived (UTF-8); `ended`: nothing more comes. Advances `dt` seconds (a
    // frame's limit at most). A text shorter than what was shown is another answer: from its start.
    void step(std::string_view text, bool ended, float dt) noexcept;
    // All of `text` shown at once (an answer that was there before it was first drawn).
    void show_all(std::string_view text) noexcept;
    void reset() noexcept;

    // Bytes shown: on a character's boundary, at a word's end unless the text ended.
    [[nodiscard]] auto shown() const noexcept -> std::size_t { return shown_; }
    // More may come out without anything new arriving.
    [[nodiscard]] auto flowing() const noexcept -> bool { return flowing_; }

private:
    std::size_t shown_ = 0;
    double credit_ = 0; // characters earned, not yet shown
    bool flowing_ = false;
};

} // namespace app::scenes
