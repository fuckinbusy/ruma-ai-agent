#include "ai/cost.hpp"

namespace ai {

auto is_peak(std::chrono::system_clock::time_point at) -> bool {
    using namespace std::chrono;
    const auto day = floor<days>(at);
    const weekday w{day};
    if (w == Saturday || w == Sunday) {
        return false;
    }
    const auto hour = duration_cast<hours>(at - day).count();
    return (hour >= 1 && hour < 4) || (hour >= 6 && hour < 10);
}

auto cost_usd(const catalog::Model& m, const Usage& u, std::chrono::system_clock::time_point at) -> double {
    const auto& p = m.prices;
    const double factor = catalog::provider_of(m).peak_doubles && is_peak(at) ? 2.0 : 1.0;
    return factor * (u.cache_hit * p.cache_hit + u.cache_miss * p.cache_miss + u.completion * p.output) / 1'000'000.0;
}

} // namespace ai
