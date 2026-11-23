#include "tools/fs.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/glob.hpp"
#include "tools/guarded_fs.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <optional>
#include <regex>

// Argument and result types: named namespaces (glaze reflects only types with external linkage).
namespace tools::fs::args {

struct List {
    std::string path;
    std::optional<bool> recursive;
    std::optional<int> max_entries;
};
struct Read {
    std::string path;
    std::optional<int> from_line;
    std::optional<int> to_line;
};
struct Stat {
    std::string path;
};
struct Search {
    std::string path;
    std::optional<std::string> name;
    std::optional<std::string> text;
    std::optional<bool> regex;
    std::optional<bool> case_sensitive;
    std::optional<int> context;
    std::optional<std::vector<std::string>> types;
    std::optional<std::string> output;
    std::optional<int> max_results;
};

} // namespace tools::fs::args

namespace tools::fs::out {

struct Entry {
    std::string name; // relative to the listed folder
    std::string type; // file, dir, junction, symlink, link
    std::uint64_t size = 0;
    std::string modified;
};
struct List {
    std::vector<Entry> entries;
    std::size_t listed = 0; // entries found, including those cut from `entries`
};
struct Text {
    std::string text;
    std::string encoding;
    std::uint64_t size = 0;
    std::size_t total_lines = 0;
    std::size_t from_line = 0;
    std::size_t to_line = 0;
};
struct Binary {
    bool binary = true;
    std::uint64_t size = 0;
    std::string modified;
};
struct Stat {
    std::string type;
    std::uint64_t size = 0;
    std::string created, modified, accessed;
    std::vector<std::string> attributes;
    std::optional<std::string> target; // a link: where it leads
};
struct Match {
    std::string path; // relative to the searched folder
    std::optional<std::size_t> line;
    std::optional<std::string> text;
    std::optional<std::vector<std::string>> before, after; // the lines around it (context)
    std::optional<std::size_t> count;                       // output "count": the file's matching lines
};
struct Search {
    std::vector<Match> matches;
    std::size_t files_seen = 0;
    std::size_t skipped_large = 0;          // files over 20 MB, not read
    std::optional<std::size_t> stopped_at;  // the search took 20 s: stopped at this file
};

} // namespace tools::fs::out

template <>
struct glz::json_schema<tools::fs::args::List> {
    schema path{.description = "The folder to list; relative paths are inside the task's folder."};
    schema recursive{.description = "Also list subfolders (links are shown, not entered). Default false."};
    schema max_entries{.description = "At most this many entries (1-5000, default 500)."};
};
template <>
struct glz::json_schema<tools::fs::args::Read> {
    schema path{.description = "The file to read (text: UTF-8, UTF-16 or Windows-1251; at most 1 MB)."};
    schema from_line{.description = "First line to return, 1-based."};
    schema to_line{.description = "Last line to return, inclusive."};
};
template <>
struct glz::json_schema<tools::fs::args::Stat> {
    schema path{.description = "The file or folder."};
};
template <>
struct glz::json_schema<tools::fs::args::Search> {
    schema path{.description = "The folder to search in, with its subfolders (.git, node_modules and what .gitignore "
                                "files leave out are skipped)."};
    schema name{.description = "A file name pattern, like *.pdf or report*.docx; with folders, ** for any of them: "
                                "src/**/*.cpp."};
    schema text{.description = "Text the files contain (text files up to 20 MB), found on every line it is on."};
    schema regex{.description = "text is a regular expression (ECMAScript). Default false."};
    schema case_sensitive{.description = "Match the text's case. Default false."};
    schema context{.description = "Lines before and after each line found (0-5, default 0)."};
    schema types{.description = "Only files with these extensions, like [\"cpp\", \"md\"]."};
    schema output{.description = "\"lines\" (each line found: path, line, text; the default with text), \"files\" "
                                  "(only the paths), \"count\" (how many lines in each file)."};
    schema max_results{.description = "At most this many results (1-200, default 50)."};
};

namespace tools::fs {
namespace {

constexpr std::size_t kMaxRead = 1 << 20;
constexpr std::size_t kMaxSearchFiles = 20'000;

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

template <class T>
auto read_request_for(std::string_view tool, std::string_view json, const std::filesystem::path& wd)
    -> core::Result<security::Request> {
    auto a = parse<T>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" is empty or not UTF-8");
    }
    return security::Request{.tool = std::string{tool}, .risk = security::Risk::Read, .paths = {std::move(*path)},
                             .working_dir = wd};
}

auto target_of(const security::Permit& permit) -> std::wstring {
    return permit.paths().empty() ? std::wstring{} : permit.paths()[0].path;
}

auto link_type(std::uint32_t tag) -> std::string {
    if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
        return "junction";
    }
    if (tag == IO_REPARSE_TAG_SYMLINK) {
        return "symlink";
    }
    return "link";
}

// Serializes `data` into the envelope; while it is over kMaxOutput, `shrink` cuts the data
// (returns false when nothing is left to cut).
template <class T, class Shrink>
auto bounded(std::string_view tool, std::wstring_view path, T& data, bool truncated, Shrink shrink) -> Outcome {
    for (;;) {
        auto json = core::json::write(data);
        if (!json) {
            return {2, wrap(tool, path, R"("the result could not be written")", true)};
        }
        auto out = wrap(tool, path, *json, truncated);
        if (out.size() <= kMaxOutput) {
            return {0, std::move(out)};
        }
        truncated = true;
        if (!shrink(data, static_cast<double>(kMaxOutput) / static_cast<double>(out.size()))) {
            return {2, wrap(tool, path, R"("the result does not fit")", true)};
        }
    }
}

// Text from bytes: BOM-marked UTF-8 / UTF-16, plain UTF-8, else Windows-1251; nullopt for binary.
struct Decoded {
    std::string text;
    std::string encoding;
};
auto decode(std::string_view b, bool cut) -> std::optional<Decoded> {
    const auto from_utf16 = [](std::string_view bytes, bool big_endian) -> std::optional<std::string> {
        std::wstring w(bytes.size() / 2, L'\0');
        for (std::size_t i = 0; i < w.size(); ++i) {
            const auto lo = static_cast<unsigned char>(bytes[2 * i + (big_endian ? 1 : 0)]);
            const auto hi = static_cast<unsigned char>(bytes[2 * i + (big_endian ? 0 : 1)]);
            w[i] = static_cast<wchar_t>(lo | (hi << 8));
        }
        auto s = core::utf8::to_utf8(w);
        return s ? std::optional{std::move(*s)} : std::nullopt;
    };
    if (b.starts_with("\xEF\xBB\xBF")) {
        b.remove_prefix(3);
    } else if (b.starts_with("\xFF\xFE") || b.starts_with("\xFE\xFF")) {
        const bool be = b[0] == '\xFE';
        auto s = from_utf16(b.substr(2), be);
        if (!s) {
            return std::nullopt;
        }
        return Decoded{std::move(*s), be ? "utf-16be" : "utf-16le"};
    } else if (b.substr(0, 8192).find('\0') != std::string_view::npos) {
        return std::nullopt; // binary (UTF-16 without a mark included: rare, and unsafe to guess)
    }
    if (cut) { // read up to 1 MB: drop a sequence cut at the end
        std::size_t keep = b.size();
        for (int back = 0; back < 3 && keep > 0 && (static_cast<unsigned char>(b[keep - 1]) & 0xC0) == 0x80; ++back) {
            --keep;
        }
        if (keep > 0 && (static_cast<unsigned char>(b[keep - 1]) & 0xC0) == 0xC0) {
            --keep;
        }
        if (!core::utf8::is_valid(b) && core::utf8::is_valid(b.substr(0, keep))) {
            b = b.substr(0, keep);
        }
    }
    if (core::utf8::is_valid(b)) {
        return Decoded{std::string{b}, "utf-8"};
    }
    const int n = MultiByteToWideChar(1251, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(1251, 0, b.data(), static_cast<int>(b.size()), w.data(), n);
    auto s = core::utf8::to_utf8(w);
    if (!s) {
        return std::nullopt;
    }
    return Decoded{std::move(*s), "windows-1251"};
}

// Cuts `s` to about `fraction` of its size, on a UTF-8 boundary.
void cut_text(std::string& s, double fraction) {
    std::size_t n = static_cast<std::size_t>(static_cast<double>(s.size()) * fraction * 0.95);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    s.resize(n);
}

auto lower(std::wstring s) -> std::wstring {
    if (!s.empty()) {
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
    return s;
}

struct Walked {
    std::wstring path;     // final
    std::wstring relative; // to the root
    guarded_fs::Entry entry;
};

// Every entry under `root`, breadth first, through checked opens; links are reported, not
// entered, nor a folder `enter` turns down. Stops when `visit` returns false.
template <class Visit, class Enter>
void walk(const security::Permit& permit, const guarded_fs::Opened& root, bool recursive, Visit visit, Enter enter) {
    std::deque<std::pair<std::wstring, std::wstring>> todo; // a subfolder's path and its relative prefix
    // One folder's entries; false when `visit` stopped the walk.
    const auto folder = [&](const guarded_fs::Opened& dir, const std::wstring& prefix) {
        auto entries = guarded_fs::list(dir);
        if (!entries) {
            return true;
        }
        std::ranges::sort(*entries, [](const auto& a, const auto& b) { return a.name < b.name; });
        for (auto& e : *entries) {
            Walked w{dir.path + L"\\" + e.name, prefix + e.name, std::move(e)};
            if (!visit(w)) {
                return false;
            }
            if (recursive && w.entry.directory && w.entry.reparse_tag == 0 && enter(w)) {
                todo.emplace_back(w.path, w.relative + L"\\");
            }
        }
        return true;
    };
    if (!folder(root, L"")) {
        return;
    }
    while (!todo.empty()) {
        auto [path, prefix] = std::move(todo.front());
        todo.pop_front();
        auto dir = guarded_fs::open_read(permit, path); // checked again: it may have changed
        if (dir && !folder(*dir, prefix)) {
            return;
        }
    }
}

template <class Visit>
void walk(const security::Permit& permit, const guarded_fs::Opened& root, bool recursive, Visit visit) {
    walk(permit, root, recursive, visit, [](const Walked&) { return true; });
}

} // namespace

// fs.list

auto list_schema() -> std::string {
    return schema_of<args::List>();
}

auto list_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    return read_request_for<args::List>("fs.list", json, wd);
}

auto list_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::List>(json);
    if (!a) {
        return failure("fs.list", a.error());
    }
    auto root = guarded_fs::open_read(permit, target_of(permit));
    if (!root) {
        return failure("fs.list", root.error());
    }
    if (!root->directory) {
        return failure("fs.list", core::Error{core::Code::InvalidArgument, utf8(root->path) + " is a file; use fs.read"});
    }
    const auto max = static_cast<std::size_t>(std::clamp(a->max_entries.value_or(500), 1, 5000));
    out::List data;
    bool truncated = false;
    walk(permit, *root, a->recursive.value_or(false), [&](const Walked& w) {
        ++data.listed;
        if (data.entries.size() >= max) {
            truncated = true;
            return false;
        }
        data.entries.push_back({utf8(w.relative),
                                w.entry.reparse_tag != 0 ? link_type(w.entry.reparse_tag)
                                : w.entry.directory      ? "dir"
                                                         : "file",
                                w.entry.directory ? 0 : w.entry.size, guarded_fs::iso_time(w.entry.modified)});
        return true;
    });
    return bounded("fs.list", root->path, data, truncated, [](out::List& d, double fraction) {
        if (d.entries.empty()) {
            return false;
        }
        d.entries.resize(static_cast<std::size_t>(static_cast<double>(d.entries.size()) * fraction * 0.95));
        return true;
    });
}

// fs.read

auto read_schema() -> std::string {
    return schema_of<args::Read>();
}

auto read_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    return read_request_for<args::Read>("fs.read", json, wd);
}

auto text_of(std::string_view bytes) -> std::optional<std::string> {
    auto d = decode(bytes, false);
    return d ? std::optional<std::string>{std::move(d->text)} : std::nullopt;
}

auto read_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Read>(json);
    if (!a) {
        return failure("fs.read", a.error());
    }
    auto file = guarded_fs::open_read(permit, target_of(permit));
    if (!file) {
        return failure("fs.read", file.error());
    }
    if (file->directory) {
        return failure("fs.read", core::Error{core::Code::InvalidArgument, utf8(file->path) + " is a folder; use fs.list"});
    }
    auto bytes = guarded_fs::read(*file, kMaxRead);
    if (!bytes) {
        return failure("fs.read", bytes.error());
    }
    const bool cut = bytes->size > bytes->data.size();
    auto decoded = decode(bytes->data, cut);
    if (!decoded) {
        auto meta = guarded_fs::info(*file);
        out::Binary data{.size = bytes->size, .modified = meta ? guarded_fs::iso_time(meta->modified) : std::string{}};
        return bounded("fs.read", file->path, data, false, [](out::Binary&, double) { return false; });
    }
    // Lines: a final line without a newline counts; the newline after the last one opens none.
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < decoded->text.size(); ++i) {
        if (decoded->text[i] == '\n' && i + 1 < decoded->text.size()) {
            starts.push_back(i + 1);
        }
    }
    const std::size_t total = decoded->text.empty() ? 0 : starts.size();
    const std::size_t from = static_cast<std::size_t>(std::max(1, a->from_line.value_or(1)));
    const std::size_t to = std::min(total, static_cast<std::size_t>(std::max(1, a->to_line.value_or(static_cast<int>(total)))));
    out::Text data{.encoding = decoded->encoding, .size = bytes->size, .total_lines = total, .from_line = from, .to_line = to};
    if (from <= to && from <= total) {
        const std::size_t begin = starts[from - 1];
        const std::size_t end = to < total ? starts[to] : decoded->text.size();
        data.text = decoded->text.substr(begin, end - begin);
    }
    return bounded("fs.read", file->path, data, cut, [](out::Text& d, double fraction) {
        if (d.text.empty()) {
            return false;
        }
        cut_text(d.text, fraction);
        return true;
    });
}

// fs.stat

auto stat_schema() -> std::string {
    return schema_of<args::Stat>();
}

auto stat_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    return read_request_for<args::Stat>("fs.stat", json, wd);
}

auto stat_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Stat>(json);
    if (!a) {
        return failure("fs.stat", a.error());
    }
    auto object = guarded_fs::open_read(permit, target_of(permit));
    if (!object) {
        return failure("fs.stat", object.error());
    }
    auto meta = guarded_fs::info(*object);
    if (!meta) {
        return failure("fs.stat", meta.error());
    }
    out::Stat data{.type = object->directory ? "dir" : "file",
                   .size = meta->size,
                   .created = guarded_fs::iso_time(meta->created),
                   .modified = guarded_fs::iso_time(meta->modified),
                   .accessed = guarded_fs::iso_time(meta->accessed)};
    // The name may be a link: asked about the link itself (metadata only, nothing opened).
    const std::wstring& named = permit.paths()[0].logical.empty() ? object->path : permit.paths()[0].logical;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(security::paths::long_form(named).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        FindClose(find);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            data.type = link_type(fd.dwReserved0);
            data.target = utf8(object->path);
        }
    }
    const std::pair<DWORD, const char*> names[] = {
        {FILE_ATTRIBUTE_READONLY, "readonly"}, {FILE_ATTRIBUTE_HIDDEN, "hidden"},   {FILE_ATTRIBUTE_SYSTEM, "system"},
        {FILE_ATTRIBUTE_ARCHIVE, "archive"},   {FILE_ATTRIBUTE_COMPRESSED, "compressed"},
        {FILE_ATTRIBUTE_ENCRYPTED, "encrypted"}, {FILE_ATTRIBUTE_OFFLINE, "offline"},
        {FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, "cloud-only"}};
    for (const auto& [bit, name] : names) {
        if ((meta->attributes & bit) != 0) {
            data.attributes.emplace_back(name);
        }
    }
    return bounded("fs.stat", object->path, data, false, [](out::Stat&, double) { return false; });
}

// fs.search

auto search_schema() -> std::string {
    return schema_of<args::Search>();
}

auto search_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Search>(json);
    if (a && !a->name && !a->text && !a->types) {
        return core::make_error(core::Code::InvalidArgument, "arguments: give \"name\", \"text\" or \"types\"");
    }
    return read_request_for<args::Search>("fs.search", json, wd);
}

namespace {

constexpr std::uint64_t kMaxSearched = 20ull << 20;          // bigger files are skipped, counted
constexpr auto kSearchTime = std::chrono::seconds{20};       // then the search stops, saying where
constexpr std::size_t kShownChars = 200;                      // of a line found

// A line as shown: no line break or trailing spaces, at most kShownChars characters.
auto shown(std::string_view line) -> std::string {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    std::size_t chars = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80 && chars++ == kShownChars) {
            return std::string{line.substr(0, i)};
        }
    }
    return std::string{line};
}

auto lines_of(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = text.find('\n', at);
        auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        out.push_back(line);
        at = end == std::string_view::npos ? text.size() : end + 1;
    }
    return out;
}

// What a line is matched with: a substring (any case, or its own) or a regular expression.
class Finder {
public:
    static auto make(const args::Search& a) -> core::Result<Finder> {
        Finder f;
        f.case_sensitive_ = a.case_sensitive.value_or(false);
        f.needle_ = a.text.value_or("");
        auto w = core::utf8::to_wide(f.needle_);
        if (!w) {
            return core::make_error(core::Code::InvalidArgument, "arguments: \"text\" is not UTF-8");
        }
        f.wide_ = f.case_sensitive_ ? std::move(*w) : lower(std::move(*w));
        if (a.regex.value_or(false)) {
            try {
                auto flags = std::regex_constants::ECMAScript;
                if (!f.case_sensitive_) {
                    flags |= std::regex_constants::icase;
                }
                f.regex_.emplace(f.wide_, flags);
            } catch (const std::regex_error& e) {
                return core::make_error(core::Code::InvalidArgument, std::format("the regular expression is not valid: {}", e.what()));
            }
        }
        return f;
    }

    auto found(std::string_view line) const -> core::Result<bool> {
        if (!regex_ && case_sensitive_) {
            return line.find(needle_) != std::string_view::npos;
        }
        auto w = core::utf8::to_wide(line);
        if (!w) {
            return false;
        }
        if (!regex_) {
            return lower(std::move(*w)).find(wide_) != std::wstring::npos;
        }
        try {
            return std::regex_search(*w, *regex_);
        } catch (const std::regex_error& e) {
            return core::make_error(core::Code::InvalidArgument, std::format("the regular expression is too complex for a line: {}", e.what()));
        }
    }

private:
    bool case_sensitive_ = false;
    std::string needle_;
    std::wstring wide_;
    std::optional<std::wregex> regex_;
};

// The name pattern (alternatives with ';'): a file's name, or with a folder in it its path.
auto name_fits(const std::optional<std::string>& pattern, std::string_view name, std::string_view relative) -> bool {
    if (!pattern || pattern->empty()) {
        return true;
    }
    std::string_view rest = *pattern;
    while (!rest.empty()) {
        const auto cut = rest.find(';');
        const auto one = rest.substr(0, cut);
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
        const bool path = one.find_first_of("/\\") != std::string_view::npos;
        if (!one.empty() && glob::glob_match(one, path ? relative : name)) {
            return true;
        }
    }
    return false;
}

auto type_fits(const std::optional<std::vector<std::string>>& types, std::wstring_view name) -> bool {
    if (!types) {
        return true;
    }
    const auto dot = name.rfind(L'.');
    const auto ext = lower(std::wstring{dot == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot + 1)});
    return std::ranges::any_of(*types, [&](const std::string& t) {
        auto w = core::utf8::to_wide(std::string_view{t}.substr(t.starts_with('.') ? 1 : 0));
        return w && !w->empty() && lower(std::move(*w)) == ext;
    });
}

} // namespace

auto search_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Search>(json);
    if (!a) {
        return failure("fs.search", a.error());
    }
    auto root = guarded_fs::open_read(permit, target_of(permit));
    if (!root) {
        return failure("fs.search", root.error());
    }
    if (!root->directory) {
        return failure("fs.search", core::Error{core::Code::InvalidArgument, utf8(root->path) + " is not a folder"});
    }
    const std::string output = a->output.value_or(a->text ? "lines" : "files");
    if (output != "lines" && output != "files" && output != "count") {
        return failure("fs.search", core::Error{core::Code::InvalidArgument, "arguments: \"output\" is lines, files or count"});
    }
    const auto finder = Finder::make(*a);
    if (!finder) {
        return failure("fs.search", finder.error());
    }
    const bool by_text = a->text.has_value() && !a->text->empty();
    const auto context = static_cast<std::size_t>(std::clamp(a->context.value_or(0), 0, 5));
    const auto max = static_cast<std::size_t>(std::clamp(a->max_results.value_or(50), 1, 200));
    const auto start = now();

    // .gitignore files, each read once, through the permit, when the walk first comes into its folder.
    glob::Ignore ignore;
    std::vector<std::wstring> read_rules;
    const auto rules_of = [&](std::wstring_view relative) {
        const auto cut = relative.rfind(L'\\');
        const std::wstring folder{cut == std::wstring_view::npos ? std::wstring_view{} : relative.substr(0, cut)};
        if (std::ranges::contains(read_rules, folder)) {
            return;
        }
        read_rules.push_back(folder);
        auto file = guarded_fs::open_read(permit, root->path + (folder.empty() ? L"" : L"\\" + folder) + L"\\.gitignore");
        if (!file || file->directory) {
            return;
        }
        auto bytes = guarded_fs::read(*file, kMaxRead);
        if (auto text = bytes ? decode(bytes->data, false) : std::nullopt) {
            ignore.add(utf8(folder), text->text);
        }
    };
    const auto left_out = [&](const Walked& w) {
        rules_of(w.relative);
        return ignore.ignored(utf8(w.relative), w.entry.directory);
    };

    out::Search data;
    bool truncated = false;
    std::optional<core::Error> failed;
    const auto full = [&] {
        if (data.matches.size() >= max) {
            truncated = true;
            return true;
        }
        return false;
    };
    walk(
        permit, *root, true,
        [&](const Walked& w) {
            if (w.entry.directory || w.entry.reparse_tag != 0 || left_out(w)) {
                return true;
            }
            if (now() - start >= kSearchTime) {
                data.stopped_at = data.files_seen;
                truncated = true;
                return false;
            }
            if (++data.files_seen > kMaxSearchFiles) {
                truncated = true;
                return false;
            }
            const auto path = utf8(w.relative);
            if (!name_fits(a->name, utf8(w.entry.name), path) || !type_fits(a->types, w.entry.name)) {
                return true;
            }
            if (!by_text) {
                data.matches.push_back({.path = path});
                return !full();
            }
            if (w.entry.size > kMaxSearched) {
                ++data.skipped_large;
                return true;
            }
            auto file = guarded_fs::open_read(permit, w.path); // a secret file in the folder: refused here
            if (!file) {
                return true;
            }
            auto bytes = guarded_fs::read(*file, kMaxSearched);
            auto decoded = bytes ? decode(bytes->data, false) : std::nullopt;
            if (!decoded) {
                return true; // binary
            }
            const auto lines = lines_of(decoded->text);
            std::size_t count = 0;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto hit = finder->found(lines[i]);
                if (!hit) {
                    failed = hit.error();
                    return false;
                }
                if (!*hit) {
                    continue;
                }
                ++count;
                if (output == "files") {
                    break;
                }
                if (output == "lines") {
                    out::Match m{.path = path, .line = i + 1, .text = shown(lines[i])};
                    if (context > 0 && i > 0) {
                        m.before.emplace();
                        for (std::size_t k = i - std::min(i, context); k < i; ++k) {
                            m.before->push_back(shown(lines[k]));
                        }
                    }
                    if (context > 0 && i + 1 < lines.size()) {
                        m.after.emplace();
                        for (std::size_t k = i + 1; k < std::min(lines.size(), i + 1 + context); ++k) {
                            m.after->push_back(shown(lines[k]));
                        }
                    }
                    data.matches.push_back(std::move(m));
                    if (full()) {
                        return false;
                    }
                }
            }
            if (count > 0 && output == "files") {
                data.matches.push_back({.path = path});
            } else if (count > 0 && output == "count") {
                data.matches.push_back({.path = path, .count = count});
            }
            return !full();
        },
        [&](const Walked& w) { return !left_out(w); });
    if (failed) {
        return failure("fs.search", *failed);
    }
    return bounded("fs.search", root->path, data, truncated, [](out::Search& d, double fraction) {
        if (d.matches.empty()) {
            return false;
        }
        d.matches.resize(static_cast<std::size_t>(static_cast<double>(d.matches.size()) * fraction * 0.95));
        return true;
    });
}

} // namespace tools::fs
