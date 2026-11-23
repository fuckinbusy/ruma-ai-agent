#pragma once

// ai::types — the agent's words, not the provider's (05-AI-AGENT § The data model): a provider's
// wire format lives in its own namespace, so another one would not touch the agent.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ai {

enum class Role : std::uint8_t { System, User, Assistant, Tool };

// A call the model asked for; `name` is ours ("fs.list"), `arguments` a JSON object as text.
struct ToolCall {
    std::string id, name, arguments;
};

// A picture for the model (phase 9): its encoded bytes (PNG or JPEG), not base64; `label` says
// what it is in the history ("photo.jpg", "screen").
struct Image {
    std::string mime;
    std::string data;
    std::string label;
};

struct Message {
    Role role = Role::User;
    std::string content;
    std::string reasoning; // Assistant in thinking mode: sent back while tools are in the request
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id; // Tool: the call it answers
    std::vector<Image> images; // User only: pictures are taken in user messages alone
};

// A tool as the model sees it: `schema` is a strict JSON Schema (ai::schema), as text.
struct ToolSpec {
    std::string name, description, schema;
};

enum class Thinking : std::uint8_t { Off, Low, High, Max };

struct ChatRequest {
    std::string model; // a catalog model's id: whoever asks names it
    Thinking thinking = Thinking::Low;
    std::vector<Message> messages;
    std::span<const ToolSpec> tools; // their order is part of the cached prefix: keep it stable
    int max_tokens = 0;              // 0: the API's default
};

struct Usage {
    int prompt = 0, completion = 0, cache_hit = 0, cache_miss = 0, reasoning = 0;
};

enum class Finish : std::uint8_t { None, Stop, Length, ToolCalls, ContentFilter, NoResources, Aborted };

} // namespace ai
