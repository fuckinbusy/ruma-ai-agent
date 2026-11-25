#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/product.hpp"
#include "core/utf8.hpp"
#include "security/paths.hpp"
#include "tools/os.hpp"
#include "tools/winsys/apps.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <format>
#include <optional>
#include <span>

#include <windows.h>
#include <tlhelp32.h>

// app.uninstall (phase 13a § 4): a program removed by its own uninstaller, in its own window, asked
// every time. The program is found again at the question and at the run; afterwards the agent waits
// for the uninstaller and what it started, then looks whether the program's entry is gone — never
// "removed" while it is there. A Store app has no uninstaller window: Settings opens on its page.

namespace tools::winsys::args {

struct Uninstall {
    std::string name;
    std::optional<std::string> version; // when two share the name
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct Uninstalled {
    std::string name, version, result, note;
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

constexpr std::string_view kTool = "app.uninstall";

auto lower(std::string s) -> std::string {
    std::ranges::transform(s, s.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return s;
}

auto wide(std::string_view s) -> std::wstring {
    return core::utf8::to_wide(s).value_or(std::wstring{});
}

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

auto size_text(std::uint64_t bytes) -> std::string {
    constexpr std::array<const char*, 5> kUnits{"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < kUnits.size()) {
        v /= 1024.0;
        ++u;
    }
    return v < 10.0 && u > 0 ? std::format("{:.1f} {}", v, kUnits[u]) : std::format("{:.0f} {}", v, kUnits[u]);
}

auto read_args(std::string_view json) -> core::Result<args::Uninstall> {
    args::Uninstall a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    if (a.name.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"name\" is the program's name, as apps.list gives it");
    }
    return a;
}

// The programs the call names: by its name (any case), else by a part of it; its version when given.
// Installed first, first (the "old" one); none said, last.
auto find_apps(const args::Uninstall& a) -> core::Result<std::vector<App>> {
    const auto& ctx = context();
    auto all = apps(ctx.registry, false);
    for (auto& s : ctx.read_store()) {
        all.push_back(std::move(s));
    }
    const auto wanted = lower(a.name);
    const auto version_ok = [&](const App& app) { return !a.version || app.version == *a.version; };
    std::vector<App> found;
    for (const auto& app : all) {
        if (lower(app.name) == wanted && version_ok(app)) {
            found.push_back(app);
        }
    }
    if (found.empty()) {
        for (const auto& app : all) {
            if (lower(app.name).find(wanted) != std::string::npos && version_ok(app)) {
                found.push_back(app);
            }
        }
    }
    if (found.empty()) {
        return core::make_error(core::Code::NotFound, std::format("no program named \xE2\x80\x9C{}\xE2\x80\x9D: apps.list names them", a.name));
    }
    std::ranges::stable_sort(found, [](const App& x, const App& y) {
        return !x.date.empty() && (y.date.empty() || x.date < y.date);
    });
    return found;
}

// "20240808" -> "8 Aug 2024"; else as it is.
auto date_text(std::string_view d) -> std::string {
    static constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, m = 0, day = 0;
    if (d.size() != 8 || std::from_chars(d.data(), d.data() + 4, y).ec != std::errc{} ||
        std::from_chars(d.data() + 4, d.data() + 6, m).ec != std::errc{} || std::from_chars(d.data() + 6, d.data() + 8, day).ec != std::errc{} ||
        m < 1 || m > 12) {
        return std::string{d};
    }
    return std::format("{} {} {}", day, kMonths[static_cast<std::size_t>(m - 1)], y);
}

// A program of this folder runs now (its "in use" on the card).
auto running_in(const std::wstring& folder) -> bool {
    if (folder.empty()) {
        return false;
    }
    std::wstring base = folder;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W e{.dwSize = sizeof(PROCESSENTRY32W)};
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (h == nullptr) {
            continue;
        }
        std::array<wchar_t, 1024> image{};
        DWORD n = static_cast<DWORD>(image.size());
        if (QueryFullProcessImageNameW(h, 0, image.data(), &n) && n > base.size() && image[base.size()] == L'\\' &&
            CompareStringOrdinal(image.data(), static_cast<int>(base.size()), base.c_str(), static_cast<int>(base.size()), TRUE) == CSTR_EQUAL) {
            found = true;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

// The one program the call names: the only match, or among several the one asked about (`asked`:
// its entry and version); else an error listing them, for the model to ask the user.
auto find_app(const args::Uninstall& a, const security::Request* asked = nullptr) -> core::Result<App> {
    auto found = find_apps(a);
    if (!found) {
        return std::unexpected{found.error()};
    }
    if (found->size() == 1) {
        return found->front();
    }
    if (asked != nullptr) {
        for (const auto& app : *found) {
            if (app.name == asked->entry && app.version == asked->version) {
                return app;
            }
        }
    }
    std::string list;
    for (const auto& app : *found) {
        list += std::format("{}{} {}{}{}", list.empty() ? "" : "; ", app.name, app.version,
                            app.size ? ", " + size_text(*app.size) : std::string{}, app.date.empty() ? "" : ", installed " + date_text(app.date));
    }
    return core::make_error(core::Code::InvalidArgument,
                            std::format("{} programs match \xE2\x80\x9C{}\xE2\x80\x9D: {}. Ask the user which one, then give its name and "
                                        "version.", found->size(), a.name, list));
}

auto agent_itself(const App& app) -> bool {
    // By the name it is listed under (the installer writes the product's, core::product) or the one
    // from before the rename; its uninstaller sits in Program Files, outside the agent's data folders.
    const std::string name = lower(app.name);
    if (name == lower(std::string{core::product::kName}) || name == "winaiagent") {
        return true;
    }
    const auto& roots = security::paths::system_roots();
    return std::ranges::any_of(roots.agent_dirs, [&](const std::wstring& dir) {
        return !dir.empty() && app.uninstall.size() >= dir.size() &&
               std::search(app.uninstall.begin(), app.uninstall.end(), dir.begin(), dir.end(),
                           [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); }) != app.uninstall.end();
    });
}

// What the policy is told the program is: Windows' own parts and the agent are never removed.
auto place_of(const App& app) -> std::string {
    static constexpr std::array<std::string_view, 3> kWindows{"microsoft edge", "microsoft edge webview2 runtime", "microsoft edge update"};
    if (agent_itself(app)) {
        return "the agent itself";
    }
    if (app.system_component) {
        return "system component";
    }
    if (std::ranges::find(kWindows, lower(app.name)) != kWindows.end()) {
        return "Windows component";
    }
    return "program";
}

auto where_of(const App& app) -> std::string {
    std::string where = utf8(app.location.empty() ? app.key : app.location);
    if (app.size) {
        where += " \xC2\xB7 " + size_text(*app.size);
    }
    return where;
}

auto said(out::Uninstalled data, std::vector<Effect> effects = {}) -> Outcome {
    auto json = core::json::write(data);
    Outcome o{0, wrap(kTool, L"", json ? *json : std::string{R"("?")"}, false)};
    o.effects = std::move(effects);
    return o;
}

} // namespace

auto uninstall_schema() -> std::string {
    auto s = glz::write_json_schema<args::Uninstall>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto uninstall_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    // Several versions of a program (06b): a choice on the card, the one installed first asked about
    // until the user picks; the call is narrowed to the one picked.
    if (const auto found = find_apps(*a); found && found->size() > 1 &&
                                         std::ranges::all_of(*found, [](const App& x) { return !x.store && place_of(x) == "program"; })) {
        const auto& first = found->front();
        security::Request rq{.tool = std::string{kTool}, .risk = security::Risk::Uninstall, .working_dir = wd, .place = "program",
                             .entry = first.name, .where = where_of(first), .machine_wide = first.machine_wide, .version = first.version};
        for (const auto& x : *found) {
            std::string detail = x.date.empty() ? std::string{} : "installed " + date_text(x.date);
            if (running_in(x.location)) {
                detail += detail.empty() ? "in use" : " \xC2\xB7 in use";
            }
            rq.choices.push_back({x.name, x.version, std::move(detail), where_of(x)});
        }
        return rq;
    }
    const auto app = find_app(*a);
    if (!app) {
        return std::unexpected{app.error()};
    }
    if (app->store) { // only Settings opens: the user removes it there
        return security::Request{.tool = std::string{kTool}, .risk = security::Risk::Read, .working_dir = wd};
    }
    return security::Request{.tool = std::string{kTool}, .risk = security::Risk::Uninstall, .working_dir = wd, .place = place_of(*app),
                             .entry = app->name, .where = where_of(*app), .machine_wide = app->machine_wide, .version = app->version};
}

auto uninstall_narrow(std::string_view json, std::span<const std::size_t> keep) -> core::Result<std::string> {
    const auto a = read_args(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const auto found = find_apps(*a);
    if (!found) {
        return std::unexpected{found.error()};
    }
    if (keep.size() != 1 || keep.front() >= found->size()) {
        return core::make_error(core::Code::InvalidArgument, "one of the programs offered, by its place in the list");
    }
    const auto& app = (*found)[keep.front()];
    const auto out = core::json::write(args::Uninstall{.name = app.name, .version = app.version});
    if (!out) {
        return core::make_error(core::Code::Internal, "the choice could not be written");
    }
    return *out;
}

auto uninstall_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = read_args(json);
    if (!a) {
        return failure(kTool, a.error());
    }
    const auto app = find_app(*a, &permit.request());
    if (!app) {
        return failure(kTool, app.error());
    }
    if (app->store) {
        if (auto opened = os::current().open(L"ms-settings:appsfeatures"); !opened) {
            return failure(kTool, opened.error());
        }
        return said({app->name, app->version, "Settings opened",
                      std::format("Store apps have no uninstaller window of their own: Settings \xE2\x80\xBA Apps is open, the user removes "
                                  "\xE2\x80\x9C{}\xE2\x80\x9D there (its \xE2\x80\xA6 menu, Uninstall).",
                                  app->name)});
    }
    if (permit.risk() != security::Risk::Uninstall || permit.request().entry != app->name || place_of(*app) != "program") {
        return failure(kTool, core::Error{core::Code::Policy, "it changed since it was asked about: ask again"});
    }
    const auto [program, arguments] = uninstall_command(*app);
    const auto pid = os::current().launch_uninstaller(program, arguments);
    if (!pid) {
        return failure(kTool, pid.error());
    }
    const bool ended = wait_for_tree(*pid, control().stop, context().uninstall_wait);
    const auto now = apps(context().registry, false);
    const bool still = std::ranges::any_of(now, [&](const App& x) { return x.key == app->key; });
    if (!still) {
        return said({app->name, app->version, "removed", "Its entry is gone from Windows' list of programs: it is uninstalled."},
                    {Effect{.kind = EffectKind::Uninstalled, .from = wide(app->name), .to = app->key, .undoable = false, .before = {}}});
    }
    if (!ended) {
        return said({app->name, app->version, "the uninstaller is still open",
                     "Its uninstaller (or what it started) still runs: the user may be answering it. Do not say it is removed."});
    }
    return said({app->name, app->version, "still installed",
                 "The uninstaller ended but the program is still in Windows' list: it was cancelled or did not finish."});
}

} // namespace tools::winsys
