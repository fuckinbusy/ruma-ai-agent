#include "app/drop_target.hpp"

#include "app/sheet_model.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"

#include <shlobj.h>

#include <iterator>
#include <utility>

namespace app {
namespace {

using Microsoft::WRL::ComPtr;

auto hdrop_format() -> FORMATETC {
    return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

} // namespace

DropTarget::DropTarget(HWND hwnd, Calls calls) : hwnd_{hwnd}, calls_{std::move(calls)} {
    (void)CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_)); // none: no picture at the cursor
    const HRESULT hr = RegisterDragDrop(hwnd_, this);
    registered_ = SUCCEEDED(hr);
    if (!registered_) {
        WA_WARN("app", "drop target: RegisterDragDrop failed: 0x{:08x}", static_cast<unsigned>(hr));
    }
}

DropTarget::~DropTarget() {
    if (registered_) {
        RevokeDragDrop(hwnd_);
    }
}

HRESULT STDMETHODCALLTYPE DropTarget::QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragEnter(IDataObject* data, DWORD /*keys*/, POINTL at, DWORD* effect) {
    FORMATETC f = hdrop_format();
    files_ = data != nullptr && data->QueryGetData(&f) == S_OK;
    *effect = files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_) {
        POINT p{at.x, at.y};
        helper_->DragEnter(hwnd_, data, &p, *effect);
    }
    if (calls_.enter) {
        calls_.enter(files_);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragOver(DWORD /*keys*/, POINTL at, DWORD* effect) {
    *effect = files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_) {
        POINT p{at.x, at.y};
        helper_->DragOver(&p, *effect);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragLeave() {
    if (helper_) {
        helper_->DragLeave();
    }
    files_ = false;
    if (calls_.leave) {
        calls_.leave();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::Drop(IDataObject* data, DWORD /*keys*/, POINTL at, DWORD* effect) {
    *effect = files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper_) {
        POINT p{at.x, at.y};
        helper_->Drop(data, &p, *effect);
    }
    std::vector<std::string> paths;
    FORMATETC f = hdrop_format();
    STGMEDIUM medium{};
    if (files_ && data != nullptr && SUCCEEDED(data->GetData(&f, &medium))) {
        if (auto* drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
            paths = paths_of(drop, sheet::kMaxAttachments);
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    files_ = false;
    if (calls_.leave) {
        calls_.leave();
    }
    if (!paths.empty() && calls_.drop) {
        calls_.drop(std::move(paths));
    }
    return S_OK;
}

auto paths_of(HDROP drop, std::size_t max) -> std::vector<std::string> {
    std::vector<std::string> paths;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n && paths.size() < max; ++i) {
        wchar_t name[MAX_PATH * 2]{};
        if (DragQueryFileW(drop, i, name, static_cast<UINT>(std::size(name))) == 0) {
            continue;
        }
        if (auto u = core::utf8::to_utf8(name)) {
            paths.push_back(std::move(*u));
        }
    }
    return paths;
}

auto pick_files(HWND owner) -> std::vector<std::string> {
    std::vector<std::string> paths;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = init == S_OK || init == S_FALSE;
    {
        ComPtr<IFileOpenDialog> dialog;
        ComPtr<IShellItemArray> chosen;
        DWORD options = 0;
        const COMDLG_FILTERSPEC types[] = {
            {L"Pictures and documents", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.heic;*.tif;*.tiff;*.pdf;*.docx;*.xlsx;*.pptx;*.csv;*.txt;*.md"},
            {L"All files", L"*.*"}};
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(dialog->GetOptions(&options)) &&
            SUCCEEDED(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM)) &&
            SUCCEEDED(dialog->SetFileTypes(static_cast<UINT>(std::size(types)), types)) && SUCCEEDED(dialog->SetTitle(L"Attach")) &&
            SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResults(&chosen))) {
            DWORD n = 0;
            (void)chosen->GetCount(&n);
            for (DWORD i = 0; i < n && paths.size() < sheet::kMaxAttachments; ++i) {
                ComPtr<IShellItem> item;
                PWSTR path = nullptr;
                if (SUCCEEDED(chosen->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    if (auto u = core::utf8::to_utf8(path)) {
                        paths.push_back(std::move(*u));
                    }
                    CoTaskMemFree(path); // mem-ok: the shell's string, given back as it asks
                }
            }
        }
    }
    if (uninit) {
        CoUninitialize();
    }
    WA_INFO("app", "file picker: {} chosen", paths.size());
    return paths;
}

} // namespace app
