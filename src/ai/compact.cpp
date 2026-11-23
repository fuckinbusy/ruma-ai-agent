#include "ai/compact.hpp"

#include <algorithm>

namespace ai::compact {
namespace {

constexpr std::size_t kResultShown = 2000; // of a tool's result, in what is summarized

constexpr std::string_view kInstructions =
    "Summarize the conversation below so the assistant can go on with it without seeing it: what the user asked, what "
    "was done and what came of it, the files, folders and paths involved (exactly), the decisions made, and what is not "
    "finished. Plain sentences and short lists; keep the user's language for their words. No preamble.";

auto said(Role role, std::string content) -> Message {
    return Message{.role = role, .content = std::move(content), .reasoning = {}, .tool_calls = {}, .tool_call_id = {}};
}

// `s` at most `n` bytes, not in the middle of a character, marked when cut.
auto shortened(std::string_view s, std::size_t n) -> std::string {
    if (s.size() <= n) {
        return std::string{s};
    }
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    return std::string{s.substr(0, n)} + " [...]";
}

} // namespace

auto is_summary(const Message& m) -> bool {
    return m.role == Role::User && m.content.starts_with(kMark);
}

auto summary_message(std::string summary) -> Message {
    return said(Role::User, std::string{kMark} + "\n" + std::move(summary));
}

auto split(const std::vector<Message>& history, std::size_t keep_requests) -> std::size_t {
    std::vector<std::size_t> requests; // the user's own messages: not after a tool's results, not a summary
    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == Role::User && !is_summary(m) && (i == 0 || history[i - 1].role != Role::Tool)) {
            requests.push_back(i);
        }
    }
    if (keep_requests == 0 || requests.size() <= keep_requests) {
        return 0;
    }
    const std::size_t at = requests[requests.size() - keep_requests];
    const bool only_summaries = std::all_of(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(at), is_summary);
    return only_summaries ? 0 : at;
}

auto summary_request(std::span<const Message> old, const catalog::Model& by) -> ChatRequest {
    // A tool's name by its call's id: "Tool fs.list: ...".
    const auto name_of = [&](std::string_view id) -> std::string {
        for (const auto& m : old) {
            for (const auto& c : m.tool_calls) {
                if (c.id == id) {
                    return c.name;
                }
            }
        }
        return "a tool";
    };
    std::string text;
    for (const auto& m : old) {
        if (is_summary(m)) {
            text += "Earlier summary: " + m.content.substr(std::min(m.content.size(), kMark.size() + 1)) + "\n\n";
            continue;
        }
        switch (m.role) {
        case Role::User: text += "User: " + m.content; break;
        case Role::Assistant:
            text += "Assistant: " + m.content;
            for (const auto& c : m.tool_calls) {
                text += " [calls " + c.name + " " + shortened(c.arguments, 300) + "]";
            }
            break;
        case Role::Tool: text += "Tool " + name_of(m.tool_call_id) + ": " + shortened(m.content, kResultShown); break;
        case Role::System: continue;
        }
        for (const auto& image : m.images) {
            text += " [image: " + image.label + "]";
        }
        text += "\n\n";
    }
    ChatRequest r;
    r.model = std::string{by.id};
    r.thinking = catalog::nearest_level(by, Thinking::Off); // no thinking, or the least it has
    r.messages.push_back(said(Role::System, std::string{kInstructions}));
    r.messages.push_back(said(Role::User, std::move(text)));
    return r;
}

} // namespace ai::compact
