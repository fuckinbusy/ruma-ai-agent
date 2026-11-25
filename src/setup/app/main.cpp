// RumaSetup-<version>.exe and uninstall.exe — one program (docs/specs/installer.md). With the
// package inside (resource PAYLOAD) it installs or updates; without it, it removes the folder it
// lies in, going on from a copy of itself in %TEMP% (an exe can't delete itself while it runs).
//   --no-gl: the plain Windows dialogs, as on a computer without OpenGL 4.5
// Its own DLLs (FreeType, HarfBuzz, miniz) travel inside it as resources: unpacked into its own
// temporary folder before their first use (delay-loaded) and removed when it ends.

#include "core/win.hpp"
#include "setup/app/window.hpp"
#include "setup/self.hpp"

#include <windows.h>

#include <delayimp.h>
#include <objbase.h>

#include <cwctype>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

auto own_resource(const wchar_t* name, const wchar_t* type) -> std::span<const std::byte> {
    HRSRC found = FindResourceW(nullptr, name, type);
    if (found == nullptr)
        return {};
    HGLOBAL loaded = LoadResource(nullptr, found);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (data == nullptr)
        return {};
    return {static_cast<const std::byte*>(data), SizeofResource(nullptr, found)};
}

std::vector<std::wstring> g_dlls; // the names unpacked, for unloading at the end

BOOL CALLBACK unpack_one(HMODULE, LPCWSTR type, LPWSTR name, LONG_PTR param) {
    if (IS_INTRESOURCE(name))
        return TRUE;
    const auto bytes = own_resource(name, type);
    const std::wstring file = setup::self::dll_file_name(name);
    const auto& dir = *reinterpret_cast<const std::filesystem::path*>(param);
    HANDLE h = CreateFileW((dir / file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        const bool whole = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
        CloseHandle(h);
        if (whole)
            g_dlls.push_back(file);
        else
            DeleteFileW((dir / file).c_str()); // a half DLL is worse than none: the load fails plainly
    }
    return TRUE;
}

// uninstall.exe from the program's folder: a copy in %TEMP% does the work.
auto relaunch_from_temp(const std::filesystem::path& exe, const setup::self::Startup& s) -> int {
    const auto dir = setup::self::own_temp(L"Uninstall", GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto copy = dir / L"uninstall.exe";
    if (!CopyFileW(exe.c_str(), copy.c_str(), FALSE))
        return 1;
    std::wstring line = L"\"" + copy.native() + L"\" \"" + std::wstring{setup::self::kFromTemp} + s.app_dir.native() + L"\"";
    if (s.no_gl)
        line += L" " + std::wstring{setup::self::kNoGl};
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        return 1;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

// The copy in %TEMP% can't delete itself either: a quiet cmd removes its folder once it has ended.
// Only a folder the setup names itself, directly in %TEMP%.
void remove_after_exit(const std::filesystem::path& dir) {
    if (!setup::self::is_own_temp(dir))
        return;
    std::wstring line = L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir.native() + L"\"";
    // CREATE_NO_WINDOW alone: cmd gets a console nobody sees, and ping shares it. With DETACHED_PROCESS
    // cmd had no console at all, so ping opened a window of its own (the user saw it after removing).
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring cmd = std::wstring{system} + L"\\cmd.exe";
    if (CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::vector<std::wstring_view> args;
    for (int i = 1; i < __argc; ++i)
        args.emplace_back(__wargv[i]);
    const auto exe = core::win::module_path();
    const auto payload = own_resource(L"PAYLOAD", RT_RCDATA);
    const auto start = setup::self::startup(args, !payload.empty(), exe.parent_path());
    if (start.relaunch)
        return relaunch_from_temp(exe, start);

    auto single = setup::self::acquire_single(start.smoke ? std::wstring_view{L"Local\\WinAiAgent.Setup.Smoke"}
                                                          : setup::self::kSingleName);
    if (!single) { // another setup runs: bring it to the front (Review Focus 5)
        const std::wstring cls{setup::self::kWindowClass};
        if (HWND other = FindWindowW(cls.c_str(), nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    const auto temp = setup::self::own_temp(L"Setup", GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    EnumResourceNamesW(nullptr, L"DLL", unpack_one, reinterpret_cast<LONG_PTR>(&temp));
    // DLLs come from System32 and the setup's own folder only — never from the folder it was started in
    // (Downloads collects DLLs of every kind): that folder is not searched at all.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    DLL_DIRECTORY_COOKIE own_dlls = AddDllDirectory(temp.c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const setup::app::Host host{
        .payload = payload, .app_dir = start.app_dir, .temp = temp, .uninstall = start.uninstall, .smoke = start.smoke};
    int code = start.no_gl ? setup::app::kNoGl : setup::app::run(host);
    if (code == setup::app::kNoGl)
        code = start.smoke ? 2 : setup::app::run_plain(host); // a check shows no dialog

    CoUninitialize();
    // The DLLs out of the process, then their folder; harfbuzz first, it holds freetype.
    for (const auto* name : {"harfbuzz.dll", "freetype.dll", "freetyped.dll", "miniz.dll"})
        __FUnloadDelayLoadedDLL2(name);
    if (own_dlls != nullptr)
        RemoveDllDirectory(own_dlls);
    const bool cleaned = setup::self::remove_own_temp(temp);
    if (start.smoke)
        return cleaned ? code : 3; // the check: its temporary folder must be gone
    if (!cleaned)
        remove_after_exit(temp);
    if (start.uninstall)
        remove_after_exit(exe.parent_path()); // the copy's own folder (never the program's: not in %TEMP%)
    return code;
}
