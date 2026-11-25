#include "app/shell.hpp"
#include "app/spend.hpp"
#include "core/product.hpp"

#include "ai/catalog.hpp"
#include "app/backdrop.hpp"
#include "app/decode_jobs.hpp"
#include "app/drop_target.hpp"
#include "app/gpu.hpp"
#include "app/frame_rate.hpp"
#include "app/scene_run.hpp"
#include "app/fullscreen.hpp"
#include "app/history.hpp"
#include "app/focus.hpp"
#include "app/hotkeys.hpp"
#include "app/pacing.hpp"
#include "app/settings.hpp"
#include "app/config.hpp"
#include "app/debug_metrics.hpp"
#include "app/debug_window.hpp"
#include "app/events.hpp"
#include "app/instance.hpp"
#include "app/journal.hpp"
#include "app/keycheck.hpp"
#include "app/monitor.hpp"
#include "app/notify.hpp"
#include "app/present.hpp"
#include "app/placement.hpp"
#include "app/report.hpp"
#include "app/sheet_model.hpp"
#include "app/sheet_view.hpp"
#include "app/tray_icon.hpp"
#include "app/tray_menu.hpp"
#include "app/zorder.hpp"
#include "resource.h"
#include "ai/cassette.hpp"
#include "app/model_agent.hpp"
#include "app/sim.hpp"
#include "core/log.hpp"
#include "core/queue.hpp"
#include "core/time.hpp"
#include "core/utf8.hpp"
#include "core/version.hpp"
#include "core/agent_ids.hpp"
#include "core/win.hpp"
#include "security/audit.hpp"
#include "security/paths.hpp"
#include "security/secrets.hpp"
#include "tools/os.hpp"
#include "tools/winsys/look.hpp"
#include "ui/font_library.hpp"
#include "ui/gl/context.hpp"
#include "ui/gl/program_cache.hpp"
#include "ui/orb.hpp"
#include "ui/render/renderer.hpp"
#include "ui/text/atlas.hpp"
#include "ui/ui.hpp"

#include <windows.h>

#include <dwmapi.h>

#include <psapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace app::shell {
namespace {

// Windows' "Animation effects" (Settings → Accessibility → Visual effects): off, the window only
// fades (app::motion's reduced motion).
auto window_animations() -> bool {
    BOOL on = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) == FALSE || on != FALSE;
}

constexpr wchar_t kClassName[] = L"WinAiAgent.Sheet";
static_assert(std::wstring_view{kClassName} == core::agent_ids::kWindowClass); // the installer finds it by it
constexpr wchar_t kDimClassName[] = L"WinAiAgent.Dim";
constexpr BYTE kDimAlpha = 140; // the screen behind a question, as UAC dims it
constexpr UINT WM_APP_WAKE = WM_APP + 1; // the agent posted events
constexpr UINT WM_APP_OPEN = WM_APP + 2; // a second instance asks to open the sheet
constexpr UINT WM_APP_TRAY = WM_APP + 3; // tray icon notifications
constexpr UINT WM_APP_MONITOR = WM_APP + 4; // the monitor read the machine (only while the report is shown)
constexpr UINT WM_APP_TOAST = WM_APP + 5;   // a notification's button was clicked
constexpr UINT WM_APP_PICTURE = WM_APP + 8; // a pasted or dropped picture was decoded (a worker thread)
constexpr UINT WM_APP_KEY = WM_APP + 10;    // DeepSeek's verdict on a key: lParam owns a keycheck::Result
constexpr UINT WM_APP_GO = WM_APP + 11;     // the installer's orb has gone into the strip (--go-event)
constexpr UINT WM_APP_CAPTURE = WM_APP + 7; // screen.capture: wParam 1 hides the agent's windows from it, 0 shows them again
// How long opening the sheet waits for the window in front to say whether a password field has
// its focus: a program that never says held the sheet ~11 s, twice (2026-10-30).
constexpr std::chrono::milliseconds kPasswordAsk{300};
constexpr int kHotkeyId = 1;
constexpr UINT kTrayId = 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuExit = 2;
constexpr UINT kMenuDebug = 3; // Debug builds: the debug window

// The product icon (agent.rc) at a system size; a test exe has no such resource and gets the stock
// one. LR_SHARED: the system owns the handle.
auto app_icon(int metric_x, int metric_y) -> HICON {
    const HINSTANCE self = GetModuleHandleW(nullptr);
    if (auto* icon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                   GetSystemMetrics(metric_x), GetSystemMetrics(metric_y),
                                                   LR_SHARED)))
        return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

// Menu and message text: UTF-8 from the settings, wide for Win32.
auto wide(std::string_view s) -> std::wstring {
    auto w = core::utf8::to_wide(s);
    return w ? std::move(*w) : std::wstring{L"?"};
}

auto seconds() -> double {
    return static_cast<double>(core::time::now_ns()) / 1.0e9;
}

// A link in the answer, in the default browser: web addresses only (never a file or a program
// the model named).
// The model's clock (phase 8): a turn's time ("14:32") and how long it took. "14:32" fits a
// string's own buffer: no allocation, every frame.
void tick_clock(sheet::Model& m) {
    m.now = seconds();
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::array<char, 8> buf{};
    const auto r = std::format_to_n(buf.data(), buf.size(), "{}:{:02}", t.wHour, t.wMinute);
    const std::string_view clock{buf.data(), static_cast<std::size_t>(r.out - buf.data())};
    if (m.clock != clock) {
        m.clock.assign(clock);
    }
}

// The data panel's tools (phase 8). Copy: the rows on the clipboard (writing it needs no question,
// a decision made for it); Explorer: the set's folder, if it is one; CSV: a file where the user picks.
void copy_text(std::string_view text) {
    const auto done = tools::os::current().set_clipboard(text);
    WA_INFO("app", "panel copied to the clipboard: {} bytes, {}", text.size(), done ? "ok" : done.error().message);
}

void open_folder(std::string_view folder) {
    const auto w = core::utf8::to_wide(folder);
    if (!w) {
        return;
    }
    const DWORD attrs = GetFileAttributesW(w->c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        WA_WARN("app", "folder not opened: not a folder now");
        return;
    }
    const auto done = tools::os::current().open(*w);
    WA_INFO("app", "folder opened in Explorer: {}", done ? "yes" : "no");
}

void save_file(HWND owner, const std::string& name, const std::string& text) {
    using Microsoft::WRL::ComPtr;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = init == S_OK || init == S_FALSE;
    {
        ComPtr<IFileSaveDialog> dialog;
        const auto wide_name = core::utf8::to_wide(name);
        const bool xlsx = name.ends_with(".xlsx"); // the panel's workbook (phase 9), else its CSV
        const COMDLG_FILTERSPEC types[] = {xlsx ? COMDLG_FILTERSPEC{L"Excel workbook", L"*.xlsx"} : COMDLG_FILTERSPEC{L"CSV", L"*.csv"}};
        ComPtr<IShellItem> chosen;
        PWSTR path = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(dialog->SetFileTypes(1, types)) && SUCCEEDED(dialog->SetDefaultExtension(xlsx ? L"xlsx" : L"csv")) &&
            SUCCEEDED(dialog->SetFileName(wide_name ? wide_name->c_str() : L"data.csv")) && SUCCEEDED(dialog->Show(owner)) &&
            SUCCEEDED(dialog->GetResult(&chosen)) && SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            const std::wstring target{path};
            CoTaskMemFree(path); // mem-ok: the shell's string, given back as it asks
            // The dialog asked before overwriting; the file is replaced whole (written beside it, then moved).
            const auto written = tools::os::current().write_file(target, text, tools::os::WriteMode::Overwrite);
            WA_INFO("app", "{} saved: {} bytes, {}", xlsx ? "XLSX" : "CSV", text.size(), written ? "ok" : written.error().message);
        }
    }
    if (uninit) {
        CoUninitialize();
    }
}

void open_link(std::string_view url) {
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        WA_WARN("app", "link not opened: not a web address");
        return;
    }
    const auto w = core::utf8::to_wide(url);
    if (!w) {
        return;
    }
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w->c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    WA_INFO("app", "link opened in the browser: {}", result > 32 ? "yes" : "no");
}

auto process_age_ms() -> double {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    const auto ticks = [](FILETIME f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(now) - ticks(created)) / 10'000.0;
}

auto process_cpu_ms() -> double {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ticks = [](FILETIME f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000.0;
}

auto thread_cpu_ms() -> double {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [](FILETIME f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000.0;
}

auto monitor_at(POINT point) -> placement::Monitor {
    const HMONITOR mon = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi_x = 96, dpi_y = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    return {mi.rcMonitor, static_cast<float>(dpi_x) / 96.0f};
}

// Settings > Accessibility > Text size, 1 .. 2.25 (100 % when never set).
auto windows_text_scale() -> float {
    DWORD percent = 100;
    DWORD bytes = sizeof(percent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                     nullptr, &percent, &bytes) != ERROR_SUCCESS) {
        percent = 100;
    }
    return static_cast<float>(std::clamp<DWORD>(percent, 100, 225)) / 100.0f;
}

auto monitor_under_cursor() -> placement::Monitor {
    POINT cursor{};
    GetCursorPos(&cursor);
    return monitor_at(cursor);
}

class Shell {
public:
    explicit Shell(const Options& options) : options_(options) {}
    ~Shell() { tools::os::set_capture_hooks({}); } // they name this shell's window and fields
    Shell(const Shell&) = delete;
    auto operator=(const Shell&) -> Shell& = delete;

    auto start(const std::filesystem::path& data_dir) -> bool;
    auto loop() -> int;

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void frame();
    // `over_fullscreen`: the user asked for it there (a notification's button).
    // `at`: the monitor with that point (--after-install), not the one under the cursor.
    void open_sheet(const char* why, bool over_fullscreen = false, std::optional<POINT> at = std::nullopt);
    // Asked by the installer (core::agent_ids::kExitMessage): as the tray's Exit.
    void exit_requested();
    // --after-install: once the installer's orb is in the strip (await_installer), the sheet opens on
    // the installer's point with its usual opening; signal_ready tells the installer it is shown.
    void await_installer();
    // `held`: the installer holds our bar on the screen until we show it (its launch ran).
    void open_after_install(bool held);
    void signal_ready();
    // DeepSeek's verdict on the key typed (keycheck): kept in DPAPI, or the reason in the key's row.
    void key_checked(const keycheck::Result& result);
    // The tab's cards as notifications while a fullscreen app is in front (Task 12).
    void update_notifications();
    void take_toast_clicks();
    void close_sheet(const char* why);
    void set_open_style(bool open);
    void set_background_style(bool on);
    // Closed, only the sign's zone and its capsule take the mouse (the window's region); the
    // motion rework: the open sheet too, in its canvas — set twice a transition (placement::next_region).
    void update_region(const placement::Placement& p, const ui::layout::Box& panel);
    void set_region(const placement::Region& r);
    void clear_region();
    // Phase 10: the handle hides under a fullscreen app on its monitor, looked at once a second
    // (`now`); what it would say comes as notifications meanwhile.
    void update_handle_hidden(double now);
    void watch_strip(double now);
    bool paused_ = false; // stage 4d-2: the tray's Pause agent (not kept past a restart)
    // The user's text size (config.json text_scale) on top of the Windows one.
    void apply_text_scale();
    void apply_transparency();
    // The interface's theme (UI update, stage 1, task 4): config.json's choice, and Windows' apps'
    // mode under "as Windows" (read only). `at_once`: at start; otherwise it changes on quick.
    void apply_theme(bool at_once);
    // Windows' "Animation effects" (MOTION_SPEC § 3): the painter and the tray menu's Ui alike.
    void apply_reduced_motion();
    // The animations' frame rate (motion rework): the sheet's swap interval from config.json's
    // frame_rate and the monitor's rate, read again when the agent's monitor changes.
    void apply_frame_rate();
    // Seconds between two frames at the rate chosen (1/60 while the monitor's rate is not known).
    [[nodiscard]] auto frame_period() const noexcept -> double;
    void apply_placement(const placement::Placement& p);
    void react(const sheet::Reaction& r);
    // The model choice made in the sheet (design/ui_model): config.json, the agent's settings.
    void choose_model(sheet::ModelChoice choice);
    // The screen behind the sheet dims while it asks (sheet::wants_dim), on the sheet's monitor.
    void update_dim();
    void send(events::Command command);
    void on_key(WPARAM vk);
    void on_char(wchar_t unit);
    // Ctrl+V; `as_text` (Ctrl+Shift+V): text into the field whatever its length.
    void paste(bool as_text = false);
    // Phase 9: pictures pasted or dropped are decoded on a worker; WM_APP_PICTURE puts them in.
    void decode_picture(std::string path); // empty: the clipboard's
    void take_pictures();
    // Files brought to the window (dropped, dragged in, picked with the paperclip): their chips,
    // their pictures decoded and their details read in the background (phase 10).
    void attach(std::vector<std::string> paths);
    void read_details(std::string path);
    struct Decoded {
        std::string path; // empty: pasted
        std::optional<tools::ImageOut> image;
        std::string details; // "48 pages · 2.3 MB" (phase 10)
    };
    void drain_agent();
    auto register_hotkey() -> bool;
    void add_tray_icon();
    // The tray's menu in the agent's look (design/ui_model screen 8, app::tray_menu): its own window
    // on the sheet's GL context, closed by Esc, a click elsewhere or a choice that ends it. The
    // system menu is kept for a build without the GL window (show_system_tray_menu).
    // The tray icon is the orb (design/ui_model): the model's material, breathing while it works.
    void update_tray_icon(double now);
    void show_tray_menu();
    void show_system_tray_menu();
    void tray_choice(UINT chosen, HWND before);
    void layout_tray_menu();
    void unfold_tray_menu(double now); // its lists as they show at `now`: rows moved in place, nothing allocated
    void draw_tray_menu();
    void close_tray_menu();
    void tray_menu_click(std::size_t row);
    void render_menu_orbs();
    auto handle_menu(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
    static LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    auto build_tray_menu() -> HMENU; // the caller destroys it
    // A Settings item of the tray menu: change, apply, save.
    void apply_setting(unsigned id);
    // Tray -> Settings -> Audit log…: the newest records and whether the chain holds, in the sheet.
    void show_audit();
    // Tray -> Settings -> Change log…: changes.jsonl as the log screen.
    void show_change_log();
    // Tray -> Settings -> Rules and memory… (phase 12 § 6): the list in the panel; its rules row
    // opens agent.md; its Forget all asks first.
    void show_memory();
    // Stage 4d-1: the settings mode — opened on a section (or the last), its items and lists made
    // from config.json and the agent's data; made again after a change.
    void open_settings(std::optional<sheet::Section> section = std::nullopt);
    void refresh_settings(bool lists = true); // `lists` false: the items only (a setting changed)
    // Stage 4d-2: an answer's cost added to this month's (data\spend.json), the settings' copy too.
    void keep_spent(const events::Spent& spent);
    void open_rules();
    void forget_all();
    [[nodiscard]] auto change_log() const -> events::Log;
    // Swaps the global hotkey; keeps the old one (and says so) when the new one is taken.
    auto change_hotkey(const std::string& to) -> bool;
    std::filesystem::path data_dir_;
    std::filesystem::path logs_dir_; // data_dir_/logs: the debug window views it every frame
    bool m4_refused_ = false;        // --m4 named a folder outside %TEMP%
    double memory_sampled_at_ = -1.0; // debug::Metrics::add_memory, once a second while awake
    void ensure_orb(float scale);
    void fit_to_monitor();
    // --smoke: drives the sheet like a user would, through the model; returns the exit code
    // once finished (or -1 while still running).
    auto smoke_step() -> int;
    // --scene: an acceptance script of the motion rework (see Options::scene).
    auto scene_step() -> int;
    void scene_mouse(float x, float y);
    const scene_run::Script* scene_ = nullptr;
    std::size_t scene_at_ = 0;      // the step waited for
    double scene_since_ = 0;        // when the step before it was done
    double scene_ready_ = -1;       // since when this step's target is there (-1: not yet)
    int scene_click_ = 0;           // a click's way: 0 not begun, 1 the mouse is on it, 2 pressed, 3 released
    double scene_click_at_ = 0;
    std::uint64_t scene_click_frame_ = 0;
    // Windows' animation effects, or --reduced-motion of a smoke run.
    [[nodiscard]] auto animations_on() const -> bool;
    // --m4: the milestone run (see Options::m4).
    auto m4_step() -> int;
    // --m5: the milestone run (see Options::m5).
    auto m5_step() -> int;
    // --smoke --replay: the real agent on a recording (see Options::replay).
    auto replay_step() -> int;
    // --smoke --m7: the real agent with DeepSeek on a fenced copy (see Options::m7).
    auto m7_step() -> int;
    double m7_stage_started_ = 0;
    // The agent there is: the real one (phase 7) or the simulated one.
    void to_agent(events::Command command);
    [[nodiscard]] auto agent_last_task() const -> std::uint64_t;
    // The tray's Model, Thinking and Budget as the real agent takes them.
    [[nodiscard]] auto model_settings() const -> model::Settings;
    std::size_t m5_temp_ = 0, m5_downloads_ = 0, m5_installers_ = 0;
    std::string m5_left_out_;

    Options options_;
    config::Config config_;
    ui::gl::Window window_;
    ui::gl::Window dim_; // made at the first question
    bool dimmed_ = false;
    BYTE dim_alpha_ = 0; // the dim window's layered alpha as set last
    std::optional<ui::gl::Context> context_;
    std::optional<ui::gl::ProgramCache> cache_;
    std::optional<ui::render::Renderer> renderer_;
    std::optional<ui::FontLibrary> library_;
    std::optional<ui::Fonts> fonts_;
    std::optional<ui::text::GlyphAtlas> atlas_;
    std::optional<ui::Ui> ui_;
    // The tray menu's own (its renderer, fonts and atlas shared): its frames, up to 60 a second
    // while open, never reset the sheet's hover fades, tooltip and cursor (the review).
    std::optional<ui::Ui> menu_ui_;
    std::optional<ui::Orb> orb_;
    std::optional<ui::Orb> card_orb_; // the picker's small orbs (design/ui_model)
    // The tray's own menu (design/ui_model screen 8).
    ui::gl::Window menu_window_;
    HDC menu_dc_ = nullptr;
    std::vector<tray_menu::Row> menu_rows_;
    std::optional<std::size_t> menu_hover_;
    tray_menu::Unfold menu_settings_; // its lists unfolding
    tray_menu::Unfold menu_budget_;
    bool menu_unfolding_ = false;     // a list moved in the last pass: one more layout when it stops
    bool menu_lost_ = false;          // it goes because its focus went (a click elsewhere, its icon's press)
    double menu_lost_at_ = -1.0e9;    // when it went so: that click's release does not open it again
    double tray_pressed_at_ = -1.0e9; // the last right press on the tray icon (tray_menu::reopens)
    double menu_drawn_at_ = -1.0e9;   // its last frame: while open, its header's orb moves (60 a second at most)
    double menu_shown_at_ = -1.0e9;   // when it was shown: it comes on quick from then
    int monitor_hz_ = 0;              // the agent's monitor's refresh rate (0: not known)
    int swap_interval_ = 1;           // the sheet's: a frame every n-th vertical sync
    bool menu_tracking_ = false; // TrackMouseEvent asked: the leave comes
    HWND menu_before_ = nullptr;  // the user's app before it, to return to
    RECT menu_icon_{};            // the tray icon it opened by
    float menu_scale_ = 1.0f;
    RECT menu_work_{};               // the work area it is placed in (its glass placed in its window as it unfolds)
    float menu_top_ = 0;            // its glass's top in its window (logical): the window keeps the unfolded height
    LONG menu_window_h_ = 0;        // its window's height (physical): tray_menu::window_height, never changed while shown
    std::array<std::optional<ui::render::Target>, 2> menu_orbs_; // Flash's and Pro's, at the level chosen
    std::string_view menu_orbs_theme_;                            // the theme whose light they were made in
    // The tray icon's orb (design/ui_model): its breath made once per choice, then shown icon by icon.
    tray_icon::Animator icon_animator_;
    std::vector<std::uint8_t> icon_raw_; // the small orb read back for a pose of the icon, its room kept
    float orb_scale_ = 0;
    std::optional<sheet::Painter> painter_;
    sheet::Model model_;
    std::optional<core::Mailbox<events::Event>> mailbox_;
    std::optional<history::Writer> history_;
    bool history_recalled_ = false; // Up has brought the questions of earlier sessions in
    // --m4: the tools' Recycle Bin is a folder inside the fenced copy (an undo trashes the folders
    // the task made; the user's bin is never touched). Outlives the agent.
    std::optional<tools::os::FolderBin> m4_bin_;
    // The machine's last minute (Task 10): read once a second on its own thread; the graphics card
    // and the wake-ups only while the report is on screen.
    std::optional<monitor::Monitor> meter_;
    std::unique_ptr<notify::Notifier> notifier_; // Windows' own; a recording one in the smoke run
    notify::Courier courier_;
    std::mutex toast_mutex_;
    std::vector<std::wstring> toast_clicks_; // under toast_mutex_: from the activator's COM thread
    bool report_shown_ = false;
    double usage_at_ = 0;          // the detailed mode's foot: when the agent's usage was last measured
    std::uint64_t usage_cpu_ = 0;
    void measure_usage(double now);
    void update_monitor();
    std::optional<sim::Agent> agent_; // after the mailbox: stopped first
    std::optional<model::Agent> model_agent_; // the real one (phase 7); one of the two is made
    std::vector<events::Event> drained_;

    placement::Monitor monitor_;
    placement::Placement placed_{};
    bool open_style_ = false;
    bool background_style_ = false; // closed, the tab or its cards shown: they take the mouse, never the focus
    placement::Region region_{};    // the window's region while region_on_
    bool region_on_ = false;
    ui::layout::Box open_box_{}; // the open sheet itself, without its shadow's room (logical px in the window)
    ui::layout::Box open_to_{};  // …and where it is going
    bool smoke_broken_ = false;  // a frame's own check failed (the smoke run ends with it)
    RECT panel_on_screen_{};        // the shape last drawn, in screen pixels (the canvas is larger)
    bool handle_hidden_ = false;    // under a fullscreen app (update_handle_hidden)
    double handle_checked_at_ = -1.0;
    // What stands over the resting window, read once a second and logged when it changes (the user,
    // 2026-10-28: the strip sometimes vanished until an open and a close; app::zorder).
    HWND strip_cover_ = nullptr;
    bool strip_shown_ = true;
    double strip_checked_at_ = -1.0;
    void field_moved(std::size_t from, bool shift);
    // Clicks in a row (WM_LBUTTONDOWN): the last one's time and place, how many so far.
    LONG last_click_at_ = 0;
    int last_click_x_ = -100000;
    int last_click_y_ = -100000;
    int click_run_ = 1;
    std::string selection_text_; // Ctrl+C's text, its room kept
    HWND previous_foreground_ = nullptr;
    std::atomic<HWND> before_open_{nullptr}; // previous_foreground_, for screen.capture's "active" (a tool's thread)
    std::atomic<bool> password_at_open_{false}; // a password field had its focus then (window.look, final review I5)
    bool hotkey_ = false;
    std::optional<std::string> live_hotkey_; // the shortcut actually registered (review N-1)
    auto registrar() -> hotkeys::Registrar;
    std::uint64_t answered_task_ = 0; // the last task whose answer is in the history (review N-9)
    bool config_writable_ = true;     // false when config.json exists but could not be read
    const UINT taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated"); // Explorer restarted
    bool tray_ = false;
    bool tracking_mouse_ = false;
    wchar_t high_surrogate_ = 0;
    ui::Input input_;
    bool wants_frame_ = true;
    sheet::FrameNeed need_ = sheet::FrameNeed::Full; // after the last frame: what the next one waits for
    pacing::Timer pacer_; // the steady frames' wait
    bool first_frame_logged_ = false;
    bool quit_ = false;
    bool exit_asked_ = false;  // the installer's exit message ended the run (--smoke-exit checks it)
    bool exit_posted_ = false; // --smoke-exit: the run asked itself
    bool ready_pending_ = false; // --ready-event: set after the first open frame is shown
    bool waiting_go_ = false;    // --go-event: the installer's orb is still flying
    std::string pending_key_;    // the key DeepSeek is being asked about (wiped when it answers)
    int smoke_stage_ = 0;
    double rest_started_ = 0, rest_cpu_ = 0, rest_thread_cpu_ = 0;
    double rest_work_ = 0, rest_swap_ = 0; // wall time in frame(): until the swap, the swap
    std::uint64_t rest_frames_ = 0;
    std::uint64_t rest_ticks_ = 0, rest_dues_ = 0; // resting looks, and those that drew
    // The resting motion pauses while a fullscreen app, presentation or lock screen is up;
    // asked at most once a second.
    double rest_checked_at_ = -1.0e9;
    bool rest_allowed_ = true;
    auto rest_allowed(double now) -> bool;
    double smoke_started_ = 0;
    std::uint64_t frames_ = 0;
    // Phase 9: the frame kept after a full one while only the slow orb moves (Painter::keep_frame).
    bool frame_kept_ = false;
    double kept_at_ = 0;
    int kept_w_ = 0, kept_h_ = 0;
    float kept_scale_ = 1;
    // A frame of the orb alone, on the kept one: true when it was drawn.
    auto orb_frame(double now) -> bool;
    debug::Metrics metrics_;
#ifdef WA_DEBUG_UI
    std::unique_ptr<debug::DebugWindow> debug_; // made on the first F12
    double debug_frame_at_ = 0;
#endif
    void toggle_debug();
    void draw_debug(); // at most 30 frames a second while it shows
    // The snapshot (--snapshot=<file>) when a smoke or measuring run ends.
    void write_snapshot();
    // While the answer streams: how often frames are shown (logged when it ends).
    struct StreamStats {
        bool on = false;
        double start = 0, last = 0, longest = 0;
        double longest_work = 0, longest_swap = 0; // CPU side of one frame: until swap, the swap
        int hitches = 0;                           // gaps over 2.5 refresh periods after a moving frame
        bool moving = false;
        std::uint64_t frames = 0;
    } stream_stats_;
    void note_stream_frame(double now, double work, double swap);

    std::mutex decoded_mutex_;
    std::vector<Decoded> decoded_; // under decoded_mutex_: from the decoders
    bool ole_ = false;                       // OleInitialize on the window's thread (drag and drop)
    std::optional<DropTarget> drop_target_;  // revoked before the window goes
    Workers decoders_; // last: joined first when the shell goes
};

LRESULT CALLBACK Shell::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->window_) {
        return self->handle(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

auto Shell::start(const std::filesystem::path& data_dir) -> bool {
    data_dir_ = data_dir;
    logs_dir_ = data_dir / L"logs";
    // A smoke run leaves the user's files as they are; the real start migrates.
    if (auto m = options_.smoke ? core::Result<bool>{false} : config::migrate(data_dir); m && *m) {
        WA_INFO("app", "agent.ini moved to config.json (the old file is agent.ini.bak)");
    } else if (!m) {
        WA_WARN("app", "agent.ini not migrated: {}", m.error().message);
    }
    auto loaded = config::load(data_dir / "config.json");
    if (loaded) {
        config_ = loaded->config;
        for (const auto& p : loaded->problems) {
            WA_WARN("app", "config.json line {}: {}", p.line, p.message);
        }
    } else {
        // Saving the defaults over a file we could not read would lose it (review N-7).
        WA_WARN("app", "config.json not read: {}; settings changes will not be saved", loaded.error().message);
        config_writable_ = false;
    }
    if (options_.smoke) { // a smoke run's choices (a scene picks Pro and Max) hold until it ends: the user's file stays (review I3)
        config_writable_ = false;
    }
    const auto& a = config_.advanced;
    model_.unsafe = a.allow_system_writes || a.allow_secrets_read || a.allow_permanent_delete;
    model_.levels = agent::levels_of(config_.access);
    model_.think_seconds = config_.limits.think_seconds; // "Answer now" after a thought this long (phase 12 § 9)
    if (model_.unsafe) {
        WA_WARN("app", "advanced mode: system writes {}, secrets read {}, permanent delete {}", a.allow_system_writes,
                a.allow_secrets_read, a.allow_permanent_delete);
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon(SM_CXICON, SM_CYICON);
    wc.hIconSm = app_icon(SM_CXSMICON, SM_CYSMICON);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    monitor_ = monitor_under_cursor();
    placed_ = placement::canvas(monitor_); // made at the agent's largest (motion rework, task 5)
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                    WS_EX_TRANSPARENT,
                                kClassName, WA_PRODUCT_WIDE, WS_POPUP, placed_.window.left, placed_.window.top,
                                placed_.window.right - placed_.window.left,
                                placed_.window.bottom - placed_.window.top, nullptr, nullptr, instance, this);
    if (hwnd == nullptr) {
        WA_ERROR("app", "CreateWindowExW failed: {}", GetLastError());
        return false;
    }
    window_ = ui::gl::Window{hwnd};
    metrics_.mark_startup("window", process_age_ms());
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    backdrop::prepare(hwnd);

    // The graphics card is chosen when the first context is made (spike S2). Auto leaves Windows'
    // own setting for the agent alone.
    if (config_.gpu != config::Gpu::Auto) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (auto r = gpu::write(HKEY_CURRENT_USER, gpu::kKey, exe, config_.gpu); !r) {
            WA_WARN("app", "graphics card preference: {}", r.error().message);
        }
    }

#ifdef NDEBUG
    constexpr bool kDebugGl = false;
#else
    constexpr bool kDebugGl = true;
#endif
    auto context = ui::gl::Context::create(hwnd, kDebugGl);
    if (!context) {
        WA_ERROR("app", "OpenGL: {}", context.error().message);
        return false;
    }
    context_.emplace(std::move(*context));
    metrics_.mark_startup("gl context", process_age_ms());
    WA_INFO("app", "GL: {}", context_->driver_id());
    apply_frame_rate();
    cache_.emplace(data_dir / "shadercache", context_->driver_id());
    auto renderer = ui::render::Renderer::create(*cache_);
    auto library = ui::FontLibrary::create();
    if (!renderer || !library) {
        WA_ERROR("app", "renderer or FreeType failed");
        return false;
    }
    renderer_.emplace(std::move(*renderer));
    library_.emplace(std::move(*library));
    auto fonts = ui::Fonts::system(*library_);
    auto atlas = ui::text::GlyphAtlas::create(2048);
    if (!fonts || !atlas) {
        WA_ERROR("app", "fonts or glyph atlas failed");
        return false;
    }
    fonts_.emplace(std::move(*fonts));
    atlas_.emplace(std::move(*atlas));
    metrics_.mark_startup("renderer and fonts", process_age_ms());
    ui_.emplace(*renderer_, *atlas_, *fonts_);
    menu_ui_.emplace(*renderer_, *atlas_, *fonts_);
    apply_text_scale();
    apply_transparency();
    apply_theme(true);
    ensure_orb(monitor_.scale);
    if (!orb_) {
        return false;
    }
    painter_.emplace(*ui_, *fonts_, *orb_);
    if (card_orb_) {
        painter_->set_card_orb(&*card_orb_);
    }
    painter_->set_rest_motion(options_.rest_animation.value_or(config_.rest_animation));
    apply_reduced_motion();
    apply_theme(true); // again, now that the painter is there: the sign's theme
    fit_to_monitor();

    mailbox_.emplace([hwnd] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });
    // Files dragged onto the window (phase 10): the drop zone while they are over it, Windows' own
    // picture of them at the cursor; without OLE, dropped files still attach (phase 9).
    ole_ = SUCCEEDED(OleInitialize(nullptr));
    if (ole_) {
        drop_target_.emplace(hwnd, DropTarget::Calls{.enter =
                                                         [this](bool files) {
                                                             if (files && !model_.open) {
                                                                 open_sheet("files dragged over");
                                                             }
                                                             model_.drag_over = files && model_.open;
                                                             wants_frame_ = true;
                                                         },
                                                     .leave =
                                                         [this] {
                                                             model_.drag_over = false;
                                                             wants_frame_ = true;
                                                         },
                                                     .drop = [this](std::vector<std::string> paths) { attach(std::move(paths)); }});
    }
    if (!drop_target_ || !drop_target_->registered()) {
        DragAcceptFiles(hwnd, TRUE);
    }
    // screen.capture (a tool's thread): the agent's windows are hidden from the picture by the
    // window's own thread, and only while it is taken; a hung window is not waited for long.
    tools::os::set_capture_hooks(
        {.exclude = [hwnd](bool hide) {
             DWORD_PTR done = 0;
             SendMessageTimeoutW(hwnd, WM_APP_CAPTURE, hide ? 1 : 0, 0, SMTO_ABORTIFHUNG, 1000, &done);
         },
         .active = [this] { return static_cast<void*>(before_open_.load()); },
         .agent = [hwnd] { // where the sheet is: the panel outlines its place on the picture (phase 10)
             RECT r{};
             return GetWindowRect(hwnd, &r) ? std::array<long, 4>{r.left, r.top, r.right, r.bottom} : std::array<long, 4>{};
         },
         .password_at_open = [this] { return password_at_open_.load(); }});
    // A smoke run keeps its conversations apart from the user's.
    history_.emplace(data_dir / (options_.smoke ? L"history-smoke" : L"history"));
    // Real tools behind the policy (phase 4) in a real run; a smoke run plays the scripted
    // scenarios it checks.
    meter_.emplace(monitor::machine(agent::process_elevated()));
    meter_->start([hwnd] { PostMessageW(hwnd, WM_APP_MONITOR, 0, 0); });
    if (options_.smoke) {
        notifier_ = std::make_unique<notify::RecordingNotifier>(); // nothing reaches Windows
    } else {
        notifier_ = std::make_unique<notify::WindowsNotifier>([this, hwnd](std::wstring args) {
            {
                std::scoped_lock lock{toast_mutex_};
                toast_clicks_.push_back(std::move(args));
            }
            PostMessageW(hwnd, WM_APP_TOAST, 0, 0);
        });
    }
    std::optional<agent::Host> host;
    const std::wstring& copy = !options_.m4.empty() ? options_.m4 : options_.m5; // M4 or M5: a copy to work on
    if (options_.smoke && !copy.empty()) {
        // M4 and M5 answer their own questions: only ever on a folder under %TEMP%, behind a fence on it.
        const auto dir = security::paths::canonical(copy, std::filesystem::temp_directory_path());
        const auto temp = security::paths::canonical(std::filesystem::temp_directory_path().wstring(), L"C:\\");
        if (!dir || !temp || !dir->exists || !security::paths::is_within(dir->path, temp->path) ||
            security::paths::is_within(temp->path, dir->path)) {
            WA_ERROR("app", "m4/m5: {} is not a folder inside %TEMP%: refused", core::utf8::to_utf8(copy).value_or("?"));
            m4_refused_ = true;
        } else {
            tools::os::fence::enable(dir->path);
            m4_bin_.emplace(std::filesystem::path{dir->path} / L".agent-bin");
            tools::os::use(&*m4_bin_);
            const std::filesystem::path root{dir->path};
            const bool m5 = options_.m4.empty();
            host = agent::Host{.desktop = m5 ? root / L"Desktop" : root, .audit = root / L"audit.jsonl",
                               .sort_desktop = true, .journal = root / L"changes.jsonl", .monitor = &*meter_,
                               .temp = m5 ? root / L"Temp" : std::filesystem::path{},
                               .downloads = m5 ? root / L"Downloads" : std::filesystem::path{}};
            WA_INFO("app", "m4 on {} (fenced)", core::utf8::to_utf8(dir->path).value_or("?"));
        }
    } else if (!options_.smoke) {
        // The user's own folders: every change after its card, in the journal, undoable.
        host = agent::user_host(data_dir, agent::settings_of(config_.advanced, config_.limits, config_.access, agent::process_elevated()),
                                &*meter_);
    }
    // The real agent (phase 7) unless the simulated one is asked for or a milestone run plays it.
    const bool simulated = options_.sim || !options_.m4.empty() || !options_.m5.empty() ||
                           (options_.smoke && options_.replay.empty() && options_.m7.empty());
    if (simulated) {
        agent_.emplace([this](events::Event e) { mailbox_->post(std::move(e)); }, options_.sim_speed, std::move(host));
    } else {
        ai::Provider provider; // empty: DeepSeek, with the key in secrets.bin
        if (!options_.replay.empty()) {
            if (auto played = ai::cassette::player(options_.replay)) {
                provider = std::move(*played);
            } else {
                WA_ERROR("app", "--replay: {}", played.error().message);
                m4_refused_ = true; // the smoke run fails at once
            }
        }
        std::optional<agent::Host> own;
        if (!options_.m7.empty()) {
            // --m7: a copy of a desktop inside %TEMP%, fenced, as --m5 has it; the model is DeepSeek.
            const auto dir = security::paths::canonical(options_.m7, std::filesystem::temp_directory_path());
            const auto temp = security::paths::canonical(std::filesystem::temp_directory_path().wstring(), L"C:\\");
            if (!dir || !temp || !dir->exists || !security::paths::is_within(dir->path, temp->path) ||
                security::paths::is_within(temp->path, dir->path)) {
                WA_ERROR("app", "m7: {} is not a folder inside %TEMP%: refused", core::utf8::to_utf8(options_.m7).value_or("?"));
                m4_refused_ = true;
            } else {
                tools::os::fence::enable(dir->path);
                const std::filesystem::path root{dir->path};
                own = agent::Host{.desktop = root / L"Desktop", .audit = root / L"audit.jsonl", .journal = root / L"changes.jsonl",
                                  .monitor = &*meter_, .downloads = root / L"Downloads"};
                WA_INFO("app", "m7 on {} (fenced)", core::utf8::to_utf8(dir->path).value_or("?"));
            }
        } else if (options_.smoke) {
            // A replayed smoke run reads the machine (system.info, process.list) and changes nothing of the user's.
            const auto folder = std::filesystem::temp_directory_path() / L"agent-replay-smoke";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            own = agent::Host{.desktop = folder, .audit = folder / L"audit.jsonl", .monitor = &*meter_};
        }
        // A refused --m7 in a smoke run has no folder at all: %TEMP%, and the run fails at once anyway.
        agent::Host chosen = own ? std::move(*own) : host ? std::move(*host) : agent::Host{.desktop = std::filesystem::temp_directory_path()};
        model_agent_.emplace([this](events::Event e) { mailbox_->post(std::move(e)); }, std::move(chosen), model_settings(), std::move(provider));
    }
    sheet::resume_tasks(model_, agent_last_task()); // the journal outlives a run: ids never repeat
    // No agent without a DeepSeek key: the sheet opens on the key's field until
    // one is kept. The simulated agent, a recording and the smoke runs need none.
    model_.has_key = !model_agent_ || options_.smoke || !options_.replay.empty() ||
                     security::load_key(ai::catalog::default_provider().id, data_dir_ / L"secrets.bin", ai::catalog::default_provider().id).has_value();
    model_.choice = settings::choice_of(config_.ai); // design/ui_model: the orb and the bar show it
    model_.roles = config_.ai.roles;                 // stage 4c: where a request goes, the chip shows
    model_.disabled = config_.ai.disabled;           // stage 4d-2: not in the menu
    model_.budget_usd = config_.ai.budget_usd;

    if (options_.smoke) {
        WA_INFO("app", "smoke run: {}", options_.show ? "shown without activation" : "nothing is shown");
        if (options_.show) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        smoke_started_ = seconds();
        await_installer(); // the installer's handoff, nothing shown (handoff_test)
        return true;
    }
    hotkey_ = register_hotkey();
    add_tray_icon();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    WA_INFO("app", "ready {:.0f} ms after process start", process_age_ms());
    if (options_.open_at_start) {
        open_sheet("--open");
    }
    await_installer();
    return true;
}

void Shell::await_installer() {
    if (!options_.after_install) {
        return;
    }
    HANDLE go = options_.go_event.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE, options_.go_event.c_str());
    if (go == nullptr) { // no installer to wait for: it is gone, or there was no flight
        open_after_install(false);
        return;
    }
    // The installer started this agent with its flight: the strip is up meanwhile; the sheet opens when
    // the orb has gone into it (10 s at most: an installer that died mid-flight keeps no one waiting).
    waiting_go_ = true;
    HWND hwnd = window_.get();
    std::thread{[hwnd, go] {
        WaitForSingleObject(go, 10000);
        CloseHandle(go);
        PostMessageW(hwnd, WM_APP_GO, 0, 0);
    }}.detach();
}

void Shell::open_after_install(bool held) {
    waiting_go_ = false;
    // The installer's window has become our bar and is held there (docs/specs/motion-rework.md § 3.8):
    // the sheet comes open at once in its place — the same glass, the same orb — and the row's content
    // fades in; our event is set once that frame is on the screen (frame()) and the installer lets go.
    // With no installer holding it, the sheet opens with its usual opening.
    if (held && painter_) {
        painter_->arrive_next();
    }
    const POINT at{options_.after_install->x, options_.after_install->y};
    open_sheet("after install", true, at);
    if (model_.open) {
        sheet::after_install(model_, security::load_key(ai::catalog::default_provider().id, data_dir_ / L"secrets.bin", ai::catalog::default_provider().id).has_value());
        ready_pending_ = !options_.ready_event.empty();
    }
}

void Shell::signal_ready() {
    ready_pending_ = false;
    DwmFlush(); // the frame just presented is composed: on the screen
    if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, options_.ready_event.c_str())) {
        SetEvent(event);
        CloseHandle(event);
        WA_INFO("app", "after install: the open bar is on the screen, the installer told");
    }
}

void Shell::key_checked(const keycheck::Result& result) {
    if (result.verdict == keycheck::Verdict::Accepted) {
        std::error_code ec;
        std::filesystem::create_directories(data_dir_, ec);
        const auto stored = security::store_key(ai::catalog::default_provider().id, pending_key_, data_dir_ / L"secrets.bin", ai::catalog::default_provider().id);
        sheet::key_saved(model_, stored.has_value(), stored ? std::string_view{} : std::string_view{stored.error().message});
        WA_INFO("app", "{} key {}", ai::catalog::default_provider().name, stored ? "checked and saved" : "checked, not saved");
    } else {
        sheet::key_rejected(model_, result.message);
        WA_INFO("app", "{} key not kept: {}", ai::catalog::default_provider().name, result.verdict == keycheck::Verdict::Rejected ? "refused" : "not checked");
    }
    std::fill(pending_key_.begin(), pending_key_.end(), '\0');
    pending_key_.clear();
    wants_frame_ = true;
}

void Shell::exit_requested() {
    WA_INFO("app", "exit asked (installer)");
    exit_asked_ = true;
    quit_ = true;
    PostQuitMessage(0);
}

void Shell::ensure_orb(float scale) {
    if (orb_ && orb_scale_ == scale) {
        return;
    }
    // 3x supersampled: the orb's silhouette is a hard edge in the shader (user: "pixelated" at 2x).
    auto orb = ui::Orb::create(*cache_, static_cast<int>(std::lround(80.0f * scale * 3.0f)), ui::Orb::Style::Model);
    if (!orb) {
        WA_ERROR("app", "orb: {}", orb.error().message);
        return;
    }
    orb_.emplace(std::move(*orb));
    orb_scale_ = scale;
    if (painter_) {
        painter_->set_orb(*orb_);
    }
    // The picker's small orbs (design/ui_model): stills of a second orb, 68 px drawn from 2x.
    if (auto small = ui::Orb::create(*cache_, static_cast<int>(std::lround(68.0f * scale * 2.0f)), ui::Orb::Style::Model)) {
        card_orb_.emplace(std::move(*small));
        if (painter_) {
            painter_->set_card_orb(&*card_orb_);
        }
    } else {
        WA_WARN("app", "the picker's orb: {}", small.error().message);
    }
}

// The views are cut to the monitor the sheet hangs on (Review Focus 1 of phase 5).
void Shell::fit_to_monitor() {
    if (!painter_) {
        return;
    }
    const float s = monitor_.scale > 0 ? monitor_.scale : 1.0f;
    painter_->set_room({static_cast<float>(monitor_.rect.right - monitor_.rect.left) / s,
                        static_cast<float>(monitor_.rect.bottom - monitor_.rect.top) / s});
}

auto Shell::registrar() -> hotkeys::Registrar {
    return {[this](const config::Hotkey& h) {
                if (!RegisterHotKey(window_.get(), kHotkeyId, h.modifiers | MOD_NOREPEAT, h.key)) {
                    WA_WARN("app", "hotkey {} is taken (error {})", config::hotkey_name(h), GetLastError());
                    return false;
                }
                hotkey_ = true;
                return true;
            },
            [this] {
                UnregisterHotKey(window_.get(), kHotkeyId);
                hotkey_ = false;
            }};
}

auto Shell::register_hotkey() -> bool {
    const std::vector<std::string> candidates{config_.hotkey, "Ctrl+Shift+Space", "Ctrl+Alt+Space"};
    auto r = registrar();
    live_hotkey_ = hotkeys::register_first(candidates, r);
    if (!live_hotkey_) {
        MessageBoxW(nullptr, L"No keyboard shortcut is free for the agent.\nOpen it from the tray icon.",
                    WA_PRODUCT_WIDE, MB_OK | MB_ICONWARNING);
        return false;
    }
    WA_INFO("app", "hotkey {}", *live_hotkey_);
    if (painter_) {
        painter_->set_shortcut(*live_hotkey_); // the sign's capsule names it
    }
    if (*live_hotkey_ != config_.hotkey) {
        const std::wstring text = L"The shortcut " + wide(config_.hotkey) +
                                  L" is taken by another program.\nThe agent opens with " + wide(*live_hotkey_) +
                                  L" instead.";
        MessageBoxW(nullptr, text.c_str(), WA_PRODUCT_WIDE, MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

void Shell::add_tray_icon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = window_.get();
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = app_icon(SM_CXSMICON, SM_CYSMICON);
    wcsncpy_s(nid.szTip, tray_icon::tip(model_.choice).c_str(), _TRUNCATE);
    tray_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    icon_animator_.reshow(); // the orb takes the app's icon's place at the next pass (update_tray_icon)
}

void Shell::update_tray_icon(double now) {
    if (!tray_ || !card_orb_) {
        return;
    }
    const sheet::ModelChoice c = model_.choice; // the setting: a change for one request is not the icon's
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(window_.get()));
    // The orb at a pose, shrunk into the animator's frame: read back into a kept buffer, nothing allocated.
    const auto paint = [&](tray_icon::Pose p, std::vector<std::uint8_t>& out) {
        card_orb_->set_params({.spike = p.spike, .speed = 0.5f, .hue = 0.0f, .energy = 0.0f, .phase = p.phase});
        card_orb_->set_zoom(0.6f);
        card_orb_->set_look({.look = ui::orb_look::look_of(sheet::model_index(c.model), c.level)});
        card_orb_->render(2.4);
        card_orb_->read_into(icon_raw_);
        tray_icon::shrink_into(icon_raw_, card_orb_->size(), size, out);
    };
    if (!icon_animator_.built_for(c, size)) { // a new choice or size: its still at once (one render)
        if (auto built = icon_animator_.build(c, size, paint); !built) {
            WA_WARN("app", "the tray icon: {}", built.error().message); // the app's icon stays; not tried again
        }
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = window_.get();
        nid.uID = kTrayId;
        nid.uFlags = NIF_TIP;
        wcsncpy_s(nid.szTip, tray_icon::tip(c).c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else if (!icon_animator_.breathing()) { // its breath, one pose a pass (~0.4 ms; all at once was 13)
        if (auto grown = icon_animator_.grow(paint); !grown) {
            WA_WARN("app", "the tray icon's breath: {}", grown.error().message); // the still stays
        }
    }
    // Every pass: only which of the made icons is due — nothing rendered, read or allocated.
    if (const HICON icon = icon_animator_.due(now, sheet::busy(model_))) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = window_.get();
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON;
        nid.hIcon = icon;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

auto Shell::build_tray_menu() -> HMENU {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Agent");
#ifdef WA_DEBUG_UI
    AppendMenuW(menu, MF_STRING, kMenuDebug, L"Debug window (F12)");
#endif
    // Settings: the submenus own their items; DestroyMenu(menu) frees them all.
    const auto add = [](auto& self, HMENU into, const std::vector<settings::Item>& items) -> void {
        for (const auto& item : items) {
            const std::wstring label = wide(item.label);
            if (item.separator) {
                AppendMenuW(into, MF_SEPARATOR, 0, nullptr);
            } else if (!item.children.empty()) {
                HMENU sub = CreatePopupMenu();
                self(self, sub, item.children);
                AppendMenuW(into, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label.c_str());
            } else {
                MENUITEMINFOW mi{sizeof(mi)};
                mi.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
                mi.fType = MFT_STRING | (item.radio ? MFT_RADIOCHECK : 0);
                mi.fState = item.checked ? MFS_CHECKED : MFS_UNCHECKED;
                mi.wID = item.id;
                mi.dwTypeData = const_cast<wchar_t*>(label.c_str());
                InsertMenuItemW(into, GetMenuItemCount(into), TRUE, &mi);
            }
        }
    };
    HMENU settings_menu = CreatePopupMenu();
    add(add, settings_menu, settings::menu(config_, live_hotkey_, monitor_hz_));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings_menu), L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    return menu;
}

namespace {

// The menu as Windows holds it, one line per item: for the smoke run's log.
void log_menu(HMENU menu, int depth) {
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t text[128]{};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        mi.dwTypeData = text;
        mi.cch = 127;
        GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mi);
        const auto utf8 = core::utf8::to_utf8(text);
        WA_INFO("app", "menu: {:>{}}{}{}{} [{}]", "", depth * 2, (mi.fState & MFS_CHECKED) != 0 ? "(x) " : "",
                (mi.fType & MFT_SEPARATOR) != 0 ? "----" : (utf8 ? *utf8 : std::string{"?"}),
                (mi.fType & MFT_RADIOCHECK) != 0 ? " (radio)" : "", mi.wID);
        if (mi.hSubMenu != nullptr) {
            log_menu(mi.hSubMenu, depth + 1);
        }
    }
}

} // namespace

void Shell::show_system_tray_menu() {
    HMENU menu = build_tray_menu();
    POINT p{};
    GetCursorPos(&p);
    const HWND before = GetForegroundWindow(); // the user's app, to return to (review N-3)
    SetForegroundWindow(window_.get());        // or the menu does not close when clicking elsewhere
    const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0,
                                       window_.get(), nullptr);
    PostMessageW(window_.get(), WM_NULL, 0, 0); // TrackPopupMenu's documented companion
    DestroyMenu(menu);
    tray_choice(chosen, before);
}

void Shell::tray_choice(UINT chosen, HWND before) {
    const bool theirs = before != nullptr && before != window_.get() && IsWindow(before);
    if (chosen == kMenuOpen) {
        open_sheet("tray");
        if (theirs) {
            previous_foreground_ = before; // not the agent itself, which the menu made foreground
            before_open_.store(before);
        }
        return;
    }
    if (!model_.open && theirs) {
        SetForegroundWindow(before);
    }
    if (chosen == kMenuDebug) {
        toggle_debug();
        return;
    }
    if (chosen == tray_menu::kPauseId) { // stage 4d-2: the shortcut and the window watcher stop, the sign fades
        paused_ = !paused_;
        if (painter_) {
            painter_->set_paused(paused_);
        }
        WA_INFO("app", "agent {}", paused_ ? "paused" : "resumed");
        wants_frame_ = true;
        return;
    }
    if (chosen == kMenuExit) {
        quit_ = true;
        PostQuitMessage(0);
    } else if (chosen == settings::kAuditId) {
        show_audit();
    } else if (chosen == settings::kChangeLogId) {
        show_change_log();
    } else if (chosen == settings::kMemoryId) {
        show_memory();
    } else if (chosen == settings::kSettingsId) {
        open_settings();
    } else if (chosen == settings::kApiKeyId) {
        open_sheet("api key"); // first: the window takes clicks, focus and keys
        if (model_.open) {
            sheet::ask_key(model_);
        }
        wants_frame_ = true;
    } else if (chosen >= settings::kFirstId && chosen <= settings::kLastId) {
        apply_setting(chosen);
    }
}

namespace {

constexpr wchar_t kMenuClassName[] = L"WinAiAgent.TrayMenu";
static_assert(kMenuOpen == tray_menu::kOpenId && kMenuExit == tray_menu::kQuitId && kMenuDebug == tray_menu::kDebugId);

} // namespace

LRESULT CALLBACK Shell::menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->menu_window_ && self->menu_window_.get() == hwnd) {
        return self->handle_menu(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Shell::show_tray_menu() {
    if (menu_window_) { // its icon clicked again: it goes
        close_tray_menu();
        return;
    }
    if (!tray_menu::reopens(menu_lost_at_, tray_pressed_at_)) { // the release of the click whose press closed it
        menu_lost_at_ = -1.0e9;
        return;
    }
    menu_shown_at_ = seconds(); // it comes on quick from now (the map's unnamed: the menu's showing)
    if (!context_ || !renderer_ || !ui_ || !menu_ui_) {
        show_system_tray_menu();
        return;
    }
    menu_before_ = GetForegroundWindow();
    NOTIFYICONIDENTIFIER icon{sizeof(icon)};
    icon.hWnd = window_.get();
    icon.uID = kTrayId;
    if (FAILED(Shell_NotifyIconGetRect(&icon, &menu_icon_))) { // by the mouse, then
        POINT p{};
        GetCursorPos(&p);
        menu_icon_ = {p.x, p.y, p.x + 1, p.y + 1};
    }
    menu_settings_ = {};
    menu_budget_ = {};
    menu_unfolding_ = false;
    menu_hover_.reset();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    static const bool registered = [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = menu_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuClassName;
        return RegisterClassExW(&wc) != 0;
    }();
    HWND hwnd = registered ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClassName, WA_PRODUCT_WIDE, WS_POPUP, 0, 0, 1, 1,
                                             nullptr, nullptr, instance, this)
                           : nullptr;
    if (hwnd == nullptr) {
        WA_WARN("app", "the tray menu's window: {}", GetLastError());
        show_system_tray_menu();
        return;
    }
    menu_window_ = ui::gl::Window{hwnd};
    backdrop::prepare(hwnd);
    auto dc = context_->adopt(hwnd);
    if (!dc) {
        WA_WARN("app", "the tray menu: {}", dc.error().message);
        close_tray_menu();
        show_system_tray_menu();
        return;
    }
    menu_dc_ = *dc;
    render_menu_orbs();
    layout_tray_menu();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd); // so a click elsewhere deactivates it, and it closes
    draw_tray_menu();
}

void Shell::render_menu_orbs() {
    if (!card_orb_) {
        return;
    }
    menu_orbs_theme_ = theme::changing() ? std::string_view{} : theme::current().name; // made mid-change: again at its end
    const auto c = settings::choice_of(config_.ai);
    for (std::size_t i = 0; i < menu_orbs_.size(); ++i) {
        card_orb_->set_params({.spike = 0.35f, .speed = 0.5f, .hue = 0.0f, .energy = 0.0f, .phase = 2.4f});
        card_orb_->set_zoom(0.6f);
        card_orb_->set_look({.look = ui::orb_look::look_of(static_cast<int>(i), c.level)});
        card_orb_->render(2.4);
        if (auto still = card_orb_->still()) {
            menu_orbs_[i].emplace(std::move(*still));
        }
    }
}

void Shell::layout_tray_menu() {
    const POINT at_icon{(menu_icon_.left + menu_icon_.right) / 2, (menu_icon_.top + menu_icon_.bottom) / 2};
    menu_scale_ = monitor_at(at_icon).scale;
#ifdef WA_DEBUG_UI
    constexpr bool kDebug = true;
#else
    constexpr bool kDebug = false;
#endif
    const bool working = sheet::busy(model_);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at_icon, MONITOR_DEFAULTTONEAREST), &mi);
    const float room = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) / menu_scale_; // a list open folds the menu to it
    // A list's rows are there while it is open or still folding shut; unfold_tray_menu() sizes them.
    const double now = seconds();
    const bool settings_in = menu_settings_.open() || menu_settings_.moving(now);
    const bool budget_in = menu_budget_.open() || menu_budget_.moving(now);
    menu_rows_ = tray_menu::layout(config_, settings::menu(config_, live_hotkey_, monitor_hz_), working, settings_in, budget_in, kDebug, room,
                                   paused_);
    if (budget_in && std::ranges::none_of(menu_rows_, [](const tray_menu::Row& r) { return r.in == tray_menu::Row::In::Budget; })) {
        menu_budget_ = {}; // a small screen closed it for the settings': shut, so a click opens it again
    }
    // The window as tall as the menu ever gets, a list open or shut: never resized while it shows
    // (resized as a list began to unfold, it showed its last frame moved).
    menu_window_h_ = std::lround(tray_menu::window_height(config_, settings::menu(config_, live_hotkey_, monitor_hz_), working, kDebug, room) * menu_scale_);
    const SIZE size{std::lround(tray_menu::kWidth * menu_scale_), menu_window_h_};
    const POINT at = tray_menu::place(menu_icon_, mi.rcWork, size);
    menu_work_ = mi.rcWork;
    SetWindowPos(menu_window_.get(), HWND_TOPMOST, at.x, at.y, size.cx, size.cy, SWP_NOACTIVATE);
    unfold_tray_menu(now);
    // At rest only its glass takes the mouse: a click on the empty room over (or under) it goes to
    // what is there, and the menu closes as by any click elsewhere. While a list moves, the whole
    // window: set twice a click, never a frame.
    if (menu_settings_.moving(now) || menu_budget_.moving(now)) {
        SetWindowRgn(menu_window_.get(), nullptr, FALSE);
    } else {
        const LONG top = std::lround(menu_top_ * menu_scale_);
        HRGN rgn = CreateRectRgn(0, top, size.cx, top + std::lround(tray_menu::height(menu_rows_) * menu_scale_));
        if (rgn != nullptr && SetWindowRgn(menu_window_.get(), rgn, FALSE) == 0) { // on success the system owns it
            DeleteObject(rgn);
        }
    }
}

void Shell::unfold_tray_menu(double now) {
    tray_menu::unfold(menu_rows_, menu_settings_.shown(now), menu_budget_.shown(now));
    // Its glass where place() would put a menu that tall: the edge kept fixed by the icon, any taskbar side.
    const float s = menu_scale_;
    const LONG top = tray_menu::glass_top(menu_icon_, menu_work_, std::lround(tray_menu::kWidth * s), menu_window_h_,
                                          std::lround(tray_menu::height(menu_rows_) * s));
    menu_top_ = static_cast<float>(top) / s;
}

void Shell::draw_tray_menu() {
    if (!menu_window_ || menu_dc_ == nullptr) {
        return;
    }
    // The menu's orbs are stills in a theme's light: made again once a change of theme has ended.
    if (!theme::changing() && menu_orbs_theme_ != theme::current().name) {
        render_menu_orbs();
    }
    RECT client{};
    GetClientRect(menu_window_.get(), &client);
    const int w = client.right;
    const int h = client.bottom;
    const float s = menu_scale_;
    const double now = seconds();
    menu_drawn_at_ = now;
    context_->make_current(menu_dc_);
    if (wglSwapIntervalEXT != nullptr) {
        wglSwapIntervalEXT(0); // never a vblank's wait for the menu: the sheet swaps in the same pass (the review)
    }
    // Its header lives (design/ui_model): its orb moving in the choice's material and energy,
    // "ready" / "working" as the agent is now.
    const bool busy = sheet::busy(model_);
    tray_menu::live(menu_rows_, busy);
    GLuint live = 0;
    if (card_orb_) {
        const auto c = settings::choice_of(config_.ai);
        const auto t = static_cast<float>(now);
        card_orb_->set_params({.spike = (busy ? 0.6f : 0.35f) + 0.1f * std::sin(t * 1.7f), .speed = 0.5f, .hue = 0.0f, .energy = 0.0f,
                               .phase = 2.4f + 0.4f * t});
        card_orb_->set_zoom(0.6f);
        card_orb_->set_look({.look = ui::orb_look::look_of(sheet::model_index(c.model), c.level)});
        card_orb_->render(now);
        live = card_orb_->texture();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer_->begin(w, h, s);
    menu_ui_->begin_frame(now, s, {});
    renderer_->set_opacity(ui::motion::response(ui::motion::kQuick, now - menu_shown_at_)); // shown: on quick, not on in a frame
    tray_menu::draw(*menu_ui_, menu_rows_, menu_hover_,
                    {menu_orbs_[0] ? menu_orbs_[0]->texture() : 0u, menu_orbs_[1] ? menu_orbs_[1]->texture() : 0u},
                    static_cast<float>(w) / s, tray_menu::height(menu_rows_), {0.0f, menu_top_}, live); // its glass as it shows
    renderer_->set_opacity(1.0f);
    menu_ui_->end_frame();
    renderer_->end();
    SwapBuffers(menu_dc_);
    context_->make_current(); // back to the sheet's window
    if (wglSwapIntervalEXT != nullptr) {
        wglSwapIntervalEXT(swap_interval_); // the sheet's, as it was set (a driver may keep it per context)
    }
}

void Shell::tray_menu_click(std::size_t index) {
    if (index >= menu_rows_.size()) {
        return;
    }
    const tray_menu::Row row = menu_rows_[index];
    using tray_menu::Kind;
    switch (row.kind) {
    case Kind::Budget:
    case Kind::Settings: { // a list unfolds or folds; the main loop draws it to its end
        auto& list = row.kind == Kind::Budget ? menu_budget_ : menu_settings_;
        list.set(!list.open(), seconds());
        menu_unfolding_ = true;
        layout_tray_menu();
        draw_tray_menu();
        return;
    }
    case Kind::Model:
    case Kind::Thinking:
    case Kind::Theme:
    case Kind::BudgetValue:
    case Kind::SettingsItem:
        if (row.id == settings::kAuditId || row.id == settings::kMemoryId) { // an action, not a setting
            const HWND before = menu_before_;
            close_tray_menu();
            tray_choice(row.id, before);
            return;
        }
        if (row.id != 0) { // a setting: applied, the menu stays and shows it
            apply_setting(row.id);
            render_menu_orbs();
            layout_tray_menu();
            draw_tray_menu();
        }
        return;
    case Kind::Header:
    case Kind::Item: {
        const HWND before = menu_before_;
        close_tray_menu();
        tray_choice(row.id, before);
        return;
    }
    case Kind::Section:
    case Kind::Separator: return;
    }
}

void Shell::close_tray_menu() {
    if (!menu_window_) {
        return;
    }
    if (menu_dc_ != nullptr) {
        context_->make_current(); // never left current on a window that goes
        ReleaseDC(menu_window_.get(), menu_dc_);
        menu_dc_ = nullptr;
    }
    menu_window_ = {};
    menu_rows_.clear();
    menu_hover_.reset();
    menu_tracking_ = false;
    menu_lost_at_ = menu_lost_ ? seconds() : -1.0e9;
    menu_lost_ = false;
    if (!model_.open && menu_before_ != nullptr && IsWindow(menu_before_)) {
        SetForegroundWindow(menu_before_);
    }
}

auto Shell::handle_menu(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    HWND hwnd = menu_window_.get();
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!menu_tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
            menu_tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        const auto hot = tray_menu::hit(menu_rows_, static_cast<float>(GET_X_LPARAM(lp)) / menu_scale_,
                                        static_cast<float>(GET_Y_LPARAM(lp)) / menu_scale_ - menu_top_);
        if (hot != menu_hover_) {
            menu_hover_ = hot;
            draw_tray_menu();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        menu_tracking_ = false;
        if (menu_hover_) {
            menu_hover_.reset();
            draw_tray_menu();
        }
        return 0;
    case WM_LBUTTONUP:
        if (const auto hot = tray_menu::hit(menu_rows_, static_cast<float>(GET_X_LPARAM(lp)) / menu_scale_,
                                            static_cast<float>(GET_Y_LPARAM(lp)) / menu_scale_ - menu_top_)) {
            tray_menu_click(*hot);
        }
        return 0;
    case WM_KEYDOWN:
        // Its keys, as a menu's: ↑ ↓ the rows (the one lit), Enter chooses it, ← → the thinking level, Esc.
        if (wp == VK_ESCAPE) {
            close_tray_menu();
        } else if (wp == VK_UP || wp == VK_DOWN) {
            menu_hover_ = tray_menu::step(menu_rows_, menu_hover_, wp == VK_DOWN ? +1 : -1);
            draw_tray_menu();
        } else if (wp == VK_RETURN || wp == VK_SPACE) {
            if (menu_hover_) {
                tray_menu_click(*menu_hover_);
            }
        } else if (wp == VK_LEFT || wp == VK_RIGHT) {
            if (const auto level = tray_menu::level_step(menu_rows_, wp == VK_RIGHT ? +1 : -1)) {
                tray_menu_click(*level);
            }
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) { // a click elsewhere: it goes (after this message)
            menu_lost_ = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE: close_tray_menu(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        draw_tray_menu();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

auto Shell::change_log() const -> events::Log {
    return journal::log_of(journal::read(data_dir_ / L"changes.jsonl"), std::chrono::system_clock::now());
}

void Shell::show_change_log() {
    sheet::show_log(model_, change_log());
    WA_INFO("app", "change log: {} tasks", model_.log->tasks.size());
    open_sheet("change log");
    wants_frame_ = true;
}

// Rules and memory (phase 12 § 6): the list in the panel, from the agent's memory as it is now.
void Shell::open_settings(std::optional<sheet::Section> section) {
    refresh_settings();
    sheet::open_settings(model_, section); // where closing goes back to, before the window opens
    open_sheet("settings");                // its styles, focus and monitor, as every opening (final review C1)
    wants_frame_ = true;
}

namespace {

// "2026-11" by Win32's local time (not chrono's zones: they keep memory, the leak check).
auto this_month() -> std::string {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format("{:04}-{:02}", now.wYear, now.wMonth);
}

} // namespace

void Shell::keep_spent(const events::Spent& spent) {
    const auto* model = ai::catalog::model(spent.model);
    if (model == nullptr) {
        return;
    }
    const auto file = data_dir_ / L"spend.json";
    if (model_.settings.spend.month.empty()) {
        model_.settings.spend = spend::load(file);
    }
    spend::add(model_.settings.spend, model->provider, spent.usd, static_cast<std::int64_t>(spent.prompt) + spent.completion,
               this_month());
    if (config_writable_) { // a smoke run writes nothing of the user's
        if (auto saved = spend::save(file, model_.settings.spend); !saved) {
            WA_WARN("app", "spend.json not saved: {}", saved.error().message);
        }
    }
}

void Shell::refresh_settings(bool lists) {
    if (model_.settings.spend.month.empty()) {
        model_.settings.spend = spend::load(data_dir_ / L"spend.json");
    }
    spend::keep_month(model_.settings.spend, this_month()); // last month's is not this month's (4d-2 review I4)
    model_.settings.pages = settings::pages(config_, live_hotkey_, monitor_hz_);
    if (!lists) {
        wants_frame_ = true;
        return;
    }
    const auto entries = model_agent_ ? model_agent_->memory_entries() : std::vector<memory::Entry>{};
    const auto rules = model_agent_ ? model_agent_->private_rules() : std::vector<tools::winsys::PrivateRule>{};
    sheet::settings_lists(model_, present::memory_set(entries, {}, false), present::private_set(rules));
    wants_frame_ = true;
}

void Shell::show_memory() {
    const auto entries = model_agent_ ? model_agent_->memory_entries() : std::vector<memory::Entry>{};
    const auto rules = model_agent_ ? model_agent_->private_rules() : std::vector<tools::winsys::PrivateRule>{};
    sheet::show_memory(model_, present::memory_set(entries, rules)); // and the private windows (phase 13a)
    WA_INFO("app", "rules and memory: {} remembered", entries.size());
    open_sheet("rules and memory");
    wants_frame_ = true;
}

// agent.md in the editor Windows has for it (Notepad when it has none).
void Shell::open_rules() {
    const auto file = model_agent_ ? model_agent_->rules_file() : std::filesystem::path{};
    if (file.empty()) {
        return;
    }
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        const std::wstring args = L"\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    WA_INFO("app", "rules opened: {}", opened > 32 ? "its editor" : "notepad");
}

// "Forget all": asked first — it cannot be taken back.
void Shell::forget_all() {
    const int choice = MessageBoxW(nullptr, L"Forget everything the agent remembers about you? This cannot be undone.",
                                   WA_PRODUCT_WIDE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST);
    if (choice != IDYES) {
        return;
    }
    send(events::Forget{.id = 0, .all = true}); // the settings' lists made again by send (stage 4d-1)
    if (!model_.settings.open) {
        sheet::show_memory(model_, present::memory_set({}, model_agent_ ? model_agent_->private_rules() : std::vector<tools::winsys::PrivateRule>{}));
    }
    wants_frame_ = true;
}

void Shell::show_audit() {
    const auto file = data_dir_ / L"audit.jsonl";
    auto records = security::read_audit(file, 40);
    const auto verified = security::verify_audit(file);
    const auto& list = records ? *records : std::vector<security::AuditRecord>{};
    auto text = agent::audit_text(list, verified);
    std::optional<events::DataSet> set;
    if (!list.empty()) { // phase 8: the records as a table in the panel, the sentence in the turn
        text.resize(std::min(text.size(), text.find('\n')));
        set = agent::audit_set(list);
    }
    if (!sheet::show_report(model_, "Audit log", std::move(text), std::move(set))) {
        WA_INFO("app", "audit log not shown: a task is working");
        open_sheet("audit (busy)");
        return;
    }
    WA_INFO("app", "audit log: {}", verified ? std::to_string(*verified) + " records, the chain holds" : verified.error().message);
    open_sheet("audit");
    wants_frame_ = true;
}

auto Shell::change_hotkey(const std::string& to) -> bool {
    auto r = registrar();
    if (hotkeys::swap(live_hotkey_, to, r)) {
        live_hotkey_ = to;
        WA_INFO("app", "hotkey {}", to);
        if (painter_) {
            painter_->set_shortcut(to); // the sign's capsule names the one in force
        }
        return true;
    }
    const std::wstring keeps = live_hotkey_ ? L"The agent keeps " + wide(*live_hotkey_) + L"."
                                            : std::wstring{L"Open the agent from the tray icon."};
    const std::wstring text = L"The shortcut " + wide(to) + L" is taken by another program.\n" + keeps;
    MessageBoxW(nullptr, text.c_str(), WA_PRODUCT_WIDE, MB_OK | MB_ICONINFORMATION);
    return false;
}

void Shell::apply_setting(unsigned id) {
    config::Config next = config_;
    const auto change = settings::choose(next, id);
    if (!change) {
        return;
    }
    if (change->hotkey && !change_hotkey(next.hotkey)) {
        return; // nothing changed
    }
    if (change->gpu) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (auto r = gpu::write(HKEY_CURRENT_USER, gpu::kKey, exe, next.gpu); !r) {
            WA_WARN("app", "graphics card preference: {}", r.error().message);
        }
    }
    config_ = next;
    if (change->text) {
        apply_text_scale();
    }
    if (change->transparency) {
        apply_transparency();
    }
    if (change->frame_rate) {
        apply_frame_rate();
    }
    if (change->theme) {
        apply_theme(false);
    }
    if (change->ai) {
        model_.choice = settings::choice_of(config_.ai); // the orb and the bar show it at once
        model_.roles = config_.ai.roles;
        model_.disabled = config_.ai.disabled;
        model_.budget_usd = config_.ai.budget_usd;
        if (model_agent_) {
            model_agent_->set_settings(model_settings());
        }
    }
    if (change->rest) {
        painter_->set_rest_motion(options_.rest_animation.value_or(config_.rest_animation));
        apply_reduced_motion();
    }
    if (!config_writable_) {
        WA_WARN("app", "config.json was not readable at start: the change holds until exit only");
    } else if (auto r = config::save(data_dir_ / "config.json", config_); !r) {
        WA_WARN("app", "config.json not saved: {}", r.error().message);
    }
    wants_frame_ = true;
}

void Shell::apply_frame_rate() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // The monitor the agent is on or is about to open on (monitor_: set at start, at each open and
    // at a move); before any is known, the window's.
    const bool placed = monitor_.rect.right > monitor_.rect.left;
    const HMONITOR hmon = placed ? MonitorFromRect(&monitor_.rect, MONITOR_DEFAULTTONEAREST)
                                 : window_ ? MonitorFromWindow(window_.get(), MONITOR_DEFAULTTONEAREST) : nullptr;
    const bool known = hmon != nullptr && GetMonitorInfoW(hmon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    // 0 and 1 mean "the hardware's default" to EnumDisplaySettings: not a rate.
    monitor_hz_ = known && mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
    auto rate = config_.frame_rate;
    if (options_.smoke && options_.frame_rate != 0) { // --frame-rate: this run's, config.json as it is
        rate = options_.frame_rate == 120  ? config::FrameRate::Hz120
               : options_.frame_rate == 60 ? config::FrameRate::Hz60
               : options_.frame_rate == 30 ? config::FrameRate::Hz30
                                           : config::FrameRate::Monitor;
    }
    swap_interval_ = frame_rate::frame_interval(monitor_hz_, rate);
    if (context_ && wglSwapIntervalEXT != nullptr) {
        context_->make_current();
        wglSwapIntervalEXT(swap_interval_);
    }
    WA_INFO("app", "frame rate: monitor {} Hz, a frame every {} sync(s)", monitor_hz_, swap_interval_);
}

auto Shell::frame_period() const noexcept -> double {
    return monitor_hz_ > 0 ? static_cast<double>(swap_interval_) / monitor_hz_ : frame_rate::kIdleOrbFrame;
}

void Shell::apply_transparency() {
    if (ui_) { // 0 % opaque .. 50 %: the surface's alpha (the user's "Window transparency")
        ui_->set_surface_alpha(static_cast<std::uint8_t>(std::lround(255.0 * (100 - config_.transparency) / 100.0)));
        WA_INFO("app", "window transparency {} %", config_.transparency);
    }
}

void Shell::apply_reduced_motion() {
    const bool reduced = !animations_on();
    if (painter_) {
        painter_->set_reduced_motion(reduced);
    }
    if (menu_ui_) {
        menu_ui_->animator().set_reduced(reduced);
    }
}

void Shell::apply_theme(bool at_once) {
    // A --smoke run (tests, scenes, recordings) does not follow this machine's Windows: "as Windows"
    // is Dark there, so a run looks the same on every machine; a choice in its config.json holds.
    const std::optional<bool> windows = options_.smoke ? std::nullopt : theme::windows_light();
    const theme::Theme& to = theme::chosen(config_.theme, windows);
    if (painter_) { // the hidden agent's sign: Windows' own mode, as the taskbar (Dark in a --smoke run)
        painter_->set_sign_theme(theme::sign_theme(options_.smoke ? std::nullopt : theme::windows_system_light()));
    }
    const bool news = (theme::changing() ? theme::target().name : theme::current().name) != to.name;
    if (at_once || !painter_) {
        theme::set(to);
    } else {
        painter_->change_theme(to);
    }
    if (news || at_once) {
        WA_INFO("app", "theme {} (chosen: {})", to.name, theme::word_of(config_.theme));
    }
}

void Shell::apply_text_scale() {
    if (!ui_) {
        return;
    }
    const float s = config_.text_scale * windows_text_scale();
    if (s != ui_->text_scale()) {
        ui_->set_text_scale(s);
        if (menu_ui_) {
            menu_ui_->set_text_scale(s);
        }
        WA_INFO("app", "text scale {:.3f} (setting {:.3f} x Windows {:.2f})", s, config_.text_scale, s / config_.text_scale);
    }
}

void Shell::set_open_style(bool open) {
    if (open == open_style_) {
        return;
    }
    open_style_ = open;
    HWND hwnd = window_.get();
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR rest_bits = WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    ex = open ? (ex & ~rest_bits) : (ex | rest_bits);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    if (!open) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    } else {
        // The open sheet's region comes with its first frame (update_region): the canvas around it
        // is not the sheet's (motion rework, task 5).
        if (handle_hidden_) { // opened over the fullscreen app from a notification
            handle_hidden_ = false;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // No acrylic on the open window: the surface is the mockup's .965 and
    // switching DWM's blur was one source of the flash when closing. No DWM rounding either: its
    // shadow follows the window's rectangle, which keeps its open size while the panel leaves —
    // the user saw "a transparent window where the closed one was". The window has per-pixel
    // alpha (backdrop::prepare); the panel draws its own corners.
}

void Shell::apply_placement(const placement::Placement& p) {
    const bool moved = std::memcmp(&p.window, &placed_.window, sizeof(RECT)) != 0;
    placed_ = p;
    if (moved) {
        SetWindowPos(window_.get(), HWND_TOPMOST, p.window.left, p.window.top, p.window.right - p.window.left,
                     p.window.bottom - p.window.top, SWP_NOACTIVATE | SWP_NOREDRAW);
    }
}

void Shell::open_sheet(const char* why, bool over_fullscreen, std::optional<POINT> at) {
    if (model_.open) {
        return;
    }
    // Not Windows' BUSY alone: a meeting app sharing the screen keeps it on (app::fullscreen).
    if (!over_fullscreen && fullscreen::blocks_now() && !config_.over_fullscreen) {
        metrics_.note_open_refused("a fullscreen app is in front", seconds());
        WA_INFO("app", "open ({}) skipped: a fullscreen app is in front", why);
        return;
    }
    const RECT was = monitor_.rect;
    monitor_ = at ? monitor_at(*at) : monitor_under_cursor();
    if (std::memcmp(&was, &monitor_.rect, sizeof(RECT)) != 0) {
        apply_frame_rate(); // another monitor, another refresh rate: the swap divisor follows (Review Focus 3)
    }
    ensure_orb(monitor_.scale);
    fit_to_monitor();
    previous_foreground_ = GetForegroundWindow();
    before_open_.store(previous_foreground_);
    // Asked now, while that window still has the focus: the sheet takes it next. Not said in time
    // (a busy program): the window counts as having one, window.look gives nothing of it.
    const auto password = tools::winsys::focused_password(previous_foreground_, kPasswordAsk);
    password_at_open_.store(password.value_or(true));
    if (!password) {
        WA_WARN("app", "the window in front did not say within {} ms whether a password field has its focus: taken as yes",
                kPasswordAsk.count());
    }
    set_open_style(true);
    sheet::open(model_);
    SetForegroundWindow(window_.get());
    SetFocus(window_.get());
    wants_frame_ = true;
    WA_INFO("app", "open ({})", why);
}

void Shell::close_sheet(const char* why) {
    if (!model_.open) {
        return;
    }
    sheet::close(model_);
    // The window keeps its open styles while the panel leaves (app::motion); frame() switches it
    // to the resting ones once the painter says the close has ended.
    if (GetForegroundWindow() == window_.get() && previous_foreground_ != nullptr &&
        IsWindow(previous_foreground_)) {
        SetForegroundWindow(previous_foreground_);
    }
    wants_frame_ = true;
    WA_INFO("app", "close ({})", why);
}

void Shell::send(events::Command command) {
    if (const auto* cancel = std::get_if<events::Cancel>(&command); cancel != nullptr && history_) {
        if (auto r = history::stopped(model_, cancel->task, answered_task_, history::now_utc())) {
            history_->add(std::move(*r));
        }
    }
    const bool private_added = std::holds_alternative<events::AddPrivate>(command);
    const bool forgot = std::holds_alternative<events::Forget>(command);
    to_agent(std::move(command));
    if ((private_added || forgot) && model_.settings.open) {
        refresh_settings(); // stage 4d-1: the section's lists again, as the agent keeps them now
    } else if (private_added) {
        show_memory(); // the rule kept: the list again, with it
    }
}

void Shell::to_agent(events::Command command) {
    if (model_agent_) {
        model_agent_->send(std::move(command));
    } else if (agent_) {
        agent_->send(std::move(command));
    }
}

auto Shell::agent_last_task() const -> std::uint64_t {
    return model_agent_ ? model_agent_->last_task() : agent_ ? agent_->last_task() : 0;
}

auto Shell::model_settings() const -> model::Settings {
    const auto& a = config_.ai;
    return model::Settings{.model = a.model,
                           .thinking = model::thinking_of(a.thinking),
                           .budget_usd = a.budget_usd,
                           .roles = a.roles,
                           .disabled = a.disabled,
                           .secrets = data_dir_ / L"secrets.bin",
                           .proxy = config_.network.proxy};
}

void Shell::choose_model(sheet::ModelChoice choice) {
    settings::apply(config_.ai, choice);
    model_.choice = choice;
    if (model_agent_) {
        model_agent_->set_settings(model_settings()); // from the next request: a running one keeps its own
    }
    if (!config_writable_) {
        WA_WARN("app", "config.json was not readable at start: the change holds until exit only");
    } else if (auto saved = config::save(data_dir_ / "config.json", config_); !saved) {
        WA_WARN("app", "config.json not saved: {}", saved.error().message);
    }
    WA_INFO("app", "model {} · thinking {}", choice.model, sheet::thinking_id(choice.level));
}

void Shell::react(const sheet::Reaction& r) {
    if (model_.settings.open && model_.settings.pages[0].empty()) {
        refresh_settings(); // opened by the model (Add key, Update key: Models' key field, stage 4d-2)
    }
    if (r.setting) { // stage 4d-1: a settings item chosen in the settings mode, applied as the tray's
        if (*r.setting == settings::kChangeLogId || *r.setting == settings::kAuditId) {
            model_.settings.open = false; // the log is a screen of its own: the settings give way to it
        }
        tray_choice(*r.setting, nullptr);
        if (model_.settings.open) {
            refresh_settings(false); // the lists as they are: their table and scroll stay (final review M3)
        }
    }
    if (r.choose) {
        choose_model(*r.choose);
    }
    if (r.role) { // a role's model from Roles (stage 4d-2): config.json and the agent
        (r.role->need == ai::routing::Need::Images ? config_.ai.roles.images : config_.ai.roles.hard).model = r.role->model;
        model_.roles = config_.ai.roles;
        if (model_agent_) {
            model_agent_->set_settings(model_settings());
        }
        if (config_writable_) {
            if (auto saved = config::save(data_dir_ / "config.json", config_); !saved) {
                WA_WARN("app", "config.json not saved: {}", saved.error().message);
            }
        }
    }
    if (r.budget) { // the picker's budget per task (design/ui_model): as the tray's
        config_.ai.budget_usd = *r.budget;
        model_.budget_usd = *r.budget;
        if (model_agent_) {
            model_agent_->set_settings(model_settings());
        }
        if (config_writable_) {
            if (auto saved = config::save(data_dir_ / "config.json", config_); !saved) {
                WA_WARN("app", "config.json not saved: {}", saved.error().message);
            }
        }
    }
    if (r.command) {
        send(*r.command);
    }
    if (model_.key_entered) {
        // The DeepSeek key, from the sheet's field: DeepSeek is asked on a thread of its own (a GET
        // with it, up to ~15 s); only a key it takes goes to DPAPI (key_checked). Never logged.
        pending_key_ = std::move(*model_.key_entered);
        model_.key_entered.reset();
        if (!model_agent_) { // the simulated agent asks DeepSeek nothing
            key_checked(keycheck::Result{keycheck::Verdict::Accepted, {}});
        } else {
            HWND hwnd = window_.get();
            const std::string proxy = config_.network.proxy;
            // Detached: an exit while DeepSeek is asked doesn't wait for the answer (up to ~15 s).
            std::thread{[hwnd, proxy, key = pending_key_]() mutable {
                auto result = std::make_unique<keycheck::Result>(keycheck::check(key, proxy));
                std::fill(key.begin(), key.end(), '\0');
                // Posted: WM_APP_KEY owns it. Not posted (the window is gone): freed here, nobody waits.
                if (PostMessageW(hwnd, WM_APP_KEY, 0, reinterpret_cast<LPARAM>(result.get()))) {
                    static_cast<void>(result.release());
                }
            }}.detach();
        }
        wants_frame_ = true;
    }
    if (r.close) {
        close_sheet("ui");
    }
    if (r.levels) {
        // "Why I'm asking": a level changed, only ever stricter than the matrix; the agent takes it
        // from its next call, config.json keeps it.
        config_.access = agent::access_of(*r.levels);
        send(*r.levels);
        if (!config_writable_) {
            WA_WARN("app", "config.json was not readable at start: the access level holds until exit only");
        } else if (auto saved = config::save(data_dir_ / "config.json", config_); !saved) {
            WA_WARN("app", "config.json not saved: {}", saved.error().message);
        }
        WA_INFO("app", "access levels: reads {}, creating and moving {}", r.levels->read_asks ? "ask" : "auto",
                r.levels->change_per_folder ? "once per folder" : "always");
    }
    if (r.open && !model_.open) {
        open_sheet("a background task brought to the front");
    }
    if (r.show_log) {
        show_change_log();
    }
    if (!r.open_url.empty()) {
        open_link(r.open_url);
    }
    if (!r.clipboard.empty()) {
        copy_text(r.clipboard);
    }
    if (!r.open_folder.empty()) {
        open_folder(r.open_folder);
    }
    if (sheet::picture_file(r.open_file)) { // a thumbnail: the picture in its program, if it is still there
        if (const auto w = core::utf8::to_wide(r.open_file); w && GetFileAttributesW(w->c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto done = tools::os::current().open(*w);
            WA_INFO("app", "picture opened: {}", done ? "yes" : "no");
        }
    }
    // A file this turn made (phase 10, screen 24): in its program, or selected in Explorer — only
    // a file on this turn's cards, never a path from anywhere else.
    const auto made_here = [&](const std::string& path) {
        return !path.empty() && std::ranges::any_of(model_.created, [&](const events::Created& c) { return c.path == path; });
    };
    if (made_here(r.open_created)) {
        if (const auto w = core::utf8::to_wide(r.open_created); w && GetFileAttributesW(w->c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto done = tools::os::current().open(*w);
            WA_INFO("app", "created file opened: {}", done ? "yes" : "no");
        }
    }
    // A file a listing in the panel showed (phase 10, screen 26): its row opens it — a program is
    // never run, only shown in its folder — its "..." shows it there. Only a path of such a list.
    const auto listed_here = [&](const std::string& path) {
        const auto cut = path.find_last_of('\\');
        if (path.empty() || cut == std::string::npos) {
            return false;
        }
        const std::string_view folder{path.data(), cut}, name{path.data() + cut + 1, path.size() - cut - 1};
        return std::ranges::any_of(model_.sets, [&](const events::DataSet& s) {
            if ((s.kind != events::SetKind::Files && s.kind != events::SetKind::Matches) || s.rows.columns < 3) {
                return false;
            }
            for (std::size_t row = 0; row < s.rows.rows(); ++row) {
                const std::string_view in = s.kind == events::SetKind::Files ? std::string_view{s.location} : std::string_view{s.rows.at(row, 2).text};
                const std::string_view trimmed = in.ends_with('\\') ? in.substr(0, in.size() - 1) : in;
                if (s.rows.at(row, 1).text == name && trimmed == folder) {
                    return true;
                }
            }
            return false;
        });
    };
    const auto select_in_folder = [](const std::string& path) {
        if (const auto w = core::utf8::to_wide(path); w && GetFileAttributesW(w->c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(w->c_str()); item != nullptr) {
                const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                ILFree(item);
                WA_INFO("app", "shown in its folder: {}", SUCCEEDED(hr) ? "yes" : "no");
            }
        }
    };
    // Only documents and media open (review I1); Windows' own list of dangerous types checked too;
    // the rest is shown in its folder.
    const auto open_safely = [&](const std::string& path, std::string_view what) {
        const auto dot = path.find_last_of('.');
        std::optional<std::wstring> ext;
        if (dot != std::string::npos) {
            if (auto w = core::utf8::to_wide(std::string_view{path}.substr(dot))) {
                ext = std::move(*w);
            }
        }
        if (!sheet::opens_safely(path) || !ext || AssocIsDangerous(ext->c_str())) {
            select_in_folder(path);
        } else if (const auto w = core::utf8::to_wide(path); w && GetFileAttributesW(w->c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto done = tools::os::current().open(*w);
            WA_INFO("app", "{} opened: {}", what, done ? "yes" : "no");
        }
    };
    if (r.open_rules) { // phase 12 § 6
        open_rules();
    }
    if (r.forget_all) {
        forget_all();
    }
    if (listed_here(r.open_listed)) {
        open_safely(r.open_listed, "listed file");
    }
    if (listed_here(r.show_listed)) {
        select_in_folder(r.show_listed);
    }
    // A file or folder the answer names (phase 10, after a hand check): only a row of the turn
    // in front, looked at on the disk when the answer came — a folder in Explorer, a file as above.
    if (const auto* place = sheet::placed(model_, r.open_place)) {
        if (place->folder) {
            open_folder(place->path);
        } else {
            open_safely(place->path, "named file");
        }
    }
    if (sheet::placed(model_, r.show_place) != nullptr) {
        select_in_folder(r.show_place);
    }
    if (made_here(r.show_in_folder)) {
        if (const auto w = core::utf8::to_wide(r.show_in_folder); w && GetFileAttributesW(w->c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(w->c_str()); item != nullptr) {
                const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                ILFree(item);
                WA_INFO("app", "shown in its folder: {}", SUCCEEDED(hr) ? "yes" : "no");
            }
        }
    }
    if (r.save) {
        save_file(window_.get(), r.save->name, r.save->text);
    }
    if (r.attach) { // the paperclip: Windows' file picker, its own dialog (the sheet stays open)
        if (auto picked = pick_files(window_.get()); !picked.empty()) {
            attach(std::move(picked));
        }
    }
    set_background_style(!model_.open); // closed, the sign's zone takes the mouse (UI update, stage 3); never the focus
    update_dim();
    update_monitor();
    update_notifications();
}

// The terminal's foot: the agent's own share of the processor since the last look, and its memory.
void Shell::measure_usage(double now) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        return;
    }
    const auto ticks = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    const std::uint64_t cpu = ticks(k) + ticks(u);
    const double wall = now - usage_at_;
    double percent = 0;
    if (usage_at_ > 0 && wall > 0) {
        const double processors = static_cast<double>(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        percent = static_cast<double>(cpu - usage_cpu_) / 1.0e7 / wall / processors * 100.0;
    }
    usage_at_ = now;
    usage_cpu_ = cpu;
    PROCESS_MEMORY_COUNTERS mc{};
    mc.cb = sizeof(mc);
    const double mb = K32GetProcessMemoryInfo(GetCurrentProcess(), &mc, sizeof(mc)) ? static_cast<double>(mc.WorkingSetSize) / (1024.0 * 1024.0) : 0;
    model_.agent_usage = std::format("Agent CPU {:.0f}% \xC2\xB7 RAM {:.0f} MB", percent, mb);
}

void Shell::update_notifications() {
    if (notifier_) {
        // Only while the handle hides under a fullscreen app (the designer: there the answer comes as
        // a Windows notification): then its dot cannot be seen. Otherwise the dot says it.
        courier_.update(model_, !model_.open && !model_.drops.empty() && handle_hidden_, *notifier_);
    }
}

void Shell::take_toast_clicks() {
    std::vector<std::wstring> clicks;
    {
        std::scoped_lock lock{toast_mutex_};
        clicks.swap(toast_clicks_);
    }
    for (const auto& args : clicks) {
        const auto pressed = notify::press_of(core::utf8::to_utf8(args).value_or(std::string{}));
        if (!pressed) {
            continue;
        }
        WA_INFO("app", "notification: {} of task {}", pressed->second, pressed->first);
        auto r = sheet::drop_press(model_, pressed->first, pressed->second);
        if (r.open) {
            open_sheet("a notification's button", true); // asked for over the fullscreen app
        }
        react(r);
    }
    wants_frame_ = true;
}

void Shell::set_background_style(bool on) {
    if (on == background_style_ || open_style_) {
        background_style_ = on && !open_style_;
        return;
    }
    background_style_ = on;
    handle_checked_at_ = -1.0; // the handle just came (or went): is a fullscreen app in front?
    HWND hwnd = window_.get();
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // The sign takes the mouse (a click opens the sheet, the mouse over it its capsule); it stays a
    // window that is never activated: a click there takes no focus from the user's program.
    ex = on ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT)) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Shell::update_region(const placement::Placement& p, const ui::layout::Box& panel) {
    const auto solid = painter_->solid();
    const bool open = open_style_;
    if (!open && (!background_style_ || solid.empty())) {
        clear_region(); // nothing of the agent takes the mouse: its window lets every click through (WS_EX_TRANSPARENT)
        return;
    }
    // What takes the mouse now and where the shape goes: the open sheet with the room it keeps under
    // its bar (a tooltip, the hint, a pasted card); the background's own shapes.
    const sheet::Shape to = painter_->target_shape();
    const ui::layout::Box target_box = placement::panel_in(p, to);
    const float held = painter_->window_height();
    ui::layout::Box now_box = panel;
    now_box.h = std::max(panel.h, held);
    const ui::layout::Box to_box{target_box.x, target_box.y, target_box.w, std::max(target_box.h, held)};
    open_box_ = now_box;
    open_to_ = to_box;
    // The open window's region is the panel and its shadow's room: the shadow fades out inside it
    // (the panel alone left it only in the four corners — "a square shadow").
    const auto reach = ui::work::sheet_shadow_reach();
    const auto shadowed = [&](const ui::layout::Box& b) {
        return ui::layout::Box{b.x - reach.side, b.y, b.w + 2 * reach.side, b.h + reach.bottom};
    };
    const ui::layout::Box now_open[] = {shadowed(now_box)};
    const ui::layout::Box to_open[] = {shadowed(to_box)};
    const auto wanted = open ? placement::region_of(p, now_open) : placement::region_of(p, solid);
    const auto target = placement::region_of(p, to_open);
    if (const auto next = placement::next_region(region_, region_on_, wanted, target, painter_->moving())) {
        set_region(*next);
    }
}

void Shell::set_region(const placement::Region& r) {
    if (r.count == 0) {
        clear_region();
        return;
    }
    HRGN rgn = CreateRectRgnIndirect(&r.rects[0]);
    if (rgn == nullptr) {
        return;
    }
    for (std::size_t i = 1; i < r.count; ++i) {
        if (HRGN part = CreateRectRgnIndirect(&r.rects[i]); part != nullptr) {
            CombineRgn(rgn, rgn, part, RGN_OR);
            DeleteObject(part);
        }
    }
    if (SetWindowRgn(window_.get(), rgn, FALSE) == 0) { // on success the system owns it
        DeleteObject(rgn);
        return;
    }
    region_ = r;
    region_on_ = true;
}

void Shell::clear_region() {
    if (region_on_) {
        SetWindowRgn(window_.get(), nullptr, FALSE);
        region_on_ = false;
    }
}

void Shell::update_handle_hidden(double now) {
    const bool handle = background_style_ && !open_style_;
    if (handle && now - handle_checked_at_ < 1.0) {
        return;
    }
    handle_checked_at_ = now;
    const bool hide = handle && !config_.over_fullscreen &&
                      fullscreen::blocks_on(MonitorFromWindow(window_.get(), MONITOR_DEFAULTTONEAREST));
    if (hide == handle_hidden_) {
        return;
    }
    handle_hidden_ = hide;
    ShowWindow(window_.get(), hide ? SW_HIDE : SW_SHOWNOACTIVATE);
    WA_INFO("app", "the handle {} a fullscreen app", hide ? "hides under" : "is back after");
    update_notifications();
    wants_frame_ = true;
}

void Shell::watch_strip(double now) {
    if (model_.open || handle_hidden_ || paused_ || now - strip_checked_at_ < 1.0) { // paused: the watcher stops (4d-2)
        return;
    }
    strip_checked_at_ = now;
    HWND self = window_.get();
    if (const bool shown = IsWindowVisible(self) != FALSE; shown != strip_shown_) {
        strip_shown_ = shown;
        WA_INFO("app", "the resting window is {}", shown ? "shown again" : "not shown");
    }
    const RECT area = panel_on_screen_; // the strip's own rectangle, not the canvas around it
    HWND over = zorder::covering(self, area);
    if (over == strip_cover_) {
        return;
    }
    strip_cover_ = over;
    if (over != nullptr) {
        WA_INFO("app", "the resting window is under {}", zorder::describe(over));
    } else {
        WA_INFO("app", "nothing over the resting window now");
    }
}

void Shell::update_monitor() {
    const bool shown = model_.open && sheet::view(model_) == sheet::View::Report;
    if (!meter_ || shown == report_shown_) {
        return;
    }
    report_shown_ = shown;
    meter_->set_gpu(shown);    // ~1 ms a reading: only while it is on screen (docs/spikes/phase-5.md)
    meter_->set_notify(shown); // the sheet at rest is never woken by the monitor
}

void Shell::update_dim() {
    // MS 11: its opacity is the scene's spring (gentle), set as it moves; the window is hidden only
    // when none of it is left — never shown or gone in a frame (the map's № 17).
    const sheet::DimWindow dim = sheet::dim_window(painter_ ? painter_->dim() : 0.0f, kDimAlpha);
    const bool want = dim.shown;
    if (want && dim_ && dim.alpha != dim_alpha_) {
        SetLayeredWindowAttributes(dim_.get(), 0, dim.alpha, LWA_ALPHA);
        dim_alpha_ = dim.alpha;
    }
    if (want == dimmed_) {
        return;
    }
    dimmed_ = want; // a dim window that cannot be made is not tried again every frame
    if (want && !dim_) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kDimClassName;
        RegisterClassExW(&wc);
        // Click-through: it only dims; a click lands on what is under it.
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                        WS_EX_TRANSPARENT,
                                    kDimClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (hwnd == nullptr) {
            WA_WARN("app", "the dim window: CreateWindowExW failed: {}", GetLastError());
            return;
        }
        SetLayeredWindowAttributes(hwnd, 0, dim.alpha, LWA_ALPHA);
        dim_alpha_ = dim.alpha;
        dim_ = ui::gl::Window{hwnd};
    }
    if (!dim_) {
        return;
    }
    if (want) {
        const RECT& m = monitor_.rect;
        if (dim.alpha != dim_alpha_) { // shown again: from what is there now, not from where it was hidden
            SetLayeredWindowAttributes(dim_.get(), 0, dim.alpha, LWA_ALPHA);
            dim_alpha_ = dim.alpha;
        }
        // Just below the sheet: both topmost, the sheet over the dim.
        SetWindowPos(dim_.get(), window_.get(), m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        WA_INFO("app", "the dim: shown at alpha {} of {}", static_cast<int>(dim_alpha_), static_cast<int>(kDimAlpha)); // (a small number: it comes in, not on)
    } else {
        ShowWindow(dim_.get(), SW_HIDE);
        WA_INFO("app", "the dim: hidden from alpha {}", static_cast<int>(dim_alpha_)); // (a small number: it went out, not off)
        dim_alpha_ = 0;
    }
}

void Shell::drain_agent() {
    if (!mailbox_ || mailbox_->drain(drained_) == 0) {
        return;
    }
    for (const auto& e : drained_) {
        sheet::apply(model_, e);
        if (const auto* spent = std::get_if<events::Spent>(&e.event); spent != nullptr && !spent->model.empty()) {
            keep_spent(*spent); // stage 4d-2: this month's spending, Models' last line
        }
        if (const auto* o = std::get_if<events::OpenSettings>(&e.event); o != nullptr && e.task == model_.task && model_.open) {
            if (const auto section = sheet::section_of(o->section)) {
                open_settings(*section); // the model's ui.settings, for the task in front (4d-2 review M4)
            }
        }
        // A question waits for the user: the sheet comes up with its card (not over a fullscreen
        // app: then it waits, and the card is there when the sheet is opened).
        if (sheet::opens_sheet(model_, e)) { // a background task's question waits in its card instead
            open_sheet("a question");
            if (!model_.open) { // refused over a fullscreen app: a card behind the tab, a notification
                (void)sheet::send_back(model_);
            }
        }
        // A task ended while the change log is up (an undo from it, most often): the log shows
        // what the journal says now.
        if (model_.log && (std::holds_alternative<events::Finished>(e.event) || std::holds_alternative<events::Failed>(e.event))) {
            sheet::show_log(model_, change_log());
        }
        if (history_) {
            if (auto r = history::record_of(model_, e, history::now_utc())) {
                if (r->kind == "answer") {
                    answered_task_ = r->task;
                }
                history_->add(std::move(*r));
            }
        }
    }
    update_notifications(); // a card that came while a fullscreen app is in front
    wants_frame_ = true;
}

void Shell::decode_picture(std::string path) {
    decoders_.start([this, hwnd = window_.get(), path = std::move(path)] {
        Decoded done{.path = path, .image = {}};
        if (path.empty()) {
            if (auto copied = tools::os::current().read_clipboard(); copied && copied->picture) {
                if (auto made = tools::image::for_model(*copied->picture, true, "pasted picture")) {
                    done.image = std::move(*made);
                }
            }
        } else if (auto made = dropped_picture(path)) {
            std::error_code ec;
            const auto w = core::utf8::to_wide(path);
            const auto size = w ? std::filesystem::file_size(std::filesystem::path{*w}, ec) : 0;
            done.details = details_line(extension_of(path), ec ? 0 : size, made->source_width, made->source_height, 0, 0);
            done.image = std::move(*made);
        } else {
            WA_INFO("app", "attached picture not decoded: {}", made.error().message);
        }
        WA_INFO("app", "attached picture decoded: {}", done.image ? "yes" : "no");
        {
            const std::scoped_lock lock{decoded_mutex_};
            decoded_.push_back(std::move(done));
        }
        PostMessageW(hwnd, WM_APP_PICTURE, 0, 0);
    });
}

void Shell::take_pictures() {
    std::vector<Decoded> done;
    {
        const std::scoped_lock lock{decoded_mutex_};
        done.swap(decoded_);
    }
    for (auto& d : done) {
        if (!d.details.empty()) {
            sheet::set_details(model_, d.path, std::move(d.details));
        }
        if (!d.image) {
            continue;
        }
        if (d.path.empty()) {
            sheet::paste_picture(model_, std::move(*d.image), seconds()); // its "Ctrl+V" for 2 s from now
        } else {
            sheet::attach_image(model_, d.path, std::move(*d.image));
        }
    }
    wants_frame_ = true;
}

void Shell::read_details(std::string path) {
    decoders_.start([this, hwnd = window_.get(), path = std::move(path)] {
        Decoded done{.path = path, .image = {}, .details = file_details(path)};
        {
            const std::scoped_lock lock{decoded_mutex_};
            decoded_.push_back(std::move(done));
        }
        PostMessageW(hwnd, WM_APP_PICTURE, 0, 0);
    });
}

void Shell::attach(std::vector<std::string> paths) {
    if (!model_.open) {
        open_sheet("files dropped");
    }
    // The zones hold here too: the agent's secrets and its own files are never attached.
    const auto refusal = [](const std::string& path) -> std::string {
        const auto w = core::utf8::to_wide(path);
        const auto c = w ? security::paths::canonical(*w, std::filesystem::current_path()) : core::Result<security::paths::Canonical>{};
        if (!w || !c) {
            return "cannot be read";
        }
        switch (security::paths::zone_of(*c, security::paths::system_roots())) {
        case security::paths::Zone::Secrets: return "a secrets file: never sent";
        case security::paths::Zone::AppSelf: return "the agent's own file";
        default: return {};
        }
    };
    const std::size_t before = model_.attachments.size();
    sheet::drop_files(model_, paths, refusal);
    for (std::size_t i = before; i < model_.attachments.size(); ++i) { // the new ones: what they are, in the background
        const auto& a = model_.attachments[i];
        if (!a.refused.empty()) {
            continue;
        }
        if (sheet::picture_file(a.path)) {
            decode_picture(a.path);
        } else {
            read_details(a.path);
        }
    }
    wants_frame_ = true;
}

void Shell::paste(bool as_text) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) { // no text: a picture, perhaps (phase 9)
        if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) ||
            IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG"))) {
            decode_picture({});
        }
        return;
    }
    if (!OpenClipboard(window_.get())) {
        return;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            if (auto utf8 = core::utf8::to_utf8(text)) {
                if (as_text) {
                    sheet::type(model_, *utf8);
                } else {
                    (void)sheet::paste(model_, *utf8, seconds()); // a long one: a chip, its "Ctrl+V" for 2 s
                }
                ui_->clear_focus();
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
}

void Shell::on_char(wchar_t unit) {
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        high_surrogate_ = unit;
        return;
    }
    std::wstring w;
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (high_surrogate_ == 0) {
            return;
        }
        w = {high_surrogate_, unit};
    } else {
        w = {unit};
    }
    high_surrogate_ = 0;
    if (unit < 0x20 || unit == 0x7F) {
        return; // Enter, Tab, Escape, Backspace, Ctrl+letters come as keys
    }
    if (auto utf8 = core::utf8::to_utf8(w)) {
        sheet::type(model_, *utf8);
        ui_->clear_focus();
        if (painter_) {
            painter_->typed(); // typing: Ctrl+C and Ctrl+A are the field's (final review I5)
        }
        wants_frame_ = true;
    }
}

void Shell::field_moved(std::size_t from, bool shift) {
    // The field's caret moved by a line or to a line's edge: with shift the selection reaches it, without it goes.
    auto& e = model_.input;
    if (!shift) {
        e.anchor = sheet::Edit::kNoAnchor;
    } else if (e.anchor == sheet::Edit::kNoAnchor) {
        e.anchor = from;
    }
    if (e.anchor == e.caret) {
        e.anchor = sheet::Edit::kNoAnchor;
    }
}

void Shell::on_key(WPARAM vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    // Stage 4d-1: the settings on top, none of their fields focused — the bar behind them takes nothing.
    const bool behind_settings = model_.open && sheet::view(model_) == sheet::View::Settings && !model_.private_focus && !model_.filter_focus &&
                                 !model_.settings.key_open; // Models' key field takes a paste (4d-2 review I1)
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    std::optional<sheet::Key> key;
    switch (vk) {
    case VK_RETURN:
        if (ui_->focus() != 0 && !model_.menu.open) {
            input_.activate = true; // a focused button takes Enter
        } else {
            key = sheet::Key::Enter;
        }
        break;
    case VK_ESCAPE:
        // The right button's menu, then a selection, before the sheet (responsive text).
        if (painter_ && painter_->take_escape()) {
            wants_frame_ = true;
            return;
        }
        key = sheet::Key::Escape;
        break;
    case VK_BACK: key = sheet::Key::Backspace; break;
    case VK_PRIOR: key = sheet::Key::PageUp; break;
    case VK_NEXT: key = sheet::Key::PageDown; break;
    case VK_DELETE: key = sheet::Key::Delete; break;
    case VK_LEFT: key = sheet::Key::Left; break;
    case VK_RIGHT: key = sheet::Key::Right; break;
    case VK_HOME:
    case VK_END:
        // The field's line as shown (phase 10: it wraps); the key and the filter keep their own.
        if (const auto at = model_.input.caret; !ctrl && !model_.menu.open && painter_->caret_edge(model_, vk == VK_END)) {
            field_moved(at, shift); // with shift it selects (responsive text)
            break;
        }
        key = vk == VK_END ? sheet::Key::End : sheet::Key::Home;
        break;
    case VK_TAB:
        if (model_.menu.open) {
            break; // not the buttons' under the menu (review I2)
        }
        if (sheet::view(model_) == sheet::View::Quick && !shift) {
            key = sheet::Key::Tab; // a quick answer's "Tab — more"
        } else {
            input_.tab = true;
            input_.shift = shift;
        }
        break;
    case VK_UP:
        if (const auto at = model_.input.caret; painter_->caret_line(model_, false)) { // a line up in the field; from its first, the history
            field_moved(at, shift);
            break;
        }
        if (!history_recalled_) { // the history is read when it is first wanted, not at start
            history_recalled_ = true;
            const auto asked = history::recent_questions(data_dir_ / (options_.smoke ? L"history-smoke" : L"history"), 50);
            for (auto it = asked.rbegin(); it != asked.rend(); ++it) {
                sheet::remember(model_, *it);
            }
        }
        key = sheet::Key::Up;
        break;
    case VK_DOWN:
        if (const auto at = model_.input.caret; painter_->caret_line(model_, true)) {
            field_moved(at, shift);
            break;
        }
        key = sheet::Key::Down;
        break;
    case 'V':
        if (ctrl && !behind_settings) {
            paste(shift); // Ctrl+Shift+V: as text, whatever its length (phase 10, screen 31)
        }
        break;
    case 'C': // the field's selection when the keys are its; else the sheet's (responsive text); nothing: nothing
        if (ctrl && painter_ && painter_->keys_to_field() && !sheet::field_selected(model_).empty()) {
            copy_text(sheet::field_selected(model_));
        } else if (ctrl && ui_ && ui_->selection().active()) {
            input_.copy = true; // made at the next frame's end, from the data it draws (final review I7)
        } else if (ctrl && !sheet::field_selected(model_).empty()) {
            copy_text(sheet::field_selected(model_));
        }
        break;
    case 'X': // the field's selected text cut to the clipboard
        if (ctrl) {
            if (const auto cut = sheet::cut_field(model_); !cut.empty()) {
                copy_text(cut);
            }
        }
        break;
    case 'A': // all of the field when the keys are its; else the pane under the mouse (its next frame), else the field
        if (ctrl) {
            if (ui_ && !(painter_ && painter_->keys_to_field()) && ui_->pane_at(input_.mouse_x, input_.mouse_y) != 0) {
                input_.select_all = true;
            } else {
                if (ui_) {
                    ui_->clear_selection();
                }
                sheet::select_all_field(model_);
            }
        }
        break;
    case 'E':
        if (behind_settings) {
            break; // the bar's editor is behind the settings (final review I3)
        }
        if (ctrl && shift) { // the panel's table to XLSX (phase 10): drawn by its next frame
            model_.export_xlsx = true;
        } else if (ctrl) { // the field as the editor, or folded back (phase 10, screen 29)
            sheet::toggle_editor(model_);
        }
        break;
    case 'M': // MODEL_PICKER_SPEC § 8: Ctrl+M the model menu, Ctrl+Shift+M back to the model before
        if (behind_settings) {
            break; // its menu would open unseen behind the settings (final review I3)
        }
        if (ctrl && shift) {
            react(sheet::toggle_model(model_, seconds()));
        } else if (ctrl) {
            model_.menu.open ? sheet::close_menu(model_) : sheet::open_menu(model_);
            ui_->clear_focus(); // the keys are the menu's (review I2)
        }
        break;
    case VK_OEM_COMMA: // Ctrl+, the settings (SETTINGS_SPEC § 2), again: back
        if (ctrl) {
            model_.settings.open ? react(sheet::close_settings(model_)) : open_settings();
        }
        break;
    case VK_OEM_4: // Ctrl+[ and Ctrl+]: thinking a level down or up
    case VK_OEM_6:
        if (ctrl) {
            react(sheet::step_level(model_, vk == VK_OEM_6 ? 1 : -1, seconds()));
        }
        break;
    case 'Z':
        // Ctrl+Z: right after a long paste became a chip, the text into the field (phase 10, screen 31);
        // on the change log, "Undo last"; on a done bar with Undo, that Undo.
        if (ctrl && sheet::paste_as_text(model_)) {
            break;
        }
        if (ctrl && sheet::view(model_) == sheet::View::Log) {
            react({sheet::undo_last(model_), false});
        } else if (ctrl && sheet::view(model_) == sheet::View::Done &&
                   std::ranges::any_of(model_.done_actions, [](const events::Action& a) { return a.id == "undo"; })) {
            react({sheet::press(model_, "undo"), false});
        }
        break;
    default: break;
    }
    if (key) {
        if (*key != sheet::Key::Escape) {
            ui_->clear_focus();
        }
        tick_clock(model_); // Enter may ask: its turn takes the time now
        react(sheet::key(model_, *key, ctrl, shift));
    }
    wants_frame_ = true;
}

LRESULT Shell::handle(UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = window_.get();
    if (msg == taskbar_created_ && taskbar_created_ != 0) { // Explorer restarted (review I-3)
        if (!options_.smoke) {
            add_tray_icon();
        }
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp == kHotkeyId && paused_) {
            // Paused (the tray's Pause agent, stage 4d-2): the shortcut does nothing; Show agent still does.
        } else if (wp == kHotkeyId && model_.open && model_.settings.open) {
            react(sheet::close_settings(model_)); // SETTINGS_SPEC § 4: back to the conversation, not away
            wants_frame_ = true;
        } else if (wp == kHotkeyId) {
            model_.open ? close_sheet("hotkey") : open_sheet("hotkey");
        }
        return 0;
    case WM_APP_KEY: {
        const std::unique_ptr<keycheck::Result> result{reinterpret_cast<keycheck::Result*>(lp)};
        key_checked(*result);
        return 0;
    }
    case WM_APP_OPEN:
        open_sheet("second launch");
        return 0;
    case WM_APP_GO:
        if (waiting_go_) {
            open_after_install(true);
        }
        return 0;
    case core::agent_ids::kExitMessage:
        exit_requested();
        return 0;
    case WM_APP_CAPTURE: {
        const DWORD affinity = wp != 0 ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
        for (HWND w : {window_.get(), dim_ ? dim_.get() : HWND{}}) {
            if (w != nullptr) {
                SetWindowDisplayAffinity(w, affinity);
            }
        }
        return 0;
    }
    case WM_APP_WAKE:
        drain_agent();
        return 0;
    case WM_APP_PICTURE:
        take_pictures();
        return 0;
    case WM_DROPFILES: { // without OLE's drop target
        auto paths = paths_of(reinterpret_cast<HDROP>(wp), sheet::kMaxAttachments);
        DragFinish(reinterpret_cast<HDROP>(wp));
        attach(std::move(paths));
        return 0;
    }
    case WM_APP_TOAST:
        take_toast_clicks();
        return 0;
    case WM_APP_MONITOR:
        // A new reading while the report is shown: its lines and values move on.
        if (model_.report && meter_ && sheet::view(model_) == sheet::View::Report) {
            report::refresh(*model_.report, meter_->snapshot());
            wants_frame_ = true;
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONDOWN) {
            tray_pressed_at_ = seconds(); // its release decides: the press that closed the menu does not open it
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            show_tray_menu();
        } else if (LOWORD(lp) == WM_LBUTTONUP) {
            model_.open ? close_sheet("tray") : open_sheet("tray");
        }
        return 0;
    case WM_CHAR:
        if (model_.open) {
            on_char(static_cast<wchar_t>(wp));
        }
        return 0;
    case WM_SYSKEYDOWN:
        // Alt+Enter in the model menu: the row for the next message only (MODEL_PICKER_SPEC § 8);
        // any other Alt key is Windows' own.
        if (wp == VK_RETURN && model_.open && model_.menu.open) {
            tick_clock(model_);
            react(sheet::key(model_, sheet::Key::Enter, false, false, true));
            wants_frame_ = true;
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == L'\r' && model_.open) {
            return 0; // Alt+Enter's character: no beep of a menu Windows looks for
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F12) { // the debug window (Debug builds); F12 cannot be a global hotkey
            toggle_debug();
            return 0;
        }
        if (model_.open) {
            on_key(wp);
        }
        return 0;
    case WM_SETCURSOR:
        // The cursor the last frame wanted (screens 26–27): a hand over what can be clicked, an
        // I-beam over text, the arrow elsewhere and over what is disabled.
        if (LOWORD(lp) == HTCLIENT && ui_) {
            const auto c = ui_->cursor();
            SetCursor(LoadCursorW(nullptr, c == ui::Cursor::Hand ? IDC_HAND : c == ui::Cursor::IBeam ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        const auto p = placement::to_logical(placed_, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        input_.mouse_x = p.x;
        input_.mouse_y = p.y;
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking_mouse_ = TrackMouseEvent(&tme) != FALSE;
        }
        wants_frame_ = true;
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        input_.mouse_x = input_.mouse_y = -1.0e9f;
        wants_frame_ = true;
        return 0;
    case WM_LBUTTONDOWN: {
        // Clicks in a row (a double click a word, a triple its text): within the system's time and distance.
        const LONG at = GetMessageTime();
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (model_.open && open_style_ && open_box_.w > 0) {
            // In the shadow's room, not on the sheet: a click beside it — the sheet goes, as when a
            // click lands on another window.
            // (Not while it is still coming out of the handle — where it is going counts; nor on what
            // the sheet drew past its own edge: the right button's menu.)
            const auto p = placement::to_logical(placed_, x, y);
            const bool on_target = painter_ && std::ranges::any_of(painter_->targets(), [&](const auto& t) {
                return p.x >= t.box.x && p.x < t.box.x + t.box.w && p.y >= t.box.y && p.y < t.box.y + t.box.h;
            });
            if (!on_target && !model_.settings.open && placement::beside_sheet(p, open_box_, open_to_)) { // settings stay (4d-1)
                close_sheet("click beside");
                return 0;
            }
        }
        const bool again = static_cast<DWORD>(at - last_click_at_) <= GetDoubleClickTime() &&
                           std::abs(x - last_click_x_) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           std::abs(y - last_click_y_) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        click_run_ = again ? std::min(click_run_ + 1, 3) : 1;
        last_click_at_ = at;
        last_click_x_ = x;
        last_click_y_ = y;
        input_.clicks = click_run_;
        input_.mouse_down = true;
        input_.mouse_pressed = true;
        SetCapture(hwnd);
        wants_frame_ = true;
        return 0;
    }
    case WM_RBUTTONDOWN: // the menu with Copy and Select all (responsive text)
        input_.right_pressed = true;
        wants_frame_ = true;
        return 0;
    case WM_LBUTTONUP:
        input_.mouse_down = false;
        input_.mouse_released = true;
        ReleaseCapture();
        wants_frame_ = true;
        return 0;
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        input_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * static_cast<float>(lines);
        input_.notches += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        wants_frame_ = true;
        return 0;
    }
    case WM_ACTIVATE:
        // Gone to another program: the sheet closes; to its own dialog (a CSV's Save as): it stays.
        // The settings stay while the user is away (SETTINGS_SPEC § 4: a key fetched from the browser).
        if (LOWORD(wp) == WA_INACTIVE && model_.open && !model_.settings.open &&
            !focus::owned_by(reinterpret_cast<HWND>(lp), window_.get())) {
            close_sheet("focus lost");
        } else if (LOWORD(wp) == WA_INACTIVE && model_.menu.open) {
            sheet::close_menu(model_); // to its own dialog: the sheet stays, the model menu goes
            wants_frame_ = true;
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return model_.open ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_SETTINGCHANGE:
        apply_text_scale(); // the Windows text size may have changed
        apply_reduced_motion(); // "Animation effects" switched
        apply_theme(false); // the apps' mode may have changed (Windows says it several times: one change)
        wants_frame_ = true;
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMSUSPEND) { // next to watch_strip's lines
            WA_INFO("app", "power: {}", wp == PBT_APMSUSPEND ? "going to sleep" : "awake again");
        }
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: {
        if (msg == WM_DISPLAYCHANGE) {
            WA_INFO("app", "display changed: {} x {}", LOWORD(lp), HIWORD(lp));
        } else {
            WA_INFO("app", "DPI changed: {}", LOWORD(wp));
        }
        // The window's centre, not its top: open, the window starts above the monitor (review N-2).
        monitor_ = monitor_at({(placed_.window.left + placed_.window.right) / 2,
                               (placed_.window.top + placed_.window.bottom) / 2});
        ensure_orb(monitor_.scale);
        fit_to_monitor();
        apply_frame_rate(); // another monitor, another rate (motion rework, Review Focus 3)
        wants_frame_ = true;
        return 0;
    }
    case WM_CAPTURECHANGED: // the button state is lost with the capture (review N-8)
        input_.mouse_down = false;
        wants_frame_ = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        // Alt+F4 on the open sheet closes the sheet; the agent stays in the tray (review I-2).
        // It quits from the tray's Exit.
        if (model_.open) {
            close_sheet("alt+f4");
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

auto Shell::orb_frame(double now) -> bool {
    if (!frame_kept_ || need_ != sheet::FrameNeed::OrbIdle || wants_frame_ || !painter_->orb_frame_due(kept_at_, now)) {
        return false;
    }
    painter_->orb_step(now);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kept_w_, kept_h_);
    renderer_->begin(kept_w_, kept_h_, kept_scale_);
    const bool drawn = painter_->draw_kept(now);
    renderer_->end();
    if (!drawn) {
        return false;
    }
    const double swap_start = seconds();
    context_->swap_buffers();
    ++frames_;
    const auto& stats = renderer_->last_frame();
    metrics_.add({.t = now,
                  .update_ms = 0,
                  .draw_ms = (swap_start - now) * 1000.0,
                  .work_ms = (swap_start - now) * 1000.0,
                  .swap_ms = (seconds() - swap_start) * 1000.0,
                  .draw_calls = stats.draw_calls,
                  .instances = stats.instances,
                  .reason = debug::FrameReason::IdleOpen});
    return true;
}

void Shell::frame() {
    const double now = seconds();
    if (orb_frame(now)) {
        return; // nothing but the slow orb moved: the kept frame and the orb (phase 9)
    }
    tick_clock(model_);
    if (sheet::view(model_) == sheet::View::Terminal && now - usage_at_ >= 1.0) {
        measure_usage(now);
    }
    const sheet::Shape shape = painter_->update(model_, now);
    update_dim(); // the screen around, by the question's scene (MS 11): a step of its opacity a frame
    bool close_ended = false;
    if (open_style_ && !model_.open && painter_->window_hidden()) {
        set_open_style(false); // the close has ended: only now the resting styles (no flash)
        set_background_style(true); // …and the sign's own, before this frame's region: never a frame with the whole canvas taking the mouse
        close_ended = true;
    }
    const double updated = seconds();
    // The window is the canvas for the monitor (motion rework, task 5): made once at the agent's
    // largest, never resized in a frame; the shapes move inside it, the region decides the clicks.
    const placement::Placement p = placement::canvas(monitor_);
    apply_placement(p);
    const ui::layout::Box panel_box = placement::panel_in(p, shape);
    const int w = p.window.right - p.window.left;
    const int h = p.window.bottom - p.window.top;

    if (need_ != sheet::FrameNeed::Caret) {
        painter_->render_orb(now); // at the caret's flips the orb stands still: never a jump
    }
    // The open sheet waiting: this frame drawn into the kept one, the orb then put on its copy
    // (the orb's frames to come copy it again).
    const bool keep = need_ == sheet::FrameNeed::OrbIdle;
    if (keep) {
        painter_->begin_keep(w, h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer_->begin(w, h, p.scale);
    ui_->begin_frame(now, p.scale, input_);
    const auto reaction = painter_->draw(model_, panel_box);
    ui_->end_frame();
    renderer_->end();
    if (keep) {
        painter_->end_keep();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer_->begin(w, h, p.scale);
        (void)painter_->draw_kept(now);
        renderer_->end();
    }
    const double drawn = seconds();
    update_region(p, panel_box); // before the frame shows (MOTION_SPEC § 5.3): never a frame behind it
    if (close_ended && !region_on_ && options_.smoke) {
        // The final review (I4): the frame that ends a close left the window with no region; it then
        // took the mouse over the whole canvas until some later frame.
        WA_ERROR("app", "smoke: the frame that ended the close left the window without the handle's region");
        smoke_broken_ = true;
    }
    const double swap_start = seconds();
    context_->swap_buffers();
    if (ready_pending_ && model_.open) {
        signal_ready();
    }
    const double swap_end = seconds();

    // One frame's transitions are consumed; the button state stays.
    input_.mouse_pressed = false;
    input_.mouse_released = false;
    input_.right_pressed = false;
    input_.select_all = false;
    input_.copy = false;
    if (ui_ && ui_->take_copied(selection_text_)) { // Ctrl+C's text, made in this frame
        copy_text(selection_text_);
    }
    input_.tab = false;
    input_.activate = false;
    input_.wheel = 0;
    input_.notches = 0;
    react(reaction);
    panel_on_screen_ = {p.window.left + static_cast<LONG>(std::floor(panel_box.x * p.scale)),
                        p.window.top + static_cast<LONG>(std::floor(panel_box.y * p.scale)),
                        p.window.left + static_cast<LONG>(std::ceil((panel_box.x + panel_box.w) * p.scale)),
                        p.window.top + static_cast<LONG>(std::ceil((panel_box.y + panel_box.h) * p.scale))};
    ++frames_;
    note_stream_frame(now, swap_start - now, swap_end - swap_start);
    need_ = painter_->frame_need(model_, now);
    // Only the orb moves now and this frame was kept: the orb's frames copy it until the caret.
    frame_kept_ = keep && need_ == sheet::FrameNeed::OrbIdle;
    if (frame_kept_) {
        kept_at_ = now;
        kept_w_ = w;
        kept_h_ = h;
        kept_scale_ = p.scale;
    } else {
        painter_->drop_kept();
    }
    const auto reason = model_.streaming || painter_->revealed() < model_.answer.size() ? debug::FrameReason::Streaming
                        : painter_->moving()                                         ? debug::FrameReason::Animation
                        : need_ == sheet::FrameNeed::Full                            ? debug::FrameReason::Animation
                        : need_ != sheet::FrameNeed::None                            ? debug::FrameReason::IdleOpen
                        : painter_->rest_motion()                                    ? debug::FrameReason::RestMotion
                                                                                     : debug::FrameReason::Rest;
    const auto& stats = renderer_->last_frame();
    metrics_.add({.t = now,
                  .update_ms = (updated - now) * 1000.0,
                  .draw_ms = (drawn - updated) * 1000.0,
                  .work_ms = (swap_start - now) * 1000.0,
                  .swap_ms = (swap_end - swap_start) * 1000.0,
                  .draw_calls = stats.draw_calls,
                  .instances = stats.instances,
                  .reason = reason});
    if (options_.smoke && smoke_stage_ == 4) {
        rest_work_ += swap_start - now;
        rest_swap_ += swap_end - swap_start;
    }
    // A handle hidden under a fullscreen app draws nothing.
    wants_frame_ = need_ == sheet::FrameNeed::Full && !handle_hidden_;
    if (!first_frame_logged_) {
        first_frame_logged_ = true;
        WA_INFO("app", "first frame {:.0f} ms after process start", process_age_ms());
        metrics_.mark_startup("first frame", process_age_ms());
    }
}

void Shell::note_stream_frame(double now, double work, double swap) {
    auto& s = stream_stats_;
    if (model_.streaming) {
        if (!s.on) {
            s = {.on = true, .start = now, .last = now};
        }
        s.longest = s.moving ? std::max(s.longest, now - s.last) : s.longest;
        s.hitches += s.moving && now - s.last > 2.5 / 240.0 ? 1 : 0; // only where it should be smooth
        s.moving = painter_->moving();
        s.longest_work = std::max(s.longest_work, work);
        s.longest_swap = std::max(s.longest_swap, swap);
        s.last = now;
        ++s.frames;
        return;
    }
    if (!s.on) {
        return;
    }
    s.on = false;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const bool known = GetMonitorInfoW(MonitorFromWindow(window_.get(), MONITOR_DEFAULTTONEAREST), &mi) &&
                       EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    const double span = std::max(s.last - s.start, 1e-6);
    WA_INFO("app",
            "stream: {} frames in {:.2f} s ({:.0f} a second), longest gap {:.1f} ms, {} gaps over 10 ms; longest "
            "frame work {:.2f} ms, swap {:.2f} ms; monitor {} Hz",
            s.frames, span, static_cast<double>(s.frames - 1) / span, s.longest * 1000.0, s.hitches,
            s.longest_work * 1000.0, s.longest_swap * 1000.0,
            known ? static_cast<int>(mode.dmDisplayFrequency) : 0);
}

void Shell::toggle_debug() {
#ifdef WA_DEBUG_UI
    if (!debug_) {
        auto made = debug::DebugWindow::create(GetModuleHandleW(nullptr));
        context_->make_current(); // the debug window's context was current while it was made
        if (!made) {
            WA_WARN("debug", "debug window: {}", made.error().message);
            return;
        }
        debug_ = std::move(*made);
    }
    debug_->toggle();
    debug_frame_at_ = 0;
#endif
}

void Shell::draw_debug() {
#ifdef WA_DEBUG_UI
    if (!debug_ || !debug_->visible() || seconds() - debug_frame_at_ < 1.0 / 30.0) {
        return;
    }
    debug_frame_at_ = seconds();
    debug_->frame({.metrics = metrics_,
                   .model = model_,
                   .config = config_,
                   .live_hotkey = live_hotkey_,
                   .now = seconds(),
                   .atlas_fill = atlas_->fill(),
                   .atlas = atlas_->stats(),
                   .revealed = painter_->revealed(),
                   .rest_motion = painter_->rest_motion(),
                   .gl_driver = context_->driver_id(),
                   .logs_dir = logs_dir_.native()});
    context_->make_current(); // back to the sheet's
#endif
}

void Shell::write_snapshot() {
#ifdef WA_DEBUG_UI
    if (!options_.debug_capture.empty()) {
        if (!debug_) {
            if (auto made = debug::DebugWindow::create(GetModuleHandleW(nullptr))) {
                debug_ = std::move(*made);
            }
        }
        if (debug_) {
            const std::filesystem::path dir{options_.debug_capture};
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const char* names[] = {"performance", "memory", "log", "state"};
            for (int tab = 0; tab < 4; ++tab) {
                const auto r = debug_->capture({.metrics = metrics_,
                                                .model = model_,
                                                .config = config_,
                                                .live_hotkey = live_hotkey_,
                                                .now = seconds(),
                                                .atlas_fill = atlas_->fill(),
                                                .atlas = atlas_->stats(),
                                                .revealed = painter_->revealed(),
                                                .rest_motion = painter_->rest_motion(),
                                                .gl_driver = context_->driver_id(),
                                                .logs_dir = logs_dir_.native()},
                                               tab, 980, 760, dir / (std::string{"debug-"} + names[tab] + ".png"));
                if (!r) {
                    WA_WARN("debug", "capture: {}", r.error().message);
                }
            }
            context_->make_current();
        }
    }
#endif
    if (options_.snapshot.empty()) {
        return;
    }
    const auto text = debug::snapshot_text({.metrics = metrics_,
                                            .model = model_,
                                            .config = config_,
                                            .live_hotkey = live_hotkey_,
                                            .now = seconds(),
                                            .atlas_fill = atlas_ ? atlas_->fill() : -1.0});
    std::ofstream out(std::filesystem::path{options_.snapshot}, std::ios::binary | std::ios::trunc);
    out << text;
    WA_INFO("app", "snapshot written ({} bytes)", text.size());
}

auto Shell::m5_step() -> int {
    if (m4_refused_) {
        return 1;
    }
    const double elapsed = seconds() - smoke_started_;
    if (elapsed > 60.0) {
        WA_ERROR("app", "m5: stuck at stage {} after 60 s", smoke_stage_);
        return 1;
    }
    const std::filesystem::path dir{options_.m5};
    const auto files = [&](const wchar_t* folder) {
        std::error_code ec;
        std::size_t n = 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir / folder, ec)) {
            n += e.is_regular_file() ? 1 : 0;
        }
        return n;
    };
    if (model_.permission) {
        // M5 answers its own questions, on its fenced copy only; on the installers' card it leaves
        // the last file out, as a user would untick one.
        const auto& p = *model_.permission;
        if (smoke_stage_ == 1 && p.items.size() > 1 && p.where.find("Downloads") != std::string::npos && m5_left_out_.empty()) {
            sheet::tick(model_, p.items.size() - 1);
            m5_left_out_ = p.items.back().name;
            m5_installers_ = p.items.size();
            WA_INFO("m5", "the installers' card: {} files, {} left out", p.items.size(), m5_left_out_);
        }
        WA_INFO("app", "m5: allowing {}: {}", p.tool, p.reason);
        react({sheet::answer(model_, events::Choice::Allow), false});
        return -1;
    }
    switch (smoke_stage_) {
    case 0:
        if (frames_ == 0) {
            break;
        }
        m5_temp_ = files(L"Temp");
        m5_downloads_ = files(L"Downloads");
        set_open_style(true);
        sheet::open(model_);
        sheet::type(model_, "Clean up temp files and delete old installers from Downloads");
        react(sheet::key(model_, sheet::Key::Enter, false));
        smoke_stage_ = 1;
        break;
    case 1:
        if (sheet::view(model_) == sheet::View::Done) {
            if (model_.done_title != "Cleaned up") {
                WA_ERROR("app", "m5: ended with \"{}\" ({})", model_.done_title, model_.done_detail);
                return 1;
            }
            const auto temp_left = files(L"Temp");
            const auto downloads_left = files(L"Downloads");
            std::error_code ec;
            const bool kept = !m5_left_out_.empty() &&
                              std::filesystem::exists(dir / L"Downloads" / core::utf8::to_wide(m5_left_out_).value_or(L""), ec);
            if (temp_left != 0 || !kept || downloads_left != m5_downloads_ - (m5_installers_ - 1)) {
                WA_ERROR("app", "m5: after the clean-up {} temp files left, {} in Downloads (was {}, {} offered), \"{}\" {}", temp_left,
                         downloads_left, m5_downloads_, m5_installers_, m5_left_out_, kept ? "kept" : "gone");
                return 1;
            }
            const auto entries = journal::read(dir / L"changes.jsonl");
            const auto it = std::ranges::find(entries, model_.task, &journal::Entry::task);
            if (it == entries.end() || it->changes.size() != m5_temp_ + m5_installers_ - 1) {
                WA_ERROR("app", "m5: the journal does not have the clean-up's {} changes", m5_temp_ + m5_installers_ - 1);
                return 1;
            }
            WA_INFO("app", "m5: cleaned up: {} temp files and {} installers in the bin, {} left out; journaled", m5_temp_,
                    m5_installers_ - 1, m5_left_out_);
            react({sheet::press(model_, "undo"), false});
            smoke_stage_ = 2;
        }
        break;
    case 2:
        if (sheet::view(model_) == sheet::View::Done && model_.done_title != "Cleaned up") {
            if (model_.done_title != "Undone") {
                WA_ERROR("app", "m5: undo ended with \"{}\" ({})", model_.done_title, model_.done_detail);
                return 1;
            }
            if (files(L"Temp") != m5_temp_ || files(L"Downloads") != m5_downloads_) {
                WA_ERROR("app", "m5: after the undo {} temp files (was {}), {} in Downloads (was {})", files(L"Temp"), m5_temp_,
                         files(L"Downloads"), m5_downloads_);
                return 1;
            }
            const auto verified = security::verify_audit(dir / L"audit.jsonl");
            if (!verified) {
                WA_ERROR("app", "m5: the audit does not verify: {}", verified.error().message);
                return 1;
            }
            WA_INFO("app", "m5: undone; audit: {} records, the chain holds; passed after {:.1f} s", *verified, elapsed);
            write_snapshot();
            return 0;
        }
        break;
    default: break;
    }
    return -1;
}

auto Shell::m4_step() -> int {
    if (m4_refused_) {
        return 1;
    }
    const double elapsed = seconds() - smoke_started_;
    if (elapsed > 60.0) {
        WA_ERROR("app", "m4: stuck at stage {} after 60 s", smoke_stage_);
        return 1;
    }
    if (model_.permission) { // the run answers its own questions: M4 on a fenced copy only
        WA_INFO("app", "m4: allowing {}: {}", model_.permission->tool, model_.permission->reason);
        react({sheet::answer(model_, events::Choice::Allow), false});
        return -1;
    }
    const std::filesystem::path dir{options_.m4};
    const auto count = [&](const wchar_t* folder) {
        std::error_code ec;
        std::size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir / folder, ec)) {
            n += e.is_regular_file() ? 1 : 0;
        }
        return n;
    };
    switch (smoke_stage_) {
    case 0:
        if (frames_ == 0) {
            break;
        }
        set_open_style(true);
        sheet::open(model_);
        sheet::type(model_, "Sort my desktop");
        react(sheet::key(model_, sheet::Key::Enter, false));
        smoke_stage_ = 1;
        break;
    case 1:
        if (sheet::view(model_) == sheet::View::Done) {
            if (model_.done_title != "Desktop sorted") {
                WA_ERROR("app", "m4: ended with \"{}\" ({})", model_.done_title, model_.done_detail);
                return 1;
            }
            WA_INFO("app", "m4: sorted: Documents {}, Images {}, Installers {}, Archives {}", count(L"Documents"),
                    count(L"Images"), count(L"Installers"), count(L"Archives"));
            react({sheet::press(model_, "undo"), false});
            smoke_stage_ = 2;
        }
        break;
    case 2:
        if (sheet::view(model_) == sheet::View::Done && model_.done_title != "Desktop sorted") {
            if (model_.done_title != "Undone") {
                WA_ERROR("app", "m4: undo ended with \"{}\" ({})", model_.done_title, model_.done_detail);
                return 1;
            }
            const auto verified = security::verify_audit(dir / L"audit.jsonl");
            if (!verified) {
                WA_ERROR("app", "m4: the audit does not verify: {}", verified.error().message);
                return 1;
            }
            WA_INFO("app", "m4: restored; audit: {} records, the chain holds; passed after {:.1f} s", *verified, elapsed);
            write_snapshot();
            return 0;
        }
        break;
    default: break;
    }
    return -1;
}

auto Shell::replay_step() -> int {
    if (m4_refused_) {
        return 1;
    }
    const double elapsed = seconds() - smoke_started_;
    if (elapsed > 30.0) {
        WA_ERROR("app", "replay: stuck at stage {} after 30 s", smoke_stage_);
        return 1;
    }
    switch (smoke_stage_) {
    case 0:
        if (frames_ == 0) {
            break;
        }
        set_open_style(true);
        sheet::open(model_);
        sheet::type(model_, "Why is my computer slow?");
        react(sheet::key(model_, sheet::Key::Enter, false));
        smoke_stage_ = 1;
        break;
    case 1:
        if (model_.failed) {
            WA_ERROR("app", "replay: failed: {}", model_.done_title);
            return 1;
        }
        if (model_.report && model_.spent) {
            WA_INFO("app", "replay: the report ({} tiles, {} programs) and its cost \"{}\" after {:.1f} s", model_.report->tiles.size(),
                    model_.report->processes.size(), sheet::spent_text(*model_.spent), elapsed);
            write_snapshot();
            return 0;
        }
        break;
    default: break;
    }
    return -1;
}

// The three scenarios of the mockups on DeepSeek itself (phase 7, M7): the system report, the
// drive's quick answer, the desktop sorted — on a copy inside %TEMP% behind a fence, every
// question answered Allow by the run (only here). Each one's screen and cost go to the log.
auto Shell::m7_step() -> int {
    if (m4_refused_) {
        return 1;
    }
    constexpr std::array<std::string_view, 3> kAsked{"Why is my computer slow?", "How much space is left on drive C?",
                                                      "Sort the files on my desktop into folders by type"};
    if (seconds() - m7_stage_started_ > 180.0 && smoke_stage_ > 0) {
        WA_ERROR("app", "m7: no answer to \"{}\" after 180 s", kAsked[static_cast<std::size_t>((smoke_stage_ - 1) / 2)]);
        return 1;
    }
    if (model_.permission) {
        // Changes to files: Allow, only ever on the fenced copy. A command: No — a fence lets no
        // command through, and the model must do with its tools (it tried PowerShell first, live).
        const bool command = model_.permission->tool == "shell.run";
        WA_INFO("app", "m7: {} {}: {}", command ? "refusing" : "allowing", model_.permission->tool, model_.permission->reason);
        react({sheet::answer(model_, command ? events::Choice::Deny : events::Choice::Allow), false});
        return -1;
    }
    const auto ask = [&](std::string_view text) {
        set_open_style(true);
        sheet::open(model_);
        sheet::type(model_, text);
        react(sheet::key(model_, sheet::Key::Enter, false));
        m7_stage_started_ = seconds();
    };
    if (smoke_stage_ == 0) {
        if (frames_ == 0) {
            return -1;
        }
        ask(kAsked[0]);
        smoke_stage_ = 1;
        return -1;
    }
    if (model_.failed) {
        WA_ERROR("app", "m7: \"{}\" failed: {}", kAsked[static_cast<std::size_t>((smoke_stage_ - 1) / 2)], model_.done_title);
        return 1;
    }
    if (!model_.spent || model_.streaming) {
        return -1; // the answer is not complete yet
    }
    const auto screen = sheet::view(model_);
    WA_INFO("app", "m7: \"{}\" -> screen {}, report {}, quick {}, steps {}, cost \"{}\" (prompt {}, cache hit {}, completion {})",
            kAsked[static_cast<std::size_t>((smoke_stage_ - 1) / 2)], static_cast<int>(screen), model_.report.has_value(),
            model_.quick.has_value(), model_.steps.size(), sheet::spent_text(*model_.spent), model_.spent->prompt, model_.spent->cache_hit,
            model_.spent->completion);
    if (smoke_stage_ == 1) {
        ask(kAsked[1]);
        smoke_stage_ = 3;
    } else if (smoke_stage_ == 3) {
        ask(kAsked[2]);
        smoke_stage_ = 5;
    } else {
        std::error_code ec;
        std::size_t folders = 0;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path{options_.m7} / L"Desktop", ec)) {
            folders += e.is_directory() ? 1 : 0;
        }
        WA_INFO("app", "m7: done; the copy's desktop has {} folders now", folders);
        write_snapshot();
        return 0;
    }
    return -1;
}

auto Shell::animations_on() const -> bool {
    return !(options_.smoke && options_.reduced_motion) && window_animations();
}

void Shell::scene_mouse(float x, float y) {
    input_.mouse_x = x;
    input_.mouse_y = y;
    wants_frame_ = true;
}

// An acceptance script (app::scene_run): the keys through on_key, the mouse through the frame's
// input — the paths the user's take. The sheet is never activated: the user's window keeps the
// keyboard. Every step is logged with its time and the frames drawn so far.
auto Shell::scene_step() -> int {
    using scene_run::Do;
    const double now = seconds();
    if (scene_ == nullptr) {
        scene_ = scene_run::find(options_.scene);
        if (scene_ == nullptr) {
            WA_ERROR("app", "scene: no script named \"{}\"", options_.scene);
            return 1;
        }
        if (scene_->sandbox == options_.m5.empty()) {
            WA_ERROR("app", "scene {}: {}", scene_->name,
                     scene_->sandbox ? "it needs --m5=<a folder of made-up files under %TEMP%>" : "it is played without --m5");
            return 1;
        }
        scene_since_ = now;
        WA_INFO("app", "scene {}: {} steps; animations {}", scene_->name, scene_->steps.size(), animations_on() ? "on" : "off");
    }
    if (frames_ == 0) { // the handle first, as at a real start
        wants_frame_ = true;
        scene_since_ = now;
        return -1;
    }
    const auto& step = scene_->steps[scene_at_];
    if (now - smoke_started_ > 90.0) {
        std::string names;
        for (const auto& t : painter_->targets()) {
            names += t.name;
            names += ' ';
        }
        WA_ERROR("app", "scene {}: stuck at step {} (\"{}\") after 90 s; on screen: {}", scene_->name, scene_at_, step.text, names);
        return 1;
    }
    std::optional<ui::layout::Box> box;
    bool ready = true;
    // "name@dx,dy": a place inside the target, from its top left corner.
    std::string_view target = step.text;
    std::optional<std::pair<float, float>> offset;
    if (const auto at = target.find('@'); at != std::string_view::npos) {
        const std::string_view place = target.substr(at + 1);
        float dx = 0;
        float dy = 0;
        const auto [end, bad] = std::from_chars(place.data(), place.data() + place.size(), dx);
        if (bad == std::errc{} && end != place.data() + place.size() && *end == ',') {
            std::from_chars(end + 1, place.data() + place.size(), dy);
        }
        offset = {dx, dy};
        target = target.substr(0, at);
    }
    const auto place_of = [&](const ui::layout::Box& b) {
        return offset ? std::pair{b.x + offset->first, b.y + offset->second} : std::pair{b.x + b.w / 2, b.y + b.h / 2};
    };
    switch (step.what) {
    case Do::Press:
        ready = std::ranges::any_of(model_.actions, [&](const events::Action& a) { return a.id == step.text; });
        break;
    case Do::Click:
    case Do::Hover:
        if (step.text != "-") {
            for (const auto& t : painter_->targets()) {
                if (t.name == target) {
                    box = t.box;
                }
            }
            ready = box.has_value() || scene_click_ > 0; // a click begun goes on (its target may move away)
        }
        break;
    case Do::Settle:
        ready = model_.activity != sheet::Activity::Thinking && !model_.permission && !painter_->moving();
        break;
    default: break;
    }
    if (!ready) {
        scene_ready_ = -1;
        return -1;
    }
    if (scene_ready_ < 0) {
        scene_ready_ = now;
    }
    if (now - std::max(scene_since_, scene_ready_) < step.after) {
        return -1;
    }
    const auto open = [&] {
        if (!model_.open) {
            set_open_style(true);
            sheet::open(model_);
        }
    };
    switch (step.what) {
    case Do::Open: open(); break;
    case Do::Hotkey:
        if (model_.open) {
            close_sheet("scene");
        } else {
            open();
        }
        break;
    case Do::Type:
        for (const char c : step.text) { // plain letters: one unit each
            on_char(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        break;
    case Do::Enter: on_key(VK_RETURN); break;
    case Do::Esc: on_key(VK_ESCAPE); break;
    case Do::Press: react({sheet::press(model_, step.text), false}); break;
    case Do::Hover:
        if (box) {
            scene_mouse(place_of(*box).first, place_of(*box).second);
        } else {
            scene_mouse(-1.0e9f, -1.0e9f);
        }
        break;
    case Do::Click:
        // The mouse comes onto it, is pressed a quarter of a second later, let go a frame after,
        // and leaves a frame after that.
        if (scene_click_ == 0 && box) {
            scene_mouse(place_of(*box).first, place_of(*box).second);
            scene_click_ = 1;
            scene_click_at_ = now;
        } else if (scene_click_ == 1 && now - scene_click_at_ >= 0.25) {
            input_.clicks = 1;
            input_.mouse_down = true;
            input_.mouse_pressed = true;
            scene_click_ = 2;
            scene_click_at_ = now;
            scene_click_frame_ = frames_;
        } else if (scene_click_ == 2 && frames_ > scene_click_frame_ && now - scene_click_at_ >= 0.08) {
            input_.mouse_down = false;
            input_.mouse_released = true;
            scene_click_ = 3;
            scene_click_frame_ = frames_;
        } else if (scene_click_ == 3 && frames_ > scene_click_frame_) {
            scene_mouse(-1.0e9f, -1.0e9f);
            scene_click_ = 4;
        }
        wants_frame_ = true;
        if (scene_click_ != 4) {
            return -1;
        }
        break;
    case Do::Settle: break;
    case Do::End:
        WA_INFO("app", "scene {}: passed, {} frames in {:.1f} s", scene_->name, frames_, now - smoke_started_);
        return 0;
    }
    WA_INFO("app", "scene {}: step {} done at {:.2f} s, {} frames", scene_->name, scene_at_, now - smoke_started_, frames_);
    ++scene_at_;
    scene_since_ = now;
    scene_ready_ = -1;
    scene_click_ = 0;
    wants_frame_ = true;
    return -1;
}

auto Shell::smoke_step() -> int {
    if (options_.smoke_exit) { // the installer's exit message, sent to this window as it sends it
        // After a frame or two of the open sheet: --after-install's handoff has its frame (the installer told).
        if (!exit_posted_ && frames_ < 2) {
            // A fullscreen app in front pauses the resting motion and no second frame comes by itself
            // (the run then waited its 10 s out: the gate, 2026-11-11, a video playing): the run asks.
            wants_frame_ = true;
        }
        if (!exit_posted_ && frames_ >= 2 && !waiting_go_ && !ready_pending_) {
            exit_posted_ = true;
            PostMessageW(window_.get(), core::agent_ids::kExitMessage, 0, 0);
        }
        if (seconds() - smoke_started_ > 10.0) {
            WA_ERROR("app", "smoke: the exit message did not end the run");
            return 1;
        }
        return -1;
    }
    if (!options_.scene.empty()) {
        return scene_step();
    }
    if (!options_.replay.empty()) {
        return replay_step();
    }
    if (!options_.m7.empty()) {
        return m7_step();
    }
    if (!options_.m4.empty()) {
        return m4_step();
    }
    if (!options_.m5.empty()) {
        return m5_step();
    }
    const double elapsed = seconds() - smoke_started_;
    if (elapsed > 30.0 && smoke_stage_ != 4) { // 4 is the measured rest, timed by itself
        WA_ERROR("app", "smoke: stuck at stage {} after 30 s", smoke_stage_);
        return 1;
    }
    if (options_.measure && smoke_stage_ == 0) { // no scenario: straight to the measured rest
        if (frames_ == 0) {
            if (options_.open_idle) {
                sheet::open(model_); // --open-idle: the open sheet with nothing to do is measured
            }
            wants_frame_ = true;
            return -1;
        }
        if (options_.open_idle && (painter_->animating() || painter_->moving())) {
            return -1; // measured once the sheet has opened
        }
        smoke_stage_ = 4;
        rest_started_ = seconds();
        rest_cpu_ = process_cpu_ms();
        rest_thread_cpu_ = thread_cpu_ms();
        rest_frames_ = frames_;
        return -1;
    }
    switch (smoke_stage_) {
    case 0: // the handle first, then open and ask: the opening plays as it does for the user
        if (frames_ == 0 || elapsed < 0.3) {
            break;
        }
        if (HMENU menu = build_tray_menu()) { // the tray menu as built, never shown
            log_menu(menu, 0);
            DestroyMenu(menu);
        }
        set_open_style(true);
        sheet::open(model_);
        sheet::type(model_, "Why is my computer slow?");
        react(sheet::key(model_, sheet::Key::Enter, false));
        smoke_stage_ = 1;
        break;
    case 1: // the answer streams in and offers its buttons
        if (!model_.actions.empty()) {
            WA_INFO("app", "smoke: answer of {} bytes after {:.1f} s, {} frames", model_.answer.size(), elapsed,
                    frames_);
            react({sheet::press(model_, "close"), false});
            smoke_stage_ = 2;
        }
        break;
    case 2: // the button's work finishes on the done bar
        if (sheet::view(model_) == sheet::View::Done && model_.done_title == "OBS closed" && !painter_->moving() && region_on_) {
            WA_INFO("app", "smoke: done after {:.1f} s, {} frames", elapsed, frames_);
            // The open window's region: the panel and its shadow's room (with the
            // panel alone the shadow showed only in the corners, square) — and no more than that.
            const auto box = placement::panel_in(placed_, painter_->target_shape());
            const auto reach = ui::work::sheet_shadow_reach();
            const auto in_region = [&](float x, float y) {
                const POINT at{static_cast<LONG>(std::lround(x * placed_.scale)), static_cast<LONG>(std::lround(y * placed_.scale))};
                HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                const bool in = rgn != nullptr && GetWindowRgn(window_.get(), rgn) != ERROR && PtInRegion(rgn, at.x, at.y) != 0;
                DeleteObject(rgn);
                return in;
            };
            const bool shadowed = in_region(box.x + box.w / 2, box.y + box.h + 20) && in_region(box.x - 16, box.y + box.h / 2);
            const bool bounded = !in_region(box.x + box.w / 2, box.y + box.h + reach.bottom + 40);
            WA_INFO("app", "smoke: the open window's region: its shadow's room {}, past it {}", shadowed ? "in" : "NOT IN",
                    bounded ? "the desktop" : "THE AGENT");
            if (!shadowed || !bounded) {
                WA_ERROR("app", "smoke: the open window's region must hold the shadow's room and end after it");
                return 1;
            }
            close_sheet("smoke");
            smoke_stage_ = 3;
        }
        break;
    case 3: // the sheet settles back to the handle; then a watcher and a result behind it: the handle says so
            // (its dot), and is still all there is (UI update, stage 3: no tongue, no card)
        if (!painter_->animating()) {
            model_.watchers.push_back({.task = 9001, .title = "Watching memory", .detail = "over 90%", .on = true});
            model_.drops.push_back({.task = 9002, .kind = sheet::DropKind::Result, .title = "brochures.zip is ready",
                                    .text = "1.5 GB in 412 MB", .actions = {}, .failed = false, .more = {}, .answer = false});
            ++model_.drops_made;
            wants_frame_ = true;
            smoke_stage_ = 7;
        }
        break;
    case 7: // the handle again: its zone is all of the window that takes the mouse (UI update, stage 3);
        if (smoke_broken_) {
            return 1;
        }
            // then Alt+F4, as Windows sends it
        if (!painter_->animating() && region_on_) {
            std::optional<ui::layout::Box> sign;
            for (const auto& t : painter_->targets()) {
                sign = t.name == "sign" ? std::optional{t.box} : sign;
            }
            const auto lands = [&](float x, float y) {
                const POINT at{static_cast<LONG>(std::lround(x * placed_.scale)), static_cast<LONG>(std::lround(y * placed_.scale))};
                if (options_.show) {
                    return WindowFromPoint({placed_.window.left + at.x, placed_.window.top + at.y}) == window_.get();
                }
                HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                const bool in = rgn != nullptr && GetWindowRgn(window_.get(), rgn) != ERROR && PtInRegion(rgn, at.x, at.y) != 0;
                DeleteObject(rgn);
                return in;
            };
            const bool on = sign && lands(sign->x + sign->w / 2, sign->y + sign->h / 2);
            const bool beside = sign && !lands(sign->x - 30, sign->y + sign->h / 2) && !lands(sign->x + sign->w + 30, sign->y + sign->h / 2);
            const bool under = sign && !lands(sign->x + sign->w / 2, sign->y + sign->h + 30);
            if (!on || !beside || !under) {
                break; // the open window's region is still going; a region that never becomes the zone is "stuck at stage 7"
            }
            WA_INFO("app", "smoke: the handle's zone ({}): {} x {}, {} rect(s); in it the agent, beside and under it the desktop",
                    options_.show ? "Windows' hit test" : "the window's region", sign->w, sign->h, region_.count);
            if (sheet::view(model_) != sheet::View::Rest || sheet::presence_of(model_).state != sheet::PresenceState::Done) {
                WA_ERROR("app", "smoke: with a result behind it the closed agent must be the handle, saying done");
                return 1;
            }
            model_.watchers.clear();
            model_.drops.clear();
            set_open_style(true);
            sheet::open(model_);
            PostMessageW(window_.get(), WM_SYSCOMMAND, SC_CLOSE, 0);
            smoke_stage_ = 5;
        }
        break;
    case 5: // the sheet closed, the agent still runs
        if (!model_.open) {
            WA_INFO("app", "smoke: Alt+F4 closed the sheet, the agent stays");
            WA_INFO("app", "smoke: passed, {} frames", frames_);
            if (options_.rest_seconds <= 0) {
                write_snapshot();
                return 0;
            }
            smoke_stage_ = 4;
            rest_started_ = seconds();
            rest_cpu_ = process_cpu_ms();
            rest_thread_cpu_ = thread_cpu_ms();
            rest_frames_ = frames_;
        }
        break;
    case 4: { // at rest, drawn only as the loop decides by itself: what does it cost?
        const double span = seconds() - rest_started_;
        if (span < options_.rest_seconds) {
            return -1;
        }
        const double cpu = process_cpu_ms() - rest_cpu_;
        const auto frames = frames_ - rest_frames_;
        WA_INFO("app", "smoke rest: {} frames in {:.1f} s ({:.1f} a second), CPU {:.1f} ms = {:.2f} % of one core; "
                       "rest animation {}",
                frames, span, static_cast<double>(frames) / span, cpu, cpu / (span * 10.0),
                painter_->rest_motion() ? "on" : "off");
        write_snapshot();
        const double per = frames > 0 ? 1000.0 / static_cast<double>(frames) : 0.0;
        WA_INFO("app", "smoke rest looks: {} ticks, {} due", rest_ticks_, rest_dues_);
        WA_INFO("app", "smoke rest detail: UI thread CPU {:.1f} ms; a frame: {:.3f} ms until the swap, {:.3f} ms swap",
                thread_cpu_ms() - rest_thread_cpu_, rest_work_ * per, rest_swap_ * per);
        return 0;
    }
    default: break;
    }
    wants_frame_ = true;
    return -1;
}

auto Shell::rest_allowed(double now) -> bool {
    if (now - rest_checked_at_ >= 1.0) {
        rest_checked_at_ = now;
        QUERY_USER_NOTIFICATION_STATE s{};
        const bool away = SUCCEEDED(SHQueryUserNotificationState(&s)) && s == QUNS_NOT_PRESENT; // locked
        rest_allowed_ = !away && !fullscreen::blocks_now();
    }
    return rest_allowed_;
}

auto Shell::loop() -> int {
    double last_frame = 0;
    double rest_tick = 0; // the resting motion is looked at every kRestFrame, drawn when it shows
    constexpr double kRestFrame = 1.0 / 30.0;
    constexpr double kOrbFrame = frame_rate::kIdleOrbFrame; // the open idle sheet's orb
    while (!quit_) {
        if (options_.smoke) {
            if (const int code = smoke_step(); code >= 0) {
                return code;
            }
        }
        const bool rest_motion = !wants_frame_ && painter_->rest_motion();
        const bool resting = rest_motion && rest_allowed(seconds());
        // The open sheet with nothing to do: a frame at the caret's flip (Windows' animations
        // off), or the orb's, 60 a second on a precise wait — the orb alone on a kept frame.
        const bool open_idle =
            !wants_frame_ && (need_ == sheet::FrameNeed::Caret || need_ == sheet::FrameNeed::OrbIdle);
        const double open_due = !open_idle                         ? 0.0
                                : need_ == sheet::FrameNeed::Caret ? painter_->next_caret_flip(last_frame)
                                                                   : last_frame + kOrbFrame;
        if (open_idle && need_ == sheet::FrameNeed::OrbIdle) {
            // A timer wait rounds to the 15.6 ms tick (31 or 47 ms: a stutter); this one does not.
            const double wait = open_due - seconds();
            if (wait > 0.001) {
                pacer_.wait(wait);
            }
        } else if (!wants_frame_) {
            // Idle: 0 % CPU. With the resting motion, a look every kRestFrame; while it is
            // paused, a look once a second whether it may go on. A smoke run measuring the rest
            // wakes up when its time is over.
            const auto deadline = options_.smoke && smoke_stage_ == 4
                                      ? std::optional{options_.rest_seconds - (seconds() - rest_started_)}
                                      : std::nullopt;
            DWORD ms =
                pacing::idle_wait_ms(resting, rest_motion && !resting, seconds() - rest_tick, kRestFrame, deadline);
            if (open_idle) {
                ms = std::min<DWORD>(ms, static_cast<DWORD>(std::max(0.0, (open_due - seconds()) * 1000.0)));
            }
            if (!open_style_) {
                // The handle looks for a fullscreen app once a second, the resting window for what
                // stands over it (watch_strip): microseconds a second.
                ms = std::min<DWORD>(ms, 1000);
            }
#ifdef WA_DEBUG_UI
            if (debug_ && debug_->visible()) {
                ms = std::min<DWORD>(ms, 33); // the debug window's frames
            }
#endif
            if (menu_window_) {
                // The tray menu open (its orb lives, a list unfolds): its next frame on time — a
                // millisecond wait ends on the 15.6 ms tick and stuttered it (the review).
                const double wait = std::min(menu_drawn_at_ + 1.0 / 60.0 - seconds(), ms / 1000.0);
                if (wait > 0.001) {
                    pacer_.wait(wait);
                }
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
            }
        } else if (!painter_->moving()) {
            // The agent works (a spinner, the rail, the orb thinking): at the frame rate chosen
            // (motion rework; it was 60 for good).
            const double wait = frame_period() - (seconds() - last_frame);
            if (wait > 0.001) {
                pacer_.wait(wait); // to the microsecond, not to the next 15.6 ms tick
            }
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit_ = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit_) {
            break;
        }
        drain_agent();
        update_handle_hidden(seconds());
        watch_strip(seconds());
        update_tray_icon(seconds()); // design/ui_model: nothing at rest, at most 10 a second while working
        if (menu_unfolding_ && menu_window_) { // a list of the tray menu unfolding: a frame each pass, to its end
            const double t = seconds();
            if (menu_settings_.moving(t) || menu_budget_.moving(t)) {
                unfold_tray_menu(t);
            } else {
                menu_unfolding_ = false;
                layout_tray_menu(); // its end: a shut list's rows go, the window takes the menu's height
            }
            draw_tray_menu();
        } else if (menu_unfolding_) {
            menu_unfolding_ = false;
        } else if (menu_window_ && seconds() - menu_drawn_at_ >= 1.0 / 60.0 - 1.0e-4) {
            draw_tray_menu(); // open: its header's orb moves, its state follows the agent
        }
        // Memory over time for the snapshot and the debug window. Never a wake-up of its own:
        // while the agent sleeps there is nothing to sample.
        if (const double t = seconds(); t - memory_sampled_at_ >= 1.0) {
            memory_sampled_at_ = t;
            metrics_.add_memory(debug::memory_now(t));
        }
        bool rest_due = false;
        if (resting && seconds() - rest_tick >= kRestFrame - 0.001) {
            rest_tick = seconds();
            rest_due = painter_->rest_frame_due(rest_tick); // only frames that change a pixel
            ++rest_ticks_;
            rest_dues_ += rest_due ? 1 : 0;
        }
        const bool open_frame_due = open_idle && !wants_frame_ && seconds() >= open_due - 0.001;
        if (rest_due || open_frame_due ||
            (wants_frame_ && (painter_->moving() || seconds() - last_frame >= frame_period() - 0.001 ||
                              input_.mouse_pressed || input_.mouse_released))) {
            last_frame = seconds();
            frame();
        }
        draw_debug();
    }
    if (options_.smoke) {
        if (options_.smoke_exit && exit_asked_) {
            WA_INFO("app", "smoke: the exit message ended the run");
            return 0;
        }
        WA_ERROR("app", "smoke: the agent quit before the run passed");
        return 1;
    }
    drop_target_.reset(); // revoked while the window is there
    if (ole_) {
        OleUninitialize();
        ole_ = false;
    }
    WA_INFO("app", "exit"); // the log says when the agent ended (2026-10-13: it did not)
    if (tray_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = window_.get();
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (hotkey_) {
        UnregisterHotKey(window_.get(), kHotkeyId);
    }
    return 0;
}

// data\ beside agent.exe in a portable build (build/<Config>/), otherwise %LOCALAPPDATA%\Agent.
auto data_directory() -> std::filesystem::path {
    return core::win::agent_data_dir();
}

} // namespace

auto parse_point(std::wstring_view text) -> std::optional<ScreenPoint> {
    const auto comma = text.find(L',');
    if (comma == std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto number = [](std::wstring_view s) -> std::optional<int> {
        std::string narrow;
        for (const wchar_t c : s) {
            if (c > 0x7F) {
                return std::nullopt;
            }
            narrow.push_back(static_cast<char>(c));
        }
        int value = 0;
        const auto [end, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
        if (ec != std::errc{} || end != narrow.data() + narrow.size() || narrow.empty()) {
            return std::nullopt;
        }
        return value;
    };
    const auto x = number(text.substr(0, comma));
    const auto y = number(text.substr(comma + 1));
    if (!x || !y) {
        return std::nullopt;
    }
    return ScreenPoint{*x, *y};
}

auto run(const Options& options) -> int {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto data_dir = data_directory();
    std::error_code ec;
    std::filesystem::create_directories(data_dir / L"logs", ec);
    // A smoke run keeps its own log and does not take part in the single-instance handshake:
    // it must not open the sheet of an agent the user is running.
    auto logger = core::log::start(
        {.file = data_dir / L"logs" / (options.smoke ? L"agent-smoke.log" : L"agent.log"),
#ifdef NDEBUG
         .recent_capacity = 200}); // for the snapshot
#else
         .recent_capacity = 5000}); // for the debug window's log
#endif

    auto instance = instance::acquire();
    if (instance && !instance->first() && !options.smoke) {
        // Another agent runs: let it take the foreground and open its sheet.
        if (HWND other = FindWindowW(kClassName, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_OPEN, 0, 0);
        }
        return 0;
    }

    Shell shell{options};
    if (!shell.start(data_dir)) {
        MessageBoxW(nullptr, L"The agent could not start. See the log in %LOCALAPPDATA%\\Agent\\logs.",
                    WA_PRODUCT_WIDE, MB_OK | MB_ICONERROR);
        return 2;
    }
    return shell.loop();
}

} // namespace app::shell
