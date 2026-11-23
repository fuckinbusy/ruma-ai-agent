#include "tools/winsys/defender.hpp"

#include "core/utf8.hpp"
#include "tools/winsys/com_thread.hpp"

#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <chrono>
#include <map>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "comsupp.lib")

namespace tools::winsys {
namespace {

using Microsoft::WRL::ComPtr;

auto get(IWbemClassObject* o, const wchar_t* name) -> _variant_t {
    _variant_t v;
    o->Get(name, 0, &v, nullptr, nullptr);
    return v;
}

// A CIM datetime "20261028093000.000000+000" as seconds since 1970, UTC (the offset ignored: minutes).
auto cim_time(const _variant_t& v) -> std::int64_t {
    if (v.vt != VT_BSTR || v.bstrVal == nullptr || SysStringLen(v.bstrVal) < 14) {
        return 0;
    }
    const std::wstring s{v.bstrVal};
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(std::stoi(s.substr(0, 4)));
    st.wMonth = static_cast<WORD>(std::stoi(s.substr(4, 2)));
    st.wDay = static_cast<WORD>(std::stoi(s.substr(6, 2)));
    st.wHour = static_cast<WORD>(std::stoi(s.substr(8, 2)));
    st.wMinute = static_cast<WORD>(std::stoi(s.substr(10, 2)));
    st.wSecond = static_cast<WORD>(std::stoi(s.substr(12, 2)));
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return 0;
    }
    const auto ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10'000'000ULL) - 11'644'473'600LL;
}

template <class F>
auto each(IWbemServices* svc, const wchar_t* wql, F&& f) -> bool {
    ComPtr<IEnumWbemClassObject> rows;
    if (FAILED(svc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows))) {
        return false;
    }
    bool any = true;
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG got = 0;
        const HRESULT hr = rows->Next(5000, 1, &o, &got);
        if (hr != WBEM_S_NO_ERROR || got == 0) {
            any = hr == WBEM_S_FALSE || hr == WBEM_S_NO_ERROR || hr == WBEM_S_TIMEDOUT;
            break;
        }
        f(o.Get());
    }
    return any;
}

} // namespace

auto defender_status() -> DefenderStatus {
    return on_own_mta([] {
        DefenderStatus d;
        ComPtr<IWbemLocator> locator;
        ComPtr<IWbemServices> svc;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) ||
            FAILED(locator->ConnectServer(_bstr_t(L"ROOT\\Microsoft\\Windows\\Defender"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)) ||
            FAILED(CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
            d.why_not = "Microsoft Defender's WMI namespace could not be opened (another antivirus, or Defender removed)";
            return d;
        }
        const bool status = each(svc.Get(), L"SELECT AMServiceEnabled, RealTimeProtectionEnabled, AntivirusSignatureLastUpdated FROM MSFT_MpComputerStatus",
                                 [&](IWbemClassObject* o) {
                                     d.present = true;
                                     const auto on = get(o, L"RealTimeProtectionEnabled");
                                     d.on = on.vt == VT_BOOL && on.boolVal != VARIANT_FALSE;
                                     d.signatures_at = cim_time(get(o, L"AntivirusSignatureLastUpdated"));
                                 });
        if (!status) {
            d.why_not = "Microsoft Defender's status could not be read";
            return d;
        }
        d.read = true;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        d.up_to_date = d.signatures_at > 0 && now - d.signatures_at < 7 * 86'400;
        // The history: usually administrators only (the helper reads it then, phase 13a § 6).
        std::map<std::int64_t, std::string> names;
        each(svc.Get(), L"SELECT ThreatID, ThreatName FROM MSFT_MpThreat", [&](IWbemClassObject* o) {
            const auto id = get(o, L"ThreatID");
            const auto name = get(o, L"ThreatName");
            if (name.vt == VT_BSTR && name.bstrVal != nullptr) {
                names[id.vt == VT_BSTR ? _wtoi64(id.bstrVal) : static_cast<std::int64_t>(id.llVal)] =
                    core::utf8::to_utf8(name.bstrVal).value_or("");
            }
        });
        d.threats_read = each(svc.Get(), L"SELECT ThreatID, Resources, ActionSuccess, InitialDetectionTime FROM MSFT_MpThreatDetection",
                              [&](IWbemClassObject* o) {
                                  Threat t;
                                  const auto id = get(o, L"ThreatID");
                                  t.name = names[id.vt == VT_BSTR ? _wtoi64(id.bstrVal) : static_cast<std::int64_t>(id.llVal)];
                                  const auto ok = get(o, L"ActionSuccess");
                                  t.resolved = ok.vt == VT_BOOL && ok.boolVal != VARIANT_FALSE;
                                  t.at = cim_time(get(o, L"InitialDetectionTime"));
                                  const auto res = get(o, L"Resources");
                                  if ((res.vt & VT_ARRAY) != 0 && res.parray != nullptr) {
                                      BSTR* items = nullptr;
                                      if (SUCCEEDED(SafeArrayAccessData(res.parray, reinterpret_cast<void**>(&items)))) {
                                          if (res.parray->rgsabound[0].cElements > 0 && items[0] != nullptr) {
                                              t.path = core::utf8::to_utf8(items[0]).value_or("");
                                          }
                                          SafeArrayUnaccessData(res.parray);
                                      }
                                  }
                                  d.threats.push_back(std::move(t));
                              });
        return d;
    });
}

} // namespace tools::winsys
