#pragma once

// app::sheet::ModelChoice — the model and the thinking level the user chose (UI update, stage 4b):
// a model of ai::catalog by its id, Off/Low/High/Max. The sheet shows and changes it; config.json
// keeps it as "ai.model" (the id) and "ai.thinking" ("off" / "low" / "high" / "max"). The level is
// the one asked for: a model without it is sent the nearest it has (ai::catalog::nearest_level),
// and the asked one comes back with a model that has it.

#include "ai/catalog.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace app::sheet {

struct ModelChoice {
    // A catalog model's id: it views the catalog's own text (static), never a copy that may go.
    std::string_view model = ai::catalog::default_model().id;
    int level = 1; // 0 Off, 1 Low, 2 High, 3 Max
    auto operator==(const ModelChoice& o) const -> bool { return model == o.model && level == o.level; }
};

// The choice's model; one the catalog does not have: its default (the agent refuses that id by name).
[[nodiscard]] inline auto model_of(const ModelChoice& c) noexcept -> const ai::catalog::Model& {
    const auto* m = ai::catalog::model(c.model);
    return m != nullptr ? *m : ai::catalog::default_model();
}

// A model's place in the catalog (the orb's ring knows a change by it); -1: none.
[[nodiscard]] inline auto model_index(std::string_view id) noexcept -> int {
    const auto all = ai::catalog::models();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The model after `id` in the catalog, round to the first (a quick change without the menu).
[[nodiscard]] inline auto next_model(std::string_view id) noexcept -> std::string_view {
    const auto all = ai::catalog::models();
    const int at = model_index(id);
    return all[static_cast<std::size_t>(at + 1) % all.size()].id;
}

inline constexpr std::array<std::string_view, 4> kThinkingIds{"off", "low", "high", "max"};
inline constexpr std::array<std::string_view, 4> kThinkingNames{"Off", "Low", "High", "Max"};

[[nodiscard]] constexpr auto thinking_id(int level) noexcept -> std::string_view {
    return kThinkingIds[static_cast<std::size_t>(level < 0 ? 0 : level > 3 ? 3 : level)];
}

[[nodiscard]] constexpr auto thinking_name(int level) noexcept -> std::string_view {
    return kThinkingNames[static_cast<std::size_t>(level < 0 ? 0 : level > 3 ? 3 : level)];
}

// "low" -> 1; anything else nothing.
[[nodiscard]] constexpr auto level_of(std::string_view thinking) noexcept -> std::optional<int> {
    for (std::size_t i = 0; i < kThinkingIds.size(); ++i) {
        if (kThinkingIds[i] == thinking) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

// The level a model is sent for the one asked: the asked one, or the nearest it has.
[[nodiscard]] inline auto level_for(const ai::catalog::Model& m, int asked) noexcept -> int {
    return static_cast<int>(ai::catalog::nearest_level(m, static_cast<ai::Thinking>(asked < 0 ? 0 : asked > 3 ? 3 : asked)));
}

} // namespace app::sheet
