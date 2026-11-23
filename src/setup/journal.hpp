#pragma once

// setup::Journal — what an install made: files, folders, shortcuts, registry keys and values.
// Kept beside the program as install.json; update and uninstall work from it, so uninstall removes
// exactly what the install made and nothing of anyone else's.

#include "core/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace setup {

// A registry value under HKCU (the key relative to it). An empty name is the key's default value.
struct RegValue {
    std::wstring key;
    std::wstring name;
};

struct Journal {
    std::filesystem::path root; // the install folder
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> dirs;
    std::vector<std::filesystem::path> shortcuts;
    std::vector<std::wstring> keys; // whole keys the install created (HKCU-relative), e.g. the Uninstall entry
    std::vector<RegValue> values;   // single values in keys that aren't ours, e.g. Run
    std::string version;
};

inline constexpr std::wstring_view kJournalName = L"install.json";

auto journal_path(const std::filesystem::path& root) -> std::filesystem::path;

auto to_json(const Journal& journal) -> std::string;

// A journal without a root is an error: uninstall must never guess the folder it deletes from.
auto journal_from(std::string_view text) -> core::Result<Journal>;

} // namespace setup
