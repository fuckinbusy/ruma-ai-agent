#pragma once

// app::terminal — the detailed mode's terminal (design/interaction-topbar 7_chat_terminal): what
// the agent's processes print, kept as lines (the last kMaxLines a tab), each inked when it came
// as the mockup inks PowerShell (.ps prompt, .k commands, .s strings and paths, .d parameters,
// operators and comments, .w warnings, .c values) — so a frame only draws.

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace app::terminal {

enum class Ink : std::uint8_t { Plain, Prompt, Command, String, Dim, Warn, Value };

struct Token {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    Ink ink = Ink::Plain;
};

// Tokens that cover the line in order (spaces included, Plain).
auto tokenize(std::string_view line) -> std::vector<Token>;

inline constexpr std::size_t kMaxLines = 5000;

struct Line {
    std::string text;
    std::vector<Token> tokens;
    int step = -1; // the plan step whose command printed it; -1: none
};

class Buffer {
public:
    // Text as it comes: whole lines end with '\n'; a last piece without one goes on in the next
    // call. `step`: the plan step for the lines this call starts.
    void append(std::string_view text, int step = -1);
    [[nodiscard]] auto size() const noexcept -> std::size_t { return lines_.size(); }
    // 0: the oldest kept.
    [[nodiscard]] auto line(std::size_t i) const -> const Line& { return lines_[i]; }

private:
    std::deque<Line> lines_;
    bool open_ = false; // the last line has no end yet
};

} // namespace app::terminal
