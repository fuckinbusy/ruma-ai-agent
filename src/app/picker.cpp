#include "app/picker.hpp"

#include <algorithm>
#include <format>

namespace app::picker {

namespace {

auto lower(char c) -> char {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Where `query` is in `text`, any case (ASCII): at a word's start first, else anywhere; npos: not.
struct Found {
    std::size_t at = std::string_view::npos;
    bool word_start = false;
};
auto find(std::string_view text, std::string_view query) -> Found {
    Found found;
    if (query.empty() || query.size() > text.size()) {
        return found;
    }
    for (std::size_t i = 0; i + query.size() <= text.size(); ++i) {
        bool same = true;
        for (std::size_t k = 0; k < query.size() && same; ++k) {
            same = lower(text[i + k]) == lower(query[k]);
        }
        if (!same) {
            continue;
        }
        const bool start = i == 0 || text[i - 1] == ' ' || text[i - 1] == '-' || text[i - 1] == '.' || text[i - 1] == '_';
        if (start) {
            return {i, true};
        }
        if (found.at == std::string_view::npos) {
            found.at = i;
        }
    }
    return found;
}

// 2: a word's start, 1: inside a word, 0: no match.
auto rank(const Found& f) -> int {
    return f.at == std::string_view::npos ? 0 : (f.word_start ? 2 : 1);
}

auto state_of(std::string_view provider, std::span<const events::ProviderState> states) -> events::Trouble {
    events::Trouble t = events::Trouble::None;
    for (const auto& s : states) {
        if (s.provider == provider) {
            t = s.trouble;
        }
    }
    return t;
}

} // namespace

auto groups(std::string_view query, std::span<const events::ProviderState> states, std::span<const ai::catalog::Provider> providers,
            std::span<const ai::catalog::Model> models, std::span<const std::string> disabled) -> std::vector<Group> {
    std::vector<Group> out;
    std::vector<Group> keyless; // providers without a key: at the end
    for (const auto& p : providers) {
        Group g;
        g.provider = &p;
        g.trouble = state_of(p.id, states);
        const bool dimmed = g.trouble == events::Trouble::NoKey || g.trouble == events::Trouble::KeyRejected ||
                            g.trouble == events::Trouble::Offline;
        const std::string_view action = g.trouble == events::Trouble::NoKey         ? "Add key"
                                        : g.trouble == events::Trouble::KeyRejected ? "Update key"
                                                                                    : std::string_view{};
        const int by_provider = std::max(rank(find(p.name, query)), rank(find(p.monogram, query)));
        std::vector<std::pair<int, Row>> ranked;
        for (const auto& m : models) {
            if (m.provider != p.id || std::ranges::find(disabled, m.id) != disabled.end()) {
                continue;
            }
            ++g.total;
            Row row{.model = &m, .dimmed = dimmed, .action = action};
            int r = 2; // no query: every row
            if (!query.empty()) {
                const Found in_name = find(m.short_name, query);
                r = std::max(rank(in_name), by_provider);
                if (in_name.at != std::string_view::npos) {
                    row.match_from = static_cast<std::uint8_t>(std::min<std::size_t>(in_name.at, 255));
                    row.match_len = static_cast<std::uint8_t>(std::min<std::size_t>(query.size(), 255));
                }
            }
            if (r > 0) {
                ranked.emplace_back(r, row);
            }
        }
        if (ranked.empty()) {
            continue;
        }
        std::ranges::stable_sort(ranked, [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& [r, row] : ranked) {
            g.rows.push_back(row);
        }
        (g.trouble == events::Trouble::NoKey ? keyless : out).push_back(std::move(g));
    }
    for (auto& g : keyless) {
        out.push_back(std::move(g));
    }
    return out;
}

auto groups(std::string_view query, std::span<const events::ProviderState> states, std::span<const std::string> disabled)
    -> std::vector<Group> {
    return groups(query, states, ai::catalog::providers(), ai::catalog::models(), disabled);
}

auto status_text(const Group& g, bool searching, std::span<char> buf) -> std::string_view {
    if (searching) {
        const auto end = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), "{} of {}", g.rows.size(), g.total);
        return {buf.data(), static_cast<std::size_t>(end.out - buf.data())};
    }
    switch (g.trouble) {
    case events::Trouble::KeyRejected: return "key rejected";
    case events::Trouble::Busy: return "busy";
    case events::Trouble::Offline: return "offline";
    case events::Trouble::Balance: return "balance empty";
    case events::Trouble::None:
    case events::Trouble::NoKey: break;
    }
    return {};
}

auto context_text(std::size_t tokens, std::span<char> buf) -> std::string_view {
    const auto end = tokens >= 1000000
                         ? std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), "{}M", tokens / 1000000)
                         : std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), "{}K", tokens / 1000);
    return {buf.data(), static_cast<std::size_t>(end.out - buf.data())};
}

auto price_tier(const ai::catalog::Model& m) -> std::string_view {
    const double out = m.prices.output;
    if (out <= 0) {
        return "free";
    }
    if (out < 2) {
        return "$";
    }
    return out <= 10 ? "$$" : "$$$";
}

} // namespace app::picker
