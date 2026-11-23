#pragma once

// app::hotkeys — the agent's global shortcut: which one is live. The registering itself goes
// through a Registrar (RegisterHotKey in the shell, a fake in tests).

#include "app/config.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>

namespace app::hotkeys {

struct Registrar {
    std::function<bool(const config::Hotkey&)> add; // false: taken by another program
    std::function<void()> remove;                   // the one we hold
};

// The first of `names` that can be registered; nullopt when none is free.
auto register_first(std::span<const std::string> names, Registrar& r) -> std::optional<std::string>;
// From the live shortcut (if any) to `to`. When `to` is taken or not a shortcut, the live one
// stays registered and the result is false.
auto swap(const std::optional<std::string>& live, const std::string& to, Registrar& r) -> bool;

} // namespace app::hotkeys
