#pragma once

// setup::app — the setup program's window (design/ui_installer): the glass window and, for the
// appearance and the launch, a click-through layer over its monitor (nothing dims the desktop).
// The work runs on a thread of its own; the window shows its progress. Without OpenGL 4.5 the
// installer says so in a plain dialog and uninstall.exe works through plain dialogs.

#include <cstddef>
#include <filesystem>
#include <span>

namespace setup::app {

struct Host {
    std::span<const std::byte> payload; // the package; empty in uninstall.exe
    std::filesystem::path app_dir;      // uninstall: the folder to remove
    std::filesystem::path temp;         // the run's own temporary folder (GL program cache, the license)
    bool uninstall = false;
    bool smoke = false; // --smoke: every phase drawn into windows never shown; no agent started
};

// The GL window until the user is done; the process exit code. kNoGl: no OpenGL 4.5 here.
inline constexpr int kNoGl = 100;
auto run(const Host& host) -> int;
// The plain dialogs (TaskDialog): the installer explains and leaves; uninstall.exe asks, removes, says so.
auto run_plain(const Host& host) -> int;

} // namespace setup::app
