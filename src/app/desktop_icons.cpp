#include "app/desktop_icons.hpp"

#include "core/utf8.hpp"

#include <windows.h>
#include <wbemidl.h> // brings OLE in before the shell headers (WIN32_LEAN_AND_MEAN leaves it out)
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")

namespace app::desktop_icons {
namespace {

using Microsoft::WRL::ComPtr;

auto desktop_view() -> ComPtr<IFolderView2> {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) {
        return {};
    }
    VARIANT loc{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    VARIANT empty{};
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) {
        return {};
    }
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder_view;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folder_view))) {
        return {};
    }
    return folder_view;
}

// Each of the desktop's items that `wanted` names (case-insensitively, by its parsing name):
// `found(i, view, child)` with the index into `wanted`.
template <class Found>
void for_items(std::span<const std::wstring> wanted, Found found) {
    const auto view = desktop_view();
    ComPtr<IShellFolder> folder;
    ComPtr<IEnumIDList> items;
    if (!view || FAILED(view->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items)))) {
        return;
    }
    for (PITEMID_CHILD child = nullptr; items->Next(1, &child, nullptr) == S_OK; ILFree(child)) {
        STRRET name{};
        wchar_t text[MAX_PATH]{};
        if (FAILED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &name)) || FAILED(StrRetToBufW(&name, child, text, MAX_PATH))) {
            continue;
        }
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (CompareStringOrdinal(text, -1, wanted[i].c_str(), -1, TRUE) == CSTR_EQUAL) {
                found(i, *view.Get(), child);
            }
        }
    }
}

struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Com() = default;
    Com(const Com&) = delete;
    auto operator=(const Com&) -> Com& = delete;
    ~Com() {
        if (hr == S_OK || hr == S_FALSE) {
            CoUninitialize();
        }
    }
};

} // namespace

auto icon_spots(std::span<const std::string> paths) -> std::vector<std::optional<journal::Spot>> {
    std::vector<std::optional<journal::Spot>> out(paths.size());
    std::vector<std::wstring> wanted;
    for (const auto& p : paths) {
        wanted.push_back(core::utf8::to_wide(p).value_or(std::wstring{}));
    }
    const Com com;
    for_items(wanted, [&](std::size_t i, IFolderView2& view, PCUITEMID_CHILD child) {
        POINT pt{};
        if (!out[i] && SUCCEEDED(view.GetItemPosition(child, &pt))) {
            out[i] = journal::Spot{paths[i], pt.x, pt.y};
        }
    });
    return out;
}

auto place_icons(std::span<const journal::Spot> spots) -> std::size_t {
    std::vector<std::wstring> wanted;
    for (const auto& s : spots) {
        wanted.push_back(core::utf8::to_wide(s.path).value_or(std::wstring{}));
    }
    const Com com;
    if (const auto view = desktop_view()) {
        DWORD flags = 0;
        if (SUCCEEDED(view->GetCurrentFolderFlags(&flags)) && (flags & FWF_AUTOARRANGE) != 0) {
            return 0; // Windows arranges them itself
        }
    }
    std::vector<bool> placed(spots.size(), false);
    std::size_t count = 0;
    // A file moved back shows on the desktop a moment later: up to a second of looks.
    for (int look = 0; look < 10 && count < spots.size(); ++look) {
        if (look > 0) {
            Sleep(100);
        }
        for_items(wanted, [&](std::size_t i, IFolderView2& view, PCUITEMID_CHILD child) {
            POINT pt{spots[i].x, spots[i].y};
            if (!placed[i] && SUCCEEDED(view.SelectAndPositionItems(1, &child, &pt, SVSI_POSITIONITEM))) {
                placed[i] = true;
                ++count;
            }
        });
    }
    return count;
}

} // namespace app::desktop_icons
