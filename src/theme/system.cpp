#include "theme/system.hpp"

#include <windows.h>

namespace theme {

auto choice_of(std::string_view word) noexcept -> std::optional<Choice> {
    if (word == "auto") {
        return Choice::Auto;
    }
    if (word == "dark") {
        return Choice::Dark;
    }
    if (word == "light") {
        return Choice::Light;
    }
    return std::nullopt;
}

auto word_of(Choice c) noexcept -> std::string_view {
    switch (c) {
    case Choice::Dark: return "dark";
    case Choice::Light: return "light";
    case Choice::Auto: break;
    }
    return "auto";
}

namespace {

// A DWORD of Windows' Personalize key as a yes or a no; nullopt when it is not there or not a DWORD.
auto mode(HKEY root, const std::wstring& key, const wchar_t* name) noexcept -> std::optional<bool> {
    DWORD value = 0;
    DWORD size = sizeof value;
    // RRF_RT_REG_DWORD: a value of another type is an error, not a misread number.
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value != 0;
}

} // namespace

auto windows_light(HKEY root, const std::wstring& key) noexcept -> std::optional<bool> {
    return mode(root, key, L"AppsUseLightTheme");
}

auto windows_light() noexcept -> std::optional<bool> {
    return windows_light(HKEY_CURRENT_USER, std::wstring{kPersonalizeKey});
}

auto chosen(Choice c, std::optional<bool> windows_light) noexcept -> const Theme& {
    switch (c) {
    case Choice::Dark: return dark();
    case Choice::Light: return light();
    case Choice::Auto: break;
    }
    return windows_light.value_or(false) ? light() : dark();
}

auto windows_system_light(HKEY root, const std::wstring& key) noexcept -> std::optional<bool> {
    return mode(root, key, L"SystemUsesLightTheme");
}

auto windows_system_light() noexcept -> std::optional<bool> {
    return windows_system_light(HKEY_CURRENT_USER, std::wstring{kPersonalizeKey});
}

auto sign_theme(std::optional<bool> system_light) noexcept -> const Theme& {
    return system_light.value_or(false) ? light() : dark();
}

} // namespace theme
