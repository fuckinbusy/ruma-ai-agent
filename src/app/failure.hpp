#pragma once

// app::failure — a provider's failure as the conversation shows it (UI update, stage 4c; the
// designer's screen 06): what happened, what the agent already did, and what the user can do, the
// main action first. Told by the error's kind and code, never its words; never a bare HTTP code.

#include "ai/catalog.hpp"
#include "app/events.hpp"
#include "core/error.hpp"

#include <vector>

namespace app::failure {

struct Shown {
    events::Card card;
    std::vector<events::Action> actions;
};

// `last`: the model the request failed on (the last of its chain); its provider is the one named.
[[nodiscard]] auto card(const core::Error& e, const ai::catalog::Model& last) -> Shown;

} // namespace app::failure
