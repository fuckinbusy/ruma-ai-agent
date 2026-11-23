#pragma once

// ai::cassette — DeepSeek's exchanges written down and played back (05-AI-AGENT § The CLI harness):
// agent-cli --record writes one from a live run, the tests and agent.exe --replay play it with
// no network. The raw stream is kept, so a recording is also the parser's oracle.
//
// Format v1 (text, UTF-8, LF), one block per exchange:
//   # winaiagent cassette v1
//   >>> POST /beta/chat/completions
//   <the request's body, one line>
//   <<< 200
//   data: {...}            (each event as it came; a blank line after each)
//   <<< 401                (or a non-2xx with its body; 0: no answer)
// No key and no headers are ever written; the user's profile folder becomes C:\Users\user and
// the computer's name PC, at every level of JSON escaping.

#include "ai/provider.hpp"
#include "core/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace ai::cassette {

// A tap that writes each exchange to `file` (made anew) when it ends.
auto recorder(const std::filesystem::path& file, std::string profile, std::string computer) -> core::Result<Tap>;

// A provider that plays `file`'s exchanges in order, on the caller's thread: each turn's deltas,
// then its end; a call past the last exchange ends as Parse.
auto player(const std::filesystem::path& file) -> core::Result<Provider>;

// `text` with the profile folder and the computer's name replaced, case-insensitively, also with
// backslashes escaped once and twice (JSON, and JSON inside a JSON string); the user's name alone
// (the folder's last part, 3 characters or more) becomes "user".
auto scrub(std::string_view text, std::string_view profile, std::string_view computer) -> std::string;

} // namespace ai::cassette
