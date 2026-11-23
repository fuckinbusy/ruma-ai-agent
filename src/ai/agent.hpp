#pragma once

// ai::agent — one request of the user, worked through with the model (05-AI-AGENT § The agent loop):
// a turn of the model; its tool calls run one after another through the caller's `execute` (which
// asks the user itself); their results back to the model; again — until the model answers, a
// limit asks "go on?", the network fails or the user stops it. All on the caller's thread: the
// provider's pieces come through a queue, the events are called from here.

#include "ai/catalog.hpp"
#include "ai/provider.hpp"
#include "ai/types.hpp"
#include "core/error.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ai::agent {

struct Limits {
    int turns = 25;                       // the model's turns before "go on?"
    double budget_usd = 0.10;             // the task's spending before "go on?"
    std::size_t result_bytes = 64 * 1024; // one tool result, as the model gets it
};

// A tool's result as the model gets it.
struct Result {
    std::string content;
    bool ok = true;
    std::vector<Image> images; // pictures it gives the model: a user message after the turn's results
};

// What "go on?" is about; Repeat (phase 12 § 9): the model went on repeating itself after a warning.
enum class Ask : std::uint8_t { Budget, Turns, Repeat };

// What the model is told when the user presses "Answer now" (phase 12 § 9).
inline constexpr std::string_view kAnswerNow = "Answer now from what you have; no more tools.";

// Called on the caller's thread.
struct Events {
    std::function<void()> on_turn; // a turn begins (before its request goes out)
    std::function<void(std::string_view)> on_reasoning, on_text;
    std::function<void(const ToolCall&)> on_tool_started;
    std::function<void(const ToolCall&, const Result&)> on_tool_finished;
    std::function<void(const Usage&, double usd_total)> on_usage; // after each turn
    std::function<bool(Ask, double usd_total)> ask_continue;       // waits for the user; false: stop
    // A request with a picture went to this model: the chosen one does not see pictures.
    std::function<void(const catalog::Model&)> on_vision_model;
    std::function<void(std::string_view what)> on_repeat; // the model was told it repeats itself
    // Phase 12 § 10: the last turn's prompt as a percent of Limits::history_tokens, after each
    // turn; the old part of the history summarized.
    std::function<void(int percent)> on_context;
    std::function<void()> on_compacted;
};

struct Setup {
    Provider provider;
    std::span<const ToolSpec> tools;
    std::string system; // the system prompt: first in every request, not kept in the history
    // When set, the system prompt asked anew for each request instead (phase 12 § 6: the user's
    // rules and memory change between requests).
    std::function<std::string()> system_of;
    // What answers (never null in a run: without it the run fails at once), what answers a request
    // with a picture when `model` does not see them (null: none), and what writes the summary of
    // an old history (null: `model`). Each request's limit and price are those of the model it
    // goes to.
    const catalog::Model* model = nullptr;
    const catalog::Model* vision_model = nullptr;
    const catalog::Model* summary_model = nullptr;
    Thinking thinking = Thinking::Low;
    Limits limits;
    std::function<Result(const ToolCall&)> execute;          // a known tool with arguments that parse
    std::function<std::optional<std::string>()> take_refine; // what the user typed while it worked
    std::function<std::chrono::system_clock::time_point()> now; // for the price
    // Added to the end of the user's request (and kept there): what changes from one request to
    // the next, such as the local time — after the prompt, so the prompt stays cached.
    std::function<std::string()> context;
    // "Answer now" (phase 12 § 9), raised from another thread: the turn under way is cut, not kept,
    // and the next request asks for the answer (kAnswerNow) with thinking off. Taken (lowered) here;
    // the caller lowers it before a run (a press may come before the run's first request).
    std::atomic<bool>* answer_now = nullptr;
    Events events;
};

enum class End : std::uint8_t { Done, Cancelled, Failed, Stopped };

struct Outcome {
    End end = End::Done;
    std::string text; // the answer, or what came of it before a failure
    core::Error error;
    Usage usage; // all turns together
    double usd = 0;
};

// One request in the conversation `history` (the user's request, the model's turns and the tools'
// results are added to it; a turn that failed half-way is not).
auto run(std::vector<Message>& history, std::string_view request, const Setup& s, std::stop_token stop) -> Outcome;
// The same with the user's own pictures (pasted or dropped) in the request.
auto run(std::vector<Message>& history, std::string_view request, std::vector<Image> images, const Setup& s, std::stop_token stop)
    -> Outcome;

// Past `most` messages with pictures, the oldest lose theirs (a line says what was there) until
// `keep` are left: in a batch, so DeepSeek's cached prefix breaks once in a while, not on every
// picture. Returns how many lost them; (0, 0) drops them all (a follow-up on a model without vision).
auto thin_images(std::vector<Message>& history, std::size_t most, std::size_t keep) -> std::size_t;

// Goes on from `history` as it is, adding no message of the user's: Retry after a turn failed
// (that turn was not kept, so the history ends with the request, a refine or a tool's result).
auto resume(std::vector<Message>& history, const Setup& s, std::stop_token stop) -> Outcome;

// A rough count, for the history's limit: bytes / 4 for ASCII, / 3.5 for the rest (Cyrillic).
auto estimate_tokens(std::string_view text) -> std::size_t;

} // namespace ai::agent
