#include "app/settings.hpp"

#include "ai/catalog.hpp"

#include <array>
#include <cmath>
#include <string_view>

namespace app::settings {
namespace {

constexpr std::array<std::string_view, 4> kHotkeys{"Ctrl+Space", "Ctrl+Shift+Space", "Ctrl+Alt+Space",
                                                   "Alt+Shift+Space"};
constexpr unsigned kHotkeyId = 100;
constexpr unsigned kCustomHotkeyId = 110; // the one set by hand in config.json

struct GpuChoice {
    config::Gpu gpu;
    std::string_view label;
};
constexpr std::array<GpuChoice, 3> kGpus{{
    {config::Gpu::Auto, "Let Windows decide"},
    {config::Gpu::Integrated, "Integrated (saves power)"},
    // Spike S2: on hybrid laptops the discrete card copies its frames through the integrated one.
    {config::Gpu::Discrete, "Discrete (may be slower for this window)"},
}};
constexpr unsigned kGpuId = 200;

struct TextChoice {
    float scale;
    std::string_view label;
};
// The answer's 14 px become 14, 15, 16, 17.5, 19.
constexpr std::array<TextChoice, 5> kTexts{{
    {1.0f, "Small"},
    {15.0f / 14.0f, "Default"},
    {16.0f / 14.0f, "Large"},
    {17.5f / 14.0f, "Larger"},
    {19.0f / 14.0f, "Largest"},
}};
constexpr unsigned kTextId = 300;
constexpr unsigned kTransparencyId = 440; // 440 .. 450: 0 % .. 50 %

// The model (phase 7): config.json's "ai".
struct Named {
    std::string_view value;
    std::string_view label;
};
constexpr unsigned kModelId = 410;
constexpr std::array<Named, 4> kThinking{{{"off", "Off"}, {"low", "Low"}, {"high", "High"}, {"max", "Max"}}};
constexpr unsigned kThinkingId = 420;
struct Budget {
    double usd;
    std::string_view label;
};
constexpr std::array<Budget, 3> kBudgets{{{kBudgetUsd[0], "$0.10"}, {kBudgetUsd[1], "$0.50"}, {kBudgetUsd[2], "$1.00"}}};
constexpr unsigned kBudgetId = 430;

constexpr unsigned kFullscreenId = 400;

// The animations' frame rate (motion rework): the monitor's, or at most 120, 60, 30 a second.
constexpr std::array<config::FrameRate, 4> kFrameRates{config::FrameRate::Monitor, config::FrameRate::Hz120,
                                                       config::FrameRate::Hz60, config::FrameRate::Hz30};
constexpr std::array<std::string_view, 4> kFrameRateLabels{"Monitor", "120", "60", "30"};
constexpr unsigned kFrameRateId = 460; // 460 .. 463

// The interface's theme (UI update, stage 1): as Windows' apps, or Dark, or Light.
constexpr std::array<theme::Choice, 3> kThemes{theme::Choice::Auto, theme::Choice::Dark, theme::Choice::Light};
constexpr std::array<std::string_view, 3> kThemeLabels{"As Windows", "Dark", "Light"};
constexpr unsigned kThemeId = 470; // 470 .. 472

} // namespace

auto menu(const config::Config& c, const std::optional<std::string>& live, int monitor_hz) -> std::vector<Item> {
    const std::string& current = live ? *live : c.hotkey;
    std::vector<Item> out;

    Item hotkey{.label = "Keyboard shortcut"};
    bool listed = false;
    for (unsigned i = 0; i < kHotkeys.size(); ++i) {
        const bool on = current == kHotkeys[i];
        listed = listed || on;
        hotkey.children.push_back({.id = kHotkeyId + i, .label = std::string{kHotkeys[i]}, .checked = on, .radio = true});
    }
    if (!listed) {
        hotkey.children.push_back({.id = kCustomHotkeyId, .label = current, .checked = true, .radio = true});
    }
    out.push_back(std::move(hotkey));

    Item gpu{.label = "Graphics card (after restart)"};
    for (unsigned i = 0; i < kGpus.size(); ++i) {
        gpu.children.push_back(
            {.id = kGpuId + i, .label = std::string{kGpus[i].label}, .checked = c.gpu == kGpus[i].gpu, .radio = true});
    }
    out.push_back(std::move(gpu));

    Item text{.label = "Text size"};
    for (unsigned i = 0; i < kTexts.size(); ++i) {
        text.children.push_back({.id = kTextId + i,
                                 .label = std::string{kTexts[i].label},
                                 .checked = std::abs(c.text_scale - kTexts[i].scale) < 0.005f,
                                 .radio = true});
    }
    out.push_back(std::move(text));

    Item look{.label = "Theme"};
    for (unsigned i = 0; i < kThemes.size(); ++i) {
        look.children.push_back({.id = kThemeId + i, .label = std::string{kThemeLabels[i]}, .checked = c.theme == kThemes[i], .radio = true});
    }
    out.push_back(std::move(look));

    // How much of the desktop shows through the window: 0 % is opaque.
    Item see{.label = "Window transparency"};
    for (int p = 0; p <= config::kMostTransparency; p += config::kTransparencyStep) {
        see.children.push_back({.id = kTransparencyId + static_cast<unsigned>(p / config::kTransparencyStep),
                                .label = p == 0 ? std::string{"0% (opaque)"} : std::to_string(p) + "%",
                                .checked = c.transparency == p,
                                .radio = true});
    }
    out.push_back(std::move(see));

    Item rate{.label = "Animation frame rate"};
    for (unsigned i = 0; i < kFrameRates.size(); ++i) {
        std::string label{kFrameRateLabels[i]};
        if (i == 0 && monitor_hz > 0) {
            label += " (" + std::to_string(monitor_hz) + " Hz)";
        }
        rate.children.push_back({.id = kFrameRateId + i, .label = std::move(label), .checked = c.frame_rate == kFrameRates[i], .radio = true});
    }
    out.push_back(std::move(rate));

    out.push_back({.separator = true});
    out.push_back({.id = kFullscreenId, .label = "Open over fullscreen apps", .checked = c.over_fullscreen});
    out.push_back({.separator = true});
    Item model{.label = "Model"};
    const auto models = ai::catalog::models(); // each by its full name, in the catalog's order
    for (unsigned i = 0; i < models.size(); ++i) {
        if (std::ranges::find(c.ai.disabled, models[i].id) != c.ai.disabled.end() && c.ai.model != models[i].id) {
            continue; // switched off in Models (stage 4d-2)
        }
        model.children.push_back({.id = kModelId + i, .label = std::string{models[i].full_name}, .checked = c.ai.model == models[i].id, .radio = true});
    }
    out.push_back(std::move(model));
    Item thinking{.label = "Thinking"};
    for (unsigned i = 0; i < kThinking.size(); ++i) {
        thinking.children.push_back(
            {.id = kThinkingId + i, .label = std::string{kThinking[i].label}, .checked = c.ai.thinking == kThinking[i].value, .radio = true});
    }
    out.push_back(std::move(thinking));
    Item budget{.label = "Budget per task"};
    for (unsigned i = 0; i < kBudgets.size(); ++i) {
        budget.children.push_back({.id = kBudgetId + i,
                                   .label = std::string{kBudgets[i].label},
                                   .checked = std::abs(c.ai.budget_usd - kBudgets[i].usd) < 0.0005,
                                   .radio = true});
    }
    out.push_back(std::move(budget));
    out.push_back({.separator = true});
    // Phase 12 § 6: the user's rules (agent.md) and what the agent remembers, in the data panel.
    out.push_back({.id = kMemoryId, .label = "Rules and memory\xE2\x80\xA6"});
    out.push_back({.id = kChangeLogId, .label = "Change log\xE2\x80\xA6"});
    out.push_back({.id = kAuditId, .label = "Audit log\xE2\x80\xA6"});
    return out;
}

namespace {

// Where an item of menu() goes (stage 4d-1, SETTINGS_SPEC § 7 and the user's word on the logs):
// a section's index, kPages for the tray, or none (a separator, Rules and memory — a section itself).
auto place_of(const Item& item) -> std::optional<std::size_t> {
    if (item.separator || item.id == kMemoryId) {
        return std::nullopt;
    }
    const unsigned id = item.id != 0 ? item.id : item.children.empty() ? 0 : item.children.front().id;
    if (id == kFullscreenId || (id >= kGpuId && id < kGpuId + kGpus.size())) {
        return 0; // General
    }
    if ((id >= kThemeId && id < kThemeId + kThemes.size()) || (id >= kTextId && id < kTextId + kTexts.size()) ||
        (id >= kTransparencyId && id <= kTransparencyId + config::kMostTransparency / config::kTransparencyStep) ||
        (id >= kFrameRateId && id < kFrameRateId + kFrameRates.size())) {
        return 1; // Appearance
    }
    if (id >= kBudgetId && id < kBudgetId + kBudgets.size()) {
        return 3; // Roles (stage 4d-2)
    }
    if (id == kChangeLogId || id == kAuditId) {
        return 5; // Privacy
    }
    if ((id >= kHotkeyId && id < kHotkeyId + kHotkeys.size()) || id == kCustomHotkeyId) {
        return 6; // Shortcuts
    }
    return kPages; // the model, its thinking: the tray's own
}

} // namespace

auto pages(const config::Config& c, const std::optional<std::string>& live, int monitor_hz) -> std::array<std::vector<Item>, kPages> {
    std::array<std::vector<Item>, kPages> out;
    for (auto& item : menu(c, live, monitor_hz)) {
        if (const auto at = place_of(item); at && *at < kPages) {
            out[*at].push_back(std::move(item));
        }
    }
    // Models (stage 4d-2): each model of the catalog, on or off (the chosen one stays on).
    const auto models = ai::catalog::models();
    for (unsigned i = 0; i < models.size(); ++i) {
        out[2].push_back({.id = kModelOnId + i, .label = std::string{models[i].full_name},
                          .checked = std::ranges::find(c.ai.disabled, models[i].id) == c.ai.disabled.end() || c.ai.model == models[i].id});
    }
    return out;
}

auto tray_items(const config::Config& c) -> std::vector<Item> {
    std::vector<Item> out;
    for (auto& item : menu(c)) {
        if (place_of(item) == kPages) {
            out.push_back(std::move(item));
        }
    }
    return out;
}

auto choose(config::Config& c, unsigned id) -> std::optional<Change> {
    Change ch;
    if (id >= kHotkeyId && id < kHotkeyId + kHotkeys.size()) {
        c.hotkey = std::string{kHotkeys[id - kHotkeyId]};
        ch.hotkey = true;
    } else if (id == kCustomHotkeyId) {
        return Change{}; // already the current one
    } else if (id >= kGpuId && id < kGpuId + kGpus.size()) {
        c.gpu = kGpus[id - kGpuId].gpu;
        ch.gpu = true;
    } else if (id >= kTextId && id < kTextId + kTexts.size()) {
        c.text_scale = kTexts[id - kTextId].scale;
        ch.text = true;
    } else if (id >= kTransparencyId && id <= kTransparencyId + config::kMostTransparency / config::kTransparencyStep) {
        c.transparency = static_cast<int>(id - kTransparencyId) * config::kTransparencyStep;
        ch.transparency = true;
    } else if (id >= kFrameRateId && id < kFrameRateId + kFrameRates.size()) {
        c.frame_rate = kFrameRates[id - kFrameRateId];
        ch.frame_rate = true;
    } else if (id >= kThemeId && id < kThemeId + kThemes.size()) {
        c.theme = kThemes[id - kThemeId];
        ch.theme = true;
    } else if (id == kFullscreenId) {
        c.over_fullscreen = !c.over_fullscreen;
        ch.fullscreen = true;
    } else if (id >= kModelOnId && id < kModelOnId + ai::catalog::models().size()) {
        const std::string m{ai::catalog::models()[id - kModelOnId].id};
        if (m == c.ai.model) {
            return std::nullopt; // the chosen one stays on
        }
        if (std::erase(c.ai.disabled, m) == 0) {
            c.ai.disabled.push_back(m);
        }
        ch.ai = true;
    } else if (id >= kModelId && id < kModelId + ai::catalog::models().size()) {
        c.ai.model = std::string{ai::catalog::models()[id - kModelId].id};
        ch.ai = true;
    } else if (id >= kThinkingId && id < kThinkingId + kThinking.size()) {
        c.ai.thinking = std::string{kThinking[id - kThinkingId].value};
        ch.ai = true;
    } else if (id >= kBudgetId && id < kBudgetId + kBudgets.size()) {
        c.ai.budget_usd = kBudgets[id - kBudgetId].usd;
        ch.ai = true;
    } else {
        return std::nullopt;
    }
    return ch;
}

auto choice_of(const config::Ai& ai) -> sheet::ModelChoice {
    const auto level = sheet::level_of(ai.thinking);
    const auto* model = ai::catalog::model(ai.model);
    if (model == nullptr || !level) {
        return {};
    }
    return {.model = model->id, .level = *level}; // the catalog's own text, not the config's
}

void apply(config::Ai& ai, sheet::ModelChoice choice) {
    ai.model = std::string{choice.model};
    ai.thinking = std::string{sheet::thinking_id(choice.level)};
}

} // namespace app::settings
