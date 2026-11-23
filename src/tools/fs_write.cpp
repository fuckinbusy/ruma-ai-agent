#include "tools/fs_write.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/edit_text.hpp"
#include "tools/guarded_fs.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <deque>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

namespace tools::fs::args {

struct Write {
    std::string path;
    std::string content;
    std::optional<std::string> mode; // create_only (default), overwrite, append
};
struct Mkdir {
    std::string path;
};
struct Pair {
    std::string from;
    std::string to;
};
struct Move {
    std::vector<Pair> moves;
    std::optional<bool> replace;
};
struct Copy {
    std::vector<Pair> copies;
    std::optional<bool> replace;
};

} // namespace tools::fs::args

namespace tools::fs::out {

struct Written {
    std::uint64_t bytes = 0;
    std::string mode;
};
struct Made {
    bool created = true;
};
struct Moved {
    std::string from;
    std::string to;
};
struct Moves {
    std::vector<Moved> moved;
};
struct Copies {
    std::vector<Moved> copied;
    std::size_t files = 0;
};

} // namespace tools::fs::out

template <>
struct glz::json_schema<tools::fs::args::Write> {
    schema path{.description = "The file to write."};
    schema content{.description = "The text to write, UTF-8."};
    schema mode{.description = "create_only (default: fails when the file exists), overwrite or append.",
                .enumeration = std::vector<std::string_view>{"create_only", "overwrite", "append"}};
};
template <>
struct glz::json_schema<tools::fs::args::Mkdir> {
    schema path{.description = "The folder to create, with its missing parents."};
};
template <>
struct glz::json_schema<tools::fs::args::Move> {
    schema moves{.description = "Pairs {from, to}: files or folders to move or rename."};
    schema replace{.description = "Replace what is already at `to`. Default false."};
};
template <>
struct glz::json_schema<tools::fs::args::Copy> {
    schema copies{.description = "Pairs {from, to}: files or whole folders to copy."};
    schema replace{.description = "Replace files already at `to`. Default false."};
};

namespace tools::fs {
namespace {

using security::Risk;

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

template <class T>
auto parse(std::string_view json) -> core::Result<T> {
    T value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return value;
}

template <class T>
auto schema_of() -> std::string {
    auto s = glz::write_json_schema<T>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto wide(std::string_view s) -> core::Result<std::wstring> {
    auto w = core::utf8::to_wide(s);
    if (!w || w->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: a path is empty or not UTF-8");
    }
    return w;
}

auto mode_of(const std::optional<std::string>& m) -> core::Result<os::WriteMode> {
    if (!m || *m == "create_only") {
        return os::WriteMode::CreateOnly;
    }
    if (*m == "overwrite") {
        return os::WriteMode::Overwrite;
    }
    if (*m == "append") {
        return os::WriteMode::Append;
    }
    return core::make_error(core::Code::InvalidArgument, "arguments: \"mode\" is create_only, overwrite or append");
}

template <class T>
auto envelope(std::string_view tool, std::wstring_view path, const T& data) -> Outcome {
    auto json = core::json::write(data);
    return {0, wrap(tool, path, json ? *json : std::string{"null"}, false)};
}

// Objects inside a folder (the folder itself not counted), up to `limit` + 1; and their bytes.
struct Tree {
    std::uint64_t objects = 0;
    std::uint64_t bytes = 0;
};
auto measure(const std::wstring& root, std::uint64_t limit) -> Tree {
    Tree t;
    std::deque<std::wstring> todo{root};
    while (!todo.empty() && t.objects <= limit) {
        const std::wstring dir = std::move(todo.front());
        todo.pop_front();
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(security::paths::long_form(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            const std::wstring_view name{fd.cFileName};
            if (name == L"." || name == L"..") {
                continue;
            }
            ++t.objects;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    todo.push_back(dir + L"\\" + fd.cFileName);
                }
            } else {
                t.bytes += ULARGE_INTEGER{.LowPart = fd.nFileSizeLow, .HighPart = fd.nFileSizeHigh}.QuadPart;
            }
        } while (t.objects <= limit && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return t;
}

// A path of the request, canonicalized for counting only (the policy canonicalizes on its own).
auto resolved(const std::wstring& path, const std::filesystem::path& wd) -> std::optional<security::paths::Canonical> {
    auto c = security::paths::canonical(path, wd);
    return c ? std::optional{std::move(*c)} : std::nullopt;
}

} // namespace

// fs.write

auto write_schema() -> std::string {
    return schema_of<args::Write>();
}

auto write_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Write>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (auto m = mode_of(a->mode); !m) {
        return std::unexpected{m.error()};
    }
    auto path = wide(a->path);
    if (!path) {
        return std::unexpected{path.error()};
    }
    return security::Request{.tool = "fs.write", .risk = Risk::Write, .paths = {std::move(*path)}, .working_dir = wd,
                             .bytes = a->content.size()};
}

auto write_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Write>(json);
    if (!a) {
        return failure("fs.write", a.error());
    }
    const auto mode = mode_of(a->mode);
    auto path = guarded_fs::target(permit, 0);
    if (!path) {
        return failure("fs.write", path.error());
    }
    if (auto alone = guarded_fs::one_name_only(*path); !alone) {
        return failure("fs.write", alone.error());
    }
    const bool existed = GetFileAttributesW(security::paths::long_form(*path).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (auto done = os::current().write_file(*path, a->content, *mode); !done) {
        return failure("fs.write", done.error());
    }
    auto o = envelope("fs.write", *path, out::Written{a->content.size(), a->mode.value_or("create_only")});
    // A new file goes back to the Recycle Bin; what was in an old one is gone (no copy is kept).
    o.effects.push_back({.kind = existed ? EffectKind::Overwritten : EffectKind::Created, .to = *path, .undoable = !existed});
    return o;
}

auto write_preview(std::string_view json, const security::Request& request) -> std::string {
    const auto a = parse<args::Write>(json);
    if (!a || request.paths.empty()) {
        return {};
    }
    const auto c = resolved(request.paths[0], request.working_dir);
    std::string before;
    if (c && c->exists) {
        std::ifstream in(std::filesystem::path{c->long_form()}, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        before = s.str().substr(0, 1 << 20);
    }
    const auto mode = a->mode.value_or("create_only");
    if (mode == "append") {
        return std::format("adds {} bytes at the end:\n", a->content.size()) + edit::unified_diff("", a->content);
    }
    if (!c || !c->exists) {
        return std::format("a new file, {} bytes:\n", a->content.size()) + edit::unified_diff("", a->content);
    }
    // The old file as text (its encoding and line breaks read), the same diff as fs.edit's card.
    const auto old = edit::decode(before);
    if (!old) {
        return std::format("replaces {} bytes ({}) with {}\n", before.size(), old.error().message, a->content.size());
    }
    return std::format("replaces {} bytes with {}:\n", before.size(), a->content.size()) + edit::unified_diff(old->utf8, a->content);
}

// fs.mkdir

auto mkdir_schema() -> std::string {
    return schema_of<args::Mkdir>();
}

auto mkdir_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Mkdir>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    auto path = wide(a->path);
    if (!path) {
        return std::unexpected{path.error()};
    }
    return security::Request{.tool = "fs.mkdir", .risk = Risk::Write, .paths = {std::move(*path)}, .working_dir = wd};
}

auto mkdir_run(const security::Permit& permit, std::string_view json) -> Outcome {
    if (auto a = parse<args::Mkdir>(json); !a) {
        return failure("fs.mkdir", a.error());
    }
    auto path = guarded_fs::target(permit, 0);
    if (!path) {
        return failure("fs.mkdir", path.error());
    }
    const bool existed = GetFileAttributesW(security::paths::long_form(*path).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (auto done = os::current().make_dir(*path); !done) {
        return failure("fs.mkdir", done.error());
    }
    auto o = envelope("fs.mkdir", *path, out::Made{.created = !existed});
    if (!existed) {
        o.effects.push_back({.kind = EffectKind::Created, .to = *path, .undoable = true});
    }
    return o;
}

// fs.move and fs.copy: the request's paths are from, to, from, to, …

namespace {

template <class Args>
auto pairs_request(std::string_view tool, Risk risk, const std::vector<args::Pair>& pairs, const std::filesystem::path& wd,
                   std::uint64_t max_tree) -> core::Result<security::Request> {
    if (pairs.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: nothing to do");
    }
    security::Request r{.tool = std::string{tool}, .risk = risk, .working_dir = wd};
    for (const auto& p : pairs) {
        auto from = wide(p.from);
        auto to = wide(p.to);
        if (!from || !to) {
            return std::unexpected{(from ? to : from).error()};
        }
        if (const auto c = resolved(*from, wd); c && c->exists) {
            const auto t = measure(c->path, max_tree);
            r.tree_objects += t.objects;
            r.bytes += t.bytes;
            if (t.objects == 0) {
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (GetFileAttributesExW(security::paths::long_form(c->path).c_str(), GetFileExInfoStandard, &fa) &&
                    (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    r.bytes += ULARGE_INTEGER{.LowPart = fa.nFileSizeLow, .HighPart = fa.nFileSizeHigh}.QuadPart;
                }
            }
        }
        r.paths.push_back(std::move(*from));
        r.paths.push_back(std::move(*to));
    }
    if (risk == Risk::RenameMove) {
        r.bytes = 0; // a move writes nothing new
    }
    return r;
}

auto pairs_preview(std::string_view verb, const security::Request& request) -> std::string {
    std::string out;
    for (std::size_t i = 0; i + 1 < request.paths.size() && i < 80; i += 2) {
        const auto from = resolved(request.paths[i], request.working_dir);
        const auto to = resolved(request.paths[i + 1], request.working_dir);
        out += std::format("{} {} -> {}\n", verb, utf8(from ? from->path : request.paths[i]),
                           utf8(to ? to->path : request.paths[i + 1]));
    }
    if (request.paths.size() > 80) {
        out += std::format("… and {} more\n", request.paths.size() / 2 - 40);
    }
    if (request.tree_objects > 0) {
        out += std::format("{} objects inside the folders\n", request.tree_objects);
    }
    return out;
}

} // namespace

auto move_schema() -> std::string {
    return schema_of<args::Move>();
}

auto move_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Move>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    return pairs_request<args::Move>("fs.move", Risk::RenameMove, a->moves, wd, security::kCountObjectsUpTo);
}

// The sources of the pairs after pair `i` (paths i+2, i+4, …): what a stopped batch never started.
auto sources_after(const security::Permit& permit, std::size_t i) -> std::vector<std::wstring> {
    std::vector<std::wstring> out;
    for (std::size_t k = i + 2; k + 1 < permit.paths().size(); k += 2) {
        out.push_back(permit.paths()[k].logical);
    }
    return out;
}

template <class T>
auto json_of(const T& value) -> std::string {
    auto j = core::json::write(value);
    return j ? std::move(*j) : std::string{"[]"};
}

auto move_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Move>(json);
    if (!a) {
        return failure("fs.move", a.error());
    }
    out::Moves data;
    std::vector<Effect> effects;
    const std::size_t total = permit.paths().size() / 2;
    for (std::size_t i = 0; i + 1 < permit.paths().size(); i += 2) {
        // The moves before it stay done: the result says which, and which never started.
        const auto stop = [&](const core::Error& e) {
            auto o = partial("fs.move", e, json_of(data.moved), permit.paths()[i].logical, sources_after(permit, i));
            o.effects = std::move(effects);
            return o;
        };
        if (!proceed()) {
            return stop(stopped_error());
        }
        auto from = guarded_fs::target(permit, i);
        auto to = from ? guarded_fs::target(permit, i + 1) : from;
        if (!from || !to) {
            return stop((from ? to : from).error());
        }
        if (auto done = os::current().move(*from, *to, a->replace.value_or(false)); !done) {
            return stop(done.error());
        }
        data.moved.push_back({utf8(*from), utf8(*to)});
        const auto parent = [](const std::wstring& p) { return std::wstring_view{p}.substr(0, p.find_last_of(L'\\')); };
        effects.push_back({.kind = parent(*from) == parent(*to) ? EffectKind::Renamed : EffectKind::Moved,
                           .from = *from,
                           .to = *to,
                           .undoable = true});
        report(i / 2 + 1, total, *to);
    }
    auto o = envelope("fs.move", L"", data);
    o.effects = std::move(effects);
    return o;
}

auto move_narrow(std::string_view json, std::span<const std::size_t> keep) -> core::Result<std::string> {
    auto a = parse<args::Move>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    args::Move kept{.moves = {}, .replace = a->replace};
    for (std::size_t i = 0; i < a->moves.size(); ++i) {
        if (std::ranges::find(keep, i) != keep.end()) {
            kept.moves.push_back(a->moves[i]);
        }
    }
    for (const std::size_t k : keep) {
        if (k >= a->moves.size()) {
            return core::make_error(core::Code::InvalidArgument, std::format("there is no move {}", k));
        }
    }
    if (kept.moves.empty()) {
        return core::make_error(core::Code::InvalidArgument, "nothing left to do");
    }
    auto out = core::json::write(kept);
    if (!out) {
        return core::make_error(core::Code::Internal, "the narrowed arguments could not be written");
    }
    return std::move(*out);
}

auto move_preview(std::string_view, const security::Request& request) -> std::string {
    return pairs_preview("move", request);
}

auto copy_schema() -> std::string {
    return schema_of<args::Copy>();
}

auto copy_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Copy>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    return pairs_request<args::Copy>("fs.copy", Risk::Write, a->copies, wd, security::kCountObjectsUpTo);
}

auto copy_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Copy>(json);
    if (!a) {
        return failure("fs.copy", a.error());
    }
    const bool replace = a->replace.value_or(false);
    out::Copies data;
    std::vector<Effect> effects;
    const std::size_t total = permit.paths().size() / 2;
    for (std::size_t i = 0; i + 1 < permit.paths().size(); i += 2) {
        // The copies before it stay done (a folder copied part way stays part way): said exactly.
        const auto stop = [&](const core::Error& e) {
            auto o = partial("fs.copy", e, json_of(data.copied), permit.paths()[i].logical, sources_after(permit, i));
            o.effects = std::move(effects);
            return o;
        };
        if (!proceed()) {
            return stop(stopped_error());
        }
        auto from = guarded_fs::target(permit, i);
        auto to = from ? guarded_fs::target(permit, i + 1) : from;
        if (!from || !to) {
            return stop((from ? to : from).error());
        }
        // A new copy goes to the Recycle Bin on undo; one over an existing file cannot be undone.
        // The change is recorded once something is there: a copy that never happened is none.
        const bool existed = GetFileAttributesW(security::paths::long_form(*to).c_str()) != INVALID_FILE_ATTRIBUTES;
        const Effect made{.kind = existed ? EffectKind::Overwritten : EffectKind::Created, .to = *to, .undoable = !existed};
        const DWORD attrs = GetFileAttributesW(security::paths::long_form(*from).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return stop(core::Error{core::Code::NotFound, utf8(*from) + " does not exist"});
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (auto done = os::current().copy_file(*from, *to, replace); !done) {
                return stop(done.error());
            }
            effects.push_back(made);
            ++data.files;
        } else {
            // A folder: the whole tree is looked at first — every source checked against what
            // was allowed as the source (a secret deep inside stops the copy before anything is
            // created), every destination against the destination. Only then is it copied.
            std::vector<std::wstring> folders;
            std::vector<std::pair<std::wstring, std::wstring>> files;
            std::deque<std::pair<std::wstring, std::wstring>> todo{{*from, *to}};
            while (!todo.empty()) {
                auto [src, dst] = std::move(todo.front());
                todo.pop_front();
                auto source = guarded_fs::inside(permit, i, src);
                auto target = source ? guarded_fs::inside(permit, i + 1, dst) : source;
                if (!source || !target) {
                    return stop((source ? target : source).error());
                }
                folders.push_back(*target);
                WIN32_FIND_DATAW fd{};
                HANDLE h = FindFirstFileExW(security::paths::long_form(src + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                            nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) {
                    continue;
                }
                do {
                    const std::wstring_view name{fd.cFileName};
                    if (name == L"." || name == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                        continue; // links are not followed into
                    }
                    const std::wstring s = src + L"\\" + fd.cFileName;
                    const std::wstring d = dst + L"\\" + fd.cFileName;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        todo.emplace_back(s, d);
                        continue;
                    }
                    auto one = guarded_fs::inside(permit, i, s);
                    auto into = one ? guarded_fs::inside(permit, i + 1, d) : one;
                    if (!one || !into) {
                        FindClose(h);
                        return stop((one ? into : one).error());
                    }
                    files.emplace_back(*one, *into);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            for (std::size_t k = 0; k < folders.size(); ++k) {
                if (auto dir = os::current().make_dir(folders[k]); !dir) {
                    return stop(dir.error());
                }
                if (k == 0) {
                    effects.push_back(made); // the copy's top folder: what undo takes, with what is in it
                }
            }
            for (const auto& [s, d] : files) {
                if (auto done = os::current().copy_file(s, d, replace); !done) {
                    return stop(done.error());
                }
                ++data.files;
            }
        }
        data.copied.push_back({utf8(*from), utf8(*to)});
        report(i / 2 + 1, total, *to);
    }
    auto o = envelope("fs.copy", L"", data);
    o.effects = std::move(effects);
    return o;
}

auto copy_narrow(std::string_view json, std::span<const std::size_t> keep) -> core::Result<std::string> {
    auto a = parse<args::Copy>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    args::Copy kept{.copies = {}, .replace = a->replace};
    for (const std::size_t k : keep) {
        if (k >= a->copies.size()) {
            return core::make_error(core::Code::InvalidArgument, std::format("there is no copy {}", k));
        }
    }
    for (std::size_t i = 0; i < a->copies.size(); ++i) {
        if (std::ranges::find(keep, i) != keep.end()) {
            kept.copies.push_back(a->copies[i]);
        }
    }
    auto out = core::json::write(kept);
    if (!out) {
        return core::make_error(core::Code::Internal, "the narrowed arguments could not be written");
    }
    return std::move(*out);
}

auto copy_preview(std::string_view, const security::Request& request) -> std::string {
    return pairs_preview("copy", request);
}

} // namespace tools::fs
