#pragma once

// core::secret_like — what looks like a secret (phase 12 § 6): an API key (sk-…, ghp_…, AKIA…,
// xoxb-…), an Authorization header's credential, a password=, token=, secret= or api_key= value,
// a card number (13 to 19 digits, spaces or dashes between, a card's first digits, Luhn). The log
// masks these (core::log::mask_secrets), memory.add refuses them. No allocation in `at`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace core::secret_like {

struct Found {
    std::size_t value = 0; // where the secret itself starts (before it: its name, kept when masked)
    std::size_t end = 0;   // one past it
    std::string_view kind; // "an API key", "a password", "a card number", "a token"
};

// A secret starting exactly at `i` (its name or its first character), or none.
[[nodiscard]] auto at(std::string_view text, std::size_t i) noexcept -> std::optional<Found>;
// Why `text` looks like a secret ("it looks like a card number"), or none.
[[nodiscard]] auto looks_secret(std::string_view text) -> std::optional<std::string>;

} // namespace core::secret_like
