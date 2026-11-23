#pragma once

// core::fs — file operations the agent's own code needs (settings, logs, conversations).
// Paths of any length work: long paths get the \\?\ prefix internally.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs {

struct Entry {
    std::wstring name; // UTF-16 as stored by NTFS (may contain unpaired surrogates)
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;
    std::int64_t modified_unix_ms = 0;
    bool is_directory = false;
};

// Reads the whole file. Files larger than max_bytes are refused (InvalidArgument).
auto read_file(const std::filesystem::path& path, std::size_t max_bytes = 64ull << 20)
    -> core::Result<std::string>;

// Writes to a temporary file in the same directory, flushes it to disk and renames it over the
// target, so readers see either the old or the new content, never a half-written file.
auto write_file_atomic(const std::filesystem::path& path, std::string_view bytes)
    -> core::Result<void>;

// Lists a directory (without "." and ".."); `out` is cleared first.
auto list_directory(const std::filesystem::path& path, std::vector<Entry>& out)
    -> core::Result<void>;

// The path in the form Win32 accepts at any length (\\?\ prefix when needed).
auto os_path(const std::filesystem::path& path) -> std::wstring;

} // namespace core::fs
