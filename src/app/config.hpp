#pragma once

// app::config — the user's settings in %LOCALAPPDATA%\Agent\config.json (JSON, `//` comments
// allowed). Unknown keys, nested ones too, are kept and written back (a newer version's settings
// survive an older one). A bad value is reported and its default stays; a bad type never turns a
// safety default off. The old agent.ini (phase 3) is migrated once by migrate().

#include "ai/catalog.hpp"
#include "core/error.hpp"
#include "theme/system.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace app::config {

enum class Gpu : std::uint8_t { Auto, Integrated, Discrete };
enum class Backdrop : std::uint8_t { Acrylic, Solid }; // Solid: no blur (fallback, spike R1)

// 03-SECURITY § The advanced user mode: only by editing the file.
struct Advanced {
    bool allow_system_writes = false;
    bool allow_secrets_read = false;
    bool allow_permanent_delete = false;
};

// 03-SECURITY § blast radius, per task.
struct Limits {
    std::uint32_t max_files = 50;       // changed files before asking again
    std::uint32_t max_megabytes = 200;  // written before asking again
    std::uint32_t max_trash_batch = 100;
    std::uint32_t max_tree = 1000;      // objects in a moved or deleted folder before always asking
    std::uint32_t max_calls = 25;       // tool calls before "continue?"
    std::uint32_t think_seconds = 60;   // a thought this long: "Answer now" by it (phase 12 § 9)
};

// 03-SECURITY § Access levels: the panel "Why I'm asking" sets them, only ever stricter than
// the matrix — reads asked or not, creating and moving once per folder or always. Deletes and
// the system are always asked: they have no level.
enum class ReadLevel : std::uint8_t { Auto, Ask };
enum class ChangeLevel : std::uint8_t { Folder, Always };
struct Access {
    ReadLevel read = ReadLevel::Auto;
    ChangeLevel change = ChangeLevel::Folder;
};

// How the agent reaches the model (phase 6): the proxy Windows names ("system", its bypass list
// too), none, or one given as "http://host:port".
struct Network {
    std::string proxy = "system";
};

// The model (phase 7): which one, how long it thinks ("off", "low", "high", "max"), and what a
// task may spend before it asks "go on?". The tray's Model, Thinking and Budget per task.
struct Ai {
    std::string model{ai::catalog::default_model().id}; // a model's id in ai::catalog
    std::string thinking = "low";
    double budget_usd = 0.10;
};

// The animations' frame rate (motion rework; chosen, not 60 for good): the
// monitor's own, or at most 120, 60 or 30 a second. Motion depends on a frame's time, never on this.
enum class FrameRate : std::uint8_t { Monitor, Hz120, Hz60, Hz30 };

// The window's transparency: 0 .. 50 percent, a step of 5.
inline constexpr int kMostTransparency = 50;
inline constexpr int kTransparencyStep = 5;

struct Config {
    std::string hotkey = "Ctrl+Space";
    Gpu gpu = Gpu::Auto;
    bool over_fullscreen = false; // open the sheet over fullscreen apps too
    Backdrop backdrop = Backdrop::Acrylic;
    // Text size against the mockup's, on top of the DPI scale and Windows' own text size: a bit
    // larger by default, so the answer's 14 px are 15. 0.75 .. 2.
    float text_scale = 15.0f / 14.0f;
    // The window's surface: how much of the desktop shows through it, in percent - 0 opaque, at
    // most 50, a step of 5. Opaque by default (the UI update).
    int transparency = 0;
    // The resting handle's slow drift of light; off: a still strip, 0 frames.
    // Off by default: nothing moves while the agent does nothing.
    bool rest_animation = false;
    FrameRate frame_rate = FrameRate::Monitor;
    // The interface's theme (UI update, stage 1): as Windows' apps by default, or Dark, or Light.
    theme::Choice theme = theme::Choice::Auto;
    Advanced advanced;
    Access access;
    Limits limits;
    Network network;
    Ai ai;
    std::string document; // the JSON as read: what serialize() starts from, unknown keys included
};

struct Problem {
    int line = 0; // 1-based; 0 when the place is not known
    std::string message;
};

struct Parsed {
    Config config;
    std::vector<Problem> problems;
};

auto parse(std::string_view json) -> Parsed;
auto serialize(const Config& config) -> std::string;
// A missing file is not an error: it gives the defaults.
auto load(const std::filesystem::path& file) -> core::Result<Parsed>;
// Creates the directory; writes atomically.
auto save(const std::filesystem::path& file, const Config& config) -> core::Result<void>;

// dir\agent.ini (phase 3, `key = value` lines) -> dir\config.json, the old file renamed to
// agent.ini.bak. True when it migrated; false when there was nothing to do (no agent.ini, or a
// config.json already there).
auto migrate(const std::filesystem::path& dir) -> core::Result<bool>;
// The phase 3 format, for migrate(): unknown keys come back as strings.
auto parse_ini(std::string_view text) -> Parsed;

// "Ctrl+Space", "Alt+Shift+K", "Ctrl+Alt+F12": at least one modifier and one key.
struct Hotkey {
    unsigned modifiers = 0; // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    unsigned key = 0;       // virtual-key code
};
auto parse_hotkey(std::string_view text) -> std::optional<Hotkey>;
// Canonical spelling: modifiers in the order Ctrl, Alt, Shift, Win, then the key.
auto hotkey_name(const Hotkey& hotkey) -> std::string;

} // namespace app::config
