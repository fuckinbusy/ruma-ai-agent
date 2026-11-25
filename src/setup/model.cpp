#include "setup/model.hpp"
#include "core/product.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <system_error>

namespace setup {
namespace {

constexpr std::wstring_view kProductFolder = core::product::kFolder;

auto without_trailing_separators(std::filesystem::path p) -> std::filesystem::path {
    while (!p.has_filename() && p.has_relative_path())
        p = p.parent_path();
    return p;
}

auto parse_version(std::string_view text) -> std::optional<std::array<unsigned, 3>> {
    std::array<unsigned, 3> parts{};
    const char* at = text.data();
    const char* const end = text.data() + text.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto [next, ec] = std::from_chars(at, end, parts[i]);
        if (ec != std::errc{} || next == at)
            return std::nullopt;
        at = next;
        if (i + 1 < parts.size()) {
            if (at == end || *at != '.')
                return std::nullopt;
            ++at;
        }
    }
    if (at != end)
        return std::nullopt;
    return parts;
}

auto nearest_existing(std::filesystem::path p) -> std::filesystem::path {
    std::error_code ec;
    while (!std::filesystem::is_directory(p, ec)) {
        if (!p.has_relative_path())
            return p;
        p = p.parent_path();
    }
    return p;
}

} // namespace

auto install_dir(const std::filesystem::path& chosen) -> std::filesystem::path {
    const auto dir = without_trailing_separators(chosen);
    const auto name = dir.filename().native();
    if (CompareStringOrdinal(name.c_str(), static_cast<int>(name.size()), kProductFolder.data(),
                             static_cast<int>(kProductFolder.size()), TRUE) == CSTR_EQUAL)
        return dir;
    return dir / kProductFolder;
}

auto holds_others(const std::filesystem::path& dir) -> bool {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return false;
    if (std::filesystem::exists(dir / L"install.json", ec))
        return false;
    return std::filesystem::directory_iterator{dir, ec} != std::filesystem::directory_iterator{};
}

auto update_dir(const std::filesystem::path& installed) -> std::filesystem::path {
    const auto dir = without_trailing_separators(installed);
    const auto name = dir.filename().native();
    const auto old = core::product::kOldFolder;
    if (CompareStringOrdinal(name.c_str(), static_cast<int>(name.size()), old.data(), static_cast<int>(old.size()), TRUE) !=
        CSTR_EQUAL)
        return dir;
    const auto sibling = dir.parent_path() / kProductFolder;
    return holds_others(sibling) ? dir : sibling; // someone's own folder of that name: the program stays put
}

auto check_folder(const std::filesystem::path& dir, std::uint64_t needed) -> FolderCheck {
    FolderCheck check;
    check.needed = needed;
    const auto existing = nearest_existing(dir);

    // A probe file with a name nobody else uses, deleted on close: what the install will do first.
    const auto probe = existing / (L"wa-setup-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    check.writable = file != INVALID_HANDLE_VALUE;
    if (check.writable)
        CloseHandle(file);

    ULARGE_INTEGER free{};
    ULARGE_INTEGER total{};
    if (GetDiskFreeSpaceExW(existing.c_str(), &free, &total, nullptr)) {
        check.free = free.QuadPart;
        check.total = total.QuadPart;
    }

    if (dir.native().size() > kMaxDirLength)
        check.problem = "This folder's path is too long";
    else if (!check.writable)
        check.problem = "You can't write to this folder";
    else if (holds_others(dir))
        check.problem = "This folder isn't empty \xE2\x80\x94 choose another";
    else if (check.free < needed)
        check.problem = "Not enough space";
    return check;
}

auto relate(std::optional<std::string_view> installed, std::string_view mine) -> Relation {
    if (!installed)
        return Relation::NotInstalled;
    const auto theirs = parse_version(*installed);
    const auto ours = parse_version(mine);
    if (!theirs || !ours)
        return Relation::Older;
    return *theirs >= *ours ? Relation::SameOrNewer : Relation::Older;
}

} // namespace setup
