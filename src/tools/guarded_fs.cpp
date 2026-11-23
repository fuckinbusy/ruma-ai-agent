#include "tools/guarded_fs.hpp"

#include "core/utf8.hpp"
#include "security/paths.hpp"

#include <windows.h>

#include <format>

namespace tools::guarded_fs {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto final_path(HANDLE h) -> std::wstring {
    std::wstring buf(512, L'\0');
    for (;;) {
        const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(n + 1);
    }
    if (buf.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + buf.substr(8);
    }
    if (buf.starts_with(L"\\\\?\\")) {
        return buf.substr(4);
    }
    return buf;
}

auto to_int(FILETIME t) -> std::int64_t {
    return static_cast<std::int64_t>(ULARGE_INTEGER{.LowPart = t.dwLowDateTime, .HighPart = t.dwHighDateTime}.QuadPart);
}

auto to_int(LARGE_INTEGER t) -> std::int64_t {
    return t.QuadPart;
}

} // namespace

namespace {

auto open_checked(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened>;

} // namespace

auto open_read(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened> {
    if (permit.risk() != security::Risk::Read) {
        return core::make_error(core::Code::Policy, "a read needs a permission to read");
    }
    return open_checked(permit, path);
}

auto open_to_change(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened> {
    if (permit.risk() != security::Risk::Write) {
        return core::make_error(core::Code::Policy, "a change needs a permission to change");
    }
    return open_checked(permit, path);
}

namespace {

// Opened by handle, its final path checked against the Permit's paths and their zones.
auto open_checked(const security::Permit& permit, std::wstring_view path) -> core::Result<Opened> {
    const std::wstring name{path};
    core::win::FileHandle h{CreateFileW(security::paths::long_form(name).c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (!h) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return core::make_error(core::Code::NotFound, utf8(name) + " does not exist");
        }
        return core::os_error(e, "opening " + utf8(name));
    }
    Opened o;
    o.path = final_path(h.get());
    if (o.path.empty()) {
        return core::make_error(core::Code::Policy, "cannot tell what " + utf8(name) + " is");
    }
    // What was opened, checked against what was decided.
    const auto permitted = permit.paths();
    const auto zones = permit.zones();
    std::size_t at = permitted.size();
    for (std::size_t i = 0; i < permitted.size(); ++i) {
        if (security::paths::is_within(o.path, permitted[i].path)) {
            at = i;
            break;
        }
    }
    if (at == permitted.size()) {
        return core::make_error(core::Code::Policy,
                                std::format("{} is not what the permission was given for (it is {} now)", utf8(name), utf8(o.path)));
    }
    const auto canonical = security::paths::canonical(o.path, o.path);
    if (!canonical) {
        return core::make_error(core::Code::Policy, "cannot place " + utf8(o.path));
    }
    const auto zone = security::paths::zone_of(*canonical, security::paths::system_roots());
    if (at < zones.size() && security::paths::stricter(zone, zones[at]) != zones[at]) {
        return core::make_error(core::Code::Policy,
                                std::format("{} is in the {} zone: stricter than the folder that was allowed",
                                            utf8(o.path), security::paths::zone_name(zone)));
    }
    BY_HANDLE_FILE_INFORMATION bi{};
    if (GetFileInformationByHandle(h.get(), &bi)) {
        o.directory = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    o.handle = std::move(h);
    return o;
}

} // namespace

auto list(const Opened& dir) -> core::Result<std::vector<Entry>> {
    if (!dir.directory) {
        return core::make_error(core::Code::InvalidArgument, utf8(dir.path) + " is not a folder");
    }
    std::vector<Entry> out;
    alignas(8) unsigned char buf[64 * 1024];
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(dir.handle.get(), cls, buf, sizeof(buf))) {
            const DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES) {
                break;
            }
            return core::os_error(e, "listing " + utf8(dir.path));
        }
        cls = FileIdBothDirectoryInfo;
        for (auto* p = buf;;) {
            const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
            const std::wstring_view name{info->FileName, info->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..") {
                Entry e;
                e.name = std::wstring{name};
                e.attributes = info->FileAttributes;
                e.directory = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.reparse_tag = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? info->EaSize : 0;
                e.size = static_cast<std::uint64_t>(info->EndOfFile.QuadPart);
                e.modified = to_int(info->LastWriteTime);
                out.push_back(std::move(e));
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            p += info->NextEntryOffset;
        }
    }
    return out;
}

auto read(const Opened& file, std::size_t max) -> core::Result<Bytes> {
    if (file.directory) {
        return core::make_error(core::Code::InvalidArgument, utf8(file.path) + " is a folder");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle.get(), &size)) {
        return core::os_error(GetLastError(), "reading " + utf8(file.path));
    }
    Bytes b;
    b.size = static_cast<std::uint64_t>(size.QuadPart);
    b.data.resize(static_cast<std::size_t>(std::min<std::uint64_t>(b.size, max)));
    std::size_t got = 0;
    while (got < b.data.size()) {
        DWORD n = 0;
        if (!ReadFile(file.handle.get(), b.data.data() + got, static_cast<DWORD>(b.data.size() - got), &n, nullptr)) {
            return core::os_error(GetLastError(), "reading " + utf8(file.path));
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    b.data.resize(got);
    return b;
}

auto info(const Opened& object) -> core::Result<Info> {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(object.handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(object.handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
        return core::os_error(GetLastError(), "reading the metadata of " + utf8(object.path));
    }
    return Info{.size = object.directory ? 0 : static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
                .attributes = basic.FileAttributes,
                .created = to_int(basic.CreationTime),
                .modified = to_int(basic.LastWriteTime),
                .accessed = to_int(basic.LastAccessTime)};
}

namespace {

auto changes(security::Risk r) -> bool {
    return r == security::Risk::Write || r == security::Risk::RenameMove || r == security::Risk::Delete;
}

// `name` canonicalized now: refused unless it lands where `allowed` says, in a zone no stricter.
auto recheck(const security::Permit& permit, std::size_t index, std::wstring_view name, bool exact)
    -> core::Result<security::paths::Canonical> {
    if (!changes(permit.risk())) {
        return core::make_error(core::Code::Policy, "a change needs a permission to change");
    }
    if (index >= permit.paths().size() || index >= permit.zones().size()) {
        return core::make_error(core::Code::Policy, "no permission for that path");
    }
    const auto& decided = permit.paths()[index];
    const auto now = security::paths::canonical(name, decided.path);
    if (!now || now->device || now->ads) {
        return core::make_error(core::Code::Policy, "cannot place " + utf8(name));
    }
    const bool same = exact ? CompareStringOrdinal(now->path.c_str(), static_cast<int>(now->path.size()), decided.path.c_str(),
                                                   static_cast<int>(decided.path.size()), TRUE) == CSTR_EQUAL
                            : security::paths::is_within(now->path, decided.path);
    if (!same) {
        return core::make_error(core::Code::Policy,
                                std::format("{} is not what the permission was given for (it is {} now)", utf8(name), utf8(now->path)));
    }
    const auto zone = security::paths::zone_of(*now, security::paths::system_roots());
    if (security::paths::stricter(zone, permit.zones()[index]) != permit.zones()[index]) {
        return core::make_error(core::Code::Policy, std::format("{} is in the {} zone now", utf8(now->path),
                                                                security::paths::zone_name(zone)));
    }
    return std::move(*now);
}

// Moving or deleting a link is about the link itself, not about what it leads to: "put this
// shortcut in the Recycle Bin" must not take the folder it points at with it. Writing, by
// contrast, is about the file the name opens, which is the resolved one.
auto acts_on_the_name(security::Risk r) -> bool {
    return r == security::Risk::RenameMove || r == security::Risk::Delete;
}

} // namespace

auto target(const security::Permit& permit, std::size_t index) -> core::Result<std::wstring> {
    if (index >= permit.paths().size()) {
        return core::make_error(core::Code::Policy, "no permission for that path");
    }
    const auto& decided = permit.paths()[index];
    const std::wstring& named = decided.logical.empty() ? decided.path : decided.logical;
    auto now = recheck(permit, index, named, true);
    if (!now) {
        return std::unexpected{now.error()};
    }
    return acts_on_the_name(permit.risk()) && !now->logical.empty() ? now->logical : now->path;
}

auto inside(const security::Permit& permit, std::size_t index, std::wstring_view path) -> core::Result<std::wstring> {
    auto now = recheck(permit, index, path, false);
    if (!now) {
        return std::unexpected{now.error()};
    }
    return acts_on_the_name(permit.risk()) && !now->logical.empty() ? now->logical : now->path;
}

auto one_name_only(const std::wstring& path) -> core::Result<void> {
    const std::wstring long_path = security::paths::long_form(path);
    core::win::FileHandle h{CreateFileW(long_path.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!h) {
        return {}; // not there yet, or in use: the write itself will say
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h.get(), &info) && info.nNumberOfLinks > 1) {
        return core::make_error(core::Code::Policy,
                                std::format("{} is the same file under {} names: writing it changes the others too",
                                            utf8(path), info.nNumberOfLinks));
    }
    return {};
}

auto iso_time(std::int64_t filetime) -> std::string {
    const ULARGE_INTEGER u{.QuadPart = static_cast<std::uint64_t>(filetime)};
    const FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME t{};
    if (filetime <= 0 || !FileTimeToSystemTime(&ft, &t)) {
        return {};
    }
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

} // namespace tools::guarded_fs
