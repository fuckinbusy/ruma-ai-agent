#include "setup/self.hpp"

#include <shlobj.h> // first: exdisp.h and shldisp.h need its OLE types
#include <exdisp.h>
#include <sddl.h>
#include <shldisp.h>
#include <wrl/client.h>

#include <cstddef>
#include <cwctype>
#include <string>
#include <vector>

namespace setup::self {
namespace {

auto temp_root() -> std::filesystem::path {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    return std::filesystem::path{std::wstring{temp, n}}.parent_path(); // without the trailing separator
}

auto same(const std::filesystem::path& a, const std::filesystem::path& b) -> bool {
    const auto& x = a.native();
    const auto& y = b.native();
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) ==
           CSTR_EQUAL;
}

// WinAiAgent<letters>-<digits>
auto ours(std::wstring_view name) -> bool {
    constexpr std::wstring_view kPrefix = L"WinAiAgent";
    if (!name.starts_with(kPrefix))
        return false;
    name.remove_prefix(kPrefix.size());
    const auto dash = name.find(L'-');
    if (dash == 0 || dash == std::wstring_view::npos || dash + 1 == name.size())
        return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (i < dash ? !std::iswalpha(c) : i > dash && !std::iswdigit(c))
            return false;
    }
    return true;
}

void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view n{fd.cFileName};
            if (n == L"." || n == L"..")
                continue;
            const std::wstring child = dir + L"\\" + fd.cFileName;
            const bool sub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (sub && !link) {
                remove_tree(child);
            } else if (sub) {
                RemoveDirectoryW(child.c_str()); // the link itself, never its target
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

} // namespace

auto acquire_single(std::wstring_view name) -> std::optional<core::win::KernelHandle> {
    const std::wstring n{name};
    core::win::KernelHandle mutex{CreateMutexW(nullptr, FALSE, n.c_str())};
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return std::nullopt;
    return mutex;
}

auto startup(std::span<const std::wstring_view> args, bool has_payload, const std::filesystem::path& exe_dir) -> Startup {
    Startup s;
    s.uninstall = !has_payload;
    s.app_dir = exe_dir;
    bool from_temp = false;
    for (const auto a : args) {
        if (a == kNoGl) {
            s.no_gl = true;
        } else if (a == kSmoke) {
            s.smoke = true;
        } else if (a.starts_with(kFromTemp)) {
            from_temp = true;
            s.app_dir = std::filesystem::path{a.substr(kFromTemp.size())};
        }
    }
    s.relaunch = s.uninstall && !from_temp && !s.smoke;
    return s;
}

auto dll_resource_name(std::wstring_view file) -> std::wstring {
    std::wstring out;
    for (const wchar_t c : file)
        out.push_back(c == L'.' ? L'_' : static_cast<wchar_t>(std::towupper(c)));
    return out;
}

auto dll_file_name(std::wstring_view resource) -> std::wstring {
    std::wstring out;
    for (const wchar_t c : resource)
        out.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (out.ends_with(L"_dll"))
        out.replace(out.size() - 4, 4, L".dll");
    return out;
}

namespace {

// A process's user as SID text; empty when it can't be read.
auto user_of(HANDLE process) -> std::wstring {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return {};
    }
    const core::win::KernelHandle t{token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buf(size);
    wchar_t* text = nullptr;
    if (size == 0 || !GetTokenInformation(token, TokenUser, buf.data(), size, &size) ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
        return {};
    }
    const core::win::LocalPtr<wchar_t*> owned{text};
    return std::wstring{text};
}

// The desktop's user: the one Explorer's shell runs as.
auto desktop_user() -> std::wstring {
    DWORD pid = 0;
    if (HWND shell = GetShellWindow(); shell == nullptr || GetWindowThreadProcessId(shell, &pid) == 0) {
        return {};
    }
    const core::win::KernelHandle p{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    return p ? user_of(p.get()) : std::wstring{};
}

} // namespace

auto elevated_by_another_account() -> bool {
    const auto desktop = desktop_user();
    const auto own = user_of(GetCurrentProcess());
    return !desktop.empty() && !own.empty() && desktop != own;
}

auto event_for_agent(const std::wstring& name) -> core::win::KernelHandle {
    auto user = desktop_user();
    if (user.empty()) {
        user = user_of(GetCurrentProcess());
    }
    // The user, the administrators, the system; medium integrity, so an unelevated agent may set it.
    const std::wstring sddl = L"D:(A;;GA;;;" + user + L")(A;;GA;;;BA)(A;;GA;;;SY)S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (user.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return core::win::KernelHandle{CreateEventW(nullptr, TRUE, FALSE, name.c_str())};
    }
    const core::win::LocalPtr<PSECURITY_DESCRIPTOR> owned{sd};
    SECURITY_ATTRIBUTES sa{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = sd, .bInheritHandle = FALSE};
    return core::win::KernelHandle{CreateEventW(&sa, TRUE, FALSE, name.c_str())};
}

auto launch_as_desktop_user(const std::filesystem::path& exe, const std::wstring& args, const std::filesystem::path& dir) -> bool {
    using Microsoft::WRL::ComPtr;
    // The desktop's own folder view, and its Shell.Application: what it starts runs as Explorer does.
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) {
        return false;
    }
    VARIANT none{};
    VariantInit(&none);
    long hwnd = 0;
    ComPtr<IDispatch> desktop;
    if (windows->FindWindowSW(&none, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) != S_OK || !desktop) {
        return false;
    }
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folder;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(desktop.As(&provider)) || FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
        FAILED(background.As(&folder)) || FAILED(folder->get_Application(&application)) || FAILED(application.As(&shell))) {
        return false;
    }
    BSTR file = SysAllocString(exe.c_str());
    VARIANT a{}, d{}, verb{}, show{};
    a.vt = VT_BSTR;
    a.bstrVal = SysAllocString(args.c_str());
    d.vt = VT_BSTR;
    d.bstrVal = SysAllocString(dir.c_str());
    verb.vt = VT_BSTR;
    verb.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    const HRESULT hr = shell->ShellExecute(file, a, d, verb, show);
    SysFreeString(file);
    VariantClear(&a);
    VariantClear(&d);
    VariantClear(&verb);
    return SUCCEEDED(hr);
}

auto own_temp(std::wstring_view kind, DWORD pid) -> std::filesystem::path {
    return temp_root() / (L"WinAiAgent" + std::wstring{kind} + L"-" + std::to_wstring(pid));
}

auto is_own_temp(const std::filesystem::path& dir) -> bool {
    const auto normal = dir.lexically_normal();
    return same(normal.parent_path(), temp_root()) && ours(normal.filename().native());
}

auto remove_own_temp(const std::filesystem::path& dir) -> bool {
    const auto normal = dir.lexically_normal();
    if (!is_own_temp(normal))
        return false;
    const DWORD a = GetFileAttributesW(normal.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return RemoveDirectoryW(normal.c_str()) != FALSE; // a link named like ours: the link only
    remove_tree(normal.native());
    return GetFileAttributesW(normal.c_str()) == INVALID_FILE_ATTRIBUTES;
}

} // namespace setup::self
