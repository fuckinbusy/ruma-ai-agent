#include "app/hotkeys.hpp"

namespace app::hotkeys {

auto register_first(std::span<const std::string> names, Registrar& r) -> std::optional<std::string> {
    for (const auto& name : names) {
        if (const auto hk = config::parse_hotkey(name); hk && r.add(*hk)) {
            return name;
        }
    }
    return std::nullopt;
}

auto swap(const std::optional<std::string>& live, const std::string& to, Registrar& r) -> bool {
    const auto next = config::parse_hotkey(to);
    if (!next) {
        return false;
    }
    if (live) {
        r.remove();
    }
    if (r.add(*next)) {
        return true;
    }
    if (live) { // we held it a moment ago
        if (const auto back = config::parse_hotkey(*live)) {
            (void)r.add(*back);
        }
    }
    return false;
}

} // namespace app::hotkeys
