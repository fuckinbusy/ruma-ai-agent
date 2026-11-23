#pragma once

// net::sse — text/event-stream, incrementally (the WHATWG HTML standard, § server-sent events):
// bytes in whatever pieces the network gives, events out when an empty line ends them. The model's
// stream (05-AI-AGENT) is one: "data: {json}" events, ": keep-alive" comments, "data: [DONE]".

#include "core/error.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace net::sse {

struct Event {
    std::string event; // "message" unless an "event:" field said otherwise
    std::string data;  // the "data:" lines joined by "\n"
    std::string id;    // the last "id:" seen on this stream (it carries over to later events)
};

class Parser {
public:
    // `max_event`: the most one event (with its unfinished line) may hold; past it the stream is
    // an error (core::Code::Parse) and stays one — memory never grows without a bound.
    explicit Parser(std::size_t max_event = std::size_t{8} << 20) : max_event_(max_event) {}

    // Lines end at "\r\n", "\n" or "\r" — a "\r" at a piece's end and a "\n" at the next one's
    // start are one end. Comments (":...") and events without data give nothing.
    auto feed(std::span<const std::byte> bytes, const std::function<void(const Event&)>& on_event) -> core::Result<void>;

private:
    void line(std::string_view text, const std::function<void(const Event&)>& on_event);

    std::size_t max_event_;
    std::string line_;          // the line so far
    std::string data_;          // the event's data so far ("\n" after each line)
    std::string event_;         // its "event:" field
    std::string id_;            // the last event id
    bool have_data_ = false;
    bool after_cr_ = false;     // the last byte was "\r": a "\n" next ends nothing more
    bool start_ = true;         // nothing fed yet: a BOM may come
    std::size_t bom_seen_ = 0;  // bytes of a BOM matched so far
    bool failed_ = false;
};

} // namespace net::sse
