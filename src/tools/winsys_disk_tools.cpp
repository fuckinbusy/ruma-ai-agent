#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "security/paths.hpp"
#include "tools/winsys/disk.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <memory>
#include <optional>

// disk.usage (phase 13a § 4): what takes the space under a folder, counted with its progress on the
// step and a stop. The model gets the folders by size two levels down and the largest files; the
// panel gets the whole tree. Names and sizes only; below a folder of secrets or of the agent's own,
// nothing is named.

namespace tools::winsys::args {

struct DiskUsage {
    std::optional<std::string> path; // the system drive if null
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct SubFolder {
    std::string name;
    std::uint64_t bytes = 0, files = 0;
    bool locked = false, link = false;
};
struct Folder {
    std::string name;
    std::uint64_t bytes = 0, files = 0;
    bool locked = false, link = false;
    bool inside_not_named = false; // secrets or the agent's own: its size only
    std::vector<SubFolder> folders;
};
struct File {
    std::string name;
    std::uint64_t bytes = 0;
};
struct Usage {
    std::string path;
    std::uint64_t bytes = 0, files = 0;
    bool complete = true;
    std::optional<std::uint64_t> disk_total, disk_free;
    std::vector<Folder> folders;
    std::vector<File> largest_files;
    std::uint64_t other_files = 0, other_bytes = 0;
    std::string note;
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

constexpr std::size_t kTopFolders = 15;
constexpr std::size_t kSubFolders = 8;
constexpr std::size_t kTopFiles = 10;

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

auto system_drive() -> std::wstring {
    wchar_t drive[8]{};
    const DWORD n = GetEnvironmentVariableW(L"SystemDrive", drive, 8);
    return (n > 0 && n < 8 ? std::wstring{drive} : std::wstring{L"C:"}) + L"\\";
}

auto join(const std::wstring& a, const std::wstring& b) -> std::wstring {
    return a + (a.ends_with(L'\\') ? L"" : L"\\") + b;
}

// Secrets or the agent's own data: counted, never named inside.
auto sealed(const std::wstring& path, const security::paths::Roots& roots) -> bool {
    const auto c = security::paths::canonical(path, std::filesystem::path{path});
    if (!c) {
        return false;
    }
    const auto z = security::paths::zone_of(*c, roots);
    return z == security::paths::Zone::Secrets || z == security::paths::Zone::AppSelf;
}

auto usage_of(const DiskScan& s, const security::paths::Roots& roots) -> out::Usage {
    out::Usage u{.path = utf8(s.path), .bytes = s.root.bytes, .files = s.root.files, .complete = s.complete, .disk_total = std::nullopt,
                 .disk_free = std::nullopt, .folders = {}, .largest_files = {}, .other_files = s.root.other_files,
                 .other_bytes = s.root.other_bytes, .note = {}};
    if (s.disk_total > 0) {
        u.disk_total = s.disk_total;
        u.disk_free = s.disk_free;
    }
    for (std::size_t i = 0; i < std::min(kTopFolders, s.root.children.size()); ++i) {
        const auto& c = s.root.children[i];
        out::Folder f{.name = utf8(c.name), .bytes = c.bytes, .files = c.files, .locked = c.locked, .link = c.link,
                      .inside_not_named = sealed(join(s.path, c.name), roots), .folders = {}};
        if (!f.inside_not_named) {
            for (std::size_t j = 0; j < std::min(kSubFolders, c.children.size()); ++j) {
                const auto& g = c.children[j];
                f.folders.push_back({utf8(g.name), g.bytes, g.files, g.locked, g.link});
            }
        }
        u.folders.push_back(std::move(f));
    }
    for (std::size_t i = 0; i < std::min(kTopFiles, s.root.largest.size()); ++i) {
        u.largest_files.push_back({utf8(s.root.largest[i].name), s.root.largest[i].bytes});
    }
    if (!s.complete) {
        u.note = "Stopped part way: the sizes are what was counted until then.";
    } else if (s.root.children.size() > kTopFolders) {
        u.note = "The largest folders only; the user sees all of them in the panel.";
    }
    return u;
}

} // namespace

auto disk_for_model(const DiskScan& s, const security::paths::Roots& roots) -> std::string {
    auto json = core::json::write(usage_of(s, roots));
    return json ? std::move(*json) : std::string{R"("?")"};
}

auto disk_schema() -> std::string {
    auto s = glz::write_json_schema<args::DiskUsage>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto disk_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    args::DiskUsage a{};
    if (!json.empty() && json != "null") {
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
            return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
        }
    }
    std::wstring path = a.path ? core::utf8::to_wide(*a.path).value_or(std::wstring{}) : system_drive();
    if (path.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" names a folder, or null for the system drive");
    }
    return security::Request{.tool = "disk.usage", .risk = security::Risk::Read, .paths = {std::move(path)}, .working_dir = wd};
}

auto disk_run(const security::Permit& permit, std::string_view) -> Outcome {
    constexpr std::string_view tool = "disk.usage";
    if (permit.paths().empty()) {
        return failure(tool, core::Error{core::Code::InvalidArgument, "no folder"});
    }
    const std::wstring path = permit.paths().front().path;
    const DWORD attrs = GetFileAttributesW(security::paths::long_form(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return failure(tool, core::Error{core::Code::NotFound, "not a folder: " + utf8(path)});
    }
    auto s = std::make_shared<DiskScan>();
    s->path = path;
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(path.c_str(), &avail, &total, &free)) {
        s->disk_total = total.QuadPart;
        s->disk_free = free.QuadPart;
    }
    // The step's "182 GB of ~350 GB": the drive's used space, when the whole drive is counted.
    const bool whole = path.size() <= 3;
    const std::uint64_t estimate = whole && s->disk_total > s->disk_free ? s->disk_total - s->disk_free : 0;
    // Hard links (WinSxS) count once per name: the sum may pass the drive's used space; the bar stops full.
    // Each report carries the tree so far (the root's folders as counted): the panel grows while it counts.
    auto result = scan(path, control().stop, [&](const ScanProgress& p) {
        const auto& c = control();
        if (!c.progress) {
            return;
        }
        auto part = std::make_shared<DiskScan>();
        part->path = path;
        part->disk_total = s->disk_total;
        part->disk_free = s->disk_free;
        part->complete = false;
        part->counting = true;
        part->root.name = path;
        // Their sizes only, not what is below them: a whole drive's subtrees are not copied each time.
        for (const auto& t : p.tops) {
            part->root.children.push_back({.name = t.name, .bytes = t.bytes, .files = t.files, .locked = t.locked, .link = t.link});
        }
        std::ranges::sort(part->root.children, std::greater{}, &DirNode::bytes);
        part->root.bytes = p.bytes;
        part->root.files = p.files;
        c.progress(ProgressInfo{.done = std::min(p.bytes, estimate), .total = estimate, .bytes = p.bytes, .current = p.current,
                                .disk = std::move(part)});
    }, context().progress_every);
    s->root = std::move(result.root);
    s->complete = result.complete;
    Outcome o{0, wrap(tool, path, disk_for_model(*s, security::paths::system_roots()), false)};
    o.disk = std::move(s);
    return o;
}

} // namespace tools::winsys
