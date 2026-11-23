#include "tools/winsys/startup.hpp"

#include "core/win.hpp"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <atomic>
#include <cwctype>

namespace tools::winsys {
namespace {

constexpr std::wstring_view kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr std::wstring_view kRunOnce = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr std::wstring_view kRun32 = L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr std::wstring_view kPolicyRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run";
constexpr std::wstring_view kApproved = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
constexpr std::wstring_view kWinlogon = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
constexpr std::wstring_view kIfeo = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";
constexpr std::wstring_view kSilentExit = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\SilentProcessExit";
constexpr std::wstring_view kWindows = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
constexpr std::wstring_view kWindows32 = L"Software\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
constexpr std::wstring_view kAppCert = L"System\\CurrentControlSet\\Control\\Session Manager\\AppCertDlls";
constexpr std::wstring_view kActiveSetup = L"Software\\Microsoft\\Active Setup\\Installed Components";
constexpr std::wstring_view kActiveSetup32 = L"Software\\WOW6432Node\\Microsoft\\Active Setup\\Installed Components";
constexpr std::wstring_view kLsa = L"System\\CurrentControlSet\\Control\\Lsa";
constexpr std::wstring_view kSession = L"System\\CurrentControlSet\\Control\\Session Manager";
constexpr std::wstring_view kShellObjects = L"Software\\Microsoft\\Windows\\CurrentVersion\\ShellServiceObjectDelayLoad";

auto lower(std::wstring s) -> std::wstring {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

auto trim(std::wstring_view s) -> std::wstring_view {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
        s.remove_suffix(1);
    }
    return s;
}

auto expand(std::wstring_view s) -> std::wstring {
    const std::wstring in{s};
    const DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (n == 0) {
        return in;
    }
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(in.c_str(), out.data(), n);
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

// The first word of a command: quoted, or up to ".exe" (a path with spaces), or up to a space.
auto program_and_rest(std::wstring_view c) -> std::pair<std::wstring, std::wstring_view> {
    c = trim(c);
    if (c.empty()) {
        return {};
    }
    if (c.front() == L'"') {
        const auto end = c.find(L'"', 1);
        if (end == std::wstring_view::npos) {
            return {std::wstring{c.substr(1)}, {}};
        }
        return {std::wstring{c.substr(1, end - 1)}, c.substr(end + 1)};
    }
    const auto low = lower(std::wstring{c});
    if (const auto exe = low.find(L".exe"); exe != std::wstring::npos) {
        const auto end = exe + 4;
        if (end == low.size() || low[end] == L' ' || low[end] == L',' || low[end] == L'\t') {
            return {std::wstring{c.substr(0, end)}, c.substr(end)};
        }
    }
    const auto space = c.find_first_of(L" ,");
    return {std::wstring{c.substr(0, space)}, space == std::wstring_view::npos ? std::wstring_view{} : c.substr(space)};
}

auto approved_off(const RegistryView& reg, Hive hive, std::wstring_view which, std::wstring_view name) -> bool {
    const auto v = reg.value(hive, std::wstring{kApproved} + std::wstring{which}, name);
    // Task Manager writes 02 (on) or 03 (off) as the first byte; an odd one is off.
    return v && !v->bytes.empty() && (std::to_integer<unsigned>(v->bytes[0]) & 1U) != 0;
}

auto hive_prefix(Hive h) -> std::wstring {
    return h == Hive::Machine ? L"HKLM\\" : L"HKCU\\";
}

auto items(std::wstring_view multi) -> std::vector<std::wstring> {
    std::vector<std::wstring> out;
    std::size_t at = 0;
    while (at <= multi.size()) {
        const auto nl = multi.find(L'\n', at);
        const auto item = trim(multi.substr(at, nl == std::wstring_view::npos ? std::wstring_view::npos : nl - at));
        if (!item.empty()) {
            out.emplace_back(item);
        }
        if (nl == std::wstring_view::npos) {
            break;
        }
        at = nl + 1;
    }
    return out;
}

// A shortcut's target (COM on this thread for the call); empty when it has none.
auto shortcut_target(const std::filesystem::path& lnk) -> std::wstring {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    std::wstring out;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ))) {
                wchar_t target[MAX_PATH]{};
                if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                    out = expand(target);
                }
            }
            file->Release();
        }
        link->Release();
    }
    if (com) {
        CoUninitialize();
    }
    return out;
}

std::atomic<const Context*> g_context{nullptr};

} // namespace

auto place_name(Place p) -> std::string_view {
    switch (p) {
    case Place::Run: return "run";
    case Place::RunOnce: return "run once";
    case Place::Run32: return "run (32-bit)";
    case Place::StartupFolder: return "startup folder";
    case Place::CommonStartupFolder: return "startup folder (all users)";
    case Place::Task: return "scheduled task";
    case Place::Service: return "service";
    case Place::Wmi: return "WMI subscription";
    case Place::Winlogon: return "Winlogon";
    case Place::Ifeo: return "Image File Execution Options";
    case Place::AppInit: return "AppInit DLLs";
    case Place::ActiveSetup: return "Active Setup";
    case Place::Lsa: return "LSA packages";
    case Place::BootExecute: return "BootExecute";
    case Place::ShellObjects: return "shell service objects";
    case Place::Policies: return "policy Run";
    }
    return "?";
}

auto file_of(std::wstring_view command) -> std::wstring {
    const std::wstring expanded = expand(command); // `rest` looks into it
    auto [program, rest] = program_and_rest(expanded);
    if (program.empty()) {
        return {};
    }
    // rundll32 runs a DLL: that DLL is the program that matters.
    const auto name = lower(std::filesystem::path{program}.filename().wstring());
    if (name == L"rundll32.exe" || name == L"rundll32") {
        auto [dll, unused] = program_and_rest(rest);
        if (const auto comma = dll.find(L','); comma != std::wstring::npos) {
            dll.resize(comma);
        }
        return std::wstring{trim(dll)};
    }
    return program;
}

auto Folders::real() -> Folders {
    return {.user_startup = core::win::known_folder(FOLDERID_Startup).value_or(std::filesystem::path{}),
            .common_startup = core::win::known_folder(FOLDERID_CommonStartup).value_or(std::filesystem::path{})};
}

auto startup_entries(const RegistryView& reg, const Folders& folders) -> StartupList {
    StartupList out;
    const auto add = [&](Hive h, std::wstring_view key, Place place, std::wstring name, std::wstring command, bool enabled) {
        StartupEntry e{.name = std::move(name), .command = std::move(command), .file = {}, .place = place, .enabled = enabled,
                       .machine_wide = h == Hive::Machine, .impact_ms = std::nullopt, .key_or_folder = hive_prefix(h) + std::wstring{key}};
        e.file = file_of(e.command);
        out.entries.push_back(std::move(e));
    };
    for (const Hive h : {Hive::User, Hive::Machine}) {
        for (const auto& v : reg.values(h, kRun)) {
            add(h, kRun, Place::Run, v.name, v.text, !approved_off(reg, h, L"Run", v.name));
        }
        for (const auto& v : reg.values(h, kRunOnce)) {
            add(h, kRunOnce, Place::RunOnce, v.name, v.text, true);
        }
        for (const auto& v : reg.values(h, kRun32)) {
            add(h, kRun32, Place::Run32, v.name, v.text, !approved_off(reg, h, L"Run32", v.name));
        }
        for (const auto& v : reg.values(h, kPolicyRun)) {
            add(h, kPolicyRun, Place::Policies, v.name, v.text, true);
        }
        for (const wchar_t* name : {L"Shell", L"Userinit", L"Taskman"}) {
            if (const auto v = reg.value(h, kWinlogon, name)) {
                add(h, kWinlogon, Place::Winlogon, v->name, v->text, true);
            }
        }
    }
    // The startup folders: what is in them, a shortcut's target as its program.
    for (const auto& [folder, place, hive] :
         {std::tuple{folders.user_startup, Place::StartupFolder, Hive::User},
          std::tuple{folders.common_startup, Place::CommonStartupFolder, Hive::Machine}}) {
        std::error_code ec;
        if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
            continue;
        }
        for (const auto& item : std::filesystem::directory_iterator{folder, ec}) {
            const auto name = item.path().filename().wstring();
            if (!item.is_regular_file(ec) || lower(name) == L"desktop.ini") {
                continue;
            }
            const bool lnk = lower(item.path().extension().wstring()) == L".lnk";
            StartupEntry e{.name = name, .command = item.path().wstring(), .file = lnk ? shortcut_target(item.path()) : item.path().wstring(),
                           .place = place, .enabled = !approved_off(reg, hive, L"StartupFolder", name), .machine_wide = hive == Hive::Machine,
                           .impact_ms = std::nullopt, .key_or_folder = folder.wstring()};
            if (e.file.empty()) {
                e.file = item.path().wstring();
            }
            out.entries.push_back(std::move(e));
        }
    }
    // The machine's deeper places (phase 13a § 5.1): looked at, never changed (rule 2).
    const Hive m = Hive::Machine;
    for (const auto& exe : reg.subkeys(m, kIfeo)) {
        const auto key = std::wstring{kIfeo} + L"\\" + exe;
        if (const auto v = reg.value(m, key, L"Debugger"); v && !v->text.empty()) {
            add(m, key, Place::Ifeo, exe, v->text, true);
        }
    }
    for (const auto& exe : reg.subkeys(m, kSilentExit)) {
        const auto key = std::wstring{kSilentExit} + L"\\" + exe;
        if (const auto v = reg.value(m, key, L"MonitorProcess"); v && !v->text.empty()) {
            add(m, key, Place::Ifeo, exe, v->text, true);
        }
    }
    for (const auto key : {kWindows, kWindows32}) {
        if (const auto v = reg.value(m, key, L"AppInit_DLLs"); v && !trim(v->text).empty()) {
            add(m, key, Place::AppInit, L"AppInit_DLLs", v->text, true);
        }
    }
    for (const auto& v : reg.values(m, kAppCert)) {
        add(m, kAppCert, Place::AppInit, v.name, v.text, true);
    }
    for (const auto key : {kActiveSetup, kActiveSetup32}) {
        for (const auto& id : reg.subkeys(m, key)) {
            const auto sub = std::wstring{key} + L"\\" + id;
            if (const auto v = reg.value(m, sub, L"StubPath"); v && !trim(v->text).empty()) {
                add(m, sub, Place::ActiveSetup, id, v->text, true);
            }
        }
    }
    for (const wchar_t* name : {L"Authentication Packages", L"Notification Packages", L"Security Packages"}) {
        if (const auto v = reg.value(m, kLsa, name)) {
            for (auto& item : items(v->text)) {
                add(m, kLsa, Place::Lsa, name, std::move(item), true);
            }
        }
    }
    if (const auto v = reg.value(m, kSession, L"BootExecute")) {
        for (auto& item : items(v->text)) {
            add(m, kSession, Place::BootExecute, L"BootExecute", std::move(item), true);
        }
    }
    for (const auto& v : reg.values(m, kShellObjects)) {
        add(m, kShellObjects, Place::ShellObjects, v.name, v.text, true);
    }
    return out;
}

auto context() -> const Context& {
    if (const Context* c = g_context.load(); c != nullptr) {
        return *c; // a test's: the real one is never made then
    }
    static const Context real{};
    return real;
}

auto private_file() -> std::filesystem::path {
    const Context* c = g_context.load();
    return c != nullptr ? c->private_file : std::filesystem::path{};
}

void use_context(const Context* c) {
    g_context.store(c);
}

} // namespace tools::winsys
