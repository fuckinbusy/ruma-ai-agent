#pragma once

// app::space — what takes the space, as the data panel shows it (phase 13a § 4, design/ui_system
// 02b–02c): a counted tree (disk.usage) at one of its folders — its folders and its largest files
// by size, the rest of its files in one line — or the largest files of the whole count. Made once
// when the count comes and again on each step down or back (a click), never in a frame.

#include "app/events.hpp"
#include "tools/winsys/disk.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace app::space {

enum class View : std::uint8_t { Folders, LargestFiles };

// The folder `path` leads to (indices into children, from the root); past the tree: the deepest it reaches.
[[nodiscard]] auto node_at(const tools::winsys::DirNode& root, std::span<const std::uint32_t> path) -> const tools::winsys::DirNode&;
// Its full path: the count's folder and the names down to it.
[[nodiscard]] auto path_of(const tools::winsys::DiskScan& scan, std::span<const std::uint32_t> path) -> std::wstring;

// The set for the panel: `scan` at `path`, in `view`. Its rows: Icon, Name (a note under it), Size,
// "of used" / "of folder", Items, and the action ("Open" for a folder to step into). A row's action
// number is the child's index to step into, -1 for none. `when`: the count's time ("21:12:51").
[[nodiscard]] auto set_of(std::shared_ptr<const tools::winsys::DiskScan> scan, std::vector<std::uint32_t> path, View view,
                          std::uint32_t id, std::string when) -> events::DataSet;

// "Disk C:" for a drive's root, else the folder's own name.
[[nodiscard]] auto title_of(const tools::winsys::DiskScan& scan, std::span<const std::uint32_t> path) -> std::string;

} // namespace app::space
