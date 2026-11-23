#pragma once

// app::gpu — which graphics card draws the agent: the per-app value Windows' Settings > System >
// Display > Graphics writes (spike S2, docs/spikes/s2-gpu-selection.md). It works for OpenGL and
// is read when the first context is made, so a change applies at the next start. Auto writes
// nothing and removes our value: Windows decides.

#include "app/config.hpp"
#include "core/error.hpp"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace app::gpu {

inline constexpr std::wstring_view kKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

// "GpuPreference=1;" (power saving) or "GpuPreference=2;" (high performance); none for Auto.
auto preference_value(config::Gpu gpu) -> std::optional<std::wstring>;
// Our GpuPreference entry in the value named `exe` under root\key; other entries Windows keeps
// there stay. Auto removes our entry (and the value, when nothing else is left).
auto write(HKEY root, std::wstring_view key, std::wstring_view exe, config::Gpu gpu) -> core::Result<void>;
auto read(HKEY root, std::wstring_view key, std::wstring_view exe) -> std::optional<std::wstring>;

} // namespace app::gpu
