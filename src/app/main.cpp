// agent.exe — the desktop agent. Runs in the tray; Ctrl+Space opens the sheet.
//   agent.exe [--open] [--sim-speed=N] [--rest-animation=on|off] [--smoke [--show] [--rest-seconds=N]]
//   agent.exe --headless-measure [--rest-seconds=N] [--rest-animation=on|off]
//   agent.exe --diag-keys[=seconds] [--no-box]
//   --snapshot=<file> with --smoke or --headless-measure: the debug snapshot at the end
//   --m4=<dir under %TEMP%> with --smoke: milestone M4 on a copy of a desktop (see shell.hpp)
//   --m5=<dir under %TEMP%> with --smoke: milestone M5 on copies of Temp, Downloads, Desktop
//   --after-install=<x>,<y>: opened at once and at rest on the monitor with that point (the installer)
//   --smoke-exit with --smoke: the run ends by the installer's exit message
//   --scene=<name> with --smoke --show: an acceptance script of the motion rework (app/scene_run.hpp);
//   --frame-rate=<120|60|30> and --reduced-motion with --smoke: that rate, no motion but opacity

#include "app/diag_keys.hpp"
#include "app/shell.hpp"
#include "core/crash.hpp"
#include "core/win.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // --diag-keys[=seconds] [--no-box]: where the shortcut gets lost (see app/diag_keys.hpp).
    for (int i = 1; i < __argc; ++i) {
        const std::wstring_view arg = __wargv[i];
        if (arg.starts_with(L"--diag-keys")) {
            const double seconds = arg.size() > 12 ? std::wcstod(arg.substr(12).data(), nullptr) : 30.0;
            bool box = true;
            for (int j = 1; j < __argc; ++j) {
                box = box && std::wstring_view{__wargv[j]} != L"--no-box";
            }
            const std::filesystem::path dir = core::win::agent_data_dir();
            return app::diag::keys(seconds > 0 ? seconds : 30.0, dir / L"logs" / L"keys-diag.log", box);
        }
    }
    app::shell::Options options;
    for (int i = 1; i < __argc; ++i) {
        const std::wstring_view arg = __wargv[i];
        if (arg == L"--open") {
            options.open_at_start = true;
        } else if (arg == L"--smoke") {
            options.smoke = true;
        } else if (arg == L"--smoke-exit") {
            options.smoke_exit = true;
        } else if (arg.starts_with(L"--after-install=")) {
            options.after_install = app::shell::parse_point(arg.substr(16));
        } else if (arg.starts_with(app::shell::kReadyArg)) {
            options.ready_event = std::wstring{arg.substr(app::shell::kReadyArg.size())};
        } else if (arg.starts_with(app::shell::kGoArg)) {
            options.go_event = std::wstring{arg.substr(app::shell::kGoArg.size())};
        } else if (arg.starts_with(L"--debug-capture=")) {
            options.debug_capture = std::wstring{arg.substr(16)};
        } else if (arg.starts_with(L"--m4=")) {
            options.m4 = std::wstring{arg.substr(5)};
        } else if (arg.starts_with(L"--m5=")) {
            options.m5 = std::wstring{arg.substr(5)};
        } else if (arg == L"--sim") {
            options.sim = true;
        } else if (arg.starts_with(L"--replay=")) {
            options.replay = std::wstring{arg.substr(9)};
        } else if (arg.starts_with(L"--m7=")) {
            options.m7 = std::wstring{arg.substr(5)};
        } else if (arg.starts_with(L"--snapshot=")) {
            options.snapshot = std::wstring{arg.substr(11)};
        } else if (arg.starts_with(L"--scene=")) {
            for (const wchar_t c : arg.substr(8)) { // a script's name: plain letters
                options.scene.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
        } else if (arg.starts_with(L"--frame-rate=")) {
            options.frame_rate = static_cast<int>(std::wcstol(arg.substr(13).data(), nullptr, 10));
        } else if (arg == L"--reduced-motion") {
            options.reduced_motion = true;
        } else if (arg == L"--show") {
            options.show = true;
        } else if (arg == L"--headless-measure") {
            options.measure = true;
            options.smoke = true;
        } else if (arg == L"--open-idle") {
            options.open_idle = true;
        } else if (arg == L"--rest-animation=off") {
            options.rest_animation = false;
        } else if (arg == L"--rest-animation=on") {
            options.rest_animation = true;
        } else if (arg.starts_with(L"--rest-seconds=")) {
            options.rest_seconds = std::max(0.0, std::wcstod(arg.substr(15).data(), nullptr));
        } else if (arg.starts_with(L"--sim-speed=")) {
            options.sim_speed = std::wcstod(arg.substr(12).data(), nullptr);
            if (!(options.sim_speed > 0)) {
                options.sim_speed = 1.0;
            }
        }
    }
    if (options.measure && options.rest_seconds <= 0) {
        options.rest_seconds = 10;
    }
    if (options.smoke) {
        core::crash::quiet_failures(); // nobody watches a smoke run: no dialog on the desktop, exit code 3
    }
    return app::shell::run(options);
}
