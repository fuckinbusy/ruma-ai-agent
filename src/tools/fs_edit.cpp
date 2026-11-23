#include "tools/fs_edit.hpp"

#include "core/hash.hpp"
#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/edit_text.hpp"
#include "tools/guarded_fs.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <format>
#include <mutex>
#include <fstream>
#include <optional>
#include <sstream>

namespace tools::fs::args {

struct Piece {
    std::string old_text;
    std::string new_text;
    std::optional<bool> all;
};
struct Revert {
    std::string path;
    std::string backup;
    std::string after;
};
struct Edit {
    std::string path;
    std::vector<Piece> edits;
};

} // namespace tools::fs::args

namespace tools::fs::out {

struct Edited {
    std::size_t edits = 0;
    std::uint64_t bytes = 0; // the file's size after
};

} // namespace tools::fs::out

template <>
struct glz::json_schema<tools::fs::args::Piece> {
    schema old_text{.description = "The exact text in the file to replace (with its spaces and line breaks, \\n)."};
    schema new_text{.description = "What it becomes."};
    schema all{.description = "Replace every occurrence. Default false: the old text must occur exactly once."};
};
template <>
struct glz::json_schema<tools::fs::args::Edit> {
    schema path{.description = "The text file to change."};
    schema edits{.description = "The changes, applied in order, each to the text the one before left. If any old "
                                "text is not found (or found twice without all), nothing is changed. Files up to 10 MB."};
};

namespace tools::fs {
namespace {

auto parse(std::string_view json) -> core::Result<args::Edit> {
    args::Edit value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    if (value.edits.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: no edits");
    }
    return value;
}

// The edits as edit::apply takes them; in a CRLF file the model's line breaks are \n.
auto pieces(const args::Edit& a, bool crlf) -> std::vector<edit::Edit> {
    const auto lf = [&](std::string s) {
        if (!crlf) {
            return s;
        }
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) {
                out += s[i];
            }
        }
        return out;
    };
    std::vector<edit::Edit> out;
    for (const auto& p : a.edits) {
        out.push_back({.old_text = lf(p.old_text), .new_text = lf(p.new_text), .all = p.all.value_or(false)});
    }
    return out;
}

struct Changed {
    std::string before; // the file's bytes
    std::string after;
    std::string before_text, after_text; // as text, for the diff
};

// The file's bytes edited, or why not.
auto change(std::string bytes, const args::Edit& a) -> core::Result<Changed> {
    auto text = edit::decode(bytes);
    if (!text) {
        return std::unexpected{text.error()};
    }
    auto edits = pieces(a, text->crlf);
    auto done = edit::apply(text->utf8, edits);
    if (!done) {
        return std::unexpected{done.error()};
    }
    Changed c{.before = std::move(bytes), .after = {}, .before_text = text->utf8, .after_text = *done};
    text->utf8 = std::move(*done);
    auto encoded = edit::encode(*text);
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    c.after = std::move(*encoded);
    return c;
}

auto too_big() -> core::Error {
    return core::Error{core::Code::InvalidArgument, "the file is over 10 MB: fs.edit changes smaller text files"};
}

} // namespace

auto edit_schema() -> std::string {
    auto s = glz::write_json_schema<args::Edit>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto edit_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: the path is empty or not UTF-8");
    }
    std::uint64_t bytes = 0;
    for (const auto& p : a->edits) {
        bytes += p.new_text.size();
    }
    return security::Request{.tool = "fs.edit", .risk = security::Risk::Write, .paths = {std::move(*path)}, .working_dir = wd,
                             .bytes = bytes};
}

auto edit_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse(json);
    if (!a) {
        return failure("fs.edit", a.error());
    }
    auto path = guarded_fs::target(permit, 0);
    if (!path) {
        return failure("fs.edit", path.error());
    }
    if (auto alone = guarded_fs::one_name_only(*path); !alone) {
        return failure("fs.edit", alone.error());
    }
    std::string bytes;
    {
        auto file = guarded_fs::open_to_change(permit, *path);
        if (!file) {
            return failure("fs.edit", file.error());
        }
        if (file->directory) {
            return failure("fs.edit", core::Error{core::Code::InvalidArgument, "it is a folder, not a file"});
        }
        auto read = guarded_fs::read(*file, kMaxEdited + 1);
        if (!read) {
            return failure("fs.edit", read.error());
        }
        if (read->size > kMaxEdited) {
            return failure("fs.edit", too_big());
        }
        bytes = std::move(read->data);
    } // closed before the write replaces it
    auto c = change(std::move(bytes), *a);
    if (!c) {
        return failure("fs.edit", c.error());
    }
    if (auto done = os::current().write_file(*path, c->after, os::WriteMode::Overwrite); !done) {
        return failure("fs.edit", done.error());
    }
    auto data = core::json::write(out::Edited{.edits = a->edits.size(), .bytes = c->after.size()});
    Outcome o{0, wrap("fs.edit", *path, data ? *data : std::string{"null"}, false)};
    o.effects.push_back({.kind = EffectKind::Edited, .to = *path, .undoable = true, .before = std::move(c->before)});
    return o;
}

namespace {

struct Read {
    std::string bytes;
    bool too_big = false; // over kMaxEdited: not read
};

// A file of at most kMaxEdited bytes, its size known first (final review E2): a larger one is never
// read, one that grew meanwhile is too big as well.
auto read_file(const std::filesystem::path& file) -> std::optional<Read> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    if (size > kMaxEdited) {
        return Read{.too_big = true};
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Read r{.bytes = std::string(static_cast<std::size_t>(size) + 1, '\0')};
    in.read(r.bytes.data(), static_cast<std::streamsize>(r.bytes.size()));
    if (static_cast<std::uint64_t>(in.gcount()) > kMaxEdited) {
        return Read{.too_big = true};
    }
    r.bytes.resize(static_cast<std::size_t>(in.gcount()));
    return r;
}

} // namespace

auto edit_preview(std::string_view json, const security::Request& request) -> std::string {
    const auto a = parse(json);
    if (!a || request.paths.empty()) {
        return {};
    }
    const auto canon = security::paths::canonical(request.paths[0], request.working_dir);
    if (!canon || !canon->exists) {
        return "the file does not exist: fs.edit changes an existing file";
    }
    auto read = read_file(std::filesystem::path{canon->long_form()});
    if (read && read->too_big) {
        return too_big().message;
    }
    if (!read) {
        return "the file cannot be read";
    }
    auto c = change(std::move(read->bytes), *a);
    if (!c) {
        return "the edit cannot apply: " + c.error().message;
    }
    return edit::unified_diff(c->before_text, c->after_text);
}

// fs.revert

namespace {

std::mutex undo_mutex;
// set_undo_root; none when unset (a path at rest would hold memory past the leak check, in Debug even empty).
std::optional<std::filesystem::path> undo_folder;

auto parse_revert(std::string_view json) -> core::Result<args::Revert> {
    args::Revert value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return value;
}

// As text for the card's diff; a file that is not text is said in bytes.
auto as_text(const std::string& bytes) -> std::string {
    auto t = edit::decode(bytes);
    return t ? t->utf8 : std::format("({} bytes, not text)\n", bytes.size());
}

} // namespace

void set_undo_root(std::filesystem::path root) {
    const std::scoped_lock lock{undo_mutex};
    if (root.empty()) {
        undo_folder.reset();
    } else {
        undo_folder = std::move(root);
    }
}

auto undo_root() -> std::filesystem::path {
    const std::scoped_lock lock{undo_mutex};
    return undo_folder.value_or(std::filesystem::path{});
}

namespace {

// The copy to revert from, only in the agent's undo folder: never a file the call names elsewhere
// (the run and the card alike, final review E1).
auto copy_in_undo(const std::string& backup) -> core::Result<security::paths::Canonical> {
    const auto root = undo_root();
    const auto wide = core::utf8::to_wide(backup);
    const auto copy = wide ? security::paths::canonical(*wide, root) : std::unexpected{wide.error()};
    const auto folder = root.empty() ? std::unexpected{core::Error{core::Code::Policy, "no undo folder"}}
                                     : security::paths::canonical(root.wstring(), root);
    if (!copy || !folder || !copy->exists || !security::paths::is_within(copy->path, folder->path) || copy->path == folder->path) {
        return core::make_error(core::Code::Policy, "the copy to revert from is not in the agent's undo folder");
    }
    return *copy;
}

} // namespace

auto content_hash(std::string_view bytes) -> std::string {
    return std::format("{:016x}", core::hash::fnv1a64(bytes));
}

auto revert_schema() -> std::string {
    return R"({"type":"object","properties":{"path":{"type":"string"},"backup":{"type":"string"},"after":{"type":"string"}},)"
           R"("required":["path","backup","after"],"additionalProperties":false})";
}

auto revert_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse_revert(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: the path is empty or not UTF-8");
    }
    security::Request r{.tool = "fs.revert", .risk = security::Risk::Write, .paths = {*path}, .working_dir = wd};
    // Changed since the edit (by hand, by another program): the revert takes that away — asked.
    if (const auto canon = security::paths::canonical(*path, wd); canon && canon->exists) {
        const auto now = read_file(std::filesystem::path{canon->long_form()});
        if (!now || now->too_big || content_hash(now->bytes) != a->after) {
            r.must_ask = "the file changed since the edit: reverting it takes those changes away";
        }
    }
    return r;
}

auto revert_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse_revert(json);
    if (!a) {
        return failure("fs.revert", a.error());
    }
    const auto copy = copy_in_undo(a->backup);
    if (!copy) {
        return failure("fs.revert", copy.error());
    }
    const auto kept = read_file(std::filesystem::path{copy->long_form()});
    if (!kept || kept->too_big) {
        return failure("fs.revert", core::Error{core::Code::NotFound, "the copy of the file was not found"});
    }
    const std::string* bytes = &kept->bytes;
    auto path = guarded_fs::target(permit, 0);
    if (!path) {
        return failure("fs.revert", path.error());
    }
    std::string now;
    if (auto file = guarded_fs::open_to_change(permit, *path)) {
        if (auto read = guarded_fs::read(*file, kMaxEdited + 1)) {
            now = std::move(read->data);
        }
    }
    if (auto done = os::current().write_file(*path, *bytes, os::WriteMode::Overwrite); !done) {
        return failure("fs.revert", done.error());
    }
    auto data = core::json::write(out::Edited{.edits = 0, .bytes = bytes->size()});
    Outcome o{0, wrap("fs.revert", *path, data ? *data : std::string{"null"}, false)};
    o.effects.push_back({.kind = EffectKind::Edited, .to = *path, .undoable = false, .before = std::move(now)});
    return o;
}

auto revert_preview(std::string_view json, const security::Request& request) -> std::string {
    const auto a = parse_revert(json);
    if (!a || request.paths.empty()) {
        return {};
    }
    const auto copy = copy_in_undo(a->backup);
    if (!copy) {
        return copy.error().message;
    }
    const auto then = read_file(std::filesystem::path{copy->long_form()});
    if (!then || then->too_big) {
        return "the copy of the file was not found";
    }
    const auto canon = security::paths::canonical(request.paths[0], request.working_dir);
    const auto now = canon && canon->exists ? read_file(std::filesystem::path{canon->long_form()}) : std::nullopt;
    if (now && now->too_big) {
        return "puts back what the file held before the edit; it is now over 10 MB, so the difference is not shown";
    }
    return "puts back what the file held before the edit:\n" + edit::unified_diff(now ? as_text(now->bytes) : std::string{}, as_text(then->bytes));
}

} // namespace tools::fs
