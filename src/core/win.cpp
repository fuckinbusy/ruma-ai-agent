#include "core/win.hpp"

#include "core/product.hpp"

#include <objbase.h>
#include <shlobj.h>

#include <format>
#include <string>

namespace core::win {
namespace {

// SHGetKnownFolderPath allocates the result with CoTaskMemAlloc; it must go back the same way.
struct CoTaskStringTraits {
    using type = PWSTR;
    static auto invalid() noexcept -> PWSTR { return nullptr; }
    static void close(PWSTR p) noexcept { CoTaskMemFree(p); } // mem-ok: shell-owned buffer
};

} // namespace

auto known_folder(const GUID& id) -> core::Result<std::filesystem::path> {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    const core::Handle<CoTaskStringTraits> owned{raw}; // freed on every path, even on failure
    if (FAILED(hr)) {
        if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
            return core::os_error(static_cast<std::uint32_t>(HRESULT_CODE(hr)),
                                  "looking up a known folder");
        }
        return core::make_error(core::Code::Io,
                                std::format("looking up a known folder: HRESULT 0x{:08X}",
                                            static_cast<std::uint32_t>(hr)));
    }
    return std::filesystem::path{owned.get()};
}

auto module_path() -> std::filesystem::path {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2); // truncated: a longer path
    }
}

auto data_dir_beside(const std::filesystem::path& exe) -> std::optional<std::filesystem::path> {
    std::error_code ec;
    auto dir = exe.parent_path() / L"data";
    if (std::filesystem::is_directory(dir, ec)) {
        return dir;
    }
    return std::nullopt;
}

auto data_dir_for(const std::filesystem::path& exe, const std::filesystem::path& program_files,
                  const std::filesystem::path& local_app_data) -> std::filesystem::path {
    // Under Program Files, a folder of it and not one only named like it ("Program Files Extra").
    std::wstring folder = program_files.wstring();
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) {
        folder.pop_back();
    }
    folder += L'\\';
    const std::wstring path = exe.wstring();
    const bool installed = !program_files.empty() && path.size() > folder.size() &&
                           CompareStringOrdinal(path.c_str(), static_cast<int>(folder.size()), folder.c_str(), static_cast<int>(folder.size()),
                                                TRUE) == CSTR_EQUAL;
    if (installed) {
        return local_app_data / product::kFolder / L"data";
    }
    if (auto beside = data_dir_beside(exe)) {
        return *beside;
    }
    return local_app_data / L"Agent";
}

auto adopt_old_data(const std::filesystem::path& local_app_data) -> bool {
    const auto old = local_app_data / product::kOldFolder;
    const auto now = local_app_data / product::kFolder;
    std::error_code ec;
    if (std::filesystem::exists(old / L"install.json", ec)) {
        return false; // a per-user install's own folder: its program is never the data (the installer moves its data)
    }
    if (!std::filesystem::is_directory(old, ec) || std::filesystem::exists(now, ec)) {
        return false; // nothing to move, or the new one is there already: never over it
    }
    std::filesystem::rename(old, now, ec); // one volume: whole, at once (the key, the history, the memory)
    return !ec;
}

auto agent_data_dir() -> std::filesystem::path {
    const auto local = known_folder(FOLDERID_LocalAppData);
    if (!local) {
        return data_dir_beside(module_path()).value_or(std::filesystem::temp_directory_path() / L"Agent");
    }
    auto dir = data_dir_for(module_path(), known_folder(FOLDERID_ProgramFiles).value_or(std::filesystem::path{}), *local);
    if (dir == *local / product::kFolder / L"data") {
        (void)adopt_old_data(*local); // an installed agent from before the rename: its data comes with it
    }
    return dir;
}

void keep_mta() noexcept {
    // Its cookie is never given back: the MTA is the process's from here on (as C++/WinRT's own
    // CoIncrementMTAUsage leaves it when it finds no apartment).
    static const bool kept = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));
    }();
    (void)kept;
}

} // namespace core::win
