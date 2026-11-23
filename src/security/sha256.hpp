#pragma once

// security::sha256 — SHA-256 through Windows CNG (BCrypt), for the audit's hash chain.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace security {

using Digest = std::array<std::uint8_t, 32>;

auto sha256(std::string_view data) -> Digest;
// Lowercase hex, 64 characters.
auto hex(const Digest& d) -> std::string;

} // namespace security
