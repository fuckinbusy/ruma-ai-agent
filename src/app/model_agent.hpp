#pragma once

// app::model — the real agent (phase 7): what the user asks goes to DeepSeek; the model's tool
// calls go through the same tools, policy, questions and journal as the simulated agent's; the
// screens come from what the tools returned (app::present). It stands where sim::Agent stood:
// send() from the UI thread, events through `post`. Each request of a task runs on a thread of
// its own (ai::agent::run); a question, the budget's "go on?" and a pause wait there, never on
// the UI thread.

#include "ai/catalog.hpp"
#include "ai/provider.hpp"
#include "ai/types.hpp"
#include "app/agent_tools.hpp"
#include "app/events.hpp"
#include "app/memory.hpp"
#include "tools/winsys/private_rules.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace app::model {

// The thinking level as config.json and the answer's cost name it.
[[nodiscard]] inline auto thinking_id(ai::Thinking thinking) noexcept -> std::string_view {
    switch (thinking) {
    case ai::Thinking::Off: return "off";
    case ai::Thinking::High: return "high";
    case ai::Thinking::Max: return "max";
    case ai::Thinking::Low: break;
    }
    return "low";
}

// config.json's "ai.thinking" ("off", "low", "high", "max"); anything else: Low.
[[nodiscard]] inline auto thinking_of(std::string_view thinking) noexcept -> ai::Thinking {
    return thinking == "off" ? ai::Thinking::Off : thinking == "high" ? ai::Thinking::High : thinking == "max" ? ai::Thinking::Max : ai::Thinking::Low;
}

struct Settings {
    std::string model{ai::catalog::default_model().id}; // a model's id in ai::catalog
    ai::Thinking thinking = ai::Thinking::Low;
    double budget_usd = 0.10;           // a task's spending before "go on?"
    std::filesystem::path secrets;      // the DPAPI key file (security::load_key)
    std::string proxy = "system";       // config.json's network.proxy
    // A new request continues the last task's conversation if that one ended this recently.
    std::chrono::minutes follow_up{15};
};

class Agent {
public:
    using Post = std::function<void(events::Event)>;

    // `provider` empty: DeepSeek, its key read from settings.secrets and its network made at the
    // first request (nothing of the network at start).
    Agent(Post post, agent::Host host, Settings settings, ai::Provider provider = {});
    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // From the UI thread; never blocks for long.
    void send(events::Command command);
    // The largest task id in the journal: the sheet's ids start after it.
    [[nodiscard]] auto last_task() const -> std::uint64_t;
    // The tray's choices (model, thinking, budget), for the next request on.
    void set_settings(const Settings& settings);
    // Rules and memory (phase 12 § 6): what is remembered now, and the user's rules file
    // (agent.md, made with a line of help when there is none yet; empty: no data folder).
    [[nodiscard]] auto memory_entries() const -> std::vector<memory::Entry>;
    // The user's private windows (data\private.json, phase 13a), for Rules and memory.
    [[nodiscard]] auto private_rules() const -> std::vector<tools::winsys::PrivateRule>;
    [[nodiscard]] auto rules_file() const -> std::filesystem::path;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace app::model
