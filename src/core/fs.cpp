#include "core/fs.hpp"

#include "core/win.hpp"

#include <windows.h>

#include <atomic>
#include <system_error>

namespace core::fs {
namespace {

constexpr std::size_t kLongPathThreshold = 240; // below MAX_PATH with room for a file name

auto unix_ms(const FILETIME& ft) noexcept -> std::int64_t {
    const ULARGE_INTEGER u{.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    constexpr std::uint64_t kEpochDiff100ns = 116444736000000000ull; // 1601 → 1970
    return static_cast<std::int64_t>((u.QuadPart - kEpochDiff100ns) / 10000);
}

// Creates `temp`, writes and flushes it. The handle is closed on every return, so the caller can
// delete the file afterwards (with share mode 0 a delete while it is open would fail).
// `created` tells the caller whether the file is ours to remove.
auto write_and_flush(const std::wstring& temp, std::string_view bytes, bool& created)
    -> core::Result<void> {
    const core::win::FileHandle file{CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        return core::os_error(GetLastError(), "creating a temporary file for an atomic write");
    }
    created = true;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + done, chunk, &written, nullptr)) {
            return core::os_error(GetLastError(), "writing a temporary file");
        }
        done += written;
    }
    if (!FlushFileBuffers(file.get())) {
        return core::os_error(GetLastError(), "flushing a temporary file");
    }
    return {};
}

auto open_for_read(const std::wstring& p) -> core::win::FileHandle {
    return core::win::FileHandle{CreateFileW(p.c_str(), GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                 FILE_SHARE_DELETE,
                                             nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                             nullptr)};
}

} // namespace

auto os_path(const std::filesystem::path& path) -> std::wstring {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::wstring s = absolute.lexically_normal().make_preferred().native();
    if (s.size() < kLongPathThreshold || s.starts_with(L"\\\\?\\")) {
        return s;
    }
    if (s.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + s.substr(2); // \\server\share → \\?\UNC\server\share
    }
    return L"\\\\?\\" + s;
}

auto read_file(const std::filesystem::path& path, std::size_t max_bytes)
    -> core::Result<std::string> {
    const core::win::FileHandle file = open_for_read(os_path(path));
    if (!file) {
        return core::os_error(GetLastError(), "opening a file for reading");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        return core::os_error(GetLastError(), "reading a file size");
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        return core::make_error(core::Code::InvalidArgument,
                                "file is larger than the allowed " + std::to_string(max_bytes) +
                                    " bytes");
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(file.get(), data.data() + done, chunk, &got, nullptr)) {
            return core::os_error(GetLastError(), "reading a file");
        }
        if (got == 0) {
            data.resize(done); // the file shrank while reading
            break;
        }
        done += got;
    }
    return data;
}

auto write_file_atomic(const std::filesystem::path& path, std::string_view bytes)
    -> core::Result<void> {
    static std::atomic<unsigned> counter{0};
    const std::wstring target = os_path(path);
    // The temp name gets its own long-path treatment: a target just under the threshold plus
    // the suffix may cross MAX_PATH.
    const std::wstring temp =
        os_path(path.native() + L".wa-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1)));

    bool created = false;
    auto result = write_and_flush(temp, bytes, created);
    if (result && !MoveFileExW(temp.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = core::os_error(GetLastError(), "replacing the target of an atomic write");
    }
    if (!result && created) {
        DeleteFileW(temp.c_str()); // our own temporary file; its handle is already closed
    }
    return result;
}

auto list_directory(const std::filesystem::path& path, std::vector<Entry>& out)
    -> core::Result<void> {
    out.clear();
    std::wstring pattern = os_path(path);
    if (!pattern.ends_with(L'\\')) {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW data{};
    const core::win::FindHandle find{FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                                      FindExSearchNameMatch, nullptr,
                                                      FIND_FIRST_EX_LARGE_FETCH)};
    if (!find) {
        return core::os_error(GetLastError(), "listing a directory");
    }
    do {
        const std::wstring_view name{data.cFileName};
        if (name == L"." || name == L"..") {
            continue;
        }
        Entry e;
        e.name = name;
        e.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        e.attributes = data.dwFileAttributes;
        e.modified_unix_ms = unix_ms(data.ftLastWriteTime);
        e.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(std::move(e));
    } while (FindNextFileW(find.get(), &data));

    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        return core::os_error(error, "listing a directory");
    }
    return {};
}

} // namespace core::fs
