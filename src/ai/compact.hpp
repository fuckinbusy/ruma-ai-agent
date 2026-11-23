#pragma once

// ai::compact — a long conversation summarized (phase 12 § 10): past 60 % of the history's limit
// the old part — all but the user's last 4 requests with their turns — becomes one summary,
// written by a separate request to deepseek-flash without reasoning. Only what goes to the
// model: the task journal and the history on disk keep it all.

#include "ai/catalog.hpp"
#include "ai/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ai::compact {

inline constexpr double kAt = 0.6; // of Limits::history_tokens, by the last turn's prompt
inline constexpr std::string_view kMark = "[Summary of the earlier conversation]";

// Where the part kept begins: the user's `keep_requests`-th request from the end (a message the
// user wrote, not the pictures after a tool's results), so a call never parts from its result.
// 0: nothing old (fewer requests, or only a summary before them).
[[nodiscard]] auto split(const std::vector<Message>& history, std::size_t keep_requests = 4) -> std::size_t;
// The request that writes the summary of `old`: flash, no thinking, no tools; pictures as words,
// a summary before as text.
[[nodiscard]] auto summary_request(std::span<const Message> old, const catalog::Model& by) -> ChatRequest;
// The summary as it stands in the history (the user's role, kMark first).
[[nodiscard]] auto summary_message(std::string summary) -> Message;
[[nodiscard]] auto is_summary(const Message& m) -> bool;

} // namespace ai::compact
