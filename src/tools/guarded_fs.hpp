#pragma once

// tools::guarded_fs — reading the file system with a Permit, checked after the object is open
// (Review Focus 2: the policy decided on a path; between the decision and the action the path
// may have become a junction to C:\Windows). An object is opened by handle, its final path asked
// from the file system, and it is used only when that path is inside one of the Permit's paths
// and its zone is no stricter than the zone the policy saw there. Folders are listed through
// their open handle, so the listing is of the object that was checked.

#include "core/error.hpp"
#include "core/win.hpp"
#include "security/permit.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tools::guarded_fs {

struct Opened {
    core::win::FileHandle handle;
    std::wstring path; // final, as the file system names it
    bool directory = false;
};

// For reading; refuses with core::Code::Policy when the object is not what the Permit is for.
auto open_read(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened>;
// For reading the file a change is for (fs.edit reads what it edits): a permission to change,
// the same checks — its own path, no stricter zone.
auto open_to_change(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened>;

struct Entry {
    std::wstring name;
    bool directory = false;
    std::uint32_t attributes = 0;
    std::uint32_t reparse_tag = 0; // IO_REPARSE_TAG_* when it is a link
    std::uint64_t size = 0;
    std::int64_t modified = 0; // FILETIME, UTC
};
auto list(const Opened& dir) -> core::Result<std::vector<Entry>>;

// The first `max` bytes, and the file's size.
struct Bytes {
    std::string data;
    std::uint64_t size = 0;
};
auto read(const Opened& file, std::size_t max) -> core::Result<Bytes>;

struct Info {
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;
    std::int64_t created = 0, modified = 0, accessed = 0; // FILETIME, UTC
};
auto info(const Opened& object) -> core::Result<Info>;

// For a change: the Permit's path number `index`, looked up again right before the operation
// (its name canonicalized anew) — refused with core::Code::Policy unless it is still the object
// the policy decided on, in a zone no stricter. Returns the final path to act on.
auto target(const security::Permit& permit, std::size_t index) -> core::Result<std::wstring>;
// For a change inside a permitted folder (a copied tree): `path` must resolve inside the
// Permit's path number `index`, in a zone no stricter.
auto inside(const security::Permit& permit, std::size_t index, std::wstring_view path) -> core::Result<std::wstring>;

// Refused (core::Code::Policy) when `path` is a file the system knows under more than one name:
// writing through one of them changes what the others hold, and only this one was decided on.
// A path that does not exist yet, or that cannot be opened, passes.
auto one_name_only(const std::wstring& path) -> core::Result<void>;

// "2026-10-11T12:00:00Z" from a FILETIME.
auto iso_time(std::int64_t filetime) -> std::string;

} // namespace tools::guarded_fs
