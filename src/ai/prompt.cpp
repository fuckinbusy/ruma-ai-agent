#include "ai/prompt.hpp"

namespace ai {

auto system_prompt(std::string_view profile_paths, std::string_view rules, std::string_view memory) -> std::string {
    constexpr std::string_view slot = "{{paths}}";
    std::string text{detail::kSystemPrompt};
    if (const auto at = text.find(slot); at != std::string::npos) {
        text.replace(at, slot.size(), profile_paths);
    }
    // Last: the prompt before them stays the same, DeepSeek's cached prefix with it.
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    if (!rules.empty()) {
        text += "\n\nThe user's rules (their file agent.md; follow them):\n";
        text += rules;
    }
    if (!memory.empty()) {
        text += "\n\nWhat you know about the user (memory.add; memory.forget when it is no longer true):\n";
        text += memory;
    }
    if (text.back() != '\n') {
        text += '\n';
    }
    return text;
}

} // namespace ai
