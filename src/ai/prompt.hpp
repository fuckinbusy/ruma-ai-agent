#pragma once

// The system prompt (05-AI-AGENT § The system prompt): src/ai/prompts/system.md, built into the
// program. It is the first, unchanging part of every request, so the API's cache keeps it.

#include <string>
#include <string_view>

namespace ai {

// The prompt with the user's folders in it (`profile_paths`: lines "Desktop: C:\Users\…").
// With the user's rules (agent.md) and what is remembered about them ("- text (date)" lines) as
// sections of their own at its end (phase 12 § 6); either empty: no section.
auto system_prompt(std::string_view profile_paths, std::string_view rules = {}, std::string_view memory = {}) -> std::string;

namespace detail {
// prompts/system.md as it was at build time, with "{{paths}}" where the folders go
// (prompt_text.cpp, made by src/ai/CMakeLists.txt).
extern const std::string_view kSystemPrompt;
} // namespace detail

} // namespace ai
