#include "setup/steps.hpp"
#include "core/product.hpp"

#include "core/agent_ids.hpp"
#include "core/fs.hpp"
#include "core/utf8.hpp"
#include "core/version.hpp"
#include "core/win.hpp"
#include "setup/win_path.hpp"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <format>
#include <set>
#include <vector>

namespace setup {
namespace {

constexpr std::string_view kAgentBusy = "Close the agent first";
constexpr std::wstring_view kBackup = L".wa-old";

// What has been done, undone in reverse unless the whole operation succeeded.
class Undo {
public:
    Undo() = default;
    Undo(const Undo&) = delete;
    Undo& operator=(const Undo&) = delete;
    ~Undo() {
        for (auto it = steps_.rbegin(); it != steps_.rend(); ++it)
            (*it)();
    }
    void push(std::function<void()> step) { steps_.push_back(std::move(step)); }
    void commit() { steps_.clear(); }

private:
    std::vector<std::function<void()>> steps_;
};

auto wide(std::string_view s) -> std::wstring {
    auto w = core::utf8::to_wide(s);
    return w ? std::move(*w) : std::wstring{};
}

auto narrow(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

auto quoted(const std::filesystem::path& p) -> std::wstring {
    return L"\"" + p.native() + L"\"";
}

auto same_path(const std::filesystem::path& a, const std::filesystem::path& b) -> bool {
    const auto x = a.lexically_normal().native();
    const auto y = b.lexically_normal().native();
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) ==
           CSTR_EQUAL;
}

auto inside(const std::filesystem::path& file, const std::filesystem::path& dir) -> bool {
    return same_path(file.parent_path(), dir);
}

// `a` is `b` or one of the folders above it.
auto ancestor_or_same(const std::filesystem::path& a, const std::filesystem::path& b) -> bool {
    for (auto p = b.lexically_normal(); ; p = p.parent_path()) {
        if (same_path(p, a))
            return true;
        if (!p.has_relative_path())
            return false;
    }
}

auto attributes(const std::filesystem::path& p) -> DWORD {
    return GetFileAttributesW(extended(p).c_str());
}

auto on_disk(const std::filesystem::path& p) -> bool {
    return attributes(p) != INVALID_FILE_ATTRIBUTES;
}

auto is_dir(const std::filesystem::path& p) -> bool {
    const DWORD a = attributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// True when the file is gone (or never was).
auto remove_file(const std::filesystem::path& p) -> bool {
    const auto e = extended(p);
    SetFileAttributesW(e.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(e.c_str()))
        return true;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

auto move_over(const std::filesystem::path& from, const std::filesystem::path& to) -> bool {
    return MoveFileExW(extended(from).c_str(), extended(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
}

// Deletes `dir` and what it holds; a junction or link inside (or `dir` itself, if one) is removed
// as the link, its target is never entered.
auto remove_tree(const std::filesystem::path& dir) -> bool {
    const DWORD a = attributes(dir);
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    const auto e = extended(dir);
    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW((e + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring_view name{fd.cFileName};
                if (name == L"." || name == L"..")
                    continue;
                const auto child = dir / fd.cFileName;
                const bool sub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (sub && !link)
                    remove_tree(child);
                else if (sub)
                    RemoveDirectoryW(extended(child).c_str());
                else
                    remove_file(child);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    return RemoveDirectoryW(e.c_str()) != FALSE || !on_disk(dir);
}

// Creates `dir` and the parents it lacks; each folder made is recorded (outermost first) and undone.
auto make_dirs(const std::filesystem::path& dir, std::vector<std::filesystem::path>& made, Undo& undo)
    -> core::Result<void> {
    std::vector<std::filesystem::path> missing;
    for (auto p = dir; !is_dir(p); p = p.parent_path()) {
        missing.push_back(p);
        if (!p.has_relative_path())
            break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!CreateDirectoryW(extended(*it).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return core::os_error(GetLastError(), std::format("can't create {}", narrow(it->native())));
        made.push_back(*it);
        undo.push([p = *it] { RemoveDirectoryW(extended(p).c_str()); });
    }
    return {};
}

// Writes `text` to `p`: under a temporary name, then renamed over it.
auto write_text(const std::filesystem::path& p, std::string_view text) -> core::Result<void> {
    auto temp = p;
    temp += L".wa-new";
    HANDLE f = CreateFileW(extended(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return core::os_error(GetLastError(), std::format("can't write {}", narrow(p.filename().native())));
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size() &&
                    FlushFileBuffers(f);
    const DWORD error = GetLastError();
    CloseHandle(f);
    if (!ok || !move_over(temp, p)) {
        remove_file(temp);
        return core::os_error(ok ? GetLastError() : error, std::format("can't write {}", narrow(p.filename().native())));
    }
    return {};
}

auto read_text(const std::filesystem::path& p) -> std::optional<std::string> {
    auto t = core::fs::read_file(extended(p), 16u << 20);
    return t ? std::optional{std::move(*t)} : std::nullopt;
}

auto json_string(std::string_view s) -> std::string {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            out += c;
    }
    return out + "\"";
}

auto read_journal(const std::filesystem::path& dir) -> std::optional<Journal> {
    const auto text = read_text(journal_path(dir));
    if (!text)
        return std::nullopt;
    auto j = journal_from(*text);
    if (!j || !same_path(j->root, dir))
        return std::nullopt;
    return std::move(*j);
}

void report(const Watch& watch, Step s) {
    if (watch.step)
        watch.step(s);
}

struct Plan {
    Choices choices;
    std::optional<Journal> old; // update: what the last install made
    bool update = false;
};

auto apply(System& sys, const Places& places, const Plan& plan, std::span<const std::byte> zip, const Watch& watch,
           const std::stop_token& stop) -> core::Result<Journal> {
    if (plan.choices.dir.native().size() > kMaxDirLength)
        return core::make_error(core::Code::InvalidArgument, "This folder's path is too long");
    if (!plan.update && holds_others(plan.choices.dir))
        return core::make_error(core::Code::AlreadyExists, "This folder isn't empty \xE2\x80\x94 choose another");
    if (sys.agent_running() && !sys.ask_agent_to_exit())
        return core::make_error(core::Code::AccessDenied, std::string{kAgentBusy});
    const auto entries = payload_entries(zip);
    if (!entries)
        return std::unexpected{entries.error()};

    const auto& c = plan.choices;
    const auto dir = c.dir;
    const auto exe = dir / L"agent.exe";
    Journal j;
    j.root = dir;
    j.version = std::string{core::version()};
    if (plan.old)
        j.dirs = plan.old->dirs;
    Undo undo;
    const auto cancelled = [&] { return core::make_error(core::Code::Cancelled, "cancelled"); };

    // The data kept before the rename (2026-11-25), the agent closed now: moved whole, and back where the
    // earlier agent looks for it on a failure (review C1); when it can't move, nothing goes on without it (I1).
    if (!places.user_dir.empty() && places.user_dir.filename() == core::product::kFolder) {
        const auto local = places.user_dir.parent_path();
        const auto was = local / core::product::kOldFolder;
        if (is_dir(was) && !on_disk(places.user_dir) && !on_disk(journal_path(was))) {
            if (!core::win::adopt_old_data(local))
                return core::make_error(core::Code::AccessDenied,
                                        std::format("Close what is open in {}, then try again", narrow(was.native())));
            undo.push([was, now = places.user_dir] { MoveFileExW(extended(now).c_str(), extended(was).c_str(), 0); });
        }
    }

    // Files: the folders, the old files moved aside, the package unpacked.
    report(watch, Step::Files);
    if (stop.stop_requested())
        return cancelled();
    if (auto r = make_dirs(dir, j.dirs, undo); !r)
        return std::unexpected{r.error()};
    // The agent's data: the user's own folder, not beside the program (Program Files is the administrators').
    const auto data = places.user_dir / L"data";
    if (auto r = make_dirs(data, j.dirs, undo); !r)
        return std::unexpected{r.error()};
    std::vector<std::filesystem::path> backups;
    for (const auto& e : *entries) {
        const auto target = dir / wide(e.name);
        if (!on_disk(target))
            continue;
        auto backup = target;
        backup += kBackup;
        remove_file(backup);
        if (!move_over(target, backup))
            return core::os_error(GetLastError(), std::format("{} is in use", e.name));
        backups.push_back(backup);
        undo.push([target, backup] { move_over(backup, target); });
    }
    std::vector<std::filesystem::path> written;
    const auto unpacked = extract(zip, dir, watch.progress, stop, written);
    for (const auto& w : written)
        undo.push([w] { remove_file(w); });
    if (!unpacked)
        return std::unexpected{unpacked.error()};
    j.files = written;

    // Shortcuts: made anew (an update refreshes the ones the install made).
    report(watch, Step::Shortcuts);
    if (stop.stop_requested())
        return cancelled();
    // Only in their two places and under their own name: an update keeps the desktop one when the
    // last install made it there and it is still there (a journal can't point a shortcut at any file).
    std::vector<std::filesystem::path> links{places.start_menu / core::agent_ids::kShortcut};
    const auto desktop = places.desktop / core::agent_ids::kShortcut;
    if (plan.old) {
        // Under its name from before the rename (2026-11-25) too: made again under the new one.
        const auto had = [&](const std::filesystem::path& d) {
            return on_disk(d) && std::ranges::any_of(plan.old->shortcuts, [&](const auto& l) { return same_path(l, d); });
        };
        if (had(desktop) || had(places.desktop / core::product::kOldShortcut))
            links.push_back(desktop);
    } else if (c.desktop) {
        links.push_back(desktop);
    }
    for (const auto& lnk : links) {
        if (on_disk(lnk)) {
            auto backup = lnk;
            backup += kBackup;
            if (!CopyFileW(lnk.c_str(), backup.c_str(), FALSE))
                return core::os_error(GetLastError(), "can't keep the old shortcut");
            backups.push_back(backup);
            undo.push([lnk, backup] { move_over(backup, lnk); });
        } else {
            undo.push([lnk] { remove_file(lnk); });
        }
        const Shortcut s{.lnk = lnk,
                         .target = exe,
                         .aumid = std::wstring{core::agent_ids::kAumid},
                         .activator = std::wstring{core::agent_ids::kActivatorClsid}};
        if (auto r = sys.make_shortcut(s); !r)
            return std::unexpected{r.error()};
        j.shortcuts.push_back(lnk);
    }

    // Settings: the hotkey (only into a new config), autostart, the Apps entry, the journal.
    report(watch, Step::Settings);
    if (stop.stop_requested())
        return cancelled();
    const auto config = data / L"config.json";
    if (!on_disk(config)) {
        if (auto r = write_text(config, "{\n  \"hotkey\": " + json_string(c.hotkey) + "\n}\n"); !r)
            return std::unexpected{r.error()};
        undo.push([config] { remove_file(config); });
    }
    if (plan.old) {
        j.values = plan.old->values;
    } else if (c.autostart) {
        const auto before = sys.get_value(kRunKey, kRunValue);
        if (auto r = sys.set_value(kRunKey, kRunValue, quoted(exe)); !r)
            return std::unexpected{r.error()};
        undo.push([&sys, before] {
            if (before)
                (void)sys.set_value(kRunKey, kRunValue, *before);
            else
                (void)sys.delete_value(kRunKey, kRunValue);
        });
        j.values.push_back({std::wstring{kRunKey}, std::wstring{kRunValue}});
    }

    std::uint64_t bytes = 0;
    for (const auto& e : *entries)
        bytes += e.size;
    const bool had_entry = sys.key_exists(kUninstallKey);
    // A kept entry's texts as they were: an update that moves the program changes where they point.
    std::vector<std::pair<std::wstring, std::wstring>> was;
    for (const std::wstring_view name :
         {L"DisplayName", L"DisplayVersion", L"Publisher", L"DisplayIcon", L"InstallLocation", L"UninstallString"})
        if (auto v = sys.get_value(kUninstallKey, name))
            was.emplace_back(name, std::move(*v));
    undo.push([&sys, had_entry, was] {
        if (!had_entry) {
            (void)sys.delete_key(kUninstallKey);
            return;
        }
        for (const auto& [name, value] : was)
            (void)sys.set_value(kUninstallKey, name, value);
    });
    const std::pair<std::wstring_view, RegData> entry[] = {
        {L"DisplayName", std::wstring{core::product::kNameW}},
        {L"DisplayVersion", wide(core::version())},
        {L"Publisher", std::wstring{core::product::kNameW}},
        {L"DisplayIcon", exe.native() + L",0"},
        {L"InstallLocation", dir.native()},
        {L"UninstallString", quoted(dir / L"uninstall.exe")},
        {L"EstimatedSize", static_cast<std::uint32_t>(bytes / 1024 + 1)},
        {L"NoModify", std::uint32_t{1}},
        {L"NoRepair", std::uint32_t{1}},
    };
    for (const auto& [name, value] : entry) {
        if (auto r = sys.set_value(kUninstallKey, name, value); !r)
            return std::unexpected{r.error()};
    }
    j.keys = {std::wstring{kUninstallKey}};

    const auto journal_file = journal_path(dir);
    const auto old_text = read_text(journal_file);
    if (auto r = write_text(journal_file, to_json(j)); !r)
        return std::unexpected{r.error()};
    undo.push([journal_file, old_text] {
        if (old_text)
            (void)write_text(journal_file, *old_text);
        else
            remove_file(journal_file);
    });

    // The last word: a Cancel confirmed while the settings were written still rolls it all back
    // (installer review M2: the step's one check at its start let the install finish).
    if (stop.stop_requested())
        return cancelled();
    undo.commit();
    for (const auto& b : backups)
        remove_file(b);
    if (plan.old) { // an update: files of the old program the new one no longer has
        for (const auto& f : plan.old->files) {
            const bool kept = std::ranges::any_of(j.files, [&](const auto& n) { return same_path(n, f); });
            if (!kept && inside(f, dir))
                remove_file(f);
        }
        for (const auto& l : plan.old->shortcuts) { // e.g. the name from before the rename, made anew
            const bool made = std::ranges::any_of(j.shortcuts, [&](const auto& n) { return same_path(n, l); });
            const bool placed = inside(l, places.start_menu) || inside(l, places.desktop);
            if (const auto target = made || !placed ? std::nullopt : sys.shortcut_target(l); target && same_path(*target, exe))
                remove_file(l);
        }
    }
    return j;
}

} // namespace

auto Places::for_user() -> Places {
    const auto local = core::win::known_folder(FOLDERID_LocalAppData);
    return {.start_menu = core::win::known_folder(FOLDERID_Programs).value_or(std::filesystem::path{}),
            .desktop = core::win::known_folder(FOLDERID_Desktop).value_or(std::filesystem::path{}),
            .user_dir = local ? *local / core::product::kFolder : std::filesystem::path{}};
}

auto installed(System& sys) -> std::optional<Installed> {
    const auto dir = sys.get_value(kUninstallKey, L"InstallLocation");
    const auto version = sys.get_value(kUninstallKey, L"DisplayVersion");
    if (!dir || !version || dir->empty())
        return std::nullopt;
    return Installed{std::filesystem::path{*dir}, narrow(*version)};
}

namespace {

// The earlier install for the current user only (%LOCALAPPDATA%, HKCU's Apps entry): what it made of
// the program goes once the new one is in; its data is the user's and stays — moved, when it lay elsewhere.
void remove_earlier(System& sys, const Places& places, const std::filesystem::path& old, bool user_entry) {
    const auto journal = read_journal(old);
    const auto old_exe = old / L"agent.exe";
    if (journal) {
        for (const auto& f : journal->files)
            if (inside(f, old) && !inside(f, old / L"data"))
                remove_file(f);
        for (const auto& l : journal->shortcuts) // a shortcut the new install did not take over: still the old program's
            if (const auto target = sys.shortcut_target(l); target && same_path(*target, old_exe))
                remove_file(l);
    }
    if (!journal) { // its files by the program's own names (as uninstall does), never anything else there
        for (const auto& n : payload_names())
            remove_file(old / wide(n));
    }
    remove_file(journal_path(old));
    remove_file(old / L"uninstall.exe");
    for (const auto& place : {places.start_menu, places.desktop})
        for (const auto name : {core::agent_ids::kShortcut, core::product::kOldShortcut})
            if (const auto target = sys.shortcut_target(place / name); target && same_path(*target, old_exe))
                remove_file(place / name);
    if (sys.get_value(kRunKey, kRunValue) == quoted(old_exe))
        (void)sys.delete_value(kRunKey, kRunValue); // the new install's autostart was not asked for
    (void)sys.delete_value(kGpuKey, old_exe.native());
    const std::wstring activator = L"Software\\Classes\\CLSID\\" + std::wstring{core::agent_ids::kActivatorClsid};
    if (sys.get_value(activator + L"\\LocalServer32", L"") == quoted(old_exe))
        (void)sys.delete_key(activator); // the agent writes its own again when it starts
    if (user_entry)
        (void)sys.delete_key(kUserUninstallKey);
    RemoveDirectoryW(extended(old).c_str()); // only when empty: its data, moved or not, is never removed here
}

// An install from before the rename (2026-11-25) in `from`: installed anew in `to` with the choices it
// has (the desktop shortcut, autostart), then its program removed by its journal; a failure leaves it as it was.
auto move_install(System& sys, const Places& places, const std::filesystem::path& from, const std::filesystem::path& to,
                  std::span<const std::byte> zip, const Watch& watch, const std::stop_token& stop) -> core::Result<Journal> {
    const auto old_exe = from / L"agent.exe";
    const auto points_at_old = [&](const std::filesystem::path& lnk) {
        const auto target = sys.shortcut_target(lnk);
        return target && same_path(*target, old_exe);
    };
    Choices c{.dir = to};
    c.autostart = sys.get_value(kRunKey, kRunValue) == quoted(old_exe);
    c.desktop = points_at_old(places.desktop / core::product::kOldShortcut) ||
                points_at_old(places.desktop / core::agent_ids::kShortcut);
    auto done = apply(sys, places, Plan{.choices = c}, zip, watch, stop);
    if (done)
        remove_earlier(sys, places, from, false);
    return done;
}

} // namespace

auto install(System& sys, const Places& places, const Choices& choices, std::span<const std::byte> zip,
             const Watch& watch, const std::stop_token& stop) -> core::Result<Journal> {
    // The earlier per-user install (installer-program-files.md § 4): its data where the new agent keeps
    // it first (back on a failure), its program removed once the new one is in.
    // The agent closes first: its data is moved below, while nothing of it may be open (review I2).
    if (sys.agent_running() && !sys.ask_agent_to_exit())
        return core::make_error(core::Code::AccessDenied, std::string{kAgentBusy});
    std::optional<std::filesystem::path> old;
    if (const auto at = sys.get_value(kUserUninstallKey, L"InstallLocation"); at && !at->empty() && !same_path(*at, choices.dir))
        old = std::filesystem::path{*at};
    const auto from = old ? *old / L"data" : std::filesystem::path{};
    const auto to = places.user_dir / L"data";
    bool moved = false;
    if (old && on_disk(from) && !same_path(from, to) && !on_disk(to)) {
        std::error_code ec;
        std::filesystem::create_directories(places.user_dir, ec);
        std::filesystem::rename(extended(from), extended(to), ec);
        if (ec) { // another drive: copied, then the old one removed
            std::filesystem::copy(extended(from), extended(to), std::filesystem::copy_options::recursive, ec);
            if (ec)
                return core::make_error(core::Code::Io, "the earlier install's data could not be moved: " + ec.message());
            remove_tree(from);
        }
        moved = true;
    }
    auto done = apply(sys, places, Plan{.choices = choices}, zip, watch, stop);
    if (!done) {
        if (moved) {
            std::error_code ec;
            std::filesystem::rename(extended(to), extended(from), ec); // where it was: the earlier agent still finds it
        }
        return done;
    }
    if (old)
        remove_earlier(sys, places, *old, true);
    return done;
}

auto update(System& sys, const Places& places, const std::filesystem::path& dir, std::span<const std::byte> zip,
            const Watch& watch, const std::stop_token& stop) -> core::Result<Journal> {
    if (const auto to = update_dir(dir); !same_path(to, dir))
        return move_install(sys, places, dir, to, zip, watch, stop);
    Plan plan{.choices = {.dir = dir}, .old = read_journal(dir), .update = true};
    if (!plan.old) { // no journal: what an install would have made, judged by what is there
        Journal guess;
        guess.root = dir;
        guess.shortcuts.push_back(places.start_menu / core::agent_ids::kShortcut);
        guess.shortcuts.push_back(places.start_menu / core::product::kOldShortcut);
        if (sys.get_value(kRunKey, kRunValue) == quoted(dir / L"agent.exe"))
            guess.values.push_back({std::wstring{kRunKey}, std::wstring{kRunValue}});
        plan.old = std::move(guess);
    }
    return apply(sys, places, plan, zip, watch, stop);
}

auto installed_here(System& sys, const std::filesystem::path& dir) -> bool {
    return read_journal(dir).has_value() ||
           same_path(std::filesystem::path{sys.get_value(kUninstallKey, L"InstallLocation").value_or(L"")}, dir);
}

auto uninstall(System& sys, const Places& places, const std::filesystem::path& dir, const Progress& progress)
    -> core::Result<void> {
    // Only an installed program: its journal says this folder, or the Apps entry does. uninstall.exe
    // beside a build of the agent (its data, its key) removes nothing and asks no agent to close.
    const auto journal = read_journal(dir);
    const bool listed = same_path(std::filesystem::path{sys.get_value(kUninstallKey, L"InstallLocation").value_or(L"")}, dir);
    if (!installed_here(sys, dir))
        return core::make_error(core::Code::NotFound, std::format("{} isn't installed in {}", core::product::kName, narrow(dir.native())));
    if (sys.agent_running() && !sys.ask_agent_to_exit())
        return core::make_error(core::Code::AccessDenied, std::string{kAgentBusy});
    const auto exe = dir / L"agent.exe";
    const auto uninstaller = dir / L"uninstall.exe";

    // Only the program's: files directly in its folder, shortcuts in the two places and pointing at
    // it, the Run value and the Apps entry when they are its own.
    std::vector<std::filesystem::path> files;
    if (journal) {
        for (const auto& f : journal->files)
            if (inside(f, dir))
                files.push_back(f);
    } else {
        for (const auto& n : payload_names())
            files.push_back(dir / wide(n));
    }
    std::vector<std::filesystem::path> links;
    const auto ours_in_place = [&](const std::filesystem::path& lnk) {
        return inside(lnk, places.start_menu) || inside(lnk, places.desktop);
    };
    if (journal) {
        for (const auto& l : journal->shortcuts)
            if (ours_in_place(l))
                links.push_back(l);
    }
    for (const auto& candidate : {places.start_menu / core::agent_ids::kShortcut, places.desktop / core::agent_ids::kShortcut,
                                  places.start_menu / core::product::kOldShortcut, places.desktop / core::product::kOldShortcut,
                                  places.start_menu / core::agent_ids::kBuildShortcut}) {
        const bool known = std::ranges::any_of(links, [&](const auto& l) { return same_path(l, candidate); });
        const auto target = sys.shortcut_target(candidate);
        if (!known && target && same_path(*target, exe))
            links.push_back(candidate);
    }

    const std::uint64_t total = files.size() + links.size() + 6;
    std::uint64_t done = 0;
    const auto tick = [&] {
        ++done;
        if (progress)
            progress(done, total);
    };
    std::vector<std::string> left;

    for (const auto& f : files) {
        // uninstall.exe goes last, with the journal: while anything stays, removing again must work.
        if (!same_path(f, uninstaller) && !remove_file(f))
            left.push_back(narrow(f.filename().native()));
        tick();
    }
    // data: the uninstalling user's own (installer-program-files.md: theirs, whoever installed it);
    // another user's stays — their folder is theirs. Only after the program was found installed here.
    const auto data = places.user_dir / L"data";
    const bool data_ours = !places.user_dir.empty();
    if (data_ours && !remove_tree(data))
        left.emplace_back("data");
    tick();
    for (const auto& l : links) {
        remove_file(l);
        tick();
    }

    const bool run_is_ours = journal ? std::ranges::any_of(journal->values, [](const RegValue& v) {
        return v.key == kRunKey && v.name == kRunValue;
    }) : sys.get_value(kRunKey, kRunValue) == quoted(exe);
    if (run_is_ours)
        (void)sys.delete_value(kRunKey, kRunValue);
    (void)sys.delete_value(kGpuKey, exe.native());
    tick();

    const std::wstring activator = L"Software\\Classes\\CLSID\\" + std::wstring{core::agent_ids::kActivatorClsid};
    if (sys.get_value(activator + L"\\LocalServer32", L"") == quoted(exe))
        (void)sys.delete_key(activator);
    if (!sys.key_exists(activator)) // no other copy of the agent registered: its notification settings go too
        (void)sys.delete_key(kNotificationsKey);
    tick();

    if (!left.empty()) { // uninstall.exe, the journal and the Apps entry stay: removing again finishes the job
        if (progress)
            progress(total, total);
        return core::make_error(core::Code::Io, std::format("Some files could not be removed: {}", left.front()));
    }
    if (!remove_file(uninstaller) || !remove_file(journal_path(dir))) {
        if (progress)
            progress(total, total);
        return core::make_error(core::Code::Io, "Some files could not be removed: uninstall.exe");
    }
    // Folders the install made: the program's folder, ones inside it, and the parents it created
    // for it — removed only when empty, deepest first.
    std::vector<std::filesystem::path> dirs;
    if (journal) {
        for (const auto& d : journal->dirs)
            if (ancestor_or_same(d, dir) || ancestor_or_same(dir, d))
                dirs.push_back(d);
    }
    std::ranges::sort(dirs, [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
    for (const auto& d : dirs)
        RemoveDirectoryW(extended(d).c_str());
    RemoveDirectoryW(extended(dir).c_str());
    if (data_ours) {
        RemoveDirectoryW(extended(places.user_dir).c_str()); // %LOCALAPPDATA%\Ruma, when nothing else is in it
        RemoveDirectoryW(extended(places.user_dir.parent_path() / core::product::kOldFolder).c_str()); // and an empty one from before the rename
    }
    tick();
    if (listed) // the Apps entry, when it is this folder's
        (void)sys.delete_key(kUninstallKey);
    if (progress)
        progress(total, total);
    return {};
}

} // namespace setup
