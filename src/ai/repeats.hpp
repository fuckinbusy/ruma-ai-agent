#pragma once

// ai::Repeats — the loop guard's count (phase 12 § 9): the model calling the same tool with the
// same arguments three times running, or two calls in turn three times over (2026-10-16: a
// screenshot, a page opened, a screenshot...), is warned; once more, the task asks "Go on?".

#include <string>
#include <string_view>
#include <vector>

namespace ai {

class Repeats {
public:
    // A call the model made: 0 — no repeat to speak of; 3 — the third time (once per streak):
    // warn; 4 — once more: stop and ask. Arguments count as the same JSON however written.
    auto note(std::string_view tool, std::string_view args) -> int;
    // What repeats, for the warning: "fs.list with the same arguments", "a and b in turn".
    [[nodiscard]] auto what() const -> std::string;
    // Counted afresh (the user said go on).
    void reset();

private:
    struct Call {
        std::string tool;
        std::string args; // canonical
        auto operator==(const Call&) const -> bool = default;
    };
    [[nodiscard]] auto copies(std::size_t period) const -> std::size_t;

    std::vector<Call> calls_;
    std::size_t period_ = 1; // of the longest streak at the last call
    bool warned_ = false;
};

} // namespace ai
