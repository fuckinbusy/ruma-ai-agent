#pragma once

// ai::chat — the engine that talks to a provider of the catalog (UI update, stage 4a; it was
// ai::deepseek, the one provider): the JSON of a chat completion, its stream and its errors. What
// differs from provider to provider is in its catalog entry — the address, the paths, the strict
// tools, its name in what a person reads; the code here names none. The seam is ai::types against
// ai::chat::wire (07-PRINCIPLES § 17–18).

#include "ai/catalog.hpp"
#include "ai/provider.hpp"
#include "ai/stream.hpp"
#include "ai/types.hpp"
#include "core/error.hpp"
#include "net/http.hpp"
#include "net/net.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::chat {

// A tool's name on the wire: '.' becomes '_' ("fs.list" -> "fs_list"); OpenAI-style APIs take
// letters, digits, '_' and '-' only.
auto wire_name(std::string_view tool) -> std::string;

// The provider's path for a request with tools (strict functions may live under another one),
// else its plain one.
auto path_for(const catalog::Provider& p, const ChatRequest& r) -> std::string_view;

// The request's body: streamed, with usage in the stream; thinking and its effort; every tool a
// function, strict when the provider's are. An assistant turn carries its reasoning back only
// while the request has tools (the API ignores it otherwise).
auto request_json(const catalog::Provider& p, const ChatRequest& r) -> std::string;

// A wire name back to ours: the first '_' becomes '.' ("fs_delete_permanent" ->
// "fs.delete_permanent"; our groups have no '_'). A name with no '_' stays as it is.
auto tool_name(std::string_view wire) -> std::string;

// One SSE event's data: the pieces of choices[0] (text and reasoning first, then one per tool
// call; the finish and the usage on the last), or a usage-only piece; "[DONE]": none, done.
// Fields we do not know are skipped; broken JSON is Parse.
struct Chunk {
    std::vector<Delta> deltas;
    bool done = false;
};
auto parse_chunk(std::string_view data) -> core::Result<Chunk>;

// A non-2xx answer as a person reads it, the provider by its name: 401 and 402 by what they mean,
// 400/422 with the API's own message, 429 and 5xx "<name> is busy" (Net: worth trying later); the
// raw body goes to the log only.
auto error_of(const catalog::Provider& p, const net::Response& r) -> core::Error;

// What a 401 says, whoever the provider: the caller offers the key's field for it.
inline constexpr std::string_view kKeyRejected = "The API key was not accepted";

// The message of a turn asked for without a key: "Add your <name> API key".
auto no_key(const catalog::Provider& p) -> std::string;

// One turn through the network thread: POST host + path_for(p, r), "Authorization: Bearer <key>"
// (the key lives only for this call; the log shows "Bearer ***"), each event parsed into on_delta,
// on_done at the end — a non-2xx through error_of, a broken chunk as Parse (the request is
// cancelled at once). `host` empty: the provider's own; else the tests' mock server. `tap`: a
// recording's view of the wire.
auto start_stream(net::State& net, const catalog::Provider& p, std::string_view key, const ChatRequest& r, StreamSink sink,
                  std::string_view host = {}, const Tap& tap = {}) -> net::RequestId;

// A provider of the catalog as a Provider: `key` is asked for at each turn (empty: on_done
// no_key(p), no request); cancel() is net::cancel. `p` is a catalog entry: it outlives the function.
auto provider(net::State& net, const catalog::Provider& p, std::function<std::string()> key, std::string host = {}, Tap tap = {})
    -> Provider;

} // namespace ai::chat
