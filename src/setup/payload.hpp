#pragma once

// setup::payload — the package inside the installer: a flat ZIP of the files that go to the
// install folder's root (cmake/WaPayload.cmake lists them). Only plain names are accepted: a name
// with a folder, a drive, a stream or ".." refuses the whole package before anything is written.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace setup {

struct Entry {
    std::string name;
    std::uint64_t size = 0; // unpacked
};

// Bytes unpacked so far and in all; called after every written chunk.
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

auto payload_entries(std::span<const std::byte> zip) -> core::Result<std::vector<Entry>>;

// Unpacks every entry into `dir` (which exists): each file is written under a temporary name and
// renamed over the old one, so a file is either the old or the new, never half. `written` gets
// each file once it is in place — on success, failure and cancel alike — for the rollback. A stop
// request ends it with Code::Cancelled; the file being written then is removed.
auto extract(std::span<const std::byte> zip, const std::filesystem::path& dir, const Progress& progress,
             const std::stop_token& stop, std::vector<std::filesystem::path>& written) -> core::Result<void>;

// One file of the package, whole (the License link opens the package's LICENSE).
auto read_entry(std::span<const std::byte> zip, std::string_view name) -> core::Result<std::string>;

// The names the package holds (cmake/WaPayload.cmake, compiled in): what uninstall removes when the
// journal is gone.
auto payload_names() -> std::vector<std::string>;

// A package of `files`, each under its file name (tools/pack builds the installer's with it).
auto pack(std::span<const std::filesystem::path> files) -> core::Result<std::string>;

} // namespace setup
