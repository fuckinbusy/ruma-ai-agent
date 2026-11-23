#include "tools/winsys/readonly_com.hpp"

#include <windows.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

// The only calls made here: WMI's ConnectServer, ExecQuery and the rows' Get; the Task
// Scheduler's Connect, GetFolder, GetFolders, GetTasks and each task's get_ properties. The
// tests read this file and the helper's for any other.

namespace tools::winsys::readonly {
namespace {

using Microsoft::WRL::ComPtr;

// The classes a query may name: Defender's status and history.
constexpr std::array<std::wstring_view, 3> kClasses{L"MSFT_MpComputerStatus", L"MSFT_MpThreat", L"MSFT_MpThreatDetection"};

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const wchar_t* s = nullptr) : b(s != nullptr ? SysAllocString(s) : nullptr) {}
    Bstr(const Bstr&) = delete;
    auto operator=(const Bstr&) -> Bstr& = delete;
    ~Bstr() { SysFreeString(b); }
    [[nodiscard]] auto text() const -> std::wstring { return b != nullptr ? std::wstring{b, SysStringLen(b)} : std::wstring{}; }
};

struct Variant {
    VARIANT v{};
    Variant() { VariantInit(&v); }
    Variant(const Variant&) = delete;
    auto operator=(const Variant&) -> Variant& = delete;
    ~Variant() { VariantClear(&v); }
};

// COM on this thread for one call (the helper has one thread that asks).
struct Com {
    bool own = false;
    Com() : own(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    Com(const Com&) = delete;
    auto operator=(const Com&) -> Com& = delete;
    ~Com() {
        if (own) {
            CoUninitialize();
        }
    }
};

auto upper(std::wstring_view s) -> std::wstring {
    std::wstring out{s};
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return out;
}

// SELECT ... FROM <one of kClasses> [WHERE ...]: the class it names, or false.
auto allowed(const std::wstring& wql) -> bool {
    const auto u = upper(wql);
    if (!u.starts_with(L"SELECT ")) {
        return false;
    }
    const auto from = u.find(L" FROM ");
    if (from == std::wstring::npos) {
        return false;
    }
    const auto start = from + 6;
    const auto end = u.find_first_of(L" \t\r\n", start);
    const std::wstring cls = u.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    return std::ranges::any_of(kClasses, [&](std::wstring_view c) { return upper(c) == cls; });
}

class WmiRow final : public Row {
public:
    explicit WmiRow(IWbemClassObject* o) : o_(o) {}
    [[nodiscard]] auto text(const wchar_t* name) const -> std::wstring override {
        Variant v;
        if (FAILED(o_->Get(name, 0, &v.v, nullptr, nullptr)) || v.v.vt != VT_BSTR || v.v.bstrVal == nullptr) {
            return {};
        }
        return {v.v.bstrVal, SysStringLen(v.v.bstrVal)};
    }
    [[nodiscard]] auto flag(const wchar_t* name) const -> bool override {
        Variant v;
        return SUCCEEDED(o_->Get(name, 0, &v.v, nullptr, nullptr)) && v.v.vt == VT_BOOL && v.v.boolVal != VARIANT_FALSE;
    }
    [[nodiscard]] auto number(const wchar_t* name) const -> std::int64_t override {
        Variant v;
        if (FAILED(o_->Get(name, 0, &v.v, nullptr, nullptr))) {
            return 0;
        }
        switch (v.v.vt) {
        case VT_BSTR: return v.v.bstrVal != nullptr ? _wtoi64(v.v.bstrVal) : 0; // CIM's 64-bit numbers come as text
        case VT_I4: return v.v.lVal;
        case VT_UI4: return v.v.ulVal;
        case VT_I8: return v.v.llVal;
        case VT_UI8: return static_cast<std::int64_t>(v.v.ullVal);
        case VT_I2: return v.v.iVal;
        case VT_UI1: return v.v.bVal;
        default: return 0;
        }
    }
    [[nodiscard]] auto texts(const wchar_t* name) const -> std::vector<std::wstring> override {
        Variant v;
        std::vector<std::wstring> out;
        if (FAILED(o_->Get(name, 0, &v.v, nullptr, nullptr)) || v.v.vt != (VT_ARRAY | VT_BSTR) || v.v.parray == nullptr) {
            return out;
        }
        BSTR* items = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(v.v.parray, reinterpret_cast<void**>(&items)))) {
            for (ULONG i = 0; i < v.v.parray->rgsabound[0].cElements; ++i) {
                if (items[i] != nullptr) {
                    out.emplace_back(items[i], SysStringLen(items[i]));
                }
            }
            SafeArrayUnaccessData(v.v.parray);
        }
        return out;
    }

private:
    IWbemClassObject* o_;
};

void walk(ITaskFolder* folder, std::vector<TaskRow>& out, std::vector<std::wstring>& not_read) {
    ComPtr<IRegisteredTaskCollection> list;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &list))) {
        LONG count = 0;
        list->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            Variant index;
            index.v.vt = VT_I4;
            index.v.lVal = i;
            ComPtr<IRegisteredTask> t;
            if (FAILED(list->get_Item(index.v, &t))) {
                continue;
            }
            TaskRow row;
            Bstr path;
            t->get_Path(&path.b);
            row.path = path.text();
            VARIANT_BOOL enabled = VARIANT_TRUE;
            if (SUCCEEDED(t->get_Enabled(&enabled))) {
                row.enabled = enabled != VARIANT_FALSE;
            }
            ComPtr<ITaskDefinition> def;
            if (SUCCEEDED(t->get_Definition(&def))) {
                ComPtr<IPrincipal> principal;
                if (SUCCEEDED(def->get_Principal(&principal))) {
                    Bstr user;
                    principal->get_UserId(&user.b);
                    row.account = user.text();
                }
                ComPtr<IActionCollection> actions;
                LONG n = 0;
                if (SUCCEEDED(def->get_Actions(&actions)) && SUCCEEDED(actions->get_Count(&n))) {
                    for (LONG k = 1; k <= n && row.command.empty(); ++k) {
                        ComPtr<IAction> a;
                        ComPtr<IExecAction> exec;
                        if (SUCCEEDED(actions->get_Item(k, &a)) && SUCCEEDED(a.As(&exec))) {
                            Bstr p, args;
                            exec->get_Path(&p.b);
                            exec->get_Arguments(&args.b);
                            row.command = p.text();
                            if (!args.text().empty()) {
                                row.command += L" " + args.text();
                            }
                        }
                    }
                }
            }
            out.push_back(std::move(row));
        }
    } else {
        Bstr path;
        folder->get_Path(&path.b);
        not_read.push_back(path.text());
    }
    ComPtr<ITaskFolderCollection> subs;
    if (SUCCEEDED(folder->GetFolders(0, &subs))) {
        LONG count = 0;
        subs->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            Variant index;
            index.v.vt = VT_I4;
            index.v.lVal = i;
            ComPtr<ITaskFolder> sub;
            if (SUCCEEDED(subs->get_Item(index.v, &sub))) {
                walk(sub.Get(), out, not_read);
            }
        }
    }
}

} // namespace

auto select(const wchar_t* ns, const std::wstring& wql, const std::function<void(const Row&)>& f) -> bool {
    if (!allowed(wql)) {
        return false;
    }
    const Com com;
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> svc;
    const Bstr where{ns};
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) ||
        FAILED(locator->ConnectServer(where.b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)) ||
        FAILED(CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                                 nullptr, EOAC_NONE))) {
        return false;
    }
    const Bstr lang{L"WQL"};
    const Bstr query{wql.c_str()};
    ComPtr<IEnumWbemClassObject> rows;
    if (FAILED(svc->ExecQuery(lang.b, query.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows))) {
        return false;
    }
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG got = 0;
        const HRESULT hr = rows->Next(10'000, 1, &o, &got);
        if (hr != WBEM_S_NO_ERROR || got == 0) {
            return hr == WBEM_S_FALSE || hr == WBEM_S_NO_ERROR;
        }
        f(WmiRow{o.Get()});
    }
}

auto tasks(std::vector<TaskRow>& out, std::vector<std::wstring>& not_read) -> bool {
    const Com com;
    ComPtr<ITaskService> service;
    Variant none;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))) ||
        FAILED(service->Connect(none.v, none.v, none.v, none.v))) {
        return false;
    }
    ComPtr<ITaskFolder> root;
    const Bstr slash{L"\\"};
    if (FAILED(service->GetFolder(slash.b, &root))) {
        return false;
    }
    walk(root.Get(), out, not_read);
    return true;
}

} // namespace tools::winsys::readonly
