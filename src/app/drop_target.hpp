#pragma once

// app::DropTarget and pick_files — files brought to the window (phase 10, design/ui_remastered
// screen 17). While files are dragged over it the sheet shows its drop zone and Windows draws them
// and their count at the cursor (IDropTargetHelper); the drop attaches them. The paperclip opens
// Windows' own file picker. Both on the window's thread, OLE initialised there.

#include <windows.h>

#include <oleidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

// rpcndr.h (under oleidl.h) defines `small` as `char`: our own parameters named so (ui::work) are
// not its business.
#ifdef small
#undef small
#endif

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace app {

class DropTarget final : public IDropTarget {
public:
    struct Calls {
        std::function<void(bool files)> enter; // something is dragged over; `files`: it is files
        std::function<void()> leave;           // it went away, or was dropped
        std::function<void(std::vector<std::string> paths)> drop;
    };
    // Registered for `hwnd` (RegisterDragDrop) at once; revoked when it goes. Owned by its maker:
    // Release never deletes it.
    DropTarget(HWND hwnd, Calls calls);
    DropTarget(const DropTarget&) = delete;
    auto operator=(const DropTarget&) -> DropTarget& = delete;
    ~DropTarget();
    [[nodiscard]] auto registered() const noexcept -> bool { return registered_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL at, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL at, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL at, DWORD* effect) override;

private:
    HWND hwnd_;
    Calls calls_;
    Microsoft::WRL::ComPtr<IDropTargetHelper> helper_; // the files and their count at the cursor
    bool files_ = false;
    bool registered_ = false;
    std::atomic<ULONG> refs_{1};
};

// The paths in a drop (CF_HDROP), at most `max`, as UTF-8.
auto paths_of(HDROP drop, std::size_t max) -> std::vector<std::string>;

// Windows' file picker owned by `owner`, several files at once: their paths; none when cancelled.
auto pick_files(HWND owner) -> std::vector<std::string>;

} // namespace app
