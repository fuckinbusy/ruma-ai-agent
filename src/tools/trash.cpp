#include "tools/trash.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/guarded_fs.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <deque>
#include <format>

namespace tools::trash::args {

struct Paths {
    std::vector<std::string> paths;
    std::optional<bool> skip_failed; // go on past what cannot be done (a file in use); listed as skipped
};

} // namespace tools::trash::args

namespace tools::trash::out {

struct Skipped {
    std::string path;
    std::string reason;
};
struct Trashed {
    std::vector<std::string> trashed; // where each was: fs.restore takes these
    std::vector<Skipped> skipped;
};
struct Restored {
    std::vector<std::string> restored;
    std::vector<Skipped> skipped;
};

} // namespace tools::trash::out

template <>
struct glz::json_schema<tools::trash::args::Paths> {
    schema paths{.description = "Files or folders."};
    schema skip_failed{.description = "Go on past a file that cannot be removed (in use); it is listed as skipped."};
};

namespace tools::trash {
namespace {

using security::Risk;

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto parse(std::string_view json) -> core::Result<args::Paths> {
    args::Paths value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    if (value.paths.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: no paths");
    }
    return value;
}

// Objects inside a folder, up to `limit` + 1 (for the blast-radius limit).
auto count_inside(const std::wstring& root, std::uint64_t limit) -> std::uint64_t {
    std::uint64_t n = 0;
    std::deque<std::wstring> todo{root};
    while (!todo.empty() && n <= limit) {
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
            ++n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                todo.push_back(dir + L"\\" + fd.cFileName);
            }
        } while (n <= limit && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

auto request_for(std::string_view tool, Risk risk, std::string_view json, const std::filesystem::path& wd)
    -> core::Result<security::Request> {
    auto a = parse(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    security::Request r{.tool = std::string{tool}, .risk = risk, .working_dir = wd};
    for (const auto& p : a->paths) {
        auto w = core::utf8::to_wide(p);
        if (!w || w->empty()) {
            return core::make_error(core::Code::InvalidArgument, "arguments: a path is empty or not UTF-8");
        }
        if (risk == Risk::Delete) {
            if (auto c = security::paths::canonical(*w, wd); c && c->exists) {
                r.tree_objects += count_inside(c->path, security::kCountObjectsUpTo);
            }
        }
        r.paths.push_back(std::move(*w));
    }
    return r;
}

// A batch stopped at path `i`: what is done stays done, the result says which and which never started.
auto stopped(std::string_view tool, const core::Error& e, const std::vector<std::string>& done,
             const security::Permit& permit, std::size_t i) -> Outcome {
    std::vector<std::wstring> rest;
    for (std::size_t k = i + 1; k < permit.paths().size(); ++k) {
        rest.push_back(permit.paths()[k].logical);
    }
    auto done_json = core::json::write(done);
    return partial(tool, e, done_json ? *done_json : std::string{"[]"}, permit.paths()[i].logical, rest);
}

// One object at a time: the stop and the pause ahead of each, progress after each, and what was
// done as effects — also when the batch stops part way. With `skip`, what the system refuses (a
// file in use) is listed in data.skipped and the batch goes on; a path that fails its guard
// still stops it.
template <class Data, class Op>
auto batch(std::string_view tool, const security::Permit& permit, Data& data, std::vector<std::string>& done,
           EffectKind kind, bool undoable, bool skip, Op op) -> Outcome {
    std::vector<Effect> effects;
    const std::size_t total = permit.paths().size();
    for (std::size_t i = 0; i < total; ++i) {
        const auto stop = [&](const core::Error& e) {
            auto o = stopped(tool, e, done, permit, i);
            o.effects = std::move(effects);
            return o;
        };
        if (!proceed()) {
            return stop(stopped_error());
        }
        auto path = guarded_fs::target(permit, i);
        if (!path) {
            return stop(path.error());
        }
        if (auto r = op(*path); !r) {
            if (!skip) {
                return stop(r.error());
            }
            data.skipped.push_back({utf8(*path), r.error().message});
            report(i + 1, total, *path);
            continue;
        }
        done.push_back(utf8(*path));
        effects.push_back({.kind = kind, .to = *path, .undoable = undoable});
        report(i + 1, total, *path);
    }
    auto json_out = core::json::write(data);
    Outcome o{0, wrap(tool, L"", json_out ? *json_out : std::string{"null"}, false)};
    o.effects = std::move(effects);
    return o;
}

} // namespace

auto narrow(std::string_view json, std::span<const std::size_t> keep) -> core::Result<std::string> {
    auto a = parse(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    args::Paths kept{.paths = {}, .skip_failed = a->skip_failed};
    for (const std::size_t k : keep) {
        if (k >= a->paths.size()) {
            return core::make_error(core::Code::InvalidArgument, std::format("there is no path {}", k));
        }
    }
    for (std::size_t i = 0; i < a->paths.size(); ++i) {
        if (std::ranges::find(keep, i) != keep.end()) {
            kept.paths.push_back(a->paths[i]);
        }
    }
    auto out = core::json::write(kept);
    if (!out) {
        return core::make_error(core::Code::Internal, "the narrowed arguments could not be written");
    }
    return std::move(*out);
}

auto schema() -> std::string {
    auto s = glz::write_json_schema<args::Paths>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto trash_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    return request_for("fs.trash", Risk::Delete, json, wd);
}

auto trash_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse(json);
    if (!a) {
        return failure("fs.trash", a.error());
    }
    out::Trashed data;
    return batch("fs.trash", permit, data, data.trashed, EffectKind::Trashed, true, a->skip_failed.value_or(false),
                 [](const std::wstring& p) { return os::current().trash(p); });
}

auto trash_preview(std::string_view, const security::Request& request) -> std::string {
    std::string out = "to the Recycle Bin:\n";
    for (std::size_t i = 0; i < request.paths.size() && i < 60; ++i) {
        auto c = security::paths::canonical(request.paths[i], request.working_dir);
        out += utf8(c ? c->path : request.paths[i]) + "\n";
    }
    if (request.paths.size() > 60) {
        out += std::format("… and {} more\n", request.paths.size() - 60);
    }
    if (request.tree_objects > 0) {
        out += std::format("{} objects inside the folders\n", request.tree_objects);
    }
    return out;
}

auto restore_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    return request_for("fs.restore", Risk::Write, json, wd);
}

auto restore_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse(json);
    if (!a) {
        return failure("fs.restore", a.error());
    }
    out::Restored data;
    return batch("fs.restore", permit, data, data.restored, EffectKind::Restored, true, a->skip_failed.value_or(false),
                 [](const std::wstring& p) { return os::current().restore(p); });
}

auto delete_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto r = request_for("fs.delete_permanent", Risk::Delete, json, wd);
    if (r) {
        r->tool = "fs.delete_permanent";
        r->permanent = true;
    }
    return r;
}

auto delete_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse(json);
    if (!a) {
        return failure("fs.delete_permanent", a.error());
    }
    if (!permit.request().permanent) {
        return failure("fs.delete_permanent", core::Error{core::Code::Policy, "a permanent delete needs its own permission"});
    }
    out::Trashed data; // "trashed" here: gone for good
    return batch("fs.delete_permanent", permit, data, data.trashed, EffectKind::Deleted, false, a->skip_failed.value_or(false),
                 [](const std::wstring& p) { return os::current().remove(p); });
}

auto delete_preview(std::string_view json, const security::Request& request) -> std::string {
    auto text = trash_preview(json, request);
    text.replace(0, text.find('\n'), "DELETED FOR GOOD, not to the Recycle Bin:");
    return text;
}

} // namespace tools::trash
