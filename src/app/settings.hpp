#pragma once

// app::settings — the tray menu's Settings as data: what it shows for a config and what choosing
// an item changes. The shell turns the items into a Win32 menu, applies the change and saves
// config.json.

#include "app/config.hpp"
#include "app/model_choice.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace app::settings {

struct Item {
    unsigned id = 0; // 0: a submenu or a separator
    std::string label;
    bool checked = false;
    bool radio = false; // one of a group (shown with a dot)
    bool separator = false;
    std::vector<Item> children; // a submenu
};

// What a choice changed, for the shell to apply.
struct Change {
    bool hotkey = false;
    bool gpu = false;
    bool text = false;
    bool fullscreen = false;
    bool rest = false;
    bool ai = false;      // the model, its thinking or the budget per task (phase 7)
    bool transparency = false; // the window's surface
    bool frame_rate = false;   // the animations' frame rate (motion rework)
    bool theme = false;        // the interface's theme (UI update, stage 1)
};

// Item ids are in [kFirstId, kLastId]; the shell's own menu ids stay below.
inline constexpr unsigned kFirstId = 100;
inline constexpr unsigned kLastId = 499;
// "Audit log…": opens the audit screen; no setting changes.
inline constexpr unsigned kAuditId = 490;
// "Change log…": opens the change log; no setting changes.
inline constexpr unsigned kChangeLogId = 491;
// "DeepSeek API key…": the sheet's key field; no setting changes (the key goes to DPAPI).
inline constexpr unsigned kApiKeyId = 492;
// Phase 12 § 6: "Rules and memory…" — the data panel's list: the user's rules (agent.md, Open in
// its editor), each remembered entry (Forget), Forget everything (asked first).
inline constexpr unsigned kMemoryId = 494;
// "Budget per task": its values, in the tray's menu and the picker alike.
inline constexpr std::array<double, 3> kBudgetUsd{0.10, 0.50, 1.00};

// `live`: the shortcut actually registered (a fallback when the configured one is taken); the
// menu checks it. Without it, the configured one. `monitor_hz`: the agent's monitor's rate, said
// beside "Monitor" in the frame rate's list (0: not known, not said).
auto menu(const config::Config& c, const std::optional<std::string>& live = std::nullopt, int monitor_hz = 0)
    -> std::vector<Item>;
// nullopt: not a settings item.
auto choose(config::Config& c, unsigned id) -> std::optional<Change>;

// The sheet's model choice (design/ui_model) as config.json has it; values it doesn't know (a file
// edited by hand) are Flash · Low.
auto choice_of(const config::Ai& ai) -> sheet::ModelChoice;
void apply(config::Ai& ai, sheet::ModelChoice choice);

} // namespace app::settings
