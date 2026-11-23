#include "tools/winsys/services.hpp"

#include "core/utf8.hpp"
#include "tools/winsys/startup.hpp"

#include <windows.h>

#include <array>
#include <vector>

namespace tools::winsys {
namespace {

// A service manager or service handle, closed when it goes.
class Sc {
public:
    explicit Sc(SC_HANDLE h) : h_{h} {}
    ~Sc() {
        if (h_ != nullptr) {
            CloseServiceHandle(h_);
        }
    }
    Sc(const Sc&) = delete;
    Sc& operator=(const Sc&) = delete;
    [[nodiscard]] auto get() const -> SC_HANDLE { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    SC_HANDLE h_;
};

auto state_name(DWORD s) -> std::string {
    switch (s) {
    case SERVICE_RUNNING: return "running";
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_START_PENDING: return "start pending";
    case SERVICE_STOP_PENDING: return "stop pending";
    case SERVICE_PAUSED: return "paused";
    case SERVICE_PAUSE_PENDING: return "pause pending";
    case SERVICE_CONTINUE_PENDING: return "continue pending";
    default: return "unknown";
    }
}

auto start_name(DWORD t, bool delayed) -> std::string {
    switch (t) {
    case SERVICE_AUTO_START: return delayed ? "automatic (delayed)" : "automatic";
    case SERVICE_DEMAND_START: return "manual";
    case SERVICE_DISABLED: return "disabled";
    case SERVICE_BOOT_START: return "boot";
    case SERVICE_SYSTEM_START: return "system";
    default: return "unknown";
    }
}

auto utf8(const wchar_t* s) -> std::string {
    return s != nullptr ? core::utf8::to_utf8(s).value_or("") : std::string{};
}

// A service inside svchost: the DLL it loads, Parameters\ServiceDll (or ServiceDll beside its
// ImagePath), read only and expanded as Windows reads it; empty for the others.
auto service_dll(const wchar_t* name) -> std::wstring {
    const std::wstring key = std::wstring{L"SYSTEM\\CurrentControlSet\\Services\\"} + name;
    for (const auto& sub : {key + L"\\Parameters", key}) {
        std::array<wchar_t, 1024> value{};
        DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        // RRF_RT_REG_SZ alone: a REG_EXPAND_SZ value comes back expanded.
        if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"ServiceDll", RRF_RT_REG_SZ, nullptr, value.data(), &size) == ERROR_SUCCESS) {
            return std::wstring{value.data()};
        }
    }
    return {};
}

} // namespace

auto services() -> std::vector<ServiceInfo> {
    std::vector<ServiceInfo> out;
    // Enumerate only: no right here can start, stop or change a service (phase 13a rule 1).
    const Sc manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT)};
    if (!manager) {
        return out;
    }
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, nullptr, 0, &needed, &count,
                          &resume, nullptr);
    std::vector<BYTE> buf(needed + 4096);
    resume = 0;
    if (!EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, buf.data(),
                               static_cast<DWORD>(buf.size()), &needed, &count, &resume, nullptr)) {
        return out;
    }
    const auto* items = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    std::vector<BYTE> cfg;
    for (DWORD i = 0; i < count; ++i) {
        const auto& it = items[i];
        ServiceInfo s{.name = utf8(it.lpServiceName), .display = utf8(it.lpDisplayName),
                      .state = state_name(it.ServiceStatusProcess.dwCurrentState), .start_type = "unknown", .command = {}, .file = {},
                      .account = {}, .driver = (it.ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) != 0};
        if (const Sc svc{OpenServiceW(manager.get(), it.lpServiceName, SERVICE_QUERY_CONFIG)}) {
            DWORD size = 0;
            QueryServiceConfigW(svc.get(), nullptr, 0, &size);
            cfg.assign(size, 0);
            if (size > 0 && QueryServiceConfigW(svc.get(), reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfg.data()), size, &size)) {
                const auto* c = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(cfg.data());
                bool delayed = false;
                SERVICE_DELAYED_AUTO_START_INFO d{};
                DWORD dsize = 0;
                if (QueryServiceConfig2W(svc.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<BYTE*>(&d), sizeof(d), &dsize)) {
                    delayed = d.fDelayedAutostart != FALSE;
                }
                s.start_type = start_name(c->dwStartType, delayed);
                s.command = c->lpBinaryPathName != nullptr ? c->lpBinaryPathName : L"";
                s.file = file_of(s.command);
                s.account = utf8(c->lpServiceStartName);
            }
        }
        s.dll = service_dll(it.lpServiceName);
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace tools::winsys
