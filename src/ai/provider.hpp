#pragma once

// A provider as the agent sees it: a function that starts one streamed turn. DeepSeek is one
// (ai::deepseek::provider), a recording another (ai::cassette::player), a test's script a third.

#include "ai/stream.hpp"
#include "ai/types.hpp"
#include "core/error.hpp"

#include <functional>
#include <string_view>

namespace ai {

// Called on the provider's thread (the network's, for DeepSeek): hand the pieces on, never wait.
struct StreamSink {
    std::function<void(const Delta&)> on_delta;
    std::function<void(core::Result<void>)> on_done; // exactly once
};

// A started turn: cancel() from any thread ends it with on_done(Cancelled); after on_done, nothing.
struct Stream {
    std::function<void()> cancel;
};

using Provider = std::function<Stream(const ChatRequest&, StreamSink)>;

// What a provider sent and got on the wire, for a recording (ai::cassette): the request's path and
// body, each event's data as it came, and at the end the status (0: no answer) with a non-2xx
// body. `request` runs on the caller's thread, the others on the network's.
struct Tap {
    std::function<void(std::string_view path, std::string_view body)> request;
    std::function<void(std::string_view data)> event;
    std::function<void(int status, std::string_view error_body)> answer;
};

} // namespace ai
