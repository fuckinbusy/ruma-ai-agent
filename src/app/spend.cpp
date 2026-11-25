#include "app/spend.hpp"

#include "core/fs.hpp"
#include "core/json.hpp"
#include "core/log.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>

namespace app::spend {

void add(Month& m, std::string_view provider, double usd, std::int64_t tokens, std::string_view month) {
    if (m.month != month) { // a new month: from nothing
        m.month = std::string{month};
        m.providers.clear();
    }
    auto it = std::ranges::find(m.providers, provider, &Total::provider);
    if (it == m.providers.end()) {
        m.providers.push_back({.provider = std::string{provider}, .usd = 0, .tokens = 0});
        it = m.providers.end() - 1;
    }
    it->usd += usd;
    it->tokens += tokens;
}

void keep_month(Month& m, std::string_view month) {
    if (m.month != month) {
        m.month = std::string{month};
        m.providers.clear();
    }
}

auto total(const Month& m) -> Total {
    Total t;
    for (const auto& p : m.providers) {
        t.usd += p.usd;
        t.tokens += p.tokens;
    }
    return t;
}

auto load(const std::filesystem::path& file) -> Month {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return {};
    }
    const auto text = core::fs::read_file(file, 1 << 20);
    if (!text) {
        WA_WARN("app", "{} not read: {}; this month's spending starts again", file.string(), text.error().message);
        return {};
    }
    Month m;
    if (const auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(m, *text)) {
        WA_WARN("app", "{} not read: {}; this month's spending starts again", file.string(), glz::format_error(err, *text));
        return {};
    }
    return m;
}

auto save(const std::filesystem::path& file, const Month& m) -> core::Result<void> {
    auto json = core::json::write(m);
    if (!json) {
        return core::make_error(core::Code::Internal, "the month's spending could not be written as JSON");
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return core::fs::write_file_atomic(file, *json);
}

} // namespace app::spend
