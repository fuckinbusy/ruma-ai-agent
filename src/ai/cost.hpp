#pragma once

// ai::cost — what a request costs, from its usage (the API returns tokens, not money).

#include "ai/catalog.hpp"
#include "ai/types.hpp"

#include <chrono>

namespace ai {

// Dollars at the model's own prices (ai::catalog, per 1M tokens off-peak): what hit the cache, what
// missed it, the output; doubled in the peak hours when its provider's price does that. Reasoning
// tokens are part of the output, counted once. `m` is the model that answered, not the one chosen.
auto cost_usd(const catalog::Model& m, const Usage& u, std::chrono::system_clock::time_point at) -> double;

// Peak: Monday to Friday, 01:00–04:00 and 06:00–10:00 UTC. China's public holidays are off-peak
// too; they are not known here (the price shown is then twice the real one).
auto is_peak(std::chrono::system_clock::time_point at) -> bool;

} // namespace ai
