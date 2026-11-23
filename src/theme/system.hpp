#pragma once

// theme/system — which theme the user chose, and what Windows says when the choice is "as Windows"
// (UI update, stage 1, task 4).
//
//   theme::Choice              config.json's "theme": auto (as Windows), dark, light
//   theme::choice_of(word)     the choice a word names; another word names none
//   theme::word_of(choice)     ...and back
//   theme::windows_light()     Windows' apps' mode: light, dark, or not known. Read only — the agent
//                              never writes Windows' settings
//   theme::chosen(choice, w)   the built-in theme a choice comes to (auto: Light when Windows says
//                              light, Dark otherwise)

#include "theme/theme.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct HKEY__;

namespace theme {

enum class Choice : std::uint8_t { Auto, Dark, Light };

[[nodiscard]] auto choice_of(std::string_view word) noexcept -> std::optional<Choice>;
[[nodiscard]] auto word_of(Choice c) noexcept -> std::string_view;

// Where Windows keeps the apps' mode, under HKEY_CURRENT_USER: a DWORD "AppsUseLightTheme".
inline constexpr std::wstring_view kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

// The apps' mode under `root`\`key` (tests give a branch of their own): nullopt when the key or the
// value is missing or is not a DWORD.
[[nodiscard]] auto windows_light(HKEY__* root, const std::wstring& key) noexcept -> std::optional<bool>;
// ...as Windows has it for this user.
[[nodiscard]] auto windows_light() noexcept -> std::optional<bool>;

[[nodiscard]] auto chosen(Choice c, std::optional<bool> windows_light) noexcept -> const Theme&;

// Windows' own mode — the taskbar's, a DWORD "SystemUsesLightTheme" in the same key: the hidden
// agent's sign sits on the screen as the taskbar does and takes this one, not the apps' (UI update,
// stage 3). Read only.
[[nodiscard]] auto windows_system_light(HKEY__* root, const std::wstring& key) noexcept -> std::optional<bool>;
[[nodiscard]] auto windows_system_light() noexcept -> std::optional<bool>;
// The theme the sign is drawn in: Light when Windows' own mode is light, Dark otherwise.
[[nodiscard]] auto sign_theme(std::optional<bool> system_light) noexcept -> const Theme&;

} // namespace theme
