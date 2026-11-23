#include "tools/winsys/apps.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "windowsapp.lib")

namespace tools::winsys {
namespace {

constexpr std::wstring_view kUninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
constexpr std::wstring_view kUninstall32 = L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("");
}

auto is_guid(std::wstring_view s) -> bool {
    return s.size() == 38 && s.front() == L'{' && s.back() == L'}';
}

auto starts_with_ci(std::wstring_view s, std::wstring_view prefix) -> bool {
    return s.size() >= prefix.size() &&
           std::ranges::equal(s.substr(0, prefix.size()), prefix, [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

// "MsiExec.exe /X{GUID}" or "/I{GUID}": the product code.
auto msi_code_in(std::wstring_view command) -> std::wstring {
    if (!starts_with_ci(command, L"msiexec")) {
        return {};
    }
    const auto brace = command.find(L'{');
    if (brace == std::wstring_view::npos || brace + 38 > command.size()) {
        return {};
    }
    const auto code = command.substr(brace, 38);
    return is_guid(code) ? std::wstring{code} : std::wstring{};
}

void read_branch(const RegistryView& reg, Hive hive, std::wstring_view branch, std::vector<App>& out) {
    for (const auto& id : reg.subkeys(hive, branch)) {
        const auto key = std::wstring{branch} + L"\\" + id;
        const auto values = reg.values(hive, key);
        const auto get = [&](std::wstring_view name) -> const RegValue* {
            const auto it = std::ranges::find_if(values, [&](const RegValue& v) { return _wcsicmp(v.name.c_str(), std::wstring{name}.c_str()) == 0; });
            return it == values.end() ? nullptr : &*it;
        };
        const auto* name = get(L"DisplayName");
        if (name == nullptr || name->text.empty()) {
            continue; // updates and parts: Settings does not list them either
        }
        App a;
        a.name = utf8(name->text);
        a.machine_wide = hive == Hive::Machine;
        a.key = (hive == Hive::Machine ? L"HKLM\\" : L"HKCU\\") + key;
        if (const auto* v = get(L"DisplayVersion")) a.version = utf8(v->text);
        if (const auto* v = get(L"Publisher")) a.publisher = utf8(v->text);
        if (const auto* v = get(L"InstallDate")) a.date = utf8(v->text);
        if (const auto* v = get(L"UninstallString")) a.uninstall = v->text;
        if (const auto* v = get(L"QuietUninstallString")) a.quiet_uninstall = v->text;
        if (const auto* v = get(L"InstallLocation")) a.location = v->text;
        if (const auto* v = get(L"EstimatedSize"); v != nullptr && v->type == REG_DWORD && !v->text.empty()) {
            a.size = std::stoull(v->text) * 1024ULL; // the entry says KB
        }
        if (const auto* v = get(L"SystemComponent"); v != nullptr && v->text == L"1") a.system_component = true;
        const auto* wi = get(L"WindowsInstaller");
        const auto code = msi_code_in(a.uninstall);
        a.msi = (wi != nullptr && wi->text == L"1") || !code.empty();
        a.msi_code = is_guid(id) && a.msi ? id : code;
        out.push_back(std::move(a));
    }
}

void read_store(std::vector<App>& out) {
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    try {
        winrt::Windows::Management::Deployment::PackageManager pm;
        for (const auto& p : pm.FindPackagesForUser(winrt::hstring{})) {
            if (p.IsFramework() || p.IsResourcePackage() || p.IsBundle()) {
                continue; // frameworks and parts: never offered (spec § 4)
            }
            App a;
            a.store = true;
            a.name = winrt::to_string(p.DisplayName());
            if (a.name.empty()) {
                continue;
            }
            const auto v = p.Id().Version();
            a.version = std::to_string(v.Major) + "." + std::to_string(v.Minor) + "." + std::to_string(v.Build) + "." + std::to_string(v.Revision);
            a.publisher = winrt::to_string(p.PublisherDisplayName());
            a.key = std::wstring{p.Id().FamilyName()};
            out.push_back(std::move(a));
        }
    } catch (const winrt::hresult_error&) {
        // The Store's list could not be read: the desktop programs are still all there.
    }
}

} // namespace

auto apps(const RegistryView& reg, bool with_store) -> std::vector<App> {
    std::vector<App> out;
    read_branch(reg, Hive::Machine, kUninstall, out);
    read_branch(reg, Hive::Machine, kUninstall32, out);
    read_branch(reg, Hive::User, kUninstall, out);
    if (with_store) {
        read_store(out);
    }
    std::ranges::stable_sort(out, [](const App& a, const App& b) { return a.name < b.name; });
    return out;
}

auto store_apps() -> std::vector<App> {
    std::vector<App> out;
    read_store(out);
    return out;
}

auto uninstall_command(const App& app) -> std::pair<std::wstring, std::wstring> {
    if (app.msi && !app.msi_code.empty()) {
        return {L"msiexec.exe", L"/x " + app.msi_code}; // with its own window: no /q of any kind
    }
    std::wstring_view s{app.uninstall};
    while (!s.empty() && s.front() == L' ') {
        s.remove_prefix(1);
    }
    std::wstring program;
    std::wstring_view rest;
    if (!s.empty() && s.front() == L'"') {
        const auto close = s.find(L'"', 1);
        program = std::wstring{s.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1)};
        rest = close == std::wstring_view::npos ? std::wstring_view{} : s.substr(close + 1);
    } else {
        // Unquoted, spaces and all: up to its ".exe", else up to the first space.
        std::wstring lower{s};
        std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        const auto exe = lower.find(L".exe");
        const auto end = exe != std::wstring::npos ? exe + 4 : s.find(L' ');
        program = std::wstring{s.substr(0, end)};
        rest = end == std::wstring_view::npos ? std::wstring_view{} : s.substr(end);
    }
    while (!rest.empty() && rest.front() == L' ') {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && rest.back() == L' ') {
        rest.remove_suffix(1);
    }
    return {std::move(program), std::wstring{rest}};
}

auto wait_for_tree(std::uint32_t pid, std::stop_token stop, std::chrono::milliseconds timeout) -> bool {
    if (pid == 0) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<DWORD> watched{pid};
    for (;;) {
        // Who of them runs, and whom they started (a child seen once stays watched).
        DWORD alive = 0;
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
            std::vector<PROCESSENTRY32W> all;
            PROCESSENTRY32W e{.dwSize = sizeof(PROCESSENTRY32W)};
            for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
                all.push_back(e);
            }
            CloseHandle(snap);
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& p : all) {
                    if (std::ranges::find(watched, p.th32ParentProcessID) != watched.end() &&
                        std::ranges::find(watched, p.th32ProcessID) == watched.end() && p.th32ProcessID != 0) {
                        watched.push_back(p.th32ProcessID);
                        grew = true;
                    }
                }
            }
            for (const auto& p : all) {
                if (std::ranges::find(watched, p.th32ProcessID) != watched.end()) {
                    alive = p.th32ProcessID;
                    break;
                }
            }
        }
        if (alive == 0) {
            return true;
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // Until that one ends, or a quarter of a second: another may have started meanwhile.
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, alive); h != nullptr) {
            WaitForSingleObject(h, 250);
            CloseHandle(h);
        } else {
            Sleep(250);
        }
    }
}

} // namespace tools::winsys
