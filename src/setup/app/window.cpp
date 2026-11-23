#include "setup/app/window.hpp"

#include "core/agent_ids.hpp"
#include "core/fs.hpp"
#include "core/utf8.hpp"
#include "core/version.hpp"
#include "core/win.hpp"
#include "setup/flow.hpp"
#include "setup/model.hpp"
#include "setup/payload.hpp"
#include "setup/self.hpp"
#include "setup/steps.hpp"
#include "setup/view/motion.hpp"
#include "setup/view/painter.hpp"
#include "ui/backdrop.hpp"
#include "ui/font_library.hpp"
#include "ui/gl/context.hpp"
#include "ui/gl/program_cache.hpp"
#include "ui/orb.hpp"
#include "ui/render/renderer.hpp"
#include "ui/text/atlas.hpp"
#include "ui/ui.hpp"

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace setup::app {
namespace {

using view::Painter;

constexpr wchar_t kOverlayClass[] = L"WinAiAgent.Setup.Overlay";
constexpr double kHandoffWait = 3.0; // the most the installer waits, after its flight, for the agent's window

auto window_animations() -> bool {
    BOOL on = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) == FALSE || on != FALSE;
}

auto windows_name() -> std::string {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
            fn(&v);
    }
    return v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
}

auto wide(std::string_view s) -> std::wstring {
    auto w = core::utf8::to_wide(s);
    return w ? std::move(*w) : std::wstring{};
}

// What the work thread tells the window.
struct Shared {
    std::mutex mutex;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    Step step = Step::Files;
    bool finished = false;
    Outcome outcome = Outcome::Done;
    std::string why;
};

class Runner {
public:
    explicit Runner(const Host& host) : host_{host}, reduced_{!window_animations()} {}
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ~Runner() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    auto start() -> bool;
    auto loop() -> int;

private:
    enum class Phase : std::uint8_t { Intro, Screens, Launch, Handoff };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    [[nodiscard]] auto seconds() const -> double {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }
    void init_state();
    void frame();
    void draw_main(double now);
    void draw_overlay(const std::function<void()>& draw);
    void take(const view::Reaction& r);
    void act(Command c);
    void poll_work(double now);
    void begin_launch(double now);
    void start_agent();
    void open_license();
    [[nodiscard]] auto window_box() const -> ::ui::layout::Box; // the window in the overlay's logical pixels
    // `own_phase`: nothing else advances the orb's phase this frame (the painter's update does in
    // the appearance: it went twice as fast there — the map's № 64).
    void advance_orb(float spike, float energy, float hue, double now, bool own_phase);

    Host host_;
    bool reduced_;
    HINSTANCE instance_ = GetModuleHandleW(nullptr);
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    ::ui::gl::Window main_;
    ::ui::gl::Window overlay_;
    HDC overlay_dc_ = nullptr;
    bool overlay_shown_ = false;
    bool main_shown_ = false;
    std::optional<::ui::gl::Context> context_;
    std::optional<::ui::gl::ProgramCache> cache_;
    std::optional<::ui::render::Renderer> renderer_;
    std::optional<::ui::FontLibrary> library_;
    std::optional<::ui::Fonts> fonts_;
    std::optional<::ui::text::GlyphAtlas> atlas_;
    std::optional<::ui::Ui> main_ui_;
    std::optional<::ui::Orb> orb_;
    std::optional<Painter> painter_;
    RECT monitor_{};
    float scale_ = 1.0f;
    State state_;
    RegistrySystem system_ = RegistrySystem::for_user();
    Places places_ = Places::for_user();
    Phase phase_ = Phase::Intro;
    double phase_at_ = 0;       // the intro's and the screens' start
    // The motion's clock: stepped by a frame's time, ui::motion::kMaxFrameDt at most — after a stall
    // the appearance and the launch go on from where they were (MS § 5.1), they do not leap.
    double tick_ = 0;
    double phase_tick_ = 0;     // tick_ when the intro began
    view::LaunchClock launch_;  // the launch's and the handoff's
    double last_ = 0;
    ::ui::Input input_;
    std::optional<Action> key_;
    std::jthread worker_;
    Shared shared_;
    double work_at_ = 0;
    view::LaunchFrom from_{};
    ::ui::layout::Box bar_{};   // the agent's bar: what the window becomes, the orb in its place
    bool agent_started_ = false;
    core::win::KernelHandle go_;    // set by the installer: the orb has reached the strip, the agent opens
    core::win::KernelHandle ready_; // set by the agent: its open window is on the screen
    bool quit_ = false;
    int exit_ = 0;
};

auto Runner::start() -> bool {
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi_x = 96, dpi_y = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    scale_ = static_cast<float>(dpi_x) / 96.0f;
    monitor_ = mi.rcMonitor;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    const std::wstring main_class{self::kWindowClass};
    wc.lpszClassName = main_class.c_str();
    RegisterClassExW(&wc);
    WNDCLASSEXW oc{sizeof(oc)};
    oc.style = CS_OWNDC;
    oc.lpfnWndProc = DefWindowProcW;
    oc.hInstance = instance_;
    oc.lpszClassName = kOverlayClass;
    RegisterClassExW(&oc);

    const auto px = [&](float logical) { return static_cast<int>(std::lround(logical * scale_)); };
    const int w = px(Painter::kWidth + 2 * Painter::kMarginSide);
    const int h = px(Painter::kHeight + Painter::kMarginTop + Painter::kMarginBottom);
    const RECT& work = mi.rcWork;
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + std::max<int>(0, (work.bottom - work.top - h) / 2);
    const std::wstring title = host_.uninstall ? L"Remove WinAiAgent" : L"WinAiAgent Setup";
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, main_class.c_str(), title.c_str(), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                x, y, w, h, nullptr, nullptr, instance_, this);
    if (hwnd == nullptr)
        return false;
    main_ = ::ui::gl::Window{hwnd};
    ::ui::backdrop::prepare(hwnd);
    // Lets every click through, never activated: it only shows the appearance and the launch.
    const RECT layer = self::layer_rect(monitor_);
    HWND over = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                kOverlayClass, L"", WS_POPUP, layer.left, layer.top, layer.right - layer.left,
                                layer.bottom - layer.top, nullptr, nullptr, instance_, nullptr);
    if (over == nullptr)
        return false;
    overlay_ = ::ui::gl::Window{over};
    SetLayeredWindowAttributes(over, 0, 255, LWA_ALPHA);
    ::ui::backdrop::prepare(over);

    auto context = ::ui::gl::Context::create(hwnd, false);
    if (!context)
        return false; // no OpenGL 4.5: the plain dialogs
    context_.emplace(std::move(*context));
    auto dc = context_->adopt(over);
    if (!dc)
        return false;
    overlay_dc_ = *dc;
    if (wglSwapIntervalEXT != nullptr)
        wglSwapIntervalEXT(1);
    cache_.emplace(host_.temp / L"programs", context_->driver_id());
    auto renderer = ::ui::render::Renderer::create(*cache_);
    auto library = ::ui::FontLibrary::create();
    if (!renderer || !library)
        return false;
    renderer_.emplace(std::move(*renderer));
    library_.emplace(std::move(*library));
    auto fonts = ::ui::Fonts::system(*library_);
    auto atlas = ::ui::text::GlyphAtlas::create(2048);
    if (!fonts || !atlas)
        return false;
    fonts_.emplace(std::move(*fonts));
    atlas_.emplace(std::move(*atlas));
    main_ui_.emplace(*renderer_, *atlas_, *fonts_);
    const int orb_px = std::min(1024, px(view::kOrbBody * Painter::kOrbQuad));
    auto orb = ::ui::Orb::create(*cache_, orb_px);
    if (!orb)
        return false;
    orb_.emplace(std::move(*orb));
    painter_.emplace(*main_ui_, *orb_);
    painter_->set_reduced(reduced_); // Windows' animation effects off: its screens only fade
    init_state();
    phase_ = Phase::Intro;
    phase_at_ = seconds();
    return true;
}

void Runner::init_state() {
    state_.version = std::string{core::version()};
    state_.system = windows_name();
    const auto there = installed(system_);
    if (there) {
        state_.installed_version = there->version;
        state_.installed_dir = there->dir;
    }
    if (host_.uninstall) {
        state_.uninstalling = true;
        state_.installed_dir = host_.app_dir;
        state_.screen = first_screen(Relation::NotInstalled, true);
        if (!host_.smoke && self::elevated_by_another_account()) { // its HKCU and data would be another's
            state_.screen = Screen::Failed;
            state_.note = "Run it from your own account: it must be an administrator";
            return;
        }
        if (!installed_here(system_, host_.app_dir)) { // a build's folder, not an installed program's
            state_.screen = Screen::Failed;
            state_.note = std::format("WinAiAgent isn't installed in {}",
                                      core::utf8::to_utf8(host_.app_dir.native()).value_or(std::string{"this folder"}));
        }
        return;
    }
    if (auto entries = payload_entries(host_.payload)) {
        for (const auto& e : *entries)
            state_.needed += e.size;
    }
    // Program Files, for everyone: the elevated helper lies where only administrators write.
    const auto programs = core::win::known_folder(FOLDERID_ProgramFiles);
    state_.choices.dir = install_dir(programs ? *programs : std::filesystem::path{L"C:\\Program Files"});
    // A check (--smoke) writes nothing anywhere: not even the folder's probe file.
    state_.folder = host_.smoke ? FolderCheck{.writable = true} : check_folder(state_.choices.dir, state_.needed);
    const auto relation = relate(there ? std::optional<std::string_view>{there->version} : std::nullopt, state_.version);
    state_.screen = first_screen(relation, false);
    if (state_.screen == Screen::Update)
        state_.choices.dir = there->dir;
    // Raised by another account (a standard user typed an administrator's password): the Start menu,
    // autostart and data would be that account's, not the user's (installer-program-files.md).
    if (!host_.smoke && self::elevated_by_another_account()) {
        state_.screen = Screen::Failed;
        state_.note = "Run Setup from your own account: it must be an administrator";
    }
}

auto Runner::window_box() const -> ::ui::layout::Box {
    RECT r{};
    GetWindowRect(main_.get(), &r);
    return {static_cast<float>(r.left - monitor_.left) / scale_ + Painter::kMarginSide,
            static_cast<float>(r.top - monitor_.top) / scale_ + Painter::kMarginTop, Painter::kWidth, Painter::kHeight};
}

void Runner::advance_orb(float spike, float energy, float hue, double now, bool own_phase) {
    // The prototype's phase runs at .35 + speed * .9 a second, its speed following the spike.
    const float dt = static_cast<float>(std::clamp(now - last_, 0.0, static_cast<double>(::ui::motion::kMaxFrameDt)));
    auto p = orb_->params();
    const float speed = 0.35f + (1.6f - 0.35f) * std::clamp((spike - 0.05f) / 0.95f, 0.0f, 1.0f);
    if (own_phase) {
        p.phase += (0.35f + speed * 0.9f) * dt;
    }
    p.spike = spike;
    p.energy = energy;
    p.hue = hue;
    p.speed = speed;
    orb_->set_params(p);
}

void Runner::draw_overlay(const std::function<void()>& draw) {
    RECT client{};
    GetClientRect(overlay_.get(), &client); // the layer: its monitor one row short
    const int w = client.right;
    const int h = client.bottom;
    context_->make_current(overlay_dc_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer_->begin(w, h, scale_);
    main_ui_->begin_frame(seconds(), scale_, {}); // the layer takes no input: the one Ui draws it
    draw();
    main_ui_->end_frame();
    renderer_->end();
    SwapBuffers(overlay_dc_);
    context_->make_current();
    if (!overlay_shown_ && !host_.smoke) { // shown once it has a frame: never an empty layer over the desktop
        ShowWindow(overlay_.get(), SW_SHOWNOACTIVATE);
        overlay_shown_ = true;
    }
}

void Runner::draw_main(double now) {
    RECT client{};
    GetClientRect(main_.get(), &client);
    const int w = client.right;
    const int h = client.bottom;
    context_->make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer_->begin(w, h, scale_);
    main_ui_->begin_frame(now, scale_, input_);
    const auto r = painter_->draw(state_, {Painter::kMarginSide, Painter::kMarginTop, Painter::kWidth, Painter::kHeight});
    main_ui_->end_frame();
    renderer_->end();
    context_->swap_buffers();
    input_.mouse_pressed = false;
    input_.mouse_released = false;
    input_.tab = false;
    input_.activate = false;
    take(r);
}

void Runner::take(const view::Reaction& r) {
    if (painter_->minimize_clicked())
        ShowWindow(main_.get(), SW_MINIMIZE);
    if (r.action)
        act(press(state_, *r.action));
    if (key_) {
        const auto a = *key_;
        key_.reset();
        act(press(state_, a));
    }
}

void Runner::frame() {
    const double now = seconds();
    tick_ += std::clamp(now - last_, 0.0, static_cast<double>(::ui::motion::kMaxFrameDt));
    const double t = tick_ - phase_tick_;
    switch (phase_) {
    case Phase::Intro: {
        const auto box = window_box();
        const auto f = view::intro_at(t, box, reduced_);
        painter_->update(state_, now);
        if (!reduced_)
            advance_orb(f.orb_spike, f.orb_energy, f.orb_hue, now, false);
        orb_->set_zoom(view::kOrbZoom);
        orb_->render(now);
        draw_overlay([&] { painter_->draw_intro(state_, f, box); });
        if (view::intro_done(t, reduced_)) {
            draw_main(now); // its first frame before it shows: the layer and the window agree
            if (!host_.smoke) {
                ShowWindow(main_.get(), SW_SHOW);
                SetForegroundWindow(main_.get());
                main_shown_ = true;
            }
            ShowWindow(overlay_.get(), SW_HIDE);
            overlay_shown_ = false;
            phase_ = Phase::Screens;
            phase_at_ = now;
        }
        break;
    }
    case Phase::Screens:
        poll_work(now);
        painter_->update(state_, now);
        painter_->render_orb(now);
        draw_main(now);
        if (host_.smoke && now - phase_at_ > 0.3) // the check goes straight on to the launch
            begin_launch(now);
        break; // Done waits for Launch (nothing starts by itself)
    case Phase::Launch:
    case Phase::Handoff: {
        // The window becomes the agent's bar; past its end the same frame is held — the bar with
        // its orb — until the agent has its own on the screen (no empty frame: the map's № 67).
        const auto f = view::launch_at(launch_.flight(tick_, reduced_), from_, bar_, reduced_);
        advance_orb(f.orb_spike, f.orb_energy, 0, now, true);
        orb_->set_zoom(f.orb_zoom);
        orb_->render(now);
        const auto box = from_.window;
        draw_overlay([&] { painter_->draw_launch(state_, f, box); });
        if (main_shown_) {
            ShowWindow(main_.get(), SW_HIDE); // the layer has the window's frame now
            main_shown_ = false;
        }
        if (phase_ == Phase::Launch && launch_.flown(tick_, reduced_) && host_.smoke) {
            quit_ = true; // the check starts no agent
        } else if (phase_ == Phase::Launch && launch_.flown(tick_, reduced_)) {
            // The window is the agent's bar now: the agent, started with the launch, shows its own in its place.
            if (go_)
                SetEvent(go_.get());
            phase_ = Phase::Handoff;
            launch_.hand_off(now);
        } else if (phase_ == Phase::Handoff) {
            const bool agent_shown = ready_ && WaitForSingleObject(ready_.get(), 0) == WAIT_OBJECT_0;
            if (agent_shown || launch_.waited(now) >= kHandoffWait || !agent_started_)
                quit_ = true;
        }
        break;
    }
    }
    last_ = now;
}

void Runner::poll_work(double now) {
    std::scoped_lock lock{shared_.mutex};
    if (state_.screen != Screen::Installing && state_.screen != Screen::Removing)
        return;
    state_.step = shared_.step;
    const float bytes = shared_.total > 0 ? static_cast<float>(shared_.done) / static_cast<float>(shared_.total) : 0.0f;
    if (state_.screen == Screen::Removing)
        state_.progress = bytes;
    else
        state_.progress = shared_.step == Step::Files ? 0.05f + 0.85f * bytes : shared_.step == Step::Shortcuts ? 0.92f : 0.97f;
    const double spent = now - work_at_;
    state_.eta_seconds = state_.progress > 0.05f ? spent / state_.progress * (1 - state_.progress) : -1;
    if (shared_.finished) {
        shared_.finished = false;
        state_.took_seconds = spent;
        finished(state_, shared_.outcome, shared_.why);
    }
}

void Runner::act(Command c) {
    switch (c) {
    case Command::None: break;
    case Command::Install:
    case Command::Update:
    case Command::Uninstall: {
        {
            std::scoped_lock lock{shared_.mutex};
            shared_.done = shared_.total = 0;
            shared_.step = Step::Files;
            shared_.finished = false;
            shared_.why.clear();
        }
        if (worker_.joinable())
            worker_.join();
        work_at_ = seconds();
        const auto choices = state_.choices;
        const auto app_dir = host_.app_dir;
        worker_ = std::jthread{[this, c, choices, app_dir](std::stop_token stop) {
            const Watch watch{.progress = [this](std::uint64_t d, std::uint64_t t) {
                                  std::scoped_lock lock{shared_.mutex};
                                  shared_.done = d;
                                  shared_.total = t;
                              },
                              .step = [this](Step s) {
                                  std::scoped_lock lock{shared_.mutex};
                                  shared_.step = s;
                              }};
            core::Result<void> result;
            if (c == Command::Install) {
                auto r = install(system_, places_, choices, host_.payload, watch, stop);
                result = r ? core::Result<void>{} : std::unexpected{r.error()};
            } else if (c == Command::Update) {
                auto r = update(system_, places_, choices.dir, host_.payload, watch, stop);
                result = r ? core::Result<void>{} : std::unexpected{r.error()};
            } else {
                result = uninstall(system_, places_, app_dir, watch.progress);
            }
            std::scoped_lock lock{shared_.mutex};
            shared_.finished = true;
            shared_.outcome = result ? Outcome::Done : result.error().code == core::Code::Cancelled ? Outcome::Cancelled : Outcome::Failed;
            shared_.why = result ? std::string{} : result.error().message;
        }};
        break;
    }
    case Command::CancelInstall: worker_.request_stop(); break;
    case Command::Launch: begin_launch(seconds()); break;
    case Command::License: open_license(); break;
    case Command::Quit: // on Done too: the agent starts only with Launch
        quit_ = true;
        break;
    }
}

void Runner::begin_launch(double /*now: the launch runs on the motion's clock, tick_*/) {
    if (phase_ != Phase::Screens)
        return;
    // The monitor the window stands on now (it may have been dragged): the layer covers it.
    const HMONITOR mon = MonitorFromWindow(main_.get(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    monitor_ = mi.rcMonitor;
    const RECT layer = self::layer_rect(monitor_);
    SetWindowPos(overlay_.get(), HWND_TOPMOST, layer.left, layer.top, layer.right - layer.left, layer.bottom - layer.top,
                 SWP_NOACTIVATE);
    const auto box = window_box();
    from_ = {.window = box, .orb = {box.x + box.w / 2, box.y + painter_->orb_centre_y()}, .orb_body = painter_->orb_size()};
    bar_ = view::agent_bar(static_cast<float>(monitor_.right - monitor_.left) / scale_);
    phase_ = Phase::Launch;
    launch_.begin(tick_);
    if (!host_.smoke)
        start_agent(); // with the flight: it is up, its strip at rest, by the time the orb arrives
}

void Runner::start_agent() {
    const auto dir = state_.choices.dir;
    const auto exe = dir / L"agent.exe";
    RECT r{};
    GetWindowRect(main_.get(), &r);
    // The agent waits for `go` (the orb has gone into its strip) to open with its own opening, and
    // sets `ready` once that window is on the screen; the installer quits then.
    const std::wstring tag = std::to_wstring(GetCurrentProcessId());
    const std::wstring go_name = L"Local\\WinAiAgent.Setup.Go." + tag;
    const std::wstring ready_name = L"Local\\WinAiAgent.Setup.Ready." + tag;
    // The setup is elevated, the agent is not: the events are the user's and medium-integrity.
    go_ = self::event_for_agent(go_name);
    ready_ = self::event_for_agent(ready_name);
    const std::wstring command = L"\"" + exe.native() + L"\" --after-install=" + std::to_wstring((r.left + r.right) / 2) +
                                 L"," + std::to_wstring((r.top + r.bottom) / 2) + L" --go-event=" + go_name +
                                 L" --ready-event=" + ready_name;
    AllowSetForegroundWindow(ASFW_ANY);
    // Never elevated: started by Explorer, as the desktop's user (the setup runs as an administrator).
    // Not started that way: not at all — the Start menu's shortcut is there.
    const auto args = command.substr(exe.native().size() + 3);
    agent_started_ = self::launch_as_desktop_user(exe, args, dir);
}

void Runner::open_license() {
    auto text = read_entry(host_.payload, "LICENSE");
    if (!text)
        return;
    const auto file = host_.temp / L"LICENSE.txt";
    if (core::fs::write_file_atomic(file, *text))
        ShellExecuteW(main_.get(), L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK Runner::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Runner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self != nullptr ? self->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Runner::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const auto logical = [&](LPARAM p) {
        return std::pair{static_cast<float>(static_cast<short>(LOWORD(p))) / scale_,
                         static_cast<float>(static_cast<short>(HIWORD(p))) / scale_};
    };
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / scale_;
        const float y = static_cast<float>(pt.y) / scale_;
        const auto& d = painter_ ? painter_->drag_box() : ::ui::layout::Box{};
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        const auto [x, y] = logical(lp);
        input_.mouse_x = x;
        input_.mouse_y = y;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        input_.mouse_x = input_.mouse_y = -1.0e9f;
        return 0;
    case WM_LBUTTONDOWN: {
        const auto [x, y] = logical(lp);
        input_.mouse_x = x;
        input_.mouse_y = y;
        input_.mouse_down = true;
        input_.mouse_pressed = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        const auto [x, y] = logical(lp);
        input_.mouse_x = x;
        input_.mouse_y = y;
        input_.mouse_down = false;
        input_.mouse_released = true;
        ReleaseCapture();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && main_ui_) {
            SetCursor(LoadCursorW(nullptr, main_ui_->cursor() == ::ui::Cursor::Hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN)
            key_ = Action::Enter;
        else if (wp == VK_ESCAPE)
            key_ = Action::Escape;
        else if (wp == VK_TAB) {
            input_.tab = true;
            input_.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        } else if (wp == VK_SPACE)
            input_.activate = true;
        return 0;
    case WM_CLOSE: // Alt+F4, the taskbar's Close: as the window's close button
        key_ = Action::Escape;
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

auto Runner::loop() -> int {
    while (!quit_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                quit_ = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit_)
            break;
        if (phase_ == Phase::Screens && IsIconic(main_.get())) {
            poll_work(seconds());
            Sleep(50); // minimized: no frames, the work goes on
            continue;
        }
        frame();
    }
    if (worker_.joinable()) { // a quit during the work: Cancel, then the rollback, before leaving
        worker_.request_stop();
        worker_.join();
    }
    ShowWindow(overlay_.get(), SW_HIDE);
    ShowWindow(main_.get(), SW_HIDE);
    return exit_;
}

} // namespace

auto run(const Host& host) -> int {
    Runner runner{host};
    if (!runner.start())
        return kNoGl;
    return runner.loop();
}

auto run_plain(const Host& host) -> int {
    if (!host.uninstall) {
        TaskDialog(nullptr, nullptr, L"WinAiAgent Setup", L"WinAiAgent needs a graphics card with OpenGL 4.5",
                   L"This computer's graphics driver doesn't offer it, so the agent can't draw its window here.",
                   TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, nullptr);
        return 1;
    }
    auto system = RegistrySystem::for_user();
    if (!installed_here(system, host.app_dir)) {
        const std::wstring text = L"WinAiAgent isn't installed in " + host.app_dir.native() + L".";
        TaskDialog(nullptr, nullptr, L"Remove WinAiAgent", L"Nothing was removed", text.c_str(), TDCBF_CLOSE_BUTTON,
                   TD_INFORMATION_ICON, nullptr);
        return 1;
    }
    int button = 0;
    const std::wstring what = L"The program in " + host.app_dir.native() + L", its settings, your history and the API key.";
    TaskDialog(nullptr, nullptr, L"Remove WinAiAgent", L"Remove WinAiAgent?", what.c_str(), TDCBF_YES_BUTTON | TDCBF_NO_BUTTON,
               TD_WARNING_ICON, &button);
    if (button != IDYES)
        return 0;
    const auto r = uninstall(system, Places::for_user(), host.app_dir, {});
    if (r) {
        TaskDialog(nullptr, nullptr, L"Remove WinAiAgent", L"Removed",
                   L"Nothing of WinAiAgent is left on this computer.", TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, nullptr);
        return 0;
    }
    const auto why = wide(r.error().message);
    TaskDialog(nullptr, nullptr, L"Remove WinAiAgent", L"Not everything was removed", why.c_str(), TDCBF_CLOSE_BUTTON,
               TD_ERROR_ICON, nullptr);
    return 1;
}

} // namespace setup::app
