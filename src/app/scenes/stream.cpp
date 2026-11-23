#include "app/scenes/stream.hpp"

#include "ui/motion_tokens.hpp"

#include <algorithm>

namespace app::scenes {
namespace {

namespace m = ui::motion;

auto is_lead(char c) noexcept -> bool {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
}

// The start of the character that ends at `end` (or holds the byte before it).
auto char_start(std::string_view text, std::size_t end) noexcept -> std::size_t {
    std::size_t i = end;
    while (i > 0 && !is_lead(text[i - 1])) {
        --i;
    }
    return i > 0 ? i - 1 : 0;
}

// A character after which a word has ended: a space, a line end — or the character itself is a
// word (CJK and its neighbours from U+2E80: three bytes and more from 0xE2 0xBA 0x80).
auto ends_word(std::string_view text, std::size_t start) noexcept -> bool {
    const auto c = static_cast<unsigned char>(text[start]);
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        return true;
    }
    if (c >= 0xF0) {
        return true; // four bytes: emoji, rare CJK
    }
    if (c >= 0xE3) {
        return c != 0xEF || start + 1 >= text.size() || static_cast<unsigned char>(text[start + 1]) < 0xBB; // not U+FB00…: forms, BOM
    }
    if (c == 0xE2 && start + 1 < text.size()) {
        return static_cast<unsigned char>(text[start + 1]) >= 0xBA; // U+2E80 and up
    }
    return false;
}

// How far the text may be shown: its whole characters, up to the end of its last whole word.
auto available(std::string_view text, bool ended, std::size_t shown) noexcept -> std::size_t {
    std::size_t end = text.size();
    if (end > 0) { // the last character whole? (an arrival may cut one: its bytes so far wait)
        const std::size_t start = char_start(text, end);
        const auto c = static_cast<unsigned char>(text[start]);
        const std::size_t need = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        end = end - start >= need ? end : start;
    }
    if (ended) {
        return end;
    }
    std::size_t at = end;
    while (at > shown) {
        const std::size_t start = char_start(text, at);
        if (ends_word(text, start)) {
            return at;
        }
        if (end - start >= Stream::kLongWord) {
            return end; // no end in sight: it flows
        }
        at = start;
    }
    return shown;
}

} // namespace

void Stream::reset() noexcept {
    shown_ = 0;
    credit_ = 0;
    flowing_ = false;
}

void Stream::show_all(std::string_view text) noexcept {
    shown_ = available(text, true, 0);
    credit_ = 0;
    flowing_ = false;
}

void Stream::step(std::string_view text, bool ended, float dt) noexcept {
    if (text.size() < shown_) {
        reset(); // another answer
    }
    const std::size_t avail = std::max(shown_, available(text, ended, shown_));
    std::size_t backlog = 0; // characters
    for (std::size_t i = shown_; i < avail; ++i) {
        backlog += is_lead(text[i]) ? 1 : 0;
    }
    if (backlog == 0) {
        credit_ = 0;
        flowing_ = false;
        return;
    }
    const float rate = std::clamp(static_cast<float>(backlog) * m::stream::kRatePerBacklog, m::stream::kRateMin, m::stream::kRateMax);
    credit_ += static_cast<double>(rate) * std::clamp(dt, 0.0f, m::kMaxFrameDt);
    while (credit_ >= 1.0 && shown_ < avail) {
        ++shown_;
        while (shown_ < avail && !is_lead(text[shown_])) {
            ++shown_;
        }
        credit_ -= 1.0;
    }
    flowing_ = shown_ < avail;
    if (!flowing_) {
        credit_ = 0;
    }
}

} // namespace app::scenes
