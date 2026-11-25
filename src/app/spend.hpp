#pragma once

// app::spend — what the agent's answers cost this month, by provider (UI update, stage 4d-2): the
// Models section's line under the providers' cards. Kept in data\spend.json, written whole through a
// temporary file; a month that is not this one starts from nothing.

#include "core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace app::spend {

struct Total {
    std::string provider; // the catalog's id
    double usd = 0;
    std::int64_t tokens = 0; // in and out
};

struct Month {
    std::string month; // "2026-11"
    std::vector<Total> providers;
};

// An answer's cost added to `month` ("2026-11"): another month than the one kept starts anew.
void add(Month& m, std::string_view provider, double usd, std::int64_t tokens, std::string_view month);
// `month` is now: a month kept from another one is emptied ("this month" never shows the last).
void keep_month(Month& m, std::string_view month);
// All providers together.
[[nodiscard]] auto total(const Month& m) -> Total;

// None or a broken file: an empty month (a broken one is said in the log).
[[nodiscard]] auto load(const std::filesystem::path& file) -> Month;
auto save(const std::filesystem::path& file, const Month& m) -> core::Result<void>;

} // namespace app::spend
