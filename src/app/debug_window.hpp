#pragma once

// app::debug::DebugWindow — the Debug build's debug window (Dear ImGui): performance per frame
// stage with graphs, memory per tag, the live log with filters, the sheet's state and settings,
// and "Save snapshot". Its own Win32 window and GL context, next to the sheet; F12 in the sheet
// or in the window toggles it (F12 cannot be a global hotkey: Windows keeps it for debuggers).
//
// Built only with WA_DEBUG_UI (Debug); Release has no ImGui at all. DebugView is always there,
// so the shell fills it the same way in both builds.

#include "app/config.hpp"
#include "app/debug_metrics.hpp"
#include "app/sheet_model.hpp"
#include "core/error.hpp"
#include "ui/text/atlas.hpp"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace app::debug {

struct DebugView {
    const Metrics& metrics;
    const sheet::Model& model;
    const config::Config& config;
    // Views of the shell's own strings: built for every debug frame, so nothing here is copied.
    std::optional<std::string_view> live_hotkey;
    double now = 0;
    double atlas_fill = 0;
    ui::text::GlyphAtlas::Stats atlas{};
    std::size_t revealed = 0; // answer bytes shown
    bool rest_motion = false;
    std::string_view gl_driver;
    std::wstring_view logs_dir; // where "Save snapshot" writes
};

#ifdef WA_DEBUG_UI
class DebugWindow {
public:
    // Hidden until toggle(); makes no context current for the caller when it returns.
    static auto create(HINSTANCE instance) -> core::Result<std::unique_ptr<DebugWindow>>;
    ~DebugWindow();
    DebugWindow(const DebugWindow&) = delete;
    auto operator=(const DebugWindow&) -> DebugWindow& = delete;

    void toggle();
    [[nodiscard]] auto visible() const noexcept -> bool;
    // One frame when visible (or `force`, for tests: drawn into the hidden window). Leaves the
    // debug window's context current: the caller makes its own current again.
    void frame(const DebugView& view, bool force = false);
    // Tab `tab` (0 Performance, 1 Memory, 2 Log, 3 State) drawn off-screen, width x height, into
    // a PNG: for the report and for looking at the window without showing it.
    auto capture(const DebugView& view, int tab, int width, int height, const std::filesystem::path& png)
        -> core::Result<void>;
    // The records the Log tab listed in its last frame, after its filters.
    [[nodiscard]] auto log_lines() const noexcept -> std::size_t;

    struct Impl;

private:
    explicit DebugWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace app::debug
