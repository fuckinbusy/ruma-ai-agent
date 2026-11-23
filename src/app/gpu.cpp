#include "app/gpu.hpp"

#include "core/mem/handle.hpp"

namespace app::gpu {
namespace {

struct KeyTraits {
    using type = HKEY;
    static auto invalid() noexcept -> HKEY { return nullptr; }
    static void close(HKEY k) noexcept { RegCloseKey(k); }
};

constexpr std::wstring_view kName = L"GpuPreference=";

// The value without its GpuPreference entry: Windows keeps other per-app flags in the same
// string, e.g. "SwapEffectUpgradeEnable=1;GpuPreference=2;" (review N-6).
auto without_preference(std::wstring_view value) -> std::wstring {
    std::wstring out;
    while (!value.empty()) {
        const auto end = value.find(L';');
        const auto entry = value.substr(0, end);
        if (!entry.empty() && !entry.starts_with(kName)) {
            out.append(entry);
            out += L';';
        }
        value = end == std::wstring_view::npos ? std::wstring_view{} : value.substr(end + 1);
    }
    return out;
}

} // namespace

auto preference_value(config::Gpu gpu) -> std::optional<std::wstring> {
    switch (gpu) {
    case config::Gpu::Integrated: return L"GpuPreference=1;";
    case config::Gpu::Discrete: return L"GpuPreference=2;";
    case config::Gpu::Auto: break;
    }
    return std::nullopt;
}

auto write(HKEY root, std::wstring_view key, std::wstring_view exe, config::Gpu gpu) -> core::Result<void> {
    const std::wstring key_z{key};
    const std::wstring exe_z{exe};
    std::wstring value = without_preference(read(root, key, exe).value_or(L""));
    if (const auto ours = preference_value(gpu)) {
        value += *ours;
    }
    if (value.empty()) { // nothing of ours, nothing of anyone's: no value at all
        const LSTATUS s = RegDeleteKeyValueW(root, key_z.c_str(), exe_z.c_str());
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) {
            return core::os_error(static_cast<std::uint32_t>(s), "gpu preference: delete");
        }
        return {};
    }
    HKEY raw = nullptr;
    LSTATUS s = RegCreateKeyExW(root, key_z.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr);
    if (s != ERROR_SUCCESS) {
        return core::os_error(static_cast<std::uint32_t>(s), "gpu preference: open key");
    }
    core::Handle<KeyTraits> k{raw};
    s = RegSetValueExW(k.get(), exe_z.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) {
        return core::os_error(static_cast<std::uint32_t>(s), "gpu preference: write");
    }
    return {};
}

auto read(HKEY root, std::wstring_view key, std::wstring_view exe) -> std::optional<std::wstring> {
    const std::wstring key_z{key};
    const std::wstring exe_z{exe};
    DWORD bytes = 0;
    if (RegGetValueW(root, key_z.c_str(), exe_z.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring out(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key_z.c_str(), exe_z.c_str(), RRF_RT_REG_SZ, nullptr, out.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

} // namespace app::gpu
