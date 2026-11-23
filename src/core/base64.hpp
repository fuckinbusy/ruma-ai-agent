#pragma once

// core::base64 — RFC 4648 with padding: a picture as the model gets it (data: URLs, phase 9).

#include <cstddef>
#include <span>
#include <string>

namespace core {

[[nodiscard]] auto base64(std::span<const std::byte> data) -> std::string;

} // namespace core
