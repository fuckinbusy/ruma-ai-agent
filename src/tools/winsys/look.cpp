#include "tools/winsys/look.hpp"

#include "core/utf8.hpp"
#include "tools/winsys/com_thread.hpp"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <string_view>

#pragma comment(lib, "uiautomationcore.lib")

namespace tools::winsys {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxElements = 3000;

auto role_of(CONTROLTYPEID t) -> std::string_view {
    switch (t) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_ComboBoxControlTypeId: return "combo box";
    case UIA_EditControlTypeId: return "field";
    case UIA_HyperlinkControlTypeId: return "link";
    case UIA_ListItemControlTypeId: return "list item";
    case UIA_MenuItemControlTypeId: return "menu item";
    case UIA_RadioButtonControlTypeId: return "radio button";
    case UIA_TabItemControlTypeId: return "tab";
    case UIA_TextControlTypeId: return "text";
    case UIA_TitleBarControlTypeId: return "title";
    case UIA_TreeItemControlTypeId: return "tree item";
    case UIA_DocumentControlTypeId: return "document";
    case UIA_HeaderItemControlTypeId: return "column";
    case UIA_DataItemControlTypeId: return "cell";
    case UIA_StatusBarControlTypeId: return "status";
    case UIA_GroupControlTypeId: return "group";
    case UIA_ToolBarControlTypeId: return "toolbar";
    default: return {};
    }
}

auto utf8(BSTR s) -> std::string {
    return s != nullptr ? core::utf8::to_utf8(std::wstring_view{s, SysStringLen(s)}).value_or("") : std::string{};
}

class Reader {
public:
    Reader(IUIAutomation* uia, std::size_t max) : uia_{uia}, max_{max} { uia_->get_ControlViewWalker(&walker_); }

    void walk(IUIAutomationElement* e, int depth) {
        if (e == nullptr || full_ || ++seen_ > kMaxElements || depth > 40) {
            return;
        }
        line(e);
        ComPtr<IUIAutomationElement> child;
        walker_->GetFirstChildElement(e, &child);
        while (child && !full_) {
            walk(child.Get(), depth + 1);
            ComPtr<IUIAutomationElement> next;
            walker_->GetNextSiblingElement(child.Get(), &next);
            child = next;
        }
    }
    [[nodiscard]] auto text() && -> std::string { return std::move(out_); }

private:
    void line(IUIAutomationElement* e) {
        CONTROLTYPEID type = 0;
        e->get_CurrentControlType(&type);
        const auto role = role_of(type);
        BSTR name_b = nullptr;
        e->get_CurrentName(&name_b);
        std::string name = utf8(name_b);
        SysFreeString(name_b);
        BOOL password = FALSE;
        e->get_CurrentIsPassword(&password);
        std::string value;
        if (!password) { // a password field's value is never read (phase 13a § 2)
            ComPtr<IUIAutomationValuePattern> vp;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
                BSTR v = nullptr;
                if (SUCCEEDED(vp->get_CurrentValue(&v))) {
                    value = utf8(v);
                }
                SysFreeString(v);
            }
        }
        if (name.empty() && value.empty()) {
            return;
        }
        std::string l = role.empty() ? std::string{} : std::string{role} + ": ";
        l += name;
        if (!value.empty() && value != name) {
            l += (name.empty() ? "" : " = ") + value;
        }
        if (out_.size() + l.size() + 1 > max_) {
            l.resize(max_ > out_.size() + 1 ? max_ - out_.size() - 1 : 0);
            full_ = true;
        }
        out_ += l;
        out_ += '\n';
    }

    IUIAutomation* uia_;
    ComPtr<IUIAutomationTreeWalker> walker_;
    std::size_t max_;
    std::string out_;
    int seen_ = 0;
    bool full_ = false;
};

} // namespace

auto read_window_text(HWND w, std::size_t max_bytes) -> core::Result<std::string> {
    return on_own_mta([&]() -> core::Result<std::string> {
        ComPtr<IUIAutomation> uia;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            return core::make_error(core::Code::Io, "UI Automation is not available");
        }
        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(w, &root)) || !root) {
            return core::make_error(core::Code::Io, "the window cannot be read");
        }
        Reader r{uia.Get(), max_bytes};
        r.walk(root.Get(), 0);
        return std::move(r).text();
    });
}

auto focused_password(HWND w, std::chrono::milliseconds limit) -> std::optional<bool> {
    DWORD pid = 0;
    if (w == nullptr || GetWindowThreadProcessId(w, &pid) == 0 || pid == GetCurrentProcessId()) {
        return false;
    }
    const auto ms = static_cast<DWORD>(limit.count());
    const auto said = on_own_mta_within(limit, [pid, ms]() -> std::optional<bool> {
        ComPtr<IUIAutomation2> uia;
        ComPtr<IUIAutomationElement> focus;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            return false;
        }
        // UI Automation's own limits as well (2 s to connect, 20 s for an answer): the thread left
        // behind after `limit` ends about then too, not 20 s later. Its timeout is "not said".
        uia->put_ConnectionTimeout(ms);
        uia->put_TransactionTimeout(ms);
        if (const HRESULT hr = uia->GetFocusedElement(&focus); FAILED(hr) || !focus) {
            return hr == UIA_E_TIMEOUT ? std::nullopt : std::optional<bool>{false};
        }
        int owner = 0;
        BOOL password = FALSE;
        // The focus is the window's own (a browser's page is drawn by the window's process).
        if (const HRESULT hr = focus->get_CurrentProcessId(&owner); FAILED(hr)) {
            return hr == UIA_E_TIMEOUT ? std::nullopt : std::optional<bool>{false};
        }
        if (static_cast<DWORD>(owner) != pid) {
            return false;
        }
        if (const HRESULT hr = focus->get_CurrentIsPassword(&password); FAILED(hr)) {
            return hr == UIA_E_TIMEOUT ? std::nullopt : std::optional<bool>{false};
        }
        return password != FALSE;
    });
    return said ? *said : std::nullopt;
}

} // namespace tools::winsys
