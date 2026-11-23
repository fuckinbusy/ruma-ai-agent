#pragma once

// setup::System — everything the installer changes outside the install folder's files: registry
// values under HKCU, shortcuts, and the running agent. Install and uninstall go through it so the
// tests run them against a registry branch of their own and a pretend agent.

#include "core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace setup {

using RegData = std::variant<std::wstring, std::uint32_t>;

struct Shortcut {
    std::filesystem::path lnk;
    std::filesystem::path target;
    std::wstring args;
    std::filesystem::path icon;   // empty: the target's own
    std::wstring aumid;           // empty: none
    std::wstring activator;       // the toast activator's CLSID text; empty: none
};

class System {
public:
    System() = default;
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    virtual ~System() = default;

    // Keys are relative to HKCU ("Software\\Microsoft\\..."), or to HKLM after "HKLM\\" (the Apps entry);
    // an empty name is the default value.
    virtual auto set_value(std::wstring_view key, std::wstring_view name, const RegData& data) -> core::Result<void> = 0;
    virtual auto get_value(std::wstring_view key, std::wstring_view name) -> std::optional<std::wstring> = 0;
    virtual auto delete_value(std::wstring_view key, std::wstring_view name) -> core::Result<void> = 0;
    virtual auto key_exists(std::wstring_view key) -> bool = 0;
    virtual auto delete_key(std::wstring_view key) -> core::Result<void> = 0; // with everything under it

    virtual auto make_shortcut(const Shortcut& s) -> core::Result<void> = 0;
    virtual auto shortcut_target(const std::filesystem::path& lnk) -> std::optional<std::filesystem::path> = 0;

    virtual auto agent_running() -> bool = 0;
    // Asks the agent to save and exit; true when it is gone within 5 s.
    virtual auto ask_agent_to_exit() -> bool = 0;
};

// How a running agent is recognized: its single-instance mutex and its window class.
struct AgentIds {
    std::wstring mutex;
    std::wstring window_class;
};

// The real one. Production: HKCU itself and the agent's own names (for_user). Tests: a branch under
// HKCU\Software\WinAiAgent-tests\ — any other root ends the process, so a test can never reach the
// user's Run or Uninstall keys.
class RegistrySystem : public System {
public:
    static auto for_user() -> RegistrySystem;
    RegistrySystem(std::wstring test_root, AgentIds ids);
    RegistrySystem(RegistrySystem&&) noexcept;

    auto set_value(std::wstring_view key, std::wstring_view name, const RegData& data) -> core::Result<void> override;
    auto get_value(std::wstring_view key, std::wstring_view name) -> std::optional<std::wstring> override;
    auto delete_value(std::wstring_view key, std::wstring_view name) -> core::Result<void> override;
    auto key_exists(std::wstring_view key) -> bool override;
    auto delete_key(std::wstring_view key) -> core::Result<void> override;
    auto make_shortcut(const Shortcut& s) -> core::Result<void> override;
    auto shortcut_target(const std::filesystem::path& lnk) -> std::optional<std::filesystem::path> override;
    auto agent_running() -> bool override;
    auto ask_agent_to_exit() -> bool override;

private:
    RegistrySystem() = default;

    std::wstring root_; // empty: HKCU itself
    AgentIds ids_;
};

} // namespace setup
