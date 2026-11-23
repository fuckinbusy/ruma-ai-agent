#pragma once

// Paths for Win32 calls past MAX_PATH: an absolute path gets the "\\?\" form (UNC: "\\?\UNC\"),
// which the file APIs take at any length. The installer's folder is the user's choice and may be
// deep (installer Review Focus 4).

#include <filesystem>
#include <string>

namespace setup {

inline auto extended(const std::filesystem::path& p) -> std::wstring {
    const std::wstring& s = p.native();
    if (s.starts_with(L"\\\\?\\") || !p.is_absolute())
        return s;
    std::wstring normal = p.lexically_normal().native();
    if (normal.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + normal.substr(2);
    return L"\\\\?\\" + normal;
}

} // namespace setup
