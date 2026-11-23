#include "setup/system.hpp"

#include "core/agent_ids.hpp"

#include <windows.h>
#include <utility>

#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl_core.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#pragma comment(lib, "propsys.lib")

namespace setup {
namespace {

constexpr std::wstring_view kTestBranch = L"Software\\WinAiAgent-tests\\";

auto status(LSTATUS s, std::string_view what) -> core::Result<void> {
    if (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND)
        return {};
    return core::os_error(static_cast<std::uint32_t>(s), what);
}

// COM for the shell link on this thread, for one call.
class Com {
public:
    Com() : ok_{SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))} {}
    ~Com() {
        if (ok_)
            CoUninitialize();
    }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

private:
    bool ok_;
};

template <class T>
class Ptr {
public:
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() {
        if (p_)
            p_->Release();
    }
    auto operator->() const -> T* { return p_; }
    auto put() -> T** { return &p_; }
    [[nodiscard]] auto get() const -> T* { return p_; }

private:
    T* p_ = nullptr;
};

} // namespace

auto RegistrySystem::for_user() -> RegistrySystem {
    RegistrySystem s;
    s.ids_ = {std::wstring{core::agent_ids::kMutex}, std::wstring{core::agent_ids::kWindowClass}};
    return s;
}

RegistrySystem::RegistrySystem(std::wstring test_root, AgentIds ids) : root_{std::move(test_root)}, ids_{std::move(ids)} {
    // The fence: a test's registry is its own branch, never HKCU itself.
    if (!root_.starts_with(kTestBranch) || root_.size() <= kTestBranch.size())
        std::abort();
}

RegistrySystem::RegistrySystem(RegistrySystem&& o) noexcept : root_{std::move(o.root_)}, ids_{std::move(o.ids_)} {}

namespace {

// The hive and the path a key names: HKLM for "HKLM\…", HKCU otherwise; a test's (`root`) both
// inside its own branch.
auto where(const std::wstring& root, std::wstring_view key) -> std::pair<HKEY, std::wstring> {
    constexpr std::wstring_view kMachine = L"HKLM\\";
    if (!root.empty()) {
        return {HKEY_CURRENT_USER, root + L"\\" + std::wstring{key}};
    }
    if (key.starts_with(kMachine)) {
        return {HKEY_LOCAL_MACHINE, std::wstring{key.substr(kMachine.size())}};
    }
    return {HKEY_CURRENT_USER, std::wstring{key}};
}

} // namespace

auto RegistrySystem::set_value(std::wstring_view key, std::wstring_view name, const RegData& data) -> core::Result<void> {
    const auto [hive, k] = where(root_, key);
    const std::wstring n{name};
    LSTATUS s = ERROR_SUCCESS;
    if (const auto* text = std::get_if<std::wstring>(&data)) {
        s = RegSetKeyValueW(hive, k.c_str(), n.c_str(), REG_SZ, text->c_str(),
                            static_cast<DWORD>((text->size() + 1) * sizeof(wchar_t)));
    } else {
        const DWORD number = std::get<std::uint32_t>(data);
        s = RegSetKeyValueW(hive, k.c_str(), n.c_str(), REG_DWORD, &number, sizeof(number));
    }
    if (s != ERROR_SUCCESS)
        return core::os_error(static_cast<std::uint32_t>(s), "registry: write");
    return {};
}

auto RegistrySystem::get_value(std::wstring_view key, std::wstring_view name) -> std::optional<std::wstring> {
    const auto [hive, k] = where(root_, key);
    const std::wstring n{name};
    DWORD bytes = 0;
    constexpr DWORD kFlags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (RegGetValueW(hive, k.c_str(), n.c_str(), kFlags, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hive, k.c_str(), n.c_str(), kFlags, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

auto RegistrySystem::delete_value(std::wstring_view key, std::wstring_view name) -> core::Result<void> {
    const auto [hive, k] = where(root_, key);
    const std::wstring n{name};
    return status(RegDeleteKeyValueW(hive, k.c_str(), n.c_str()), "registry: delete a value");
}

auto RegistrySystem::key_exists(std::wstring_view key) -> bool {
    const auto [hive, k] = where(root_, key);
    HKEY h = nullptr;
    if (RegOpenKeyExW(hive, k.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

auto RegistrySystem::delete_key(std::wstring_view key) -> core::Result<void> {
    const auto [hive, k] = where(root_, key);
    if (auto r = status(RegDeleteTreeW(hive, k.c_str()), "registry: delete a key"); !r)
        return r;
    return status(RegDeleteKeyW(hive, k.c_str()), "registry: delete a key");
}

auto RegistrySystem::make_shortcut(const Shortcut& s) -> core::Result<void> {
    const Com com;
    Ptr<IShellLinkW> link;
    Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(file.put()))))
        return core::make_error(core::Code::Internal, "shortcut: the shell link is not available");
    const auto icon = s.icon.empty() ? s.target : s.icon;
    if (FAILED(link->SetPath(s.target.c_str())) || FAILED(link->SetArguments(s.args.c_str())) ||
        FAILED(link->SetWorkingDirectory(s.target.parent_path().c_str())) ||
        FAILED(link->SetIconLocation(icon.c_str(), 0)))
        return core::make_error(core::Code::Io, "shortcut: the target can't be set");
    if (!s.aumid.empty() || !s.activator.empty()) {
        Ptr<IPropertyStore> props;
        if (FAILED(link->QueryInterface(IID_PPV_ARGS(props.put()))))
            return core::make_error(core::Code::Internal, "shortcut: no property store");
        if (!s.aumid.empty()) {
            PROPVARIANT id{};
            const bool ok = SUCCEEDED(InitPropVariantFromString(s.aumid.c_str(), &id)) &&
                            SUCCEEDED(props->SetValue(PKEY_AppUserModel_ID, id));
            PropVariantClear(&id);
            if (!ok)
                return core::make_error(core::Code::Io, "shortcut: the app id can't be set");
        }
        if (!s.activator.empty()) {
            CLSID clsid{};
            PROPVARIANT v{};
            const bool ok = SUCCEEDED(CLSIDFromString(s.activator.c_str(), &clsid)) &&
                            SUCCEEDED(InitPropVariantFromCLSID(clsid, &v)) &&
                            SUCCEEDED(props->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, v));
            PropVariantClear(&v);
            if (!ok)
                return core::make_error(core::Code::Io, "shortcut: the activator can't be set");
        }
        if (FAILED(props->Commit()))
            return core::make_error(core::Code::Io, "shortcut: the properties can't be saved");
    }
    if (const HRESULT hr = file->Save(s.lnk.c_str(), TRUE); FAILED(hr))
        return core::os_error(static_cast<std::uint32_t>(HRESULT_CODE(hr)), "shortcut: can't save");
    return {};
}

auto RegistrySystem::shortcut_target(const std::filesystem::path& lnk) -> std::optional<std::filesystem::path> {
    if (GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES)
        return std::nullopt;
    const Com com;
    Ptr<IShellLinkW> link;
    Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(file.put()))) || FAILED(file->Load(lnk.c_str(), STGM_READ)))
        return std::nullopt;
    wchar_t target[4096]{};
    if (FAILED(link->GetPath(target, 4096, nullptr, SLGP_RAWPATH)))
        return std::nullopt;
    return std::filesystem::path{target};
}

auto RegistrySystem::agent_running() -> bool {
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, ids_.mutex.c_str())) {
        CloseHandle(m);
        return true;
    }
    return FindWindowW(ids_.window_class.c_str(), nullptr) != nullptr;
}

auto RegistrySystem::ask_agent_to_exit() -> bool {
    using namespace std::chrono_literals;
    // The process itself, not only its window and mutex: the agent still closes its log and unloads its
    // DLLs after they are gone, and its files can't be replaced or removed until it has ended.
    HANDLE process = nullptr;
    if (HWND w = FindWindowW(ids_.window_class.c_str(), nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != 0 && pid != GetCurrentProcessId())
            process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(w, core::agent_ids::kExitMessage, 0, 0);
    }
    const auto until = std::chrono::steady_clock::now() + 5s;
    bool gone = true;
    while (agent_running()) {
        if (std::chrono::steady_clock::now() >= until) {
            gone = false;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    if (process != nullptr) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        gone = gone && WaitForSingleObject(process, static_cast<DWORD>(std::max<long long>(0, left.count()))) == WAIT_OBJECT_0;
        CloseHandle(process);
    }
    return gone;
}

} // namespace setup
