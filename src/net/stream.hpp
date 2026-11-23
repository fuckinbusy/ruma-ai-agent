#pragma once

// net — a request whose answer is a text/event-stream, told as SSE events (phase 7 reads
// DeepSeek's chat completions this way). The callbacks run on the network thread, as Sink's do.

#include "core/error.hpp"
#include "net/http.hpp"
#include "net/net.hpp"
#include "net/sse.hpp"

#include <functional>

namespace net {

struct EventSink {
    std::function<void(const Response&)> on_headers;    // once, before the events
    std::function<void(const sse::Event&)> on_event;    // 0..N times; ": keep-alive" is not one
    std::function<void(core::Result<Response>)> on_done; // exactly once
};

// start() with the body read by sse::Parser (an event of 8 MB at most). A 4xx or 5xx comes to
// on_done as start()'s does, its body in error_body. Errors besides start()'s: Parse — a 2xx that
// is not text/event-stream (the message has the start of its body), or an event past the limit
// (the request is cancelled at once).
auto start_events(State& state, Request request, EventSink sink) -> RequestId;

} // namespace net
