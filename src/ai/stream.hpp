#pragma once

// One turn of the model as it streams: the pieces a provider parses (Delta), put together into
// the assistant's message (Assembler) — text, reasoning and tool calls by their index.

#include "ai/types.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai {

// One piece of a turn. A tool call's piece names its call by `tool_index`; its id and name come
// once, its arguments in pieces. `tool_name` is ours ("fs.list").
struct Delta {
    std::string content, reasoning;
    int tool_index = -1;
    std::string tool_id, tool_name, tool_arguments;
    Finish finish = Finish::None;
    std::optional<Usage> usage;
};

class Assembler {
public:
    void add(const Delta& d);
    // The assistant's message so far; its tool calls in index order.
    [[nodiscard]] auto message() const -> const Message& { return message_; }
    [[nodiscard]] auto finish() const -> Finish { return finish_; }
    [[nodiscard]] auto usage() const -> const std::optional<Usage>& { return usage_; }

private:
    Message message_{.role = Role::Assistant, .content = {}, .reasoning = {}, .tool_calls = {}, .tool_call_id = {}};
    std::vector<int> indexes_; // tool_calls[i]'s index in the stream
    Finish finish_ = Finish::None;
    std::optional<Usage> usage_;
};

} // namespace ai
