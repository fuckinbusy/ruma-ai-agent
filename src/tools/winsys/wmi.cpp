#include "tools/winsys/wmi.hpp"

#include "core/utf8.hpp"
#include "tools/winsys/com_thread.hpp"

#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <map>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "comsupp.lib")

namespace tools::winsys {
namespace {

using Microsoft::WRL::ComPtr;

auto text(IWbemClassObject* o, const wchar_t* name) -> std::wstring {
    _variant_t v;
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal != nullptr) {
        return std::wstring{v.bstrVal, SysStringLen(v.bstrVal)};
    }
    return {};
}

auto utf8(const std::wstring& s) -> std::string {
    return core::utf8::to_utf8(s).value_or("");
}

// Each row of `wql` (ExecQuery only: nothing here can change a subscription).
template <class F>
auto each(IWbemServices* svc, const wchar_t* wql, F&& f) -> bool {
    ComPtr<IEnumWbemClassObject> rows;
    if (FAILED(svc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows))) {
        return false;
    }
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG got = 0;
        if (rows->Next(5000, 1, &o, &got) != WBEM_S_NO_ERROR || got == 0) {
            break;
        }
        f(o.Get());
    }
    return true;
}

// "__EventFilter.Name=\"x\"" -> x
auto name_in(const std::wstring& path) -> std::wstring {
    const auto eq = path.find(L"Name=\"");
    if (eq == std::wstring::npos) {
        return path;
    }
    const auto start = eq + 6;
    const auto end = path.find(L'"', start);
    return path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

auto class_in(const std::wstring& path) -> std::wstring {
    const auto colon = path.find(L':');
    const auto from = colon == std::wstring::npos ? 0 : colon + 1;
    const auto dot = path.find(L'.', from);
    return path.substr(from, dot == std::wstring::npos ? std::wstring::npos : dot - from);
}

} // namespace

auto wmi_subscriptions() -> WmiList {
    return on_own_mta([] {
        WmiList out;
        ComPtr<IWbemLocator> locator;
        ComPtr<IWbemServices> svc;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) ||
            FAILED(locator->ConnectServer(_bstr_t(L"ROOT\\subscription"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)) ||
            FAILED(CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
            out.not_read.emplace_back(L"WMI root\\subscription");
        } else {
            std::map<std::wstring, std::wstring> queries;
            std::map<std::wstring, std::wstring> commands;
            const bool filters = each(svc.Get(), L"SELECT Name, Query FROM __EventFilter",
                                      [&](IWbemClassObject* o) { queries[text(o, L"Name")] = text(o, L"Query"); });
            each(svc.Get(), L"SELECT * FROM CommandLineEventConsumer", [&](IWbemClassObject* o) {
                auto c = text(o, L"CommandLineTemplate");
                commands[L"CommandLineEventConsumer:" + text(o, L"Name")] = c.empty() ? text(o, L"ExecutablePath") : c;
            });
            each(svc.Get(), L"SELECT * FROM ActiveScriptEventConsumer", [&](IWbemClassObject* o) {
                auto s = text(o, L"ScriptText");
                if (const auto nl = s.find_first_of(L"\r\n"); nl != std::wstring::npos) {
                    s.resize(nl);
                }
                commands[L"ActiveScriptEventConsumer:" + text(o, L"Name")] = s.empty() ? text(o, L"ScriptFileName") : s;
            });
            const bool bindings = each(svc.Get(), L"SELECT * FROM __FilterToConsumerBinding", [&](IWbemClassObject* o) {
                const auto filter = name_in(text(o, L"Filter"));
                const auto consumer_path = text(o, L"Consumer");
                const auto cls = class_in(consumer_path);
                const auto name = name_in(consumer_path);
                WmiSubscription s{.filter = utf8(filter), .query = utf8(queries[filter]), .consumer = utf8(name),
                                  .consumer_class = utf8(cls), .command = commands[cls + L":" + name]};
                out.subscriptions.push_back(std::move(s));
            });
            out.read = filters && bindings;
            if (!out.read) {
                out.not_read.emplace_back(L"WMI root\\subscription");
            }
        }
        return out;
    });
}

} // namespace tools::winsys
