#include "tools/recycle.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"
#include "security/paths.hpp"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tools::recycle {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

// The volume `path` is on, as its mount point ("C:\", or "C:\mnt\disk\" for a volume mounted in
// a folder): the Recycle Bin and the "delete at once" setting belong to the volume, not to the
// first three characters of the path.
auto drive_root(std::wstring_view path) -> std::optional<std::wstring> {
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') {
        return std::nullopt; // a share or a device: no bin of ours
    }
    wchar_t mount[MAX_PATH]{};
    if (GetVolumePathNameW(std::wstring{path}.c_str(), mount, MAX_PATH) && mount[0] != 0) {
        return std::wstring{mount};
    }
    return std::wstring{path.substr(0, 3)};
}

// A volume that keeps a Recycle Bin at all: fixed, and a file system that has one.
auto keeps_a_bin(const std::wstring& root) -> bool {
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED) {
        return false; // removable sticks and cards delete at once
    }
    wchar_t fs[MAX_PATH]{};
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH)) {
        return false;
    }
    return std::wstring_view{fs} == L"NTFS" || std::wstring_view{fs} == L"ReFS";
}

auto user_sid() -> std::wstring {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    core::win::KernelHandle guard{token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        return {};
    }
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
        return {};
    }
    std::wstring sid{text};
    LocalFree(text); // mem-ok: ConvertSidToStringSidW allocates with LocalAlloc
    return sid;
}

auto dword(HKEY root, const std::wstring& key, const wchar_t* value) -> std::optional<DWORD> {
    DWORD v = 0;
    DWORD size = sizeof(v);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return v;
}

struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Com() = default;
    Com(const Com&) = delete;
    auto operator=(const Com&) -> Com& = delete;
    ~Com() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

} // namespace

auto folder_for(std::wstring_view path) -> core::Result<std::wstring> {
    const auto root = drive_root(path);
    const std::wstring sid = user_sid();
    if (!root || sid.empty()) {
        return core::make_error(core::Code::NotFound, utf8(path) + " is not on a drive with a Recycle Bin");
    }
    return *root + L"$Recycle.Bin\\" + sid;
}

auto usable(std::wstring_view path) -> core::Result<void> {
    const auto root = drive_root(path);
    const auto refuse = [&](std::string_view why) {
        return core::make_error(core::Code::Policy,
                                utf8(path) + ": " + std::string{why} + " — the agent deletes only to the Recycle Bin");
    };
    if (!root) {
        return refuse("not on a local drive");
    }
    if (!keeps_a_bin(*root)) {
        return refuse("this drive keeps no Recycle Bin");
    }
    SHQUERYRBINFO info{sizeof(info)};
    if (FAILED(SHQueryRecycleBinW(root->c_str(), &info))) {
        return refuse("this drive has no Recycle Bin");
    }
    const std::wstring explorer = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    if (dword(HKEY_CURRENT_USER, explorer, L"NoRecycleFiles").value_or(0) != 0 ||
        dword(HKEY_LOCAL_MACHINE, explorer, L"NoRecycleFiles").value_or(0) != 0) {
        return refuse("a policy turns the Recycle Bin off");
    }
    wchar_t volume[MAX_PATH]{};
    if (GetVolumeNameForVolumeMountPointW(root->c_str(), volume, MAX_PATH)) {
        std::wstring v{volume}; // \\?\Volume{GUID}\ -> {GUID}
        const auto open = v.find(L'{');
        const auto close = v.find(L'}');
        if (open != std::wstring::npos && close != std::wstring::npos) {
            const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume\\" +
                                     v.substr(open, close - open + 1);
            if (dword(HKEY_CURRENT_USER, key, L"NukeOnDelete").value_or(0) != 0) {
                return refuse("this drive is set to delete files at once");
            }
        }
    }
    return {};
}

auto find(std::wstring_view original) -> std::optional<Item> {
    auto folder = folder_for(original);
    if (!folder) {
        return std::nullopt;
    }
    std::optional<Item> best;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(security::paths::long_form(*folder + L"\\$I*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    do {
        const std::wstring info = *folder + L"\\" + fd.cFileName;
        core::win::FileHandle f{CreateFileW(security::paths::long_form(info).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!f) {
            continue;
        }
        unsigned char buf[24 + 4 + 2 * 32768]{};
        DWORD n = 0;
        if (!ReadFile(f.get(), buf, sizeof(buf), &n, nullptr) || n < 24 + 2) {
            continue;
        }
        std::int64_t version = 0;
        std::int64_t deleted = 0;
        std::memcpy(&version, buf, 8);
        std::memcpy(&deleted, buf + 16, 8);
        std::wstring from;
        if (version == 2 && n >= 28) {
            std::uint32_t chars = 0;
            std::memcpy(&chars, buf + 24, 4);
            const std::size_t room = (n - 28) / 2;
            from.assign(reinterpret_cast<const wchar_t*>(buf + 28), std::min<std::size_t>(chars, room));
        } else if (version == 1) {
            from.assign(reinterpret_cast<const wchar_t*>(buf + 24), std::min<std::size_t>((n - 24) / 2, 260));
        } else {
            continue;
        }
        if (const auto nul = from.find(L'\0'); nul != std::wstring::npos) {
            from.resize(nul);
        }
        if (CompareStringOrdinal(from.c_str(), static_cast<int>(from.size()), original.data(),
                                 static_cast<int>(original.size()), TRUE) != CSTR_EQUAL) {
            continue;
        }
        if (!best || deleted > best->deleted) {
            const std::wstring name{fd.cFileName};
            best = Item{info, *folder + L"\\$R" + name.substr(2), deleted};
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

auto send(std::wstring_view path) -> core::Result<void> {
    Com com;
    Microsoft::WRL::ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
        return core::make_error(core::Code::Io, "the shell's file operations are not available");
    }
    // FOF_WANTNUKEWARNING partly overrides FOF_NOCONFIRMATION: recycling asks nothing, destroying
    // would warn the user first.
    const DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING |
                        FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE;
    Microsoft::WRL::ComPtr<IShellItem> item;
    const std::wstring p{path};
    if (FAILED(op->SetOperationFlags(flags)) ||
        FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return core::make_error(core::Code::NotFound, utf8(path) + " cannot be found");
    }
    if (FAILED(op->DeleteItem(item.Get(), nullptr)) || FAILED(op->PerformOperations())) {
        return core::make_error(core::Code::Io, "moving " + utf8(path) + " to the Recycle Bin failed");
    }
    BOOL aborted = FALSE;
    if (SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) {
        return core::make_error(core::Code::Cancelled, "moving " + utf8(path) + " to the Recycle Bin was cancelled");
    }
    return {};
}

} // namespace tools::recycle
