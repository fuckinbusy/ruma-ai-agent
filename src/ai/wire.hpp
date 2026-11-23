#pragma once

// ai::chat::wire — the shapes of DeepSeek's JSON, as glaze reads and writes them; private to
// ai/deepseek.cpp (glaze stays out of the module's interface). Field names are the API's.
// Glaze reflects only types with external linkage: hence a named namespace.

#include <glaze/glaze.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::chat::wire {

// --- the request ---

struct Function {
    std::string name;
    std::string arguments;
};
struct Call {
    std::string id;
    std::string type = "function";
    Function function;
};

struct Text { // system and user
    std::string role;
    glz::raw_json content; // a string, or (a user's with pictures) an array of text and image_url parts
};
struct Assistant {
    std::string role = "assistant";
    glz::raw_json content; // a string, or null when the turn is only calls (the API wants the key)
    std::optional<std::string> reasoning_content;
    std::optional<std::vector<Call>> tool_calls;
};
struct ToolResult {
    std::string role = "tool";
    std::string tool_call_id;
    std::string content;
};
using Message = std::variant<Text, Assistant, ToolResult>;

struct Thinking {
    std::string type; // "enabled" | "disabled"
};
struct StreamOptions {
    bool include_usage = true;
};
struct FunctionSpec {
    std::string name;
    std::string description;
    glz::raw_json parameters; // the JSON Schema, as an object
    bool strict = true;
};
struct Tool {
    std::string type = "function";
    FunctionSpec function;
};

struct Request {
    std::string model;
    std::vector<Message> messages;
    bool stream = true;
    StreamOptions stream_options;
    Thinking thinking;
    std::optional<std::string> reasoning_effort;
    std::optional<int> max_tokens;
    std::optional<std::vector<Tool>> tools;
};

// --- the stream: one chunk (only what we read; the rest of the JSON is skipped) ---

struct FunctionPiece {
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};
struct CallPiece {
    int index = 0;
    std::optional<std::string> id;
    std::optional<FunctionPiece> function;
};
struct DeltaIn {
    std::optional<std::string> content;
    std::optional<std::string> reasoning_content;
    std::optional<std::vector<CallPiece>> tool_calls;
};
struct Choice {
    std::optional<DeltaIn> delta;
    std::optional<std::string> finish_reason;
};
struct CompletionDetails {
    int reasoning_tokens = 0;
};
struct UsageIn {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int prompt_cache_hit_tokens = 0;
    int prompt_cache_miss_tokens = 0;
    std::optional<CompletionDetails> completion_tokens_details;
};
struct ChunkIn {
    std::vector<Choice> choices;
    std::optional<UsageIn> usage;
};

// --- an error's body ---

struct ErrorDetail {
    std::string message;
};
struct ErrorBody {
    std::optional<ErrorDetail> error;
};

} // namespace ai::chat::wire
