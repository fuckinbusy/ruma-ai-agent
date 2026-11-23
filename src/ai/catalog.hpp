#pragma once

// ai::catalog — every provider and model the agent's code knows, as data (UI update, stage 4a).
// The loop, the price, the compaction, the config and the interface ask here; nothing outside
// catalog.cpp names a model or a provider. Only what exists in fact is here (the user,
// 2026-11-20): a provider is added when it is used, with the fields it then needs.

#include "ai/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ai::catalog {

// Whether a model can: Unknown is for a server that did not say (none of today's).
enum class Tri : std::uint8_t { No, Yes, Unknown };

// Dollars per 1M tokens, off-peak.
struct Prices {
    double cache_hit = 0, cache_miss = 0, output = 0;
};

struct Provider {
    std::string_view id;       // "deepseek": in config.json and the keys file
    std::string_view name;     // "DeepSeek": as the interface and the errors say it
    std::string_view monogram; // "DS"
    std::string_view host;     // "https://api.deepseek.com"
    std::string_view path;     // of a request without tools
    std::string_view path_with_tools;
    std::string_view key_prefix; // a key's form: how it starts…
    std::size_t key_min = 0;     // …and how many letters and digits at least follow
    bool strict_tools = false;   // every tool a strict function
    bool peak_doubles = false;   // the price doubles in the peak hours (ai::is_peak)
};

struct Model {
    std::string_view id;         // on the wire and in config.json: "deepseek-flash"
    std::string_view provider;   // its Provider's id
    std::string_view short_name; // "Flash": beside its provider's name
    std::string_view full_name;  // "DeepSeek Flash": alone
    Tri vision = Tri::No, tools = Tri::No, thinking = Tri::No;
    std::uint8_t levels = 0;     // a bit for each ai::Thinking it takes (level_bit)
    std::size_t context_tokens = 0;
    Prices prices;
};

[[nodiscard]] constexpr auto level_bit(Thinking level) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(level));
}
[[nodiscard]] constexpr auto has_level(const Model& m, Thinking level) noexcept -> bool {
    return (m.levels & level_bit(level)) != 0;
}
// `level` if the model takes it; else the next lower it takes; else its lowest.
[[nodiscard]] auto nearest_level(const Model& m, Thinking level) noexcept -> Thinking;

[[nodiscard]] auto providers() noexcept -> std::span<const Provider>;
[[nodiscard]] auto models() noexcept -> std::span<const Model>;
// By id; nullptr when there is none.
[[nodiscard]] auto provider(std::string_view id) noexcept -> const Provider*;
[[nodiscard]] auto model(std::string_view id) noexcept -> const Model*;
[[nodiscard]] auto provider_of(const Model& m) noexcept -> const Provider&;
// The models that stand in for `m`, of its own provider: the first that sees pictures (m itself
// when it does; null: none), and the cheapest by its output's price, for the summary of an old
// history. One rule for whoever runs the loop, until the user sets roles (stage 4c).
struct Beside {
    const Model* vision = nullptr;
    const Model* summary = nullptr;
};
[[nodiscard]] auto beside(const Model& m) noexcept -> Beside;

// What a new user starts with: the first model, and its provider.
[[nodiscard]] auto default_model() noexcept -> const Model&;
[[nodiscard]] auto default_provider() noexcept -> const Provider&;

} // namespace ai::catalog
