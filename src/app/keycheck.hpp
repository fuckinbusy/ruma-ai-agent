#pragma once

// app::keycheck — the DeepSeek key before it is kept: the agent can't be used
// without a key, and a key is kept only once DeepSeek takes it. First its form (sk- and letters and
// digits), then a real request with it; no answer is no verdict — the key is not kept, the user
// tries again.

#include "ai/chat.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace app::keycheck {

enum class Verdict : std::uint8_t { Accepted, Rejected, Unreachable };

struct Result {
    Verdict verdict = Verdict::Unreachable;
    std::string message; // for the key's row; empty when accepted
};

// "sk-" and at least 20 ASCII letters and digits: DeepSeek's keys are "sk-" and 32 hex digits.
[[nodiscard]] auto looks_right(std::string_view key) -> bool;

// The message a key of the wrong form gets, for the provider it is meant for (its name, its form).
[[nodiscard]] auto wrong_form(const ai::catalog::Provider& p) -> std::string;

// GET <host>/models with the key as a bearer, through the agent's proxy setting (config.json's
// network.proxy). Blocks up to ~15 s: call it from a thread of its own, never the UI's.
auto check(std::string_view key, std::string_view proxy, std::string_view host = {}) -> Result;

} // namespace app::keycheck
