#include "ai/chat.hpp"

#include "ai/wire.hpp"
#include "core/base64.hpp"
#include "core/json.hpp"
#include "core/log.hpp"

#include "net/stream.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <memory>

namespace ai::chat {

namespace {

auto quoted(const std::string& text) -> std::string {
    return core::json::write(text).value_or(std::string{"\"\""});
}

auto effort(Thinking t) -> std::optional<std::string> {
    switch (t) {
    case Thinking::Off: return std::nullopt;
    case Thinking::Low: return "low";
    case Thinking::High: return "high";
    case Thinking::Max: return "max";
    }
    return std::nullopt;
}

// A user's words with pictures: [{"type":"text",…},{"type":"image_url","image_url":{"url":"data:…"}}]
// (api-docs.deepseek.com/guides/vision); without pictures, the plain string.
auto user_content(const Message& m) -> std::string {
    if (m.images.empty()) {
        return quoted(m.content);
    }
    std::string out = R"([{"type":"text","text":)" + quoted(m.content) + "}";
    for (const auto& image : m.images) {
        out += R"(,{"type":"image_url","image_url":{"url":"data:)";
        out += image.mime;
        out += ";base64,";
        out += core::base64(std::as_bytes(std::span{image.data.data(), image.data.size()}));
        out += "\"}}";
    }
    out += ']';
    return out;
}

auto wire_message(const Message& m, bool with_reasoning) -> wire::Message {
    switch (m.role) {
    case Role::System: return wire::Text{.role = "system", .content = glz::raw_json{quoted(m.content)}};
    case Role::User: return wire::Text{.role = "user", .content = glz::raw_json{user_content(m)}};
    case Role::Tool: return wire::ToolResult{.role = "tool", .tool_call_id = m.tool_call_id, .content = m.content};
    case Role::Assistant: break;
    }
    wire::Assistant a;
    a.content.str = m.content.empty() && !m.tool_calls.empty() ? std::string{"null"} : quoted(m.content);
    if (with_reasoning && !m.reasoning.empty()) {
        a.reasoning_content = m.reasoning;
    }
    if (!m.tool_calls.empty()) {
        auto& calls = a.tool_calls.emplace();
        for (const auto& c : m.tool_calls) {
            calls.push_back(wire::Call{.id = c.id, .type = "function", .function = {.name = wire_name(c.name), .arguments = c.arguments}});
        }
    }
    return a;
}

constexpr glz::opts kLenient{.error_on_unknown_keys = false};

auto finish_of(const std::optional<std::string>& reason) -> Finish {
    if (!reason) {
        return Finish::None;
    }
    constexpr std::pair<std::string_view, Finish> known[] = {
        {"stop", Finish::Stop},
        {"length", Finish::Length},
        {"tool_calls", Finish::ToolCalls},
        {"content_filter", Finish::ContentFilter},
        {"insufficient_system_resource", Finish::NoResources},
        {"aborted", Finish::Aborted},
    };
    for (const auto& [text, finish] : known) {
        if (*reason == text) {
            return finish;
        }
    }
    WA_WARN("ai", "an unknown finish_reason \"{}\": taken as stop", *reason);
    return Finish::Stop;
}

auto usage_of(const wire::UsageIn& u) -> Usage {
    return Usage{.prompt = u.prompt_tokens,
                 .completion = u.completion_tokens,
                 .cache_hit = u.prompt_cache_hit_tokens,
                 .cache_miss = u.prompt_cache_miss_tokens,
                 .reasoning = u.completion_tokens_details ? u.completion_tokens_details->reasoning_tokens : 0};
}

} // namespace

auto wire_name(std::string_view tool) -> std::string {
    std::string name{tool};
    std::ranges::replace(name, '.', '_');
    return name;
}

auto path_for(const catalog::Provider& p, const ChatRequest& r) -> std::string_view {
    return r.tools.empty() ? p.path : p.path_with_tools;
}

auto request_json(const catalog::Provider& p, const ChatRequest& r) -> std::string {
    wire::Request w;
    w.model = r.model;
    const bool with_tools = !r.tools.empty();
    for (const auto& m : r.messages) {
        w.messages.push_back(wire_message(m, with_tools));
    }
    w.thinking.type = r.thinking == Thinking::Off ? "disabled" : "enabled";
    w.reasoning_effort = effort(r.thinking);
    if (r.max_tokens > 0) {
        w.max_tokens = r.max_tokens;
    }
    if (with_tools) {
        auto& tools = w.tools.emplace();
        for (const auto& t : r.tools) {
            wire::Tool tool;
            tool.function.name = wire_name(t.name);
            tool.function.description = t.description;
            tool.function.parameters.str = t.schema;
            tool.function.strict = p.strict_tools;
            tools.push_back(std::move(tool));
        }
    }
    return core::json::write(w).value_or(std::string{});
}

auto tool_name(std::string_view wire) -> std::string {
    std::string name{wire};
    if (const auto at = name.find('_'); at != std::string::npos) {
        name[at] = '.';
    }
    return name;
}

auto parse_chunk(std::string_view data) -> core::Result<Chunk> {
    Chunk out;
    if (data == "[DONE]") {
        out.done = true;
        return out;
    }
    wire::ChunkIn in;
    if (const auto error = glz::read<kLenient>(in, data)) {
        return core::make_error(core::Code::Parse, "a chunk of the stream: " + glz::format_error(error, data));
    }
    if (!in.choices.empty()) {
        const auto& c = in.choices.front();
        if (c.delta) {
            const auto& d = *c.delta;
            if (d.content || d.reasoning_content) {
                out.deltas.push_back(Delta{.content = d.content.value_or(""), .reasoning = d.reasoning_content.value_or("")});
            }
            if (d.tool_calls) {
                for (const auto& p : *d.tool_calls) {
                    Delta piece{.tool_index = p.index, .tool_id = p.id.value_or("")};
                    if (p.function) {
                        piece.tool_name = p.function->name ? tool_name(*p.function->name) : std::string{};
                        piece.tool_arguments = p.function->arguments.value_or("");
                    }
                    out.deltas.push_back(std::move(piece));
                }
            }
        }
        const Finish finish = finish_of(c.finish_reason);
        if (finish != Finish::None) {
            if (out.deltas.empty()) {
                out.deltas.emplace_back();
            }
            out.deltas.back().finish = finish;
        }
    }
    if (in.usage) {
        if (out.deltas.empty()) {
            out.deltas.emplace_back();
        }
        out.deltas.back().usage = usage_of(*in.usage);
    }
    return out;
}

auto error_of(const catalog::Provider& p, const net::Response& r) -> core::Error {
    WA_WARN("ai", "{} answered {}: {}", p.name, r.status, r.error_body);
    // The kind is what the app tells the cause by (the Add key button, the model menu's rows);
    // the message is only for the user to read.
    const auto kinded = [](core::Code code, std::string message, std::string_view kind) {
        core::Error e = core::make_error(code, std::move(message)).error();
        e.kind = std::string{kind};
        return e;
    };
    if (r.status == 401) {
        return kinded(core::Code::Api, std::string{kKeyRejected}, "key_rejected");
    }
    if (r.status == 402) {
        return kinded(core::Code::Api, std::format("The {} balance is empty", p.name), "balance_empty");
    }
    if (r.status == 429) {
        return kinded(core::Code::Net, std::format("{} is busy: too many requests", p.name), "busy");
    }
    if (r.status >= 500) {
        return kinded(core::Code::Net, std::format("{} is busy: server error {}", p.name, r.status), "busy");
    }
    wire::ErrorBody body;
    if (!glz::read<kLenient>(body, r.error_body) && body.error && !body.error->message.empty()) {
        return core::make_error(core::Code::Api, std::format("{} refused the request: {}", p.name, body.error->message)).error();
    }
    return core::make_error(core::Code::Api, std::format("{} refused the request (HTTP {})", p.name, r.status)).error();
}

namespace {

constexpr net::RequestId kFailed = std::numeric_limits<net::RequestId>::max();

// One turn on the network thread: its sink, its tap, and the first failure of its own (a chunk
// that does not parse), which ends it.
struct Turn {
    net::State& net;
    StreamSink sink;
    Tap tap;
    std::optional<core::Error> failure;
    // net's id once start_stream has it, or kFailed once a callback wanted the request cancelled
    // before that: whichever comes second cancels (as net::start_events does).
    std::atomic<net::RequestId> id{0};

    void fail(core::Error e) {
        if (failure) {
            return;
        }
        failure = std::move(e);
        if (const auto known = id.exchange(kFailed); known != 0) {
            net::cancel(net, known);
        }
    }
};

} // namespace

auto no_key(const catalog::Provider& p) -> std::string {
    return std::format("Add your {} API key", p.name);
}

auto start_stream(net::State& net, const catalog::Provider& p, std::string_view key, const ChatRequest& r, StreamSink sink,
                  std::string_view host, const Tap& tap) -> net::RequestId {
    const std::string_view path = path_for(p, r);
    std::string body = request_json(p, r);
    if (tap.request) {
        tap.request(path, body);
    }
    net::Request request{.method = "POST",
                         .url = std::string{host.empty() ? p.host : host} + std::string{path},
                         .headers = {{"Authorization", "Bearer " + std::string{key}},
                                     {"Content-Type", "application/json"},
                                     {"Accept", "text/event-stream"}},
                         .body = std::move(body)};
    auto turn = std::make_shared<Turn>(net, std::move(sink), tap);
    const catalog::Provider* by = &p; // a catalog entry: it outlives the turn
    net::EventSink events{.on_headers = {},
                          .on_event =
                              [turn](const net::sse::Event& e) {
                                  if (turn->tap.event) {
                                      turn->tap.event(e.data);
                                  }
                                  if (turn->failure) {
                                      return; // cancelled: what is still in flight is dropped
                                  }
                                  auto chunk = parse_chunk(e.data);
                                  if (!chunk) {
                                      turn->fail(std::move(chunk.error()));
                                      return;
                                  }
                                  for (const auto& d : chunk->deltas) {
                                      if (turn->sink.on_delta) {
                                          turn->sink.on_delta(d);
                                      }
                                  }
                              },
                          .on_done =
                              [turn, by](core::Result<net::Response> r) {
                                  if (turn->tap.answer) {
                                      turn->tap.answer(r ? r->status : 0, r ? std::string_view{r->error_body} : std::string_view{});
                                  }
                                  core::Result<void> outcome;
                                  if (turn->failure) {
                                      outcome = std::unexpected{*turn->failure};
                                  } else if (!r) {
                                      outcome = std::unexpected{r.error()};
                                  } else if (r->status < 200 || r->status > 299) {
                                      outcome = std::unexpected{error_of(*by, *r)};
                                  }
                                  if (turn->sink.on_done) {
                                      turn->sink.on_done(std::move(outcome));
                                  }
                              }};
    const auto id = net::start_events(net, std::move(request), std::move(events));
    if (turn->id.exchange(id) == kFailed) {
        net::cancel(net, id);
    }
    return id;
}

auto provider(net::State& net, const catalog::Provider& p, std::function<std::string()> key, std::string host, Tap tap) -> Provider {
    return [&net, by = &p, key = std::move(key), host = std::move(host), tap = std::move(tap)](const ChatRequest& r,
                                                                                           StreamSink sink) -> Stream {
        const std::string k = key ? key() : std::string{};
        if (k.empty()) {
            if (sink.on_done) {
                core::Error e = core::make_error(core::Code::Api, no_key(*by)).error();
                e.kind = "no_key";
                sink.on_done(std::unexpected(std::move(e)));
            }
            return Stream{.cancel = [] {}};
        }
        const auto id = start_stream(net, *by, k, r, std::move(sink), host, tap);
        return Stream{.cancel = [&net, id] { net::cancel(net, id); }};
    };
}

} // namespace ai::chat
