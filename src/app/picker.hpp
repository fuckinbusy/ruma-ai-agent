#pragma once

// app::picker — the model menu's rows (UI update, stage 4b; MODEL_PICKER_SPEC §§ 2, 4, 8): a group
// for each provider, its models in the catalog's order, filtered by the search and dimmed by the
// provider's trouble. Made when the query or a provider's state changes, never in a frame.

#include "ai/catalog.hpp"
#include "app/events.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace app::picker {

struct Row {
    const ai::catalog::Model* model = nullptr;
    bool dimmed = false;     // its provider cannot answer now
    std::string_view action; // what Enter or a click does instead of choosing: "Add key", "Update key"
    std::uint8_t match_from = 0, match_len = 0; // the search's match in its short name (bytes; 0 long: none)
};

struct Group {
    const ai::catalog::Provider* provider = nullptr;
    events::Trouble trouble = events::Trouble::None;
    std::size_t total = 0; // the provider's models, matched or not ("1 of 3")
    std::vector<Row> rows;
};

// The search is by a model's short name, its provider's name and monogram, any case: a match at a
// word's start comes before one inside a word. Groups with no match go; providers without a key
// come last. `states` are the agent's ProviderState events in order: the last of a provider counts.
[[nodiscard]] auto groups(std::string_view query, std::span<const events::ProviderState> states,
                          std::span<const ai::catalog::Provider> providers, std::span<const ai::catalog::Model> models)
    -> std::vector<Group>;
// The catalog's own.
[[nodiscard]] auto groups(std::string_view query, std::span<const events::ProviderState> states) -> std::vector<Group>;

// The group's header, right: "key rejected", "busy", "offline", "balance empty"; while searching
// "1 of 3"; empty when all is well.
[[nodiscard]] auto status_text(const Group& g, bool searching, std::span<char> buf) -> std::string_view;
// "200K", "32K"; from a million "1M" (rounded down).
[[nodiscard]] auto context_text(std::size_t tokens, std::span<char> buf) -> std::string_view;
// By the output's price per million: "free", "$" (< 2), "$$" (2 to 10), "$$$" (> 10).
[[nodiscard]] auto price_tier(const ai::catalog::Model& m) -> std::string_view;

} // namespace app::picker
