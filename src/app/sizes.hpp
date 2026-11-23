#pragma once

// app::sizes — sizes as the cards and the tables write them ("340 MB", "2.0 KB"), and numbers read
// back from the model's tables ("3 MB" as bytes, "12%" as 12) so they sort as numbers.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace app::sizes {

// Binary units; one decimal under 10, whole numbers from 10.
[[nodiscard]] auto text(std::uint64_t bytes) -> std::string;
// A number, its thousands maybe spaced ("1 284"), a sign or "$" before it, and maybe a unit after
// (B, KB, MB, GB, TB: as bytes; %: as is); anything else after it: not a number.
[[nodiscard]] auto parse(std::string_view cell) -> std::optional<double>;

} // namespace app::sizes
