#pragma once

// tools::recycle — the Recycle Bin, for tools::os only (trash and restore). "Delete" in this agent
// means "to the Recycle Bin" (03-SECURITY); a drive that would delete at once instead is refused
// before anything happens.

#include "core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tools::recycle {

// This user's Recycle Bin folder on the drive of `path` ("C:\$Recycle.Bin\S-1-5-21-…").
auto folder_for(std::wstring_view path) -> core::Result<std::wstring>;
// Refused when the drive of `path` has no Recycle Bin (a network drive, some removable ones) or
// is set to delete at once ("Don't move files to the Recycle Bin", or the policy NoRecycleFiles).
auto usable(std::wstring_view path) -> core::Result<void>;

struct Item {
    std::wstring info; // $I…: where it was deleted from, and when
    std::wstring data; // $R…: the file or folder itself
    std::int64_t deleted = 0; // FILETIME
};
// The newest item deleted from exactly `original`.
auto find(std::wstring_view original) -> std::optional<Item>;
// IFileOperation with FOF_ALLOWUNDO and FOF_WANTNUKEWARNING: if Windows ever decides it cannot
// recycle, the user is warned instead of the item being destroyed silently.
auto send(std::wstring_view path) -> core::Result<void>;

} // namespace tools::recycle
