#include "tools/winsys/disk.hpp"

#include "security/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace tools::winsys {
namespace {

constexpr std::uint32_t kCheckEvery = 256; // entries between looks at the stop token

struct Totals {
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> bytes{0};
};

auto file_bytes(const std::wstring& path, const WIN32_FIND_DATAW& fd) -> std::uint64_t {
    if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0) {
        DWORD high = 0;
        const DWORD low = GetCompressedFileSizeW(security::paths::long_form(path).c_str(), &high);
        if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
            return (static_cast<std::uint64_t>(high) << 32) | low;
        }
    }
    return (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
}

auto skip(const wchar_t* name) -> bool {
    return (name[0] == L'.' && name[1] == 0) || (name[0] == L'.' && name[1] == L'.' && name[2] == 0);
}

// A folder's own file, kept by name among its largest.
void keep_largest(DirNode& n, const wchar_t* name, std::uint64_t bytes) {
    if (n.largest.size() < kLargest) {
        n.largest.push_back({name, bytes});
        return;
    }
    const auto smallest = std::ranges::min_element(n.largest, {}, &FileSize::bytes);
    if (bytes > smallest->bytes) {
        *smallest = {name, bytes};
    }
}

// Sizes up from what is inside; the largest first.
void finish(DirNode& n) {
    std::ranges::sort(n.children, std::greater{}, &DirNode::bytes);
    std::ranges::sort(n.largest, std::greater{}, &FileSize::bytes);
    std::uint64_t own_named = 0;
    for (const auto& f : n.largest) {
        own_named += f.bytes;
    }
    n.other_bytes -= std::min(n.other_bytes, own_named);
    n.other_files -= std::min<std::uint64_t>(n.other_files, n.largest.size());
    for (const auto& c : n.children) {
        n.bytes += c.bytes;
        n.files += c.files;
    }
}

auto open(const std::wstring& dir, WIN32_FIND_DATAW& fd) -> HANDLE {
    return FindFirstFileExW(security::paths::long_form(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                            FIND_FIRST_EX_LARGE_FETCH);
}

// `node` (its name set) and everything below `path`, depth first without recursion: a deep tree
// cannot run the thread's stack out. Its own files go to `own` as they are found (the live count).
// False: stopped.
auto count(DirNode& node, const std::wstring& path, std::stop_token stop, Totals& totals, std::atomic<std::uint64_t>* own) -> bool {
    struct Frame {
        DirNode* node;
        std::wstring path;
        HANDLE h;
        WIN32_FIND_DATAW fd;
        bool first;
    };
    std::vector<Frame> stack;
    const auto enter = [&](DirNode& n, const std::wstring& p) {
        Frame f{&n, p, INVALID_HANDLE_VALUE, {}, true};
        f.h = open(p, f.fd);
        if (f.h == INVALID_HANDLE_VALUE) {
            n.locked = GetLastError() != ERROR_FILE_NOT_FOUND;
            return;
        }
        stack.push_back(std::move(f));
    };
    enter(node, path);
    std::uint32_t seen = 0;
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (!f.first && !FindNextFileW(f.h, &f.fd)) {
            FindClose(f.h);
            DirNode* done = f.node;
            stack.pop_back();
            finish(*done);
            continue;
        }
        f.first = false;
        if (++seen % kCheckEvery == 0 && stop.stop_requested()) {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                FindClose(it->h);
                finish(*it->node);
            }
            return false;
        }
        const WIN32_FIND_DATAW& fd = f.fd;
        if (skip(fd.cFileName)) {
            continue;
        }
        const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dir) {
            DirNode child{.name = fd.cFileName};
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                child.link = true; // never followed: a loop, another drive, a cloud folder
                f.node->children.push_back(std::move(child));
                continue;
            }
            f.node->children.push_back(std::move(child));
            const std::wstring sub = f.path + L"\\" + fd.cFileName;
            DirNode& added = f.node->children.back();
            enter(added, sub); // may grow `stack`: `f` is not used after this
            continue;
        }
        const std::uint64_t bytes = file_bytes(f.path + L"\\" + fd.cFileName, fd);
        DirNode& n = *f.node;
        ++n.other_files; // own files, the named ones taken off in finish()
        n.other_bytes += bytes;
        n.files += 1;
        n.bytes += bytes;
        keep_largest(n, fd.cFileName, bytes);
        totals.files.fetch_add(1, std::memory_order_relaxed);
        totals.bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (own != nullptr) {
            own->fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    return true;
}

// The folder holding the user's profile (C:\Users for C:\) first: what the user asks about most is counted first.
auto profile_first(std::vector<DirNode>& tops, const std::filesystem::path& root) {
    wchar_t profile[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    const std::wstring p{profile};
    const std::wstring base = root.wstring();
    const auto it = std::ranges::find_if(tops, [&](const DirNode& d) {
        const std::wstring full = base + (base.ends_with(L'\\') ? L"" : L"\\") + d.name;
        return p.size() >= full.size() && CompareStringOrdinal(p.c_str(), static_cast<int>(full.size()), full.c_str(),
                                                               static_cast<int>(full.size()), TRUE) == CSTR_EQUAL &&
               (p.size() == full.size() || p[full.size()] == L'\\');
    });
    if (it != tops.end()) {
        std::rotate(tops.begin(), it, it + 1);
    }
}

} // namespace

auto scan(const std::filesystem::path& root, std::stop_token stop, const std::function<void(const ScanProgress&)>& progress,
          std::chrono::milliseconds every) -> ScanResult {
    ScanResult out;
    std::wstring base = root.wstring();
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    const std::wstring prefix = base.ends_with(L'\\') ? base.substr(0, base.size() - 1) : base;
    out.root.name = base;
    Totals totals;

    // The root's own files here; its folders, each counted by a worker.
    WIN32_FIND_DATAW fd{};
    HANDLE h = open(prefix, fd);
    if (h == INVALID_HANDLE_VALUE) {
        out.root.locked = true;
        return out;
    }
    std::vector<DirNode> tops;
    do {
        if (skip(fd.cFileName)) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            tops.push_back(DirNode{.name = fd.cFileName, .link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
            continue;
        }
        const std::uint64_t bytes = file_bytes(prefix + L"\\" + fd.cFileName, fd);
        ++out.root.other_files;
        out.root.other_bytes += bytes;
        out.root.files += 1;
        out.root.bytes += bytes;
        keep_largest(out.root, fd.cFileName, bytes);
        totals.files.fetch_add(1);
        totals.bytes.fetch_add(bytes);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    profile_first(tops, std::filesystem::path{base});

    std::vector<std::atomic<std::uint64_t>> running(tops.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stopped{false};
    std::atomic<std::size_t> finished{0};
    std::mutex m;
    std::condition_variable cv;
    const unsigned threads = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 8);
    {
        std::vector<std::jthread> workers;
        for (unsigned t = 0; t < std::min<std::size_t>(threads, tops.size()); ++t) {
            workers.emplace_back([&] {
                for (std::size_t i = next.fetch_add(1); i < tops.size(); i = next.fetch_add(1)) {
                    if (!tops[i].link && !stopped.load()) {
                        if (!count(tops[i], prefix + L"\\" + tops[i].name, stop, totals, &running[i])) {
                            stopped = true;
                        }
                    }
                    {
                        std::scoped_lock lock{m};
                        finished.fetch_add(1);
                    }
                    cv.notify_all();
                }
            });
        }
        // Here, on the caller's thread: progress, until every folder is counted.
        const auto report = [&] {
            if (!progress) {
                return;
            }
            ScanProgress p{.files = totals.files.load(), .bytes = totals.bytes.load(), .current = {}, .tops = {}};
            p.tops.reserve(tops.size());
            for (std::size_t i = 0; i < tops.size(); ++i) {
                p.tops.push_back(DirNode{.name = tops[i].name, .bytes = running[i].load(), .link = tops[i].link});
            }
            const std::size_t at = std::min(next.load(), tops.size());
            if (at > 0) {
                p.current = tops[at - 1].name;
            }
            progress(p);
        };
        const auto step = every.count() > 0 ? every : std::chrono::milliseconds{2};
        // At once: the folders to count, before any of them is done — a fast count reported nothing
        // otherwise, and the panel stayed empty until the end (found in a Release gate, 2026-10-29).
        report();
        std::unique_lock lock{m};
        while (finished.load() < tops.size()) {
            cv.wait_for(lock, step, [&] { return finished.load() >= tops.size(); });
            if (finished.load() < tops.size()) {
                lock.unlock();
                report();
                lock.lock();
            }
        }
    }
    out.complete = !stopped.load() && !stop.stop_requested();
    out.root.children = std::move(tops);
    finish(out.root); // a stopped folder holds what was counted in it
    return out;
}

} // namespace tools::winsys
