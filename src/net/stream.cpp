#include "net/stream.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace net {

namespace {

constexpr RequestId kFailed = std::numeric_limits<RequestId>::max();
constexpr std::size_t kShownBody = 200; // of a body that is not an event stream, in the error

// One stream, shared by its three callbacks (all on the network thread) and start_events.
struct Stream {
    State& state;
    EventSink sink;
    sse::Parser parser{};
    bool events = false;     // a 2xx text/event-stream
    std::string content_type; // for the error when it is not one
    std::optional<core::Error> failure;
    // start()'s id once start_events has it, or kFailed once a callback wanted the request
    // cancelled before that: whichever of the two comes second cancels.
    std::atomic<RequestId> id{0};

    void fail(core::Error e) {
        if (failure) {
            return;
        }
        failure = std::move(e);
        if (const RequestId known = id.exchange(kFailed); known != 0) {
            cancel(state, known);
        }
    }
};

auto success(int status) -> bool {
    return status >= 200 && status <= 299;
}

auto header(const Response& r, std::string_view name) -> std::string_view {
    for (const auto& [n, value] : r.headers) {
        if (n.size() == name.size() && _strnicmp(n.c_str(), name.data(), name.size()) == 0) {
            return value;
        }
    }
    return {};
}

auto is_event_stream(std::string_view content_type) -> bool {
    constexpr std::string_view type = "text/event-stream";
    return content_type.size() >= type.size() && _strnicmp(content_type.data(), type.data(), type.size()) == 0;
}

auto not_events(const Stream& s, std::string_view body) -> core::Error {
    const std::string type = s.content_type.empty() ? std::string{"none"} : s.content_type;
    return core::make_error(core::Code::Parse, std::format("not an event stream (Content-Type: {}): {}", type,
                                                           body.substr(0, kShownBody)))
        .error();
}

} // namespace

auto start_events(State& state, Request request, EventSink sink) -> RequestId {
    auto s = std::make_shared<Stream>(state, std::move(sink));
    Sink inner{.on_headers =
                   [s](const Response& r) {
                       s->content_type = std::string{header(r, "Content-Type")};
                       s->events = success(r.status) && is_event_stream(s->content_type);
                       // A 2xx of another kind is an error at on_done, not an answer.
                       if ((s->events || !success(r.status)) && s->sink.on_headers) {
                           s->sink.on_headers(r);
                       }
                   },
               .on_data =
                   [s](std::span<const std::byte> bytes) {
                       // After a failure, until the cancel lands: fail() keeps the first error and
                       // the parser stays failed, so the pieces still in flight tell nothing.
                       if (!s->events) {
                           s->fail(not_events(*s, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}));
                           return;
                       }
                       auto fed = s->parser.feed(bytes, [&](const sse::Event& e) {
                           if (s->sink.on_event) {
                               s->sink.on_event(e);
                           }
                       });
                       if (!fed) {
                           s->fail(std::move(fed.error()));
                       }
                   },
               .on_done =
                   [s](core::Result<Response> r) {
                       if (s->failure) {
                           r = std::unexpected{*s->failure}; // our cancel's Cancelled is this
                       } else if (r && success(r->status) && !s->events) {
                           r = std::unexpected{not_events(*s, {})}; // a 2xx with no body
                       }
                       if (s->sink.on_done) {
                           s->sink.on_done(std::move(r));
                       }
                   }};
    const RequestId id = start(state, std::move(request), std::move(inner));
    if (s->id.exchange(id) == kFailed) {
        cancel(state, id);
    }
    return id;
}

} // namespace net
