#include "app/space.hpp"

#include "app/sizes.hpp"
#include "core/utf8.hpp"
#include "security/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace app::space {
namespace {

using tools::winsys::DirNode;
using tools::winsys::DiskScan;
using ui::work::Cell;
using ui::work::ColumnKind;

constexpr std::size_t kMaxFolders = 300;    // rows of folders; the rest in one line
constexpr std::size_t kMaxLargest = 200;    // the largest files of a whole count

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

// "41 233"
auto grouped(std::uint64_t n) -> std::string {
    std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            out += ' ';
        }
        out += digits[i];
    }
    return out;
}

auto share(std::uint64_t part, std::uint64_t whole) -> Cell {
    if (whole == 0) {
        return {"\xE2\x80\x94", 0};
    }
    const double pct = static_cast<double>(part) * 100.0 / static_cast<double>(whole);
    if (pct < 1.0) {
        return {part > 0 ? "<1%" : "0%", pct};
    }
    return {std::format("{:.0f}%", pct), pct};
}

auto join(const std::wstring& a, std::wstring_view b) -> std::wstring {
    return a + (a.ends_with(L'\\') ? L"" : L"\\") + std::wstring{b};
}

auto drive_root(const std::wstring& p) -> bool {
    return p.size() >= 2 && p.size() <= 3 && p[1] == L':';
}

// A file's type for its icon: its extension in lower case ("mp4"), or "file".
auto type_of(std::wstring_view name) -> std::string {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot + 1 == name.size() || name.size() - dot > 6) {
        return "file";
    }
    std::wstring ext{name.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return utf8(ext);
}

auto same_path(const std::wstring& a, const std::wstring& b) -> bool {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The valid part of `path`.
auto trimmed(const DirNode& root, std::span<const std::uint32_t> path) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out;
    const DirNode* n = &root;
    for (const auto i : path) {
        if (i >= n->children.size()) {
            break;
        }
        out.push_back(i);
        n = &n->children[i];
    }
    return out;
}

struct Builder {
    events::DataSet& s;
    void row(std::string icon, std::string name, Cell size, Cell part, Cell items, Cell action, std::string note, std::string badge) {
        s.rows.cells.push_back({std::move(icon), 0});
        s.rows.cells.push_back({std::move(name), 0});
        s.rows.cells.push_back(std::move(size));
        s.rows.cells.push_back(std::move(part));
        s.rows.cells.push_back(std::move(items));
        s.rows.cells.push_back(std::move(action));
        s.rows.notes.push_back(std::move(note));
        s.rows.badges.push_back(std::move(badge));
    }
};

void folders(events::DataSet& s, const DiskScan& scan, const DirNode& n, const std::wstring& here, std::uint64_t whole) {
    Builder b{s};
    struct Entry {
        bool dir;
        std::size_t index;
        std::uint64_t bytes;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        entries.push_back({true, i, n.children[i].bytes});
    }
    for (std::size_t i = 0; i < n.largest.size(); ++i) {
        entries.push_back({false, i, n.largest[i].bytes});
    }
    std::ranges::stable_sort(entries, std::greater{}, &Entry::bytes);
    const std::wstring& windows = security::paths::system_roots().system_root;
    std::size_t shown_dirs = 0;
    std::uint64_t more_dirs = 0, more_bytes = 0, more_files = 0;
    for (const auto& e : entries) {
        if (!e.dir) {
            const auto& f = n.largest[e.index];
            b.row(type_of(f.name), utf8(f.name), {sizes::text(f.bytes), static_cast<double>(f.bytes)}, share(f.bytes, whole),
                  {"\xE2\x80\x94", 0}, {"", -1}, "", "");
            continue;
        }
        const auto& c = n.children[e.index];
        if (shown_dirs == kMaxFolders) {
            ++more_dirs;
            more_bytes += c.bytes;
            more_files += c.files;
            continue;
        }
        ++shown_dirs;
        const bool system = !windows.empty() && same_path(join(here, c.name), windows);
        if (c.link) {
            b.row("folder", utf8(c.name), {"\xE2\x80\x94", 0}, share(0, whole), {"\xE2\x80\x94", 0}, {"", -1}, "a link, not followed", "");
            continue;
        }
        if (c.locked) {
            b.row("folder", utf8(c.name), {"\xE2\x80\x94", 0}, share(0, whole), {"\xE2\x80\x94", 0}, {"", -1}, "", "no access");
            continue;
        }
        // While it counts, what is below a folder has not come yet: nothing to step into.
        const bool opens = !system && !scan.counting && (!c.children.empty() || c.files > 0);
        b.row("folder", utf8(c.name), {sizes::text(c.bytes), static_cast<double>(c.bytes)}, share(c.bytes, whole),
              {grouped(c.files), static_cast<double>(c.files)},
              opens ? Cell{"Open", static_cast<double>(e.index)} : Cell{"", -1},
              std::format("{} {}", grouped(c.files), c.files == 1 ? "file" : "files"), system ? "system" : "");
    }
    if (more_dirs > 0) {
        b.row("folder", std::format("{} smaller folders", grouped(more_dirs)), {sizes::text(more_bytes), static_cast<double>(more_bytes)},
              share(more_bytes, whole), {grouped(more_files), static_cast<double>(more_files)}, {"", -1}, "", "");
    }
    if (n.other_files > 0) {
        b.row("file", std::format("{} smaller files", grouped(n.other_files)), {sizes::text(n.other_bytes), static_cast<double>(n.other_bytes)},
              share(n.other_bytes, whole), {grouped(n.other_files), static_cast<double>(n.other_files)}, {"", -1}, "", "");
    }
    (void)scan;
}

void largest(events::DataSet& s, const DirNode& top, const std::wstring& here, std::uint64_t whole) {
    struct Found {
        std::uint64_t bytes;
        const tools::winsys::FileSize* file;
        std::wstring folder;
    };
    std::vector<Found> found;
    std::vector<std::pair<const DirNode*, std::wstring>> stack{{&top, here}};
    while (!stack.empty()) {
        auto [n, path] = std::move(stack.back());
        stack.pop_back();
        for (const auto& f : n->largest) {
            found.push_back({f.bytes, &f, path});
        }
        for (const auto& c : n->children) {
            stack.emplace_back(&c, join(path, c.name));
        }
        if (found.size() > 4 * kMaxLargest) { // keep it small as it goes
            std::ranges::nth_element(found, found.begin() + kMaxLargest, std::greater{}, &Found::bytes);
            found.resize(kMaxLargest);
        }
    }
    std::ranges::sort(found, std::greater{}, &Found::bytes);
    if (found.size() > kMaxLargest) {
        found.resize(kMaxLargest);
    }
    Builder b{s};
    for (const auto& f : found) {
        b.row(type_of(f.file->name), utf8(f.file->name), {sizes::text(f.bytes), static_cast<double>(f.bytes)}, share(f.bytes, whole),
              {"\xE2\x80\x94", 0}, {"", -1}, utf8(f.folder), "");
    }
}

} // namespace

auto node_at(const DirNode& root, std::span<const std::uint32_t> path) -> const DirNode& {
    const DirNode* n = &root;
    for (const auto i : path) {
        if (i >= n->children.size()) {
            break;
        }
        n = &n->children[i];
    }
    return *n;
}

auto path_of(const DiskScan& scan, std::span<const std::uint32_t> path) -> std::wstring {
    std::wstring out = scan.path;
    const DirNode* n = &scan.root;
    for (const auto i : path) {
        if (i >= n->children.size()) {
            break;
        }
        n = &n->children[i];
        out = join(out, n->name);
    }
    return out;
}

auto title_of(const DiskScan& scan, std::span<const std::uint32_t> path) -> std::string {
    const auto valid = trimmed(scan.root, path);
    if (valid.empty()) {
        if (drive_root(scan.path)) {
            return "Disk " + utf8(scan.path.substr(0, 2));
        }
        return utf8(std::filesystem::path{scan.path}.filename().wstring());
    }
    return utf8(node_at(scan.root, valid).name);
}

auto set_of(std::shared_ptr<const DiskScan> scan, std::vector<std::uint32_t> path, View view, std::uint32_t id, std::string when)
    -> events::DataSet {
    path = trimmed(scan->root, path);
    const DirNode& n = node_at(scan->root, path);
    const std::wstring here = path_of(*scan, path);
    const bool drive_top = path.empty() && drive_root(scan->path) && scan->disk_total > 0;
    const std::uint64_t used = scan->disk_total > scan->disk_free ? scan->disk_total - scan->disk_free : 0;
    const std::uint64_t whole = drive_top ? used : n.bytes;
    const std::string title = title_of(*scan, path);
    events::DataSet s{.id = id,
                      .title = view == View::LargestFiles ? title + (title.ends_with(':') ? " largest files" : ": largest files")
                               : path.empty() && drive_root(scan->path) ? title + " by folder"
                                                                        : title,
                      .source = std::format("measure {} \xC2\xB7 {} files", utf8(here), grouped(n.files)),
                      .snapshot = std::move(when),
                      .location = utf8(here),
                      .columns = {{"", ColumnKind::Icon, 34, false, false},
                                  {"Name", ColumnKind::Text, 0, true, true},
                                  {"Size", ColumnKind::Size, 170, false, true},
                                  {drive_top ? "of used" : "of folder", ColumnKind::Number, 76, false, true},
                                  {"Items", ColumnKind::Number, 76, false, true},
                                  {"", ColumnKind::Action, 92, false, false}},
                      .complete = true,
                      .kind = events::SetKind::Space};
    s.rows.columns = s.columns.size();
    if (view == View::LargestFiles) {
        largest(s, n, here, whole);
    } else {
        folders(s, *scan, n, here, whole);
    }
    if (scan->counting) {
        s.footer = "Counting\xE2\x80\xA6 the folders grow as they are counted";
    } else if (!scan->complete) {
        s.footer = "Counting stopped part way: the sizes are what was counted";
    } else if (drive_top) {
        s.footer = std::format("{} used \xC2\xB7 {} free of {}", sizes::text(used), sizes::text(scan->disk_free), sizes::text(scan->disk_total));
    } else {
        s.footer = std::format("{} files \xC2\xB7 {}", grouped(n.files), sizes::text(n.bytes));
    }
    // The way back, as names; and on a drive's top, its bar: the largest folders, Other, Free.
    s.space_crumbs.push_back(drive_root(scan->path) ? utf8(scan->path.substr(0, 2)) : title_of(*scan, {}));
    {
        const DirNode* step = &scan->root;
        for (const auto i : path) {
            step = &step->children[i];
            s.space_crumbs.push_back(utf8(step->name));
        }
    }
    if (drive_top && view == View::Folders) {
        constexpr std::size_t kSlices = 6;
        const auto total = static_cast<double>(scan->disk_total);
        std::uint64_t shown = 0;
        for (const auto& c : n.children) {
            if (s.space_bar.size() == kSlices || c.bytes == 0 || static_cast<double>(c.bytes) < total * 0.01) {
                break; // the largest first: the rest are smaller still
            }
            s.space_bar.push_back({utf8(c.name), sizes::text(c.bytes), static_cast<float>(static_cast<double>(c.bytes) / total), false});
            shown += c.bytes;
        }
        if (used > shown) {
            s.space_bar.push_back({"Other", sizes::text(used - shown), static_cast<float>(static_cast<double>(used - shown) / total), false});
        }
        s.space_bar.push_back({"Free", sizes::text(scan->disk_free), static_cast<float>(static_cast<double>(scan->disk_free) / total), true});
    }
    s.space = std::move(scan);
    s.space_path = std::move(path);
    s.space_files = view == View::LargestFiles;
    return s;
}

} // namespace app::space
