#include "ai/repeats.hpp"

#include "core/json.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <format>

namespace ai {
namespace {

// The arguments as one JSON however the model spaced or ordered them; as written when not JSON.
auto canonical(std::string_view args) -> std::string {
    glz::generic j;
    if (glz::read_json(j, args)) {
        return std::string{args};
    }
    auto out = core::json::write(j);
    return out ? std::move(*out) : std::string{args};
}

} // namespace

// How many times running the last `period` calls came, back to back, at the end.
auto Repeats::copies(std::size_t period) const -> std::size_t {
    const std::size_t n = calls_.size();
    if (n < period) {
        return 0;
    }
    std::size_t k = 1;
    while ((k + 1) * period <= n &&
           std::equal(calls_.end() - static_cast<std::ptrdiff_t>((k + 1) * period), calls_.end() - static_cast<std::ptrdiff_t>(k * period),
                      calls_.end() - static_cast<std::ptrdiff_t>(period))) {
        ++k;
    }
    return k;
}

auto Repeats::note(std::string_view tool, std::string_view args) -> int {
    calls_.push_back({std::string{tool}, canonical(args)});
    const std::size_t one = copies(1);
    // Two calls in turn: only two different ones (the same one twice is the case above).
    const std::size_t two = calls_.size() >= 2 && !(calls_.back() == calls_[calls_.size() - 2]) ? copies(2) : 0;
    period_ = two > one ? 2 : 1;
    const std::size_t times = std::max(one, two);
    if (times >= 4) {
        return 4;
    }
    if (times == 3) {
        if (warned_) {
            return 0; // the same streak, shifted by a call: warned already
        }
        warned_ = true;
        return 3;
    }
    warned_ = false;
    return 0;
}

auto Repeats::what() const -> std::string {
    if (calls_.empty()) {
        return {};
    }
    if (period_ == 2 && calls_.size() >= 2) {
        return std::format("{} and {} in turn", calls_[calls_.size() - 2].tool, calls_.back().tool);
    }
    return calls_.back().tool + " with the same arguments";
}

void Repeats::reset() {
    calls_.clear();
    period_ = 1;
    warned_ = false;
}

} // namespace ai
