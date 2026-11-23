#include "app/debug_window.hpp"

#include "core/log.hpp"
#include "core/mem/mem.hpp"
#include "core/utf8.hpp"
#include "ui/gl/context.hpp"
#include "ui/render/target.hpp"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace app::debug {
namespace {

constexpr wchar_t kClass[] = L"WinAiAgent.Debug";

// ImGui's memory goes through core::mem like everything else, under its own tag.
auto imgui_alloc(std::size_t size, void*) -> void* {
    core::mem::TagScope tag{core::mem::tag_id("debug/imgui")};
    return core::mem::allocate(size);
}
void imgui_free(void* p, void*) {
    core::mem::deallocate(p);
}

auto level_color(core::log::Level l) -> ImVec4 {
    switch (l) {
    case core::log::Level::Error: return {1.0f, 0.45f, 0.45f, 1.0f};
    case core::log::Level::Warn: return {1.0f, 0.8f, 0.35f, 1.0f};
    case core::log::Level::Debug:
    case core::log::Level::Trace: return {0.6f, 0.6f, 0.7f, 1.0f};
    case core::log::Level::Info: break;
    }
    return {0.9f, 0.9f, 0.92f, 1.0f};
}

auto level_label(core::log::Level l) -> const char* {
    switch (l) {
    case core::log::Level::Trace: return "TRACE";
    case core::log::Level::Debug: return "DEBUG";
    case core::log::Level::Info: return "INFO";
    case core::log::Level::Warn: return "WARN";
    case core::log::Level::Error: return "ERROR";
    }
    return "?";
}

// std::format into a fixed buffer, cut to fit and NUL-terminated: no allocation per frame.
template <std::size_t N, class... Args>
void format_into(char (&buf)[N], std::format_string<Args...> fmt, Args&&... args) {
    const auto end = std::format_to_n(buf, N - 1, fmt, std::forward<Args>(args)...).out;
    *end = '\0';
}

constexpr std::size_t kLogRecords = 5000; // what the Log tab lists at most

auto contains_nocase(std::string_view hay, std::string_view needle) -> bool {
    if (needle.empty()) {
        return true;
    }
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != hay.end();
}

} // namespace

struct DebugWindow::Impl {
    HWND hwnd = nullptr;
    std::optional<ui::gl::Context> context;
    ImGuiContext* imgui = nullptr;
    bool shown = false;
    // Log tab
    int min_level = 0;
    std::string category;
    std::string search;
    bool follow = true;
    std::uint64_t log_version = ~std::uint64_t{0}; // core::log::recent_version() of `entries`
    std::vector<core::log::Entry> entries;
    std::vector<std::uint32_t> rows; // indices of the entries that pass the filters
    std::size_t listed = 0;
    std::string saved; // the last snapshot's path
    int select_tab = -1; // capture(): the tab to show
    // Kept between frames with room for every sample, so a frame allocates nothing.
    std::array<std::vector<float>, 5> frame_series; // interval, work, update, draw, swap
    std::array<std::vector<float>, 2> memory_series; // live MB, allocations a second
    std::vector<core::mem::TagStats> tags;

    Impl() {
        for (auto& s : frame_series) {
            s.reserve(Metrics::kFrames);
        }
        for (auto& s : memory_series) {
            s.reserve(Metrics::kMemorySamples);
        }
        tags.reserve(core::mem::kMaxTags);
        rows.reserve(kLogRecords);
    }

    static auto CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr && self->imgui != nullptr) {
            ImGui::SetCurrentContext(self->imgui);
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) != 0) {
                return 1;
            }
        }
        if (self != nullptr && (msg == WM_CLOSE || (msg == WM_KEYDOWN && wp == VK_F12))) {
            ShowWindow(hwnd, SW_HIDE); // closing only hides: F12 brings it back
            self->shown = false;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void draw(const DebugView& v);
    static void plot(const char* label, const std::vector<float>& data, float top, const char* unit);
    void performance(const DebugView& v);
    void memory(const DebugView& v);
    void log();
    void state(const DebugView& v);
    void save_snapshot(const DebugView& v);
};

DebugWindow::DebugWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

auto DebugWindow::create(HINSTANCE instance) -> core::Result<std::unique_ptr<DebugWindow>> {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = &Impl::proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    auto impl = std::make_unique<Impl>();
    impl->hwnd = CreateWindowExW(0, kClass, L"Agent — debug", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 980, 760, nullptr, nullptr, instance, nullptr);
    if (impl->hwnd == nullptr) {
        return core::os_error(GetLastError(), "debug window");
    }
    SetWindowLongPtrW(impl->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl.get()));
    auto context = ui::gl::Context::create(impl->hwnd, false);
    if (!context) {
        DestroyWindow(impl->hwnd);
        return std::unexpected{context.error()};
    }
    impl->context.emplace(std::move(*context));
    impl->context->make_current();
    if (wglSwapIntervalEXT != nullptr) {
        wglSwapIntervalEXT(0); // never slow the sheet down waiting for this window's vsync
    }
    ImGui::SetAllocatorFunctions(imgui_alloc, imgui_free, nullptr);
    impl->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(impl->imgui);
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(impl->hwnd);
    ImGui_ImplOpenGL3_Init("#version 450");
    WA_INFO("debug", "debug window ready");
    return std::unique_ptr<DebugWindow>{new DebugWindow(std::move(impl))}; // mem-ok: private constructor; operator new is core::mem's
}

DebugWindow::~DebugWindow() {
    if (impl_->context) {
        impl_->context->make_current();
    }
    if (impl_->imgui != nullptr) {
        ImGui::SetCurrentContext(impl_->imgui);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(impl_->imgui);
    }
    impl_->context.reset();
    if (impl_->hwnd != nullptr) {
        SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(impl_->hwnd);
    }
}

void DebugWindow::toggle() {
    impl_->shown = !impl_->shown;
    ShowWindow(impl_->hwnd, impl_->shown ? SW_SHOWNOACTIVATE : SW_HIDE);
}

auto DebugWindow::visible() const noexcept -> bool {
    return impl_->shown;
}

auto DebugWindow::log_lines() const noexcept -> std::size_t {
    return impl_->listed;
}

void DebugWindow::frame(const DebugView& view, bool force) {
    if (!impl_->shown && !force) {
        return;
    }
    impl_->context->make_current();
    ImGui::SetCurrentContext(impl_->imgui);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    impl_->draw(view);
    ImGui::Render();
    RECT rc{};
    GetClientRect(impl_->hwnd, &rc);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    impl_->context->swap_buffers();
}

auto DebugWindow::capture(const DebugView& view, int tab, int width, int height, const std::filesystem::path& png)
    -> core::Result<void> {
    impl_->context->make_current();
    ImGui::SetCurrentContext(impl_->imgui);
    auto target = ui::render::Target::create(width, height);
    if (!target) {
        return std::unexpected{target.error()};
    }
    impl_->select_tab = tab;
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < 3; ++i) { // the first frames lay the window and the tab out
        ImGui_ImplOpenGL3_NewFrame();
        io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
        io.DeltaTime = 1.0f / 30.0f;
        ImGui::NewFrame();
        impl_->draw(view);
        ImGui::Render();
        target->bind();
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    impl_->select_tab = -1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return target->save_png(png);
}

void DebugWindow::Impl::draw(const DebugView& v) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("debug", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Save snapshot")) {
        save_snapshot(v);
    }
    if (!saved.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("saved: %s", saved.c_str());
    }
    const auto flags = [&](int tab) {
        return select_tab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Performance", nullptr, flags(0))) {
            performance(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory", nullptr, flags(1))) {
            memory(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log", nullptr, flags(2))) {
            log();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("State", nullptr, flags(3))) {
            state(v);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DebugWindow::Impl::plot(const char* label, const std::vector<float>& data, float top, const char* unit) {
    if (data.empty()) {
        ImGui::TextDisabled("%s: no samples", label);
        return;
    }
    const float peak = *std::ranges::max_element(data);
    // snprintf, not std::format: MSVC's std::format allocates for a float with a precision.
    char id[64];
    char overlay[96];
    std::snprintf(id, sizeof id, "##%s", label);
    std::snprintf(overlay, sizeof overlay, "%s (max %.2f %s)", label, peak, unit);
    ImGui::PlotLines(id, data.data(), static_cast<int>(data.size()), 0, overlay, 0.0f, std::max(top, peak), {-1, 80});
}

void DebugWindow::Impl::performance(const DebugView& v) {
    const auto s = v.metrics.summary(v.now, 5.0);
    const auto reason = reason_name(s.last_reason);
    ImGui::Text("last 5 s: %zu frames, %.1f a second, now: %.*s", s.frames, s.fps, static_cast<int>(reason.size()),
                reason.data());
    ImGui::Text("interval between frames: p50 %.2f ms, p95 %.2f ms, max %.2f ms", s.p50_ms, s.p95_ms, s.max_ms);
    ImGui::Text("frame work (until the swap): avg %.3f ms, max %.3f ms", s.work_avg_ms, s.work_max_ms);
    ImGui::Text("swap (vsync wait included): avg %.3f ms, max %.3f ms", s.swap_avg_ms, s.swap_max_ms);
    ImGui::Text("newest frame: %u draw calls, %u instances", s.draw_calls, s.instances);

    // The last ~5 s of frames as graphs, read in place into the kept series.
    for (auto& p : frame_series) {
        p.clear();
    }
    auto& [interval, work, update, draw_ms, swap] = frame_series;
    for (std::size_t i = 0; i < v.metrics.frame_count(); ++i) {
        const auto& f = v.metrics.frame(i);
        if (f.t <= v.now - 5.0) {
            continue;
        }
        if (i > 0) {
            interval.push_back(static_cast<float>((f.t - v.metrics.frame(i - 1).t) * 1000.0));
        }
        work.push_back(static_cast<float>(f.work_ms));
        swap.push_back(static_cast<float>(f.swap_ms));
        update.push_back(static_cast<float>(f.update_ms));
        draw_ms.push_back(static_cast<float>(f.draw_ms));
    }
    plot("interval ms", interval, 16.7f, "ms");
    plot("work ms", work, 2.0f, "ms");
    plot("update ms", update, 1.0f, "ms");
    plot("draw ms", draw_ms, 1.0f, "ms");
    plot("swap ms", swap, 16.7f, "ms");

    ImGui::SeparatorText("startup (ms since the process started)");
    for (const auto& st : v.metrics.startup()) {
        ImGui::BulletText("%s: %.1f ms", st.name.c_str(), st.ms);
    }
}

void DebugWindow::Impl::memory(const DebugView& v) {
    if (ImGui::BeginTable("mem", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn("tag");
        ImGui::TableSetupColumn("live bytes");
        ImGui::TableSetupColumn("live blocks");
        ImGui::TableSetupColumn("allocations");
        ImGui::TableHeadersRow();
        core::mem::stats(tags);
        for (const auto& t : tags) {
            if (t.total_allocs == 0) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.name.data(), t.name.data() + t.name.size());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(t.live_bytes));
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(t.live_blocks));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(t.total_allocs));
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("the whole process over time (one sample a second)");
    const auto mem = v.metrics.memory_summary(v.now, 60.0);
    if (mem.samples >= 2) {
        ImGui::Text("live now: %lld bytes in %lld blocks; %.0f s before: %lld bytes in %lld blocks",
                    static_cast<long long>(mem.last.live_bytes), static_cast<long long>(mem.last.live_blocks),
                    mem.last.t - mem.first.t, static_cast<long long>(mem.first.live_bytes),
                    static_cast<long long>(mem.first.live_blocks));
        ImGui::Text("allocations: %.0f a second on average, %.0f at most", mem.allocs_per_s, mem.allocs_per_s_max);
        ImGui::TextDisabled("\"allocations\" only grows: each one is counted when made, freed or not. A leak shows "
                            "as live memory growing.");
    } else {
        ImGui::TextDisabled("not enough samples yet");
    }
    auto& [live_mb, rate] = memory_series;
    live_mb.clear();
    rate.clear();
    for (std::size_t i = 0; i < v.metrics.memory_count(); ++i) {
        const auto& m = v.metrics.memory(i);
        if (m.t <= v.now - 60.0) {
            continue;
        }
        live_mb.push_back(static_cast<float>(static_cast<double>(m.live_bytes) / (1024.0 * 1024.0)));
        if (i > 0 && m.t > v.metrics.memory(i - 1).t) {
            const auto& prev = v.metrics.memory(i - 1);
            rate.push_back(static_cast<float>(static_cast<double>(m.allocations - prev.allocations) / (m.t - prev.t)));
        }
    }
    plot("live MB", live_mb, 1.0f, "MB");
    plot("allocations a second", rate, 100.0f, "a second");

    ImGui::SeparatorText("glyph atlas");
    char fill[64];
    std::snprintf(fill, sizeof fill, "%.0f%% of the height in shelves", v.atlas_fill * 100); // as in plot()
    ImGui::ProgressBar(static_cast<float>(v.atlas_fill), {-1, 0}, fill);
    ImGui::Text("rendered %llu, uploaded %llu, shelves evicted %llu",
                static_cast<unsigned long long>(v.atlas.rendered), static_cast<unsigned long long>(v.atlas.rasterized),
                static_cast<unsigned long long>(v.atlas.evicted_shelves));
    ImGui::Text("log records dropped: %llu", static_cast<unsigned long long>(core::log::dropped()));
}

void DebugWindow::Impl::log() {
    static const char* levels[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("min level", &min_level, levels, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##category", "category", &category);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "search", &search);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &follow);
    // A copy of up to kLogRecords records is thousands of blocks: fetched only when the log moved.
    if (const auto version = core::log::recent_version(); version != log_version) {
        log_version = version;
        entries = core::log::recent(kLogRecords);
    }
    rows.clear();
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (static_cast<int>(e.level) >= min_level && contains_nocase(e.category, category) &&
            (contains_nocase(e.text, search) || contains_nocase(e.category, search))) {
            rows.push_back(i);
        }
    }
    listed = rows.size();
    ImGui::BeginChild("lines", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper; // only the rows on screen are formatted and drawn
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& e = entries[rows[static_cast<std::size_t>(r)]];
            char line[core::log::kMaxText + 64];
            format_into(line, "{}  {:<5}  {:<10}  {}", e.time, level_label(e.level), e.category, e.text);
            ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
            ImGui::TextUnformatted(line);
            ImGui::PopStyleColor();
        }
    }
    if (follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void DebugWindow::Impl::state(const DebugView& v) {
    const auto& m = v.model;
    const auto view = sheet::view_name(sheet::view(m));
    ImGui::SeparatorText("sheet");
    ImGui::Text("view: %.*s, open: %s", static_cast<int>(view.size()), view.data(), m.open ? "yes" : "no");
    if (m.permission) {
        ImGui::TextColored({1.0f, 0.8f, 0.35f, 1.0f}, "asking: %s — %s", m.permission->tool.c_str(),
                           m.permission->reason.c_str());
    }
    ImGui::Text("task %llu, activity %d, streaming %s", static_cast<unsigned long long>(m.task),
                static_cast<int>(m.activity), m.streaming ? "yes" : "no");
    ImGui::Text("answer %zu bytes, %zu shown; actions %zu; done: %s", m.answer.size(), v.revealed, m.actions.size(),
                m.done_title.c_str());
    ImGui::Text("question: %s", m.question.c_str());
    ImGui::Text("field: \"%s\" (caret %zu)", m.input.text.c_str(), m.input.caret);
    ImGui::Text("resting motion: %s", v.rest_motion ? "on" : "off");
    if (const auto& r = v.metrics.open_refused()) {
        ImGui::TextColored({1.0f, 0.8f, 0.35f, 1.0f}, "open refused at %.1f s: %s", r->t, r->reason.c_str());
    } else {
        ImGui::TextDisabled("the sheet never refused to open");
    }
    ImGui::SeparatorText("settings (config.json)");
    const auto& c = v.config;
    if (v.live_hotkey) {
        ImGui::Text("hotkey: %s (live: %.*s)", c.hotkey.c_str(), static_cast<int>(v.live_hotkey->size()),
                    v.live_hotkey->data());
    } else {
        ImGui::Text("hotkey: %s (none registered)", c.hotkey.c_str());
    }
    ImGui::Text("gpu %d, over fullscreen %s, text scale %.3f, resting motion %s", static_cast<int>(c.gpu),
                c.over_fullscreen ? "on" : "off", c.text_scale, c.rest_animation ? "on" : "off");
    ImGui::Text("advanced: system writes %s, secrets read %s, permanent delete %s",
                c.advanced.allow_system_writes ? "ON" : "off", c.advanced.allow_secrets_read ? "ON" : "off",
                c.advanced.allow_permanent_delete ? "ON" : "off");
    ImGui::SeparatorText("GL");
    ImGui::TextWrapped("%.*s", static_cast<int>(v.gl_driver.size()), v.gl_driver.data());
}

void DebugWindow::Impl::save_snapshot(const DebugView& v) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::error_code ec;
    const std::filesystem::path dir{v.logs_dir};
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / std::format("snapshot-{:04}{:02}{:02}-{:02}{:02}{:02}.txt", t.wYear, t.wMonth,
                                               t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << snapshot_text({.metrics = v.metrics, .model = v.model, .config = v.config, .live_hotkey = v.live_hotkey,
                          .now = v.now, .atlas_fill = v.atlas_fill});
    const auto name = core::utf8::to_utf8(file.wstring());
    saved = name ? *name : "?";
    WA_INFO("debug", "snapshot saved to {}", saved);
}

} // namespace app::debug
