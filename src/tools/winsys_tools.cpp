#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "core/agent_ids.hpp"
#include "tools/image.hpp"
#include "tools/os.hpp"
#include "tools/winsys/apps.hpp"
#include "tools/winsys/look.hpp"
#include "tools/winsys/private_rules.hpp"
#include "tools/winsys/reader_data.hpp"
#include "tools/winsys/sensitive.hpp"
#include "tools/winsys/events.hpp"
#include "tools/winsys/net.hpp"
#include "tools/winsys/sample.hpp"
#include "tools/winsys/services.hpp"
#include "tools/winsys/signature.hpp"
#include "tools/winsys/startup.hpp"
#include "tools/winsys/tasks.hpp"
#include "tools/winsys/wmi.hpp"

#include <glaze/glaze.hpp>

#include <optional>
#include <vector>

namespace tools::winsys::args {

struct Signature {
    std::string path;
};
struct Sample {
    std::optional<int> seconds; // 1..60, 10 if null
};
struct Events {
    std::optional<int> days; // 1..30, 7 if null
};
struct Apps {
    std::optional<bool> store; // Store apps too (true if null)
};
struct Look {
    std::optional<std::string> as; // "text" (default) or "picture"
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct Startup {
    std::string name, command, file, place, where;
    bool enabled = true;
    bool machine_wide = false;
    std::optional<int> impact_ms;
};
struct StartupList {
    std::vector<Startup> entries;
    std::vector<std::string> not_read;
    std::optional<std::uint32_t> last_boot_ms; // the helper's boot log: the last boot
    bool deeper = false;                       // the helper's facts are in it
};
struct Service {
    std::string name, display, state, start_type, file, account;
    bool driver = false;
};
struct ServiceList {
    std::vector<Service> services;
    std::string note = "Services are only looked at: the agent cannot start, stop or change one.";
};
struct Task {
    std::string path, author, account, command, file, triggers;
    bool enabled = true;
    std::optional<std::int64_t> last_run;
};
struct TaskList {
    std::vector<Task> tasks;
    std::vector<std::string> not_read;
};
struct Signature {
    bool signed_ok = false;
    bool microsoft = false;
    std::string publisher, sha256;
};
struct Process {
    std::uint32_t pid = 0;
    std::string name, path;
    std::vector<float> cpu, gpu; // percent, a point a second
    std::uint64_t memory = 0;
    bool windowless = true;
};
struct Sample {
    int seconds = 0;
    std::vector<Process> processes; // the busiest 50
};
struct Connection {
    std::string proto, local, remote, state, process;
    std::uint16_t local_port = 0, remote_port = 0;
    std::uint32_t pid = 0;
};
struct Network {
    std::vector<Connection> connections;
    std::string hosts_extra; // the hosts file beyond localhost
    std::optional<std::string> proxy, winhttp_proxy;
    std::vector<std::string> dns;
};
struct EventGroup {
    std::string provider, message;
    std::uint32_t id = 0;
    int level = 0;
    std::size_t count = 0;
    std::int64_t last = 0;
};
struct Events {
    int days = 0;
    std::size_t total = 0;
    std::vector<EventGroup> groups;
    std::vector<std::string> not_read;
};
struct App {
    std::string name, version, publisher, date, uninstall;
    std::optional<std::uint64_t> size;
    bool store = false, system_component = false, msi = false, machine_wide = false;
};
struct Apps {
    std::vector<App> apps;
};
struct Window {
    std::string program, title;
};
struct Looked {
    Window window;
    std::string text;     // as=text: a line per element ("button: OK")
    bool picture = false; // as=picture: shown to the model in the next message
};
struct Sensitive {
    bool sensitive = true;
    std::string reason;
    std::string say = "Tell the user plainly that the screen holds sensitive information, so you did not look at it or "
                      "read it, and help from what they tell you.";
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("");
}

auto empty_args(std::string_view json) -> core::Result<void> {
    glz::generic doc;
    if (json.empty() || json == "{}" || json == "null" || (!glz::read_json(doc, json) && doc.is_object() && doc.get_object().empty())) {
        return {};
    }
    return core::make_error(core::Code::InvalidArgument, "arguments: this tool takes none");
}

template <class T>
auto envelope(std::string_view tool, const T& data) -> Outcome {
    auto json = core::json::write(data);
    if (!json) {
        return {2, wrap(tool, L"", R"("the result could not be written")", true)};
    }
    return {0, wrap(tool, L"", *json, false)};
}

auto pathless(std::string_view tool, const std::filesystem::path& wd) -> security::Request {
    return {.tool = std::string{tool}, .risk = security::Risk::Read, .working_dir = wd};
}

} // namespace

auto masked(std::wstring_view text) -> std::string {
    const auto in = utf8(text);
    std::string out(in.size() + 16, '\0');
    out.resize(core::log::mask_secrets(in, out));
    return out;
}

auto empty_schema() -> std::string {
    return R"({"type":"object","properties":{},"additionalProperties":false})";
}

// startup.list

auto startup_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = empty_args(args); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("startup.list", wd);
}

auto startup_run(const security::Permit&, std::string_view) -> Outcome {
    const auto& ctx = context();
    const auto list = startup_entries(ctx.registry, ctx.folders);
    out::StartupList data;
    // The helper's boot log (Check deeper): a program's delay by its file's name; one not in the log
    // slowed nothing it recorded (0).
    const auto deeper = ctx.deeper_data ? ctx.deeper_data() : nullptr;
    const bool timed = deeper != nullptr && deeper->events_read;
    const auto impact_of = [&](const StartupEntry& e) -> std::optional<int> {
        if (!timed) {
            return e.impact_ms;
        }
        const auto leaf = std::filesystem::path{e.file}.filename().wstring();
        int most = 0;
        for (const auto& s : deeper->slow_starts) {
            const auto file = core::utf8::to_wide(s.file).value_or(L"");
            const auto other = std::filesystem::path{file}.filename().wstring();
            if (!leaf.empty() && CompareStringOrdinal(leaf.c_str(), static_cast<int>(leaf.size()), other.c_str(), static_cast<int>(other.size()),
                                                      TRUE) == CSTR_EQUAL) {
                most = std::max(most, static_cast<int>(s.total_ms));
            }
        }
        return most;
    };
    for (const auto& e : list.entries) {
        data.entries.push_back({.name = utf8(e.name), .command = masked(e.command), .file = utf8(e.file),
                                .place = std::string{place_name(e.place)}, .where = utf8(e.key_or_folder), .enabled = e.enabled,
                                .machine_wide = e.machine_wide, .impact_ms = impact_of(e)});
    }
    data.deeper = deeper != nullptr;
    if (timed && !deeper->boots.empty()) {
        data.last_boot_ms = deeper->boots.front().boot_ms; // the newest first
    }
    for (const auto& n : list.not_read) {
        data.not_read.push_back(utf8(n));
    }
    return envelope("startup.list", data);
}

// service.list — looked at only (phase 13a rule 1)

auto services_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = empty_args(args); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("service.list", wd);
}

auto services_run(const security::Permit&, std::string_view) -> Outcome {
    out::ServiceList data;
    for (const auto& s : services()) {
        data.services.push_back({.name = s.name, .display = s.display, .state = s.state, .start_type = s.start_type,
                                 .file = utf8(s.file), .account = s.account, .driver = s.driver});
    }
    return envelope("service.list", data);
}

// task.list

auto tasks_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = empty_args(args); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("task.list", wd);
}

auto tasks_run(const security::Permit&, std::string_view) -> Outcome {
    const auto list = tasks();
    out::TaskList data;
    for (const auto& t : list.tasks) {
        data.tasks.push_back({.path = t.path, .author = t.author, .account = t.account, .command = masked(t.command),
                              .file = utf8(t.file), .triggers = t.triggers, .enabled = t.enabled, .last_run = t.last_run});
    }
    data.not_read = list.not_read;
    return envelope("task.list", data);
}

// file.signature

auto signature_schema() -> std::string {
    auto s = glz::write_json_schema<args::Signature>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto signature_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    args::Signature a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    auto wide = core::utf8::to_wide(a.path);
    if (!wide || wide->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" names the file");
    }
    return security::Request{.tool = "file.signature", .risk = security::Risk::Read, .paths = {std::move(*wide)}, .working_dir = wd};
}

auto signature_run(const security::Permit& permit, std::string_view) -> Outcome {
    if (permit.paths().empty()) {
        return failure("file.signature", core::Error{core::Code::InvalidArgument, "no file"});
    }
    const auto s = signature(permit.paths().front().path);
    return envelope("file.signature", out::Signature{.signed_ok = s.signed_ok, .microsoft = s.microsoft, .publisher = s.publisher,
                                                     .sha256 = s.sha256});
}

namespace {

template <class T>
auto read_args(std::string_view json) -> core::Result<T> {
    T a{};
    if (json.empty() || json == "null") {
        return a;
    }
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return a;
}

template <class T>
auto schema_of() -> std::string {
    auto s = glz::write_json_schema<T>();
    return s ? std::move(*s) : std::string{"{}"};
}

} // namespace

// process.sample

auto sample_schema() -> std::string {
    return schema_of<args::Sample>();
}

auto sample_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Sample>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->seconds && (*a->seconds < 1 || *a->seconds > 60)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"seconds\" is 1 to 60");
    }
    return pathless("process.sample", wd);
}

auto sample_run(const security::Permit&, std::string_view json) -> Outcome {
    const auto a = read_args<args::Sample>(json).value_or(args::Sample{});
    const int seconds = a.seconds.value_or(10);
    out::Sample data{.seconds = seconds, .processes = {}};
    for (const auto& s : sample_processes(std::chrono::seconds{seconds}, {})) {
        data.processes.push_back({.pid = s.pid, .name = s.name, .path = utf8(s.path), .cpu = s.cpu, .gpu = s.gpu, .memory = s.memory,
                                  .windowless = s.windowless});
    }
    return envelope("process.sample", data);
}

// net.connections

auto connections_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = empty_args(args); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("net.connections", wd);
}

auto connections_run(const security::Permit&, std::string_view) -> Outcome {
    out::Network data;
    for (auto& c : connections()) {
        data.connections.push_back({.proto = std::move(c.proto), .local = std::move(c.local), .remote = std::move(c.remote),
                                    .state = std::move(c.state), .process = std::move(c.process), .local_port = c.local_port,
                                    .remote_port = c.remote_port, .pid = c.pid});
    }
    auto settings = net_settings();
    data.hosts_extra = std::move(settings.hosts_extra);
    if (settings.proxy) {
        data.proxy = masked(core::utf8::to_wide(*settings.proxy).value_or(L""));
    }
    data.winhttp_proxy = std::move(settings.winhttp_proxy);
    data.dns = std::move(settings.dns);
    return envelope("net.connections", data);
}

// eventlog.query

auto events_schema() -> std::string {
    return schema_of<args::Events>();
}

auto events_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Events>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->days && (*a->days < 1 || *a->days > 30)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"days\" is 1 to 30");
    }
    return pathless("eventlog.query", wd);
}

auto events_run(const security::Permit&, std::string_view json) -> Outcome {
    const int days = read_args<args::Events>(json).value_or(args::Events{}).days.value_or(7);
    const auto r = crashes(days);
    out::Events data{.days = days, .total = r.total, .groups = {}, .not_read = r.not_read};
    for (const auto& g : r.groups) {
        data.groups.push_back({.provider = g.provider, .message = masked(core::utf8::to_wide(g.message).value_or(L"")), .id = g.id,
                               .level = g.level, .count = g.count, .last = g.last});
    }
    return envelope("eventlog.query", data);
}

// apps.list

auto apps_schema() -> std::string {
    return schema_of<args::Apps>();
}

auto apps_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (const auto a = read_args<args::Apps>(json); !a) {
        return std::unexpected{a.error()};
    }
    return pathless("apps.list", wd);
}

auto apps_run(const security::Permit&, std::string_view json) -> Outcome {
    const bool store = read_args<args::Apps>(json).value_or(args::Apps{}).store.value_or(true);
    out::Apps data;
    for (const auto& a : apps(context().registry, store)) {
        data.apps.push_back({.name = a.name, .version = a.version, .publisher = a.publisher, .date = a.date, .uninstall = masked(a.uninstall),
                             .size = a.size, .store = a.store, .system_component = a.system_component, .msi = a.msi,
                             .machine_wide = a.machine_wide});
    }
    return envelope("apps.list", data);
}

// window.look — the window the user was in, unasked but always shown; never a sensitive one

auto look_schema() -> std::string {
    return schema_of<args::Look>();
}

auto look_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Look>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->as && *a->as != "text" && *a->as != "picture") {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"as\" is \"text\" or \"picture\"");
    }
    return security::Request{.tool = "window.look", .risk = security::Risk::Look, .working_dir = wd};
}

namespace {

// Nothing to look at: no window, the agent's own, the desktop or the taskbar.
auto nothing_to_look_at(HWND w) -> bool {
    if (w == nullptr || !IsWindow(w)) {
        return true;
    }
    wchar_t cls[64]{};
    GetClassNameW(w, cls, 64);
    const std::wstring_view c{cls};
    return c == core::agent_ids::kWindowClass || c == L"WinAiAgent.TrayMenu" || c == L"Progman" || c == L"WorkerW" ||
           c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd";
}

} // namespace

auto look_run(const security::Permit&, std::string_view json) -> Outcome {
    const bool picture = read_args<args::Look>(json).value_or(args::Look{}).as.value_or("text") == "picture";
    const auto& hooks = os::capture_hooks();
    const auto w = static_cast<HWND>(hooks.active ? hooks.active() : nullptr);
    if (nothing_to_look_at(w)) {
        return {0, wrap("window.look", L"", R"({"window":null})", false)};
    }
    // Checked now, at the read: a window that turned sensitive since the call was asked is not read.
    auto facts = facts_of(w);
    // A browser's password field is seen by UI Automation only while it has the focus: when the sheet opened.
    facts.password_focused = facts.password_focused || (hooks.password_at_open && hooks.password_at_open());
    const auto rules = load_private(private_file());
    if (const auto s = sensitivity_of(facts, rules); s.sensitive) {
        return envelope("window.look", out::Sensitive{.sensitive = true, .reason = s.reason});
    }
    out::Looked data{.window = {.program = facts.program, .title = masked(core::utf8::to_wide(facts.title).value_or(L""))}};
    if (!picture) {
        auto text = read_window_text(w);
        if (!text) {
            return failure("window.look", text.error());
        }
        data.text = masked(core::utf8::to_wide(*text).value_or(L""));
        return envelope("window.look", data);
    }
    // The very window checked above: never the foreground again, which may be another by now.
    auto shot = os::current().capture(os::CaptureSpec{.target = os::CaptureTarget::Active, .window = {}, .monitor = 0, .hwnd = w});
    if (!shot) {
        return failure("window.look", shot.error());
    }
    auto image = image::for_model(shot->picture, true, "window");
    if (!image) {
        return failure("window.look", image.error());
    }
    data.picture = true;
    auto out = envelope("window.look", data);
    out.images.push_back(std::move(*image));
    return out;
}

} // namespace tools::winsys
