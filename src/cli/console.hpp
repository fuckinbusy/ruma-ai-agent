#pragma once

// What the console programs (tool-runner, agent-cli) share: where the agent's files are, how a
// question is asked on the console, how a tool call is confirmed there.

#include "tools/call.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace cli {

// A line on stderr (notes, questions); stdout is the program's result.
void say(std::string_view text);

auto utf8(std::wstring_view w) -> std::string;

// The agent's data folder (core::win::agent_data_dir: data\ beside the program in a portable build,
// otherwise %LOCALAPPDATA%\Agent), or WA_AGENT_DATA when set — the tests set it to a
// folder of their own, so nothing a test runs (or a broken --secrets, --audit, --config) reaches
// the user's files.
auto data_dir() -> std::filesystem::path;

// "<question> [y/N]" on stderr, the answer from stdin; `yes` (--yes) answers yes, no answer is no.
auto ask_yes(std::string_view question, bool yes) -> bool;

// A tool call's question: the ruling's reason, its paths and preview, then "Allow?".
auto confirm(bool yes) -> tools::Confirm;

} // namespace cli
