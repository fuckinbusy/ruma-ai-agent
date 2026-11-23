#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/os.hpp"
#include "tools/winsys/change.hpp"
#include "tools/winsys/startup.hpp"
#include "tools/winsys/tasks.hpp"
#include "tools/winsys/wmi.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>

// startup.disable (phase 13a § 3): a program off at startup the way Windows itself does it, and its
// way back for the agent's Undo. What the call names is found again in Windows' own lists at the
// question and again at the run; a call that would change nothing (already off, already on, gone)
// is a read — not asked, said so — and refused if the state moved between the two.

namespace tools::winsys::args {

struct Disable {
    std::string place;
    std::string name;
    std::optional<std::string> where; // when two entries share the name
};
struct Enable {
    std::string entry; // "<place>\n<where>\n<name>", as startup.disable's change names it
};
struct TaskEnable {
    std::string path;
};
struct WmiRestore {
    std::string mof;
    std::optional<std::string> filter;
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct Switched {
    std::string name, place, state, note;
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

constexpr std::array<std::string_view, 4> kApprovedPlaces{"run", "run (32-bit)", "startup folder", "startup folder (all users)"};
constexpr std::string_view kTask = "scheduled task";
constexpr std::string_view kWmi = "WMI subscription";

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("");
}

auto wide(std::string_view s) -> std::wstring {
    return core::utf8::to_wide(s).value_or(std::wstring{});
}

template <class T>
auto read_args(std::string_view json) -> core::Result<T> {
    T a{};
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

auto said(std::string_view tool, out::Switched data, std::vector<Effect> effects = {}) -> Outcome {
    auto json = core::json::write(data);
    Outcome o{0, wrap(tool, L"", json ? *json : std::string{R"("?")"}, false)};
    o.effects = std::move(effects);
    return o;
}

auto leaf(std::string_view path) -> std::string {
    const auto slash = path.find_last_of('\\');
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

// What a call names, as Windows has it now.
struct Target {
    enum class Kind : std::uint8_t { Entry, Task, Wmi, Refused };
    Kind kind = Kind::Refused;
    StartupEntry entry;
    TaskInfo task;
    WmiSubscription wmi;
    std::string place, name, where;
    bool machine_wide = false;
    bool on = true;
};

auto find_entry(std::string_view place, std::string_view name, std::optional<std::string_view> where) -> core::Result<Target> {
    const auto& ctx = context();
    const auto list = startup_entries(ctx.registry, ctx.folders);
    const StartupEntry* found = nullptr;
    for (const auto& e : list.entries) {
        if (place_name(e.place) != place || utf8(e.name) != name || (where && utf8(e.key_or_folder) != *where)) {
            continue;
        }
        if (found != nullptr) {
            return core::make_error(core::Code::InvalidArgument,
                                    std::format("two entries named \xE2\x80\x9C{}\xE2\x80\x9D in {}: give \"where\" as startup.list gives it", name, place));
        }
        found = &e;
    }
    if (found == nullptr) {
        return core::make_error(core::Code::NotFound, std::format("no entry \xE2\x80\x9C{}\xE2\x80\x9D in {}: startup.list names them as they are", name, place));
    }
    Target t{.kind = Target::Kind::Entry, .entry = *found, .task = {}, .wmi = {}, .place = std::string{place}, .name = std::string{name},
             .where = utf8(found->key_or_folder), .machine_wide = found->machine_wide, .on = found->enabled};
    return t;
}

auto find_task(std::string_view path) -> core::Result<Target> {
    const auto list = context().read_tasks();
    const auto it = std::ranges::find(list.tasks, path, &TaskInfo::path);
    if (it == list.tasks.end()) {
        return core::make_error(core::Code::NotFound, std::format("no scheduled task \xE2\x80\x9C{}\xE2\x80\x9D: task.list names them", path));
    }
    return Target{.kind = Target::Kind::Task, .entry = {}, .task = *it, .wmi = {}, .place = std::string{kTask}, .name = it->path,
                  .where = "Task Scheduler", .machine_wide = !own_account(it->account), .on = it->enabled};
}

auto find_target(const args::Disable& a) -> core::Result<Target> {
    if (std::ranges::find(kApprovedPlaces, a.place) != kApprovedPlaces.end()) {
        return find_entry(a.place, a.name, a.where);
    }
    if (a.place == kTask) {
        return find_task(a.name);
    }
    if (a.place == kWmi) {
        const auto list = context().read_wmi();
        const auto it = std::ranges::find(list.subscriptions, a.name, &WmiSubscription::filter);
        if (it == list.subscriptions.end()) {
            return core::make_error(core::Code::NotFound, std::format("no WMI subscription with the filter \xE2\x80\x9C{}\xE2\x80\x9D", a.name));
        }
        return Target{.kind = Target::Kind::Wmi, .entry = {}, .task = {}, .wmi = *it, .place = std::string{kWmi}, .name = it->filter,
                      .where = "WMI root\\subscription", .machine_wide = true, .on = true};
    }
    // Anywhere else the policy refuses, saying how to do it yourself: where it is written, if startup.list has it.
    Target t{.kind = Target::Kind::Refused, .entry = {}, .task = {}, .wmi = {}, .place = a.place, .name = a.name,
             .where = a.where.value_or("its place in the registry"), .machine_wide = false, .on = true};
    if (!a.where) {
        if (auto e = find_entry(a.place, a.name, std::nullopt)) {
            t.where = e->where;
        }
    }
    return t;
}

auto request_of(std::string_view tool, const Target& t, bool change, bool back_on, const std::filesystem::path& wd) -> security::Request {
    return {.tool = std::string{tool}, .risk = change ? security::Risk::Startup : security::Risk::Read, .working_dir = wd,
            .place = t.place, .entry = t.name, .where = t.where, .machine_wide = t.machine_wide, .back_on = back_on};
}

// The run's own check: the call asked about is the one that runs, and a read stayed a read.
auto matches(const security::Permit& permit, const Target& t, bool change) -> bool {
    const auto& rq = permit.request();
    return rq.place == t.place && rq.entry == t.name && (permit.risk() == security::Risk::Startup) == change;
}

auto moved(std::string_view tool) -> Outcome {
    return failure(tool, core::Error{core::Code::Policy, "it changed since it was looked at: ask again"});
}

auto entry_id(const Target& t) -> std::wstring {
    return wide(t.place) + L"\n" + wide(t.where) + L"\n" + wide(t.name);
}

// A taken subscription's MOF file: data\quarantine\wmi\<when>-<n>.mof.
auto mof_file() -> std::filesystem::path {
    static std::atomic<int> n{0};
    const auto when = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return context().quarantine / "wmi" / std::format("{}-{}.mof", when, n.fetch_add(1));
}

} // namespace

// startup.disable

auto disable_schema() -> std::string {
    return schema_of<args::Disable>();
}

auto disable_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Disable>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const auto t = find_target(*a);
    if (!t) {
        return std::unexpected{t.error()};
    }
    return request_of("startup.disable", *t, t->kind == Target::Kind::Refused || t->on, false, wd);
}

auto disable_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "startup.disable";
    const auto a = read_args<args::Disable>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    const auto t = find_target(*a);
    if (!t) {
        return failure(tool, t.error());
    }
    if (t->kind == Target::Kind::Refused) {
        return moved(tool); // the policy never lets one through
    }
    if (!t->on) {
        return said(tool, {t->name, t->place, "already off", "Nothing was changed."});
    }
    if (!matches(permit, *t, true)) {
        return moved(tool); // looked at while off, on again since: asked afresh
    }
    const char* note = "Nothing is deleted: the user can turn it back on (Undo in the log).";
    switch (t->kind) {
    case Target::Kind::Entry: {
        if (auto done = os::current().set_startup_approved(context().registry, t->entry, false); !done) {
            return failure(tool, done.error());
        }
        return said(tool, {t->name, t->place, "turned off at startup", note},
                    {Effect{.kind = EffectKind::StartupOff, .from = t->entry.name, .to = entry_id(*t), .undoable = true, .before = {}}});
    }
    case Target::Kind::Task: {
        if (auto done = os::current().set_task_enabled(wide(t->task.path), false); !done) {
            return failure(tool, done.error());
        }
        return said(tool, {t->name, t->place, "switched off", note},
                    {Effect{.kind = EffectKind::TaskOff, .from = wide(leaf(t->task.path)), .to = wide(t->task.path), .undoable = true, .before = {}}});
    }
    case Target::Kind::Wmi: {
        if (context().quarantine.empty()) {
            return failure(tool, core::Error{core::Code::Internal, "there is no folder to keep the subscription in"});
        }
        const auto file = mof_file();
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto mof = os::current().take_wmi_subscription(t->wmi);
        if (!mof) {
            return failure(tool, mof.error());
        }
        std::ofstream out(file, std::ios::binary);
        out << *mof;
        out.close();
        if (!out) { // not kept: put back at once, nothing is lost
            (void)os::current().put_wmi_subscription(*mof);
            return failure(tool, core::Error{core::Code::Io, "the subscription could not be kept, so it was put back"});
        }
        return said(tool, {t->name, t->place, "taken off and kept", note},
                    {Effect{.kind = EffectKind::WmiTaken, .from = wide(t->name), .to = file.wstring(), .undoable = true, .before = {}}});
    }
    case Target::Kind::Refused: break;
    }
    return moved(tool);
}

// startup.enable — a Run entry or a startup folder's shortcut back on (the agent's Undo)

namespace {

auto enable_target(const args::Enable& a) -> core::Result<std::optional<Target>> {
    const auto first = a.entry.find('\n');
    const auto second = first == std::string::npos ? std::string::npos : a.entry.find('\n', first + 1);
    if (second == std::string::npos) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"entry\" is a change's own, from startup.disable");
    }
    const std::string_view entry{a.entry};
    const auto place = entry.substr(0, first);
    const auto where = entry.substr(first + 1, second - first - 1);
    const auto name = entry.substr(second + 1);
    auto t = find_entry(place, name, where);
    if (!t) {
        if (t.error().code == core::Code::NotFound) {
            return std::optional<Target>{}; // removed since: nothing to turn on
        }
        return std::unexpected{t.error()};
    }
    return std::optional<Target>{std::move(*t)};
}

} // namespace

auto enable_schema() -> std::string {
    return schema_of<args::Enable>();
}

auto enable_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Enable>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const auto t = enable_target(*a);
    if (!t) {
        return std::unexpected{t.error()};
    }
    if (!*t) {
        return security::Request{.tool = "startup.enable", .risk = security::Risk::Read, .working_dir = wd};
    }
    return request_of("startup.enable", **t, !(*t)->on, true, wd);
}

auto enable_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "startup.enable";
    const auto a = read_args<args::Enable>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    const auto t = enable_target(*a);
    if (!t) {
        return failure(tool, t.error());
    }
    const auto back = [&](std::string name, std::string state, std::string note) {
        return said(tool, {std::move(name), "startup", std::move(state), std::move(note)},
                    {Effect{.kind = EffectKind::SwitchedOn, .from = {}, .to = wide(a->entry), .undoable = false, .before = {}}});
    };
    if (!*t) {
        return back(a->entry.substr(a->entry.find_last_of('\n') + 1), "no longer there", "The entry was removed since: nothing to turn back on.");
    }
    const Target& target = **t;
    if (target.on) {
        return back(target.name, "already on", "It was turned back on already: nothing was changed.");
    }
    if (!matches(permit, target, true)) {
        return moved(tool);
    }
    if (auto done = os::current().set_startup_approved(context().registry, target.entry, true); !done) {
        return failure(tool, done.error());
    }
    return back(target.name, "turned back on", "It starts with Windows again.");
}

// task.enable

auto task_enable_schema() -> std::string {
    return schema_of<args::TaskEnable>();
}

auto task_enable_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::TaskEnable>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const auto t = find_task(a->path);
    if (!t) {
        return security::Request{.tool = "task.enable", .risk = security::Risk::Read, .working_dir = wd}; // gone: nothing to ask
    }
    return request_of("task.enable", *t, !t->on, true, wd);
}

auto task_enable_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "task.enable";
    const auto a = read_args<args::TaskEnable>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    const auto back = [&](std::string state, std::string note) {
        return said(tool, {leaf(a->path), std::string{kTask}, std::move(state), std::move(note)},
                    {Effect{.kind = EffectKind::SwitchedOn, .from = {}, .to = wide(a->path), .undoable = false, .before = {}}});
    };
    const auto t = find_task(a->path);
    if (!t) {
        return back("no longer there", "The task was deleted since: nothing to turn back on.");
    }
    if (t->on) {
        return back("already on", "It was switched back on already: nothing was changed.");
    }
    if (!matches(permit, *t, true)) {
        return moved(tool);
    }
    if (auto done = os::current().set_task_enabled(wide(a->path), true); !done) {
        return failure(tool, done.error());
    }
    return back("switched back on", "It runs again as it did.");
}

// wmi.restore

namespace {

auto read_mof(const std::string& file) -> std::optional<std::string> {
    std::ifstream in(std::filesystem::path{wide(file)}, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

auto still_there(const args::WmiRestore& a) -> bool {
    if (!a.filter) {
        return false;
    }
    const auto list = context().read_wmi();
    return std::ranges::find(list.subscriptions, *a.filter, &WmiSubscription::filter) != list.subscriptions.end();
}

} // namespace

auto wmi_restore_schema() -> std::string {
    return schema_of<args::WmiRestore>();
}

auto wmi_restore_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::WmiRestore>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (!read_mof(a->mof)) {
        return core::make_error(core::Code::NotFound, "the kept subscription is gone: " + a->mof);
    }
    const Target t{.kind = Target::Kind::Wmi, .entry = {}, .task = {}, .wmi = {}, .place = std::string{kWmi},
                   .name = a->filter.value_or(leaf(a->mof)), .where = "WMI root\\subscription", .machine_wide = true, .on = false};
    return request_of("wmi.restore", t, !still_there(*a), true, wd);
}

auto wmi_restore_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "wmi.restore";
    const auto a = read_args<args::WmiRestore>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    const auto name = a->filter.value_or(leaf(a->mof));
    const auto back = [&](std::string state, std::string note) {
        return said(tool, {name, std::string{kWmi}, std::move(state), std::move(note)},
                    {Effect{.kind = EffectKind::SwitchedOn, .from = {}, .to = wide(a->mof), .undoable = false, .before = {}}});
    };
    if (still_there(*a)) {
        return back("already there", "It is in place already: nothing was changed.");
    }
    const auto mof = read_mof(a->mof);
    if (!mof) {
        return failure(tool, core::Error{core::Code::NotFound, "the kept subscription is gone: " + a->mof});
    }
    if (permit.risk() != security::Risk::Startup || permit.request().place != kWmi) {
        return moved(tool);
    }
    if (auto done = os::current().put_wmi_subscription(*mof); !done) {
        return failure(tool, done.error());
    }
    return back("put back", "It runs again as it did.");
}

} // namespace tools::winsys
