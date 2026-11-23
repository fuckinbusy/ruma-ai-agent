#pragma once

// tools::winsys::scan — what takes the space under a folder (phase 13a § 4): every folder's size on
// disk and its file count, its own largest files and the rest of them summed. Read only: names and
// sizes, never contents.
//
// FindFirstFileExW with FIND_FIRST_EX_LARGE_FETCH, the root's folders counted in parallel (no more
// threads than processors, at most 8), each one depth first without recursion. A link (junction,
// symbolic link, mount point) is listed, never followed. A compressed or sparse file counts what
// it takes on disk (GetCompressedFileSizeW); any other its length. A folder that cannot be read is
// one locked node. A stop leaves what was counted.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace tools::winsys {

inline constexpr std::size_t kLargest = 20; // a folder's own largest files kept by name

struct FileSize {
    std::wstring name;
    std::uint64_t bytes = 0;
};

struct DirNode {
    std::wstring name;
    std::uint64_t bytes = 0; // on disk, everything below
    std::uint64_t files = 0; // everything below
    bool locked = false;     // could not be read (no rights): what is inside is not counted
    bool link = false;       // a link: listed, not followed, counted as nothing
    std::vector<DirNode> children; // its folders, the largest first
    std::vector<FileSize> largest; // its own largest files, the largest first
    std::uint64_t other_files = 0; // its own files beyond `largest`
    std::uint64_t other_bytes = 0;
};

struct ScanProgress {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::wstring current; // a folder of the root being counted
    // The root's folders as counted so far (name, bytes, files): the tree grows while it counts.
    std::vector<DirNode> tops;
};

struct ScanResult {
    DirNode root;
    bool complete = true; // false: stopped part way
};

// disk.usage's result for the panel's tree: the folder counted, its tree, and its drive.
struct DiskScan {
    std::wstring path;
    DirNode root;
    bool complete = true;
    bool counting = false;        // the tree so far, while it counts (its progress)
    std::uint64_t disk_total = 0; // the drive's size and free space (0: not known)
    std::uint64_t disk_free = 0;
};

// `progress` runs on the calling thread, about every `every` (0: as often as it can).
auto scan(const std::filesystem::path& root, std::stop_token stop, const std::function<void(const ScanProgress&)>& progress = {},
          std::chrono::milliseconds every = std::chrono::milliseconds{250}) -> ScanResult;

} // namespace tools::winsys
