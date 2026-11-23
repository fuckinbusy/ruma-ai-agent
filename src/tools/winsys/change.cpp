#include "tools/winsys/change.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"
#include "tools/winsys/com_thread.hpp"
#include "tools/winsys/startup.hpp"
#include "tools/winsys/wmi.hpp"

#include <windows.h>
#include <shellapi.h>
#include <aclapi.h>
#include <comdef.h>
#include <taskschd.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <format>
#include <thread>
#include <vector>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "comsupp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tools::winsys::change {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::wstring_view kApproved = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

auto wide(std::string_view s) -> std::wstring {
    return core::utf8::to_wide(s).value_or(std::wstring{});
}

auto com_error(HRESULT hr, std::string_view what) -> core::Error {
    return core::Error{core::Code::Api, std::format("{}: 0x{:08X}", what, static_cast<std::uint32_t>(hr))};
}

// A WMI object path's key: backslashes and quotes escaped.
auto key_text(std::wstring_view s) -> std::wstring {
    std::wstring out;
    for (const wchar_t c : s) {
        if (c == L'\\' || c == L'"') {
            out.push_back(L'\\');
        }
        out.push_back(c);
    }
    return out;
}

auto text(IWbemClassObject* o, const wchar_t* name) -> std::wstring {
    _variant_t v;
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal != nullptr) {
        return std::wstring{v.bstrVal, SysStringLen(v.bstrVal)};
    }
    return {};
}

auto object_text(IWbemClassObject* o) -> std::wstring {
    BSTR mof = nullptr;
    if (FAILED(o->GetObjectText(0, &mof)) || mof == nullptr) {
        return {};
    }
    std::wstring out{mof, SysStringLen(mof)};
    SysFreeString(mof);
    return out;
}

auto connect(ComPtr<IWbemServices>& svc) -> HRESULT {
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (SUCCEEDED(hr)) {
        hr = locator->ConnectServer(_bstr_t(L"ROOT\\subscription"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    }
    if (SUCCEEDED(hr)) {
        hr = CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }
    return hr;
}

// The bindings' paths and what each binds ("__EventFilter.Name=\"x\"", "CommandLineEventConsumer.Name=\"y\"").
struct Binding {
    std::wstring path, filter, consumer;
};

auto bindings(IWbemServices* svc) -> std::vector<Binding> {
    std::vector<Binding> out;
    ComPtr<IEnumWbemClassObject> rows;
    if (FAILED(svc->CreateInstanceEnum(_bstr_t(L"__FilterToConsumerBinding"), WBEM_FLAG_FORWARD_ONLY, nullptr, &rows))) {
        return out;
    }
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG got = 0;
        if (rows->Next(5000, 1, &o, &got) != WBEM_S_NO_ERROR || got == 0) {
            break;
        }
        out.push_back({text(o.Get(), L"__RELPATH"), text(o.Get(), L"Filter"), text(o.Get(), L"Consumer")});
    }
    return out;
}

// "…:__EventFilter.Name=\"x\"" ends with the path `relative` names (the namespace may stand before it).
auto same_object(std::wstring_view full, std::wstring_view relative) -> bool {
    return full == relative || (full.size() > relative.size() && full.ends_with(relative) && full[full.size() - relative.size() - 1] == L':');
}

} // namespace

auto approved_key(const StartupEntry& entry) -> std::wstring_view {
    switch (entry.place) {
    case Place::Run: return L"Run";
    case Place::Run32: return L"Run32";
    case Place::StartupFolder:
    case Place::CommonStartupFolder: return L"StartupFolder";
    default: return {};
    }
}

auto set_startup_approved(const RegistryView& reg, const StartupEntry& entry, bool enabled) -> core::Result<void> {
    const auto which = approved_key(entry);
    if (which.empty()) {
        return core::make_error(core::Code::InvalidArgument, std::string{place_name(entry.place)} + " is not switched through StartupApproved");
    }
    const Hive hive = entry.machine_wide ? Hive::Machine : Hive::User;
    const auto [root, path] = reg.locate(hive, std::wstring{kApproved} + std::wstring{which});
    const REGSAM view = !reg.is_test() && hive == Hive::Machine ? KEY_WOW64_64KEY : 0;
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | view, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        return core::os_error(static_cast<std::uint32_t>(st), "opening StartupApproved\\" + utf8(which));
    }
    // As Task Manager writes it: 02 (on) or 03 (off), three zeros, and when it was switched off.
    std::array<BYTE, 12> data{};
    data[0] = enabled ? 2 : 3;
    if (!enabled) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        std::memcpy(data.data() + 4, &now, sizeof now);
    }
    st = RegSetValueExW(key, entry.name.c_str(), 0, REG_BINARY, data.data(), static_cast<DWORD>(data.size()));
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        return core::os_error(static_cast<std::uint32_t>(st), "writing StartupApproved\\" + utf8(which) + " " + utf8(entry.name));
    }
    return {};
}

auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> {
    const std::wstring full{path};
    const auto slash = full.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= full.size()) {
        return core::make_error(core::Code::InvalidArgument, "not a task's path: " + utf8(full));
    }
    const std::wstring folder = slash == 0 ? std::wstring{L"\\"} : full.substr(0, slash);
    const std::wstring name = full.substr(slash + 1);
    return on_own_mta([&]() -> core::Result<void> {
        ComPtr<ITaskService> service;
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
        if (SUCCEEDED(hr)) {
            hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        }
        ComPtr<ITaskFolder> f;
        if (SUCCEEDED(hr)) {
            hr = service->GetFolder(_bstr_t(folder.c_str()), &f);
        }
        ComPtr<IRegisteredTask> task;
        if (SUCCEEDED(hr)) {
            hr = f->GetTask(_bstr_t(name.c_str()), &task);
        }
        if (SUCCEEDED(hr)) {
            hr = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
        }
        if (FAILED(hr)) {
            return std::unexpected{com_error(hr, (enabled ? "switching on the task " : "switching off the task ") + utf8(full))};
        }
        return {};
    });
}

auto take_wmi_subscription(const WmiSubscription& s) -> core::Result<std::string> {
    const std::wstring filter_path = L"__EventFilter.Name=\"" + key_text(wide(s.filter)) + L"\"";
    const std::wstring consumer_path = wide(s.consumer_class) + L".Name=\"" + key_text(wide(s.consumer)) + L"\"";
    return on_own_mta([&]() -> core::Result<std::string> {
        ComPtr<IWbemServices> svc;
        if (const HRESULT hr = connect(svc); FAILED(hr)) {
            return std::unexpected{com_error(hr, "opening WMI root\\subscription")};
        }
        ComPtr<IWbemClassObject> filter, consumer;
        if (const HRESULT hr = svc->GetObject(_bstr_t(filter_path.c_str()), 0, nullptr, &filter, nullptr); FAILED(hr)) {
            return std::unexpected{com_error(hr, "the WMI filter " + s.filter)};
        }
        if (const HRESULT hr = svc->GetObject(_bstr_t(consumer_path.c_str()), 0, nullptr, &consumer, nullptr); FAILED(hr)) {
            return std::unexpected{com_error(hr, "the WMI consumer " + s.consumer)};
        }
        const auto all = bindings(svc.Get());
        const Binding* bound = nullptr;
        bool filter_shared = false, consumer_shared = false;
        for (const auto& b : all) {
            const bool f = same_object(b.filter, filter_path);
            const bool c = same_object(b.consumer, consumer_path);
            if (f && c && bound == nullptr) {
                bound = &b;
            } else {
                filter_shared = filter_shared || f;
                consumer_shared = consumer_shared || c;
            }
        }
        if (bound == nullptr) {
            return std::unexpected{core::Error{core::Code::NotFound, "no binding of the WMI filter " + s.filter + " to " + s.consumer}};
        }
        ComPtr<IWbemClassObject> binding;
        if (const HRESULT hr = svc->GetObject(_bstr_t(bound->path.c_str()), 0, nullptr, &binding, nullptr); FAILED(hr)) {
            return std::unexpected{com_error(hr, "the WMI binding of " + s.filter)};
        }
        // All three as MOF, before anything is taken off: what wmi.restore compiles back.
        const std::wstring mof = L"#pragma namespace(\"\\\\\\\\.\\\\root\\\\subscription\")\n" + object_text(filter.Get()) +
                                 object_text(consumer.Get()) + object_text(binding.Get());
        auto out = core::utf8::to_utf8(mof);
        if (!out) {
            return std::unexpected{core::Error{core::Code::Internal, "the subscription's MOF could not be kept"}};
        }
        // The binding first: without it nothing runs. A filter or consumer another binding uses stays.
        if (const HRESULT hr = svc->DeleteInstance(_bstr_t(bound->path.c_str()), 0, nullptr, nullptr); FAILED(hr)) {
            return std::unexpected{com_error(hr, "taking off the WMI binding of " + s.filter)};
        }
        if (!consumer_shared) {
            (void)svc->DeleteInstance(_bstr_t(consumer_path.c_str()), 0, nullptr, nullptr);
        }
        if (!filter_shared) {
            (void)svc->DeleteInstance(_bstr_t(filter_path.c_str()), 0, nullptr, nullptr);
        }
        return std::move(*out);
    });
}

auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> {
    const std::wstring file{program};
    const std::wstring params{arguments};
    SHELLEXECUTEINFOW info{.cbSize = sizeof(SHELLEXECUTEINFOW),
                           .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
                           .lpVerb = L"open",
                           .lpFile = file.c_str(),
                           .lpParameters = params.empty() ? nullptr : params.c_str(),
                           .nShow = SW_SHOWNORMAL};
    if (!ShellExecuteExW(&info)) {
        return core::os_error(GetLastError(), "starting the uninstaller " + utf8(file));
    }
    std::uint32_t pid = 0;
    if (info.hProcess != nullptr) {
        pid = GetProcessId(info.hProcess);
        CloseHandle(info.hProcess);
    }
    return pid;
}

auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void> {
    std::wstring name = path;
    PACL old = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (const DWORD r = GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old, nullptr, &sd);
        r != ERROR_SUCCESS) {
        return core::os_error(r, "reading the rights of " + utf8(path));
    }
    const core::win::LocalPtr<PSECURITY_DESCRIPTOR> owned_sd{sd}; // `old` points into it
    std::array<BYTE, SECURITY_MAX_SID_SIZE> everyone{};
    DWORD sid_size = static_cast<DWORD>(everyone.size());
    CreateWellKnownSid(WinWorldSid, nullptr, everyone.data(), &sid_size);
    PACL fresh = nullptr;
    core::win::LocalPtr<PACL> made; // SetEntriesInAclW's list; the rebuilt one lives in `rebuilt`
    std::vector<BYTE> rebuilt;
    if (deny) {
        EXPLICIT_ACCESS_W ea{};
        ea.grfAccessPermissions = FILE_EXECUTE;
        ea.grfAccessMode = DENY_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.data());
        if (const DWORD r = SetEntriesInAclW(1, &ea, old, &fresh); r != ERROR_SUCCESS) {
            return core::os_error(r, "making the rule for " + utf8(path));
        }
        made.reset(fresh);
    } else {
        // The same list without the deny-execute for everyone.
        rebuilt.resize(old != nullptr ? old->AclSize : sizeof(ACL));
        auto* acl = reinterpret_cast<PACL>(rebuilt.data());
        InitializeAcl(acl, static_cast<DWORD>(rebuilt.size()), ACL_REVISION);
        for (DWORD i = 0; old != nullptr && i < old->AceCount; ++i) {
            void* ace = nullptr;
            if (!GetAce(old, i, &ace)) {
                continue;
            }
            const auto* header = static_cast<const ACE_HEADER*>(ace);
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
                const auto* denied = static_cast<const ACCESS_DENIED_ACE*>(ace);
                if ((denied->Mask & FILE_EXECUTE) != 0 && EqualSid(const_cast<DWORD*>(&denied->SidStart), everyone.data())) {
                    continue;
                }
            }
            AddAce(acl, ACL_REVISION, MAXDWORD, ace, header->AceSize);
        }
        fresh = acl;
    }
    const DWORD r = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, fresh, nullptr);
    if (r != ERROR_SUCCESS) {
        return core::os_error(r, (deny ? "denying running " : "allowing running ") + utf8(path));
    }
    return {};
}

auto defender_scan(const std::wstring& path) -> core::Result<os::DefenderScan> {
    // Defender's own command line: its platform folder's newest, else Program Files'.
    std::wstring tool;
    {
        std::array<wchar_t, MAX_PATH> pd{};
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", pd.data(), static_cast<DWORD>(pd.size()));
        std::error_code ec;
        const std::filesystem::path platform = std::filesystem::path{std::wstring{pd.data(), n}} / "Microsoft" / "Windows Defender" / "Platform";
        // "4.18.25110.5-0": compared number by number, not as text (4.18.9 is older than 4.18.25110).
        const auto version = [](const std::wstring& name) {
            std::vector<unsigned long> parts;
            for (std::size_t at = 0; at < name.size();) {
                wchar_t* end = nullptr;
                parts.push_back(std::wcstoul(name.c_str() + at, &end, 10));
                at = static_cast<std::size_t>(end - name.c_str());
                while (at < name.size() && !std::iswdigit(name[at])) {
                    ++at;
                }
            }
            return parts;
        };
        std::vector<unsigned long> newest;
        for (std::filesystem::directory_iterator it{platform, ec}, end; !ec && it != end; it.increment(ec)) {
            const auto v = version(it->path().filename().wstring());
            if (std::filesystem::exists(it->path() / "MpCmdRun.exe", ec) && (tool.empty() || v > newest)) {
                newest = v;
                tool = (it->path() / "MpCmdRun.exe").wstring();
            }
        }
        if (tool.empty()) {
            std::array<wchar_t, MAX_PATH> pf{};
            const DWORD m = GetEnvironmentVariableW(L"ProgramFiles", pf.data(), static_cast<DWORD>(pf.size()));
            tool = std::wstring{pf.data(), m} + L"\\Windows Defender\\MpCmdRun.exe";
        }
    }
    auto run = run_with_output(L"\"" + tool + L"\" -Scan -ScanType 3 -File \"" + path + L"\"", 10 * 60 * 1000);
    if (!run) {
        return core::make_error(run.error().code, "starting Defender's scan (" + utf8(tool) + "): " + run.error().message);
    }
    if (run->timed_out) {
        return core::make_error(core::Code::Timeout, "Defender's scan did not finish in 10 minutes");
    }
    os::DefenderScan out;
    out.code = run->code;
    out.output = std::move(run->output);
    out.threat = out.code == 2 || out.output.find("Threat ") != std::string::npos;
    return out;
}

auto run_with_output(std::wstring line, std::uint32_t limit_ms) -> core::Result<RunOutput> {
    SECURITY_ATTRIBUTES sa{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) {
        return core::os_error(GetLastError(), "making the output pipe");
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    // The pipe's write end is the one handle it inherits: none of the agent's others.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attrs(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(HANDLE), nullptr, nullptr)) {
        const DWORD e = GetLastError();
        CloseHandle(read);
        CloseHandle(write);
        return core::os_error(e, "limiting what it inherits");
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = write;
    si.StartupInfo.hStdError = write;
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD start_error = GetLastError();
    DeleteProcThreadAttributeList(list);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        return core::os_error(start_error, "starting " + utf8(line));
    }
    RunOutput out;
    std::thread reader{[&out, read] {
        std::array<char, 4096> buf{};
        DWORD got = 0;
        while (ReadFile(read, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (out.output.size() < 64 * 1024) {
                out.output.append(buf.data(), got);
            }
        }
    }};
    if (WaitForSingleObject(pi.hProcess, limit_ms) != WAIT_OBJECT_0) {
        out.timed_out = true;
        TerminateProcess(pi.hProcess, 1); // the process this call started, past its limit
        WaitForSingleObject(pi.hProcess, 5000);
    }
    reader.join(); // its pipe ends with the process (a child it started may hold it: ended above or ended by now)
    CloseHandle(read);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    out.code = static_cast<int>(code);
    return out;
}

auto put_wmi_subscription(std::string_view mof) -> core::Result<void> {
    // Unicode MOF, with its byte order mark, as the compiler reads it.
    const std::wstring text = wide(mof);
    std::vector<BYTE> buffer{0xFF, 0xFE};
    buffer.resize(2 + text.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + 2, text.data(), text.size() * sizeof(wchar_t));
    return on_own_mta([&]() -> core::Result<void> {
        ComPtr<IMofCompiler> compiler;
        HRESULT hr = CoCreateInstance(CLSID_MofCompiler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&compiler));
        if (FAILED(hr)) {
            return std::unexpected{com_error(hr, "the MOF compiler")};
        }
        WBEM_COMPILE_STATUS_INFO info{};
        hr = compiler->CompileBuffer(static_cast<long>(buffer.size()), buffer.data(), nullptr, nullptr, nullptr, nullptr, 0, 0, 0, &info);
        if (hr != WBEM_S_NO_ERROR) {
            return std::unexpected{com_error(info.hRes != 0 ? info.hRes : hr, std::format("putting the WMI subscription back (line {})", info.FirstLine))};
        }
        return {};
    });
}

} // namespace tools::winsys::change
