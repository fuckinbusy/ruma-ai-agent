#include "ai/catalog.hpp"

#include <array>

namespace ai::catalog {

namespace {

constexpr std::uint8_t kAllLevels =
    level_bit(Thinking::Off) | level_bit(Thinking::Low) | level_bit(Thinking::High) | level_bit(Thinking::Max);

// DeepSeek (05-AI-AGENT § Facts, checked 2026-10-12): strict functions live under /beta; the price
// doubles on weekdays 01:00–04:00 and 06:00–10:00 UTC.
constexpr std::array<Provider, 1> kProviders{{
    {.id = "deepseek",
     .name = "DeepSeek",
     .monogram = "DS",
     .host = "https://api.deepseek.com",
     .path = "/chat/completions",
     .path_with_tools = "/beta/chat/completions",
     .key_prefix = "sk-",
     .key_min = 20,
     .strict_tools = true,
     .peak_doubles = true},
}};

constexpr std::array<Model, 2> kModels{{
    {.id = "deepseek-flash",
     .provider = "deepseek",
     .short_name = "Flash",
     .full_name = "DeepSeek Flash",
     .vision = Tri::Yes,
     .tools = Tri::Yes,
     .thinking = Tri::Yes,
     .levels = kAllLevels,
     .context_tokens = 200'000,
     .prices = {.cache_hit = 0.003, .cache_miss = 0.15, .output = 0.6}},
    {.id = "deepseek-v4-pro",
     .provider = "deepseek",
     .short_name = "Pro",
     .full_name = "DeepSeek Pro",
     .vision = Tri::No, // pictures are taken in user messages by Flash alone
     .tools = Tri::Yes,
     .thinking = Tri::Yes,
     .levels = kAllLevels,
     .context_tokens = 200'000,
     .prices = {.cache_hit = 0.022, .cache_miss = 0.66, .output = 1.98}},
}};

// The interface shows the first two until the model menu (stage 4b): app/model_choice.hpp.
static_assert(kModels.size() >= 2);

} // namespace

auto nearest_level(const Model& m, Thinking level) noexcept -> Thinking {
    for (int i = static_cast<int>(level); i >= 0; --i) {
        if (has_level(m, static_cast<Thinking>(i))) {
            return static_cast<Thinking>(i);
        }
    }
    for (int i = static_cast<int>(level) + 1; i <= static_cast<int>(Thinking::Max); ++i) {
        if (has_level(m, static_cast<Thinking>(i))) {
            return static_cast<Thinking>(i);
        }
    }
    return level; // a model with no level at all: as asked
}

auto providers() noexcept -> std::span<const Provider> {
    return kProviders;
}

auto models() noexcept -> std::span<const Model> {
    return kModels;
}

auto provider(std::string_view id) noexcept -> const Provider* {
    for (const auto& p : kProviders) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

auto model(std::string_view id) noexcept -> const Model* {
    for (const auto& m : kModels) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

auto provider_of(const Model& m) noexcept -> const Provider& {
    const Provider* p = provider(m.provider);
    return p != nullptr ? *p : kProviders.front(); // a model's provider is always in the catalog (its test says so)
}

auto beside(const Model& m) noexcept -> Beside {
    Beside b;
    if (m.vision == Tri::Yes) {
        b.vision = &m;
    }
    for (const auto& k : kModels) {
        if (k.provider != m.provider) {
            continue;
        }
        if (b.vision == nullptr && k.vision == Tri::Yes) {
            b.vision = &k;
        }
        if (b.summary == nullptr || k.prices.output < b.summary->prices.output) {
            b.summary = &k;
        }
    }
    return b;
}

auto default_model() noexcept -> const Model& {
    return kModels.front();
}

auto default_provider() noexcept -> const Provider& {
    return provider_of(default_model());
}

} // namespace ai::catalog
