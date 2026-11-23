#pragma once

// net — HTTP requests on the network thread (01-ARCHITECTURE § net). The receiver's Sink is
// called on that thread and must not wait: it hands what it gets to its own queue.

#include "core/error.hpp"
#include "net/net.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{10'000};
    // No byte at all for this long (SSE comments count): Timeout. A model thinks long: no limit
    // on the whole request.
    std::chrono::milliseconds idle_timeout{120'000};
    int retries = 3; // tries after the first; see backoff
};

// A status is data, not an error: a 4xx or 5xx comes in on_done as a Response, its body (up to
// 64 KB) in error_body, and the receiver tells 401 from 429. Errors are failures to deliver.
struct Response {
    int status = 0;
    Headers headers;
    std::string error_body;
    int http_version = 0; // 10, 11, 20 or 30: what the answer came over
};

struct Sink {
    std::function<void(const Response&)> on_headers;           // once, before the body
    std::function<void(std::span<const std::byte>)> on_data;     // 0..N times, a 2xx body
    std::function<void(core::Result<Response>)> on_done;         // exactly once
};

using RequestId = std::uint64_t;

// From any thread. Errors in on_done: Cancelled, Timeout, Net (DNS, connection, TLS — with the
// proxy's address when one was used).
auto start(State& state, Request request, Sink sink) -> RequestId;
// From any thread; an id that is done or unknown: nothing.
void cancel(State& state, RequestId id);

// The wait before try `attempt` + 1 (attempt 1 is the first retry): 500 ms doubling, or the
// server's Retry-After, never more than 30 s. A retry comes for 429, 5xx and a failed
// connection, TLS, send or receive with no status yet; an answer that began (its headers) and
// broke or went silent is not tried again. The Sink hears only the try that counts.
auto backoff(int attempt, std::optional<std::chrono::seconds> retry_after) -> std::chrono::milliseconds;

// How many times the network thread woke up (the idle test: without requests it sleeps).
[[nodiscard]] auto wakeups(const State& state) -> std::uint64_t;

} // namespace net
