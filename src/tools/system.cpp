#include "tools/system.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"
#include "security/paths.hpp"
#include "tools/os.hpp"
#include "tools/winsys/private_rules.hpp"
#include "tools/winsys/sensitive.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>
#include <dxgi.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <optional>

#pragma comment(lib, "dxgi.lib")

namespace tools::sys::args {

struct Processes {
    std::optional<std::string> sort; // memory (default) or cpu
    std::optional<int> top;
};
struct ClipboardWrite {
    std::string text;
};
struct Open {
    std::string target;
};

} // namespace tools::sys::args

namespace tools::sys::out {

struct Cpu {
    std::string name;
    std::uint32_t cores = 0;
    std::uint32_t logical = 0;
    double load_percent = 0; // all processors, over the 200 ms the tool measured
};
struct Memory {
    std::uint64_t total = 0;
    std::uint64_t available = 0;
    std::uint32_t load_percent = 0;
    std::uint64_t commit_total = 0; // RAM and page file
    std::uint64_t commit_available = 0;
};
struct Disk {
    std::string drive;
    std::string type; // fixed, removable, network
    std::string label;
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> free;
};
struct Gpu {
    std::string id; // the adapter's LUID
    std::string name;
    std::uint64_t dedicated_memory = 0;
    std::uint64_t shared_memory = 0;
};
struct Os {
    std::string name;
    std::string version; // "24H2"
    std::uint32_t major = 0, minor = 0, build = 0;
};
struct Info {
    Cpu cpu;
    Memory memory;
    std::vector<Disk> disks;
    std::vector<Gpu> gpus;
    Os os;
    std::uint64_t uptime_s = 0;
    std::uint32_t dpi = 96;
};
struct Process {
    std::uint32_t pid = 0;
    std::uint32_t parent = 0;
    std::string name;
    std::uint32_t threads = 0;
    std::uint64_t memory = 0;  // working set, bytes
    std::uint64_t private_bytes = 0;
    double cpu_percent = 0;    // of the whole machine, over the 250 ms the tool measured
    bool readable = true;      // false: the system does not let this user look inside
};
struct Processes {
    std::vector<Process> processes;
    std::size_t total = 0;
};
struct Window {
    std::string title;
    std::string process;
    std::uint32_t pid = 0;
};
struct SensitiveWindow {
    bool sensitive = true;
    std::string reason; // why nothing of it is given
};
struct Clipboard {
    std::optional<std::string> text;
    std::optional<std::vector<std::string>> files;
    std::optional<std::string> kind; // "image": a picture, shown to the model in the next message
    std::optional<int> width;
    std::optional<int> height;
    bool empty = false;
};
struct Copied {
    std::uint64_t bytes = 0;
};
struct Opened {
    std::string target;
};

} // namespace tools::sys::out

template <>
struct glz::json_schema<tools::sys::args::ClipboardWrite> {
    schema text{.description = "The text to put on the clipboard."};
};
template <>
struct glz::json_schema<tools::sys::args::Open> {
    schema target{.description = "A web address (https://…), a file to open in its program, or a folder."};
};

template <>
struct glz::json_schema<tools::sys::args::Processes> {
    schema sort{.description = "\"memory\" (default) or \"cpu\": the biggest users first.",
                .enumeration = std::vector<std::string_view>{"memory", "cpu"}};
    schema top{.description = "At most this many processes (1-1000, default 25)."};
};

namespace tools::sys {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

// "", "{}" or "null": no arguments; anything else is refused.
auto no_args(std::string_view json) -> core::Result<void> {
    while (!json.empty() && (json.front() == ' ' || json.front() == '\n' || json.front() == '\r' || json.front() == '\t')) {
        json.remove_prefix(1);
    }
    while (!json.empty() && (json.back() == ' ' || json.back() == '\n' || json.back() == '\r' || json.back() == '\t')) {
        json.remove_suffix(1);
    }
    if (json.empty() || json == "{}" || json == "null") {
        return {};
    }
    glz::generic doc;
    if (!glz::read_json(doc, json) && doc.is_object() && doc.get_object().empty()) {
        return {};
    }
    return core::make_error(core::Code::InvalidArgument, "arguments: this tool takes none");
}

auto pathless(std::string_view tool, security::Risk risk, const std::filesystem::path& wd) -> security::Request {
    return {.tool = std::string{tool}, .risk = risk, .working_dir = wd};
}

template <class T>
auto envelope(std::string_view tool, const T& data) -> Outcome {
    auto json = core::json::write(data);
    if (!json) {
        return {2, wrap(tool, L"", R"("the result could not be written")", true)};
    }
    return {0, wrap(tool, L"", *json, false)};
}

auto registry_string(HKEY root, const wchar_t* key, const wchar_t* value) -> std::string {
    wchar_t buf[256]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring s{buf};
    while (!s.empty() && s.back() == L' ') {
        s.pop_back();
    }
    return utf8(s);
}

auto filetime(FILETIME t) -> std::uint64_t {
    return ULARGE_INTEGER{.LowPart = t.dwLowDateTime, .HighPart = t.dwHighDateTime}.QuadPart;
}

auto cores() -> std::uint32_t {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    std::vector<unsigned char> buf(size);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &size)) {
        return 0;
    }
    std::uint32_t n = 0;
    for (DWORD at = 0; at < size;) {
        const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
        ++n;
        at += p->Size;
    }
    return n;
}

auto logical_processors() -> std::uint32_t {
    return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

// Busy share of all processors over `ms`.
auto cpu_load(DWORD ms) -> double {
    FILETIME i0{}, k0{}, u0{}, i1{}, k1{}, u1{};
    GetSystemTimes(&i0, &k0, &u0);
    Sleep(ms);
    GetSystemTimes(&i1, &k1, &u1);
    const double idle = static_cast<double>(filetime(i1) - filetime(i0));
    const double total = static_cast<double>((filetime(k1) - filetime(k0)) + (filetime(u1) - filetime(u0))); // kernel includes idle
    return total > 0 ? std::clamp(100.0 * (total - idle) / total, 0.0, 100.0) : 0.0;
}

auto disks() -> std::vector<out::Disk> {
    std::vector<out::Disk> out;
    wchar_t buf[512]{};
    const DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buf)), buf);
    for (const wchar_t* d = buf; d < buf + n && *d != 0; d += wcslen(d) + 1) {
        const UINT type = GetDriveTypeW(d);
        out::Disk disk{.drive = utf8(std::wstring_view{d}.substr(0, 2))};
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            disk.type = type == DRIVE_FIXED ? "fixed" : "removable";
            ULARGE_INTEGER total{}, free{};
            if (GetDiskFreeSpaceExW(d, nullptr, &total, &free)) {
                disk.total = total.QuadPart;
                disk.free = free.QuadPart;
            }
            wchar_t label[MAX_PATH + 1]{};
            if (GetVolumeInformationW(d, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
                disk.label = utf8(label);
            }
        } else if (type == DRIVE_REMOTE) {
            disk.type = "network"; // not asked: a share that is gone can take a minute to answer
        } else {
            continue; // optical drives spin up; RAM disks and the rest are rare
        }
        out.push_back(std::move(disk));
    }
    return out;
}

auto gpus() -> std::vector<out::Gpu> {
    std::vector<out::Gpu> out;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return out;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            // The device, not the adapter object: a hybrid laptop's integrated GPU is listed twice
            // with two LUIDs.
            const std::string id = std::format("{:04x}:{:04x}:{:08x}:{:02x}", d.VendorId, d.DeviceId, d.SubSysId, d.Revision);
            if (std::ranges::none_of(out, [&](const out::Gpu& g) { return g.id == id; })) {
                out.push_back({id, utf8(d.Description), d.DedicatedVideoMemory, d.SharedSystemMemory});
            }
        }
        adapter.Reset();
    }
    return out;
}

auto os() -> out::Os {
    out::Os o;
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        OSVERSIONINFOW v{sizeof(v)};
        if (get(&v) == 0) {
            o.major = v.dwMajorVersion;
            o.minor = v.dwMinorVersion;
            o.build = v.dwBuildNumber;
        }
    }
    constexpr const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    o.name = registry_string(HKEY_LOCAL_MACHINE, kKey, L"ProductName");
    if (o.build >= 22000 && o.name.starts_with("Windows 10")) {
        o.name.replace(8, 2, "11"); // the registry keeps "Windows 10" on Windows 11
    }
    o.version = registry_string(HKEY_LOCAL_MACHINE, kKey, L"DisplayVersion");
    return o;
}

} // namespace

auto no_args_schema() -> std::string {
    return R"({"type":"object","properties":{},"additionalProperties":false})";
}

// system.info

auto info_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = no_args(json); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("system.info", security::Risk::Read, wd);
}

auto info_run(const security::Permit&, std::string_view) -> Outcome {
    out::Info data;
    data.cpu = {registry_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                L"ProcessorNameString"),
                cores(), logical_processors(), cpu_load(200)};
    MEMORYSTATUSEX m{sizeof(m)};
    if (GlobalMemoryStatusEx(&m)) {
        data.memory = {m.ullTotalPhys, m.ullAvailPhys, m.dwMemoryLoad, m.ullTotalPageFile, m.ullAvailPageFile};
    }
    data.disks = disks();
    data.gpus = gpus();
    data.os = os();
    data.uptime_s = GetTickCount64() / 1000;
    data.dpi = GetDpiForSystem();
    return envelope("system.info", data);
}

// process.list

auto processes_schema() -> std::string {
    auto s = glz::write_json_schema<args::Processes>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto processes_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    args::Processes a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    if (a.sort && *a.sort != "memory" && *a.sort != "cpu") {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"sort\" is \"memory\" or \"cpu\"");
    }
    return pathless("process.list", security::Risk::Read, wd);
}

auto processes_run(const security::Permit&, std::string_view json) -> Outcome {
    args::Processes a{};
    (void)glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json);
    core::win::FileHandle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap) {
        return failure("process.list", core::Error{core::Code::Io, "the process list cannot be read"});
    }
    struct Seen {
        out::Process p;
        core::win::KernelHandle handle;
        std::uint64_t cpu0 = 0;
    };
    std::vector<Seen> seen;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e)) {
        Seen s;
        s.p = {.pid = e.th32ProcessID, .parent = e.th32ParentProcessID, .name = utf8(e.szExeFile), .threads = e.cntThreads};
        s.handle = core::win::KernelHandle{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)};
        FILETIME c{}, x{}, k{}, u{};
        if (s.handle && GetProcessTimes(s.handle.get(), &c, &x, &k, &u)) {
            s.cpu0 = filetime(k) + filetime(u);
        } else {
            s.p.readable = false;
        }
        seen.push_back(std::move(s));
    }
    // CPU: what each used in the next 250 ms, of all processors.
    LARGE_INTEGER f{}, t0{}, t1{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Sleep(250);
    QueryPerformanceCounter(&t1);
    const double elapsed_100ns = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e7 / static_cast<double>(f.QuadPart);
    const double processors = static_cast<double>(std::max<std::uint32_t>(1, logical_processors()));
    for (auto& s : seen) {
        if (!s.p.readable) {
            continue;
        }
        FILETIME c{}, x{}, k{}, u{};
        if (GetProcessTimes(s.handle.get(), &c, &x, &k, &u)) {
            const double used = static_cast<double>(filetime(k) + filetime(u) - s.cpu0);
            s.p.cpu_percent = std::clamp(100.0 * used / (elapsed_100ns * processors), 0.0, 100.0);
        }
        PROCESS_MEMORY_COUNTERS_EX mc{};
        mc.cb = sizeof(mc);
        if (GetProcessMemoryInfo(s.handle.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc))) {
            s.p.memory = mc.WorkingSetSize;
            s.p.private_bytes = mc.PrivateUsage;
        }
    }
    out::Processes data{.total = seen.size()};
    for (auto& s : seen) {
        data.processes.push_back(std::move(s.p));
    }
    if (a.sort.value_or("memory") == "cpu") {
        std::ranges::stable_sort(data.processes, [](const auto& x, const auto& y) { return x.cpu_percent > y.cpu_percent; });
    } else {
        std::ranges::stable_sort(data.processes, [](const auto& x, const auto& y) { return x.memory > y.memory; });
    }
    data.processes.resize(std::min(data.processes.size(), static_cast<std::size_t>(std::clamp(a.top.value_or(25), 1, 1000))));
    return envelope("process.list", data);
}

// window.active

auto window_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = no_args(json); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("window.active", security::Risk::Read, wd);
}

auto window_run(const security::Permit&, std::string_view) -> Outcome {
    return window_of(GetForegroundWindow());
}

auto window_of(void* handle) -> Outcome {
    out::Window data;
    if (HWND w = static_cast<HWND>(handle)) {
        // A password manager, a bank, a private window, the user's list: not even its title (rule 7).
        if (const auto s = winsys::sensitivity_of(winsys::facts_of(w), winsys::load_private(winsys::private_file())); s.sensitive) {
            return envelope("window.active", out::SensitiveWindow{.sensitive = true, .reason = s.reason});
        }
        wchar_t title[512]{};
        GetWindowTextW(w, title, static_cast<int>(std::size(title)));
        data.title = utf8(title);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        data.pid = pid;
        if (core::win::KernelHandle p{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}) {
            wchar_t image[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(p.get(), 0, image, &size)) {
                data.process = utf8(std::filesystem::path{image}.filename().wstring());
            }
        }
    }
    return envelope("window.active", data);
}

// clipboard.read

auto clipboard_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto ok = no_args(json); !ok) {
        return std::unexpected{ok.error()};
    }
    return pathless("clipboard.read", security::Risk::Clipboard, wd);
}

auto clipboard_run(const security::Permit& permit, std::string_view) -> Outcome {
    if (permit.risk() != security::Risk::Clipboard) {
        return failure("clipboard.read", core::Error{core::Code::Policy, "a clipboard read needs its own permission"});
    }
    auto copied = os::current().read_clipboard();
    if (!copied) {
        return failure("clipboard.read", copied.error());
    }
    out::Clipboard data{.text = std::move(copied->text), .files = std::move(copied->files)};
    std::optional<ImageOut> shown;
    if (copied->picture) {
        auto made = image::for_model(*copied->picture, true, "clipboard"); // copied screenshots: small text
        if (!made) {
            return failure("clipboard.read", made.error());
        }
        data.kind = "image";
        data.width = made->width;
        data.height = made->height;
        shown = std::move(*made);
    }
    data.empty = !data.text && !data.files && !shown;
    auto o = envelope("clipboard.read", data);
    if (shown) {
        o.images.push_back(std::move(*shown));
    }
    return o;
}

// clipboard.write

auto clipboard_write_schema() -> std::string {
    auto s = glz::write_json_schema<args::ClipboardWrite>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto clipboard_write_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    args::ClipboardWrite a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    auto r = pathless("clipboard.write", security::Risk::Clipboard, wd);
    r.bytes = a.text.size();
    return r;
}

auto clipboard_write_preview(std::string_view json, const security::Request&) -> std::string {
    args::ClipboardWrite a{};
    if (glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(a, json)) {
        return {};
    }
    // The card shows what would be put there: it is the user who pastes it afterwards.
    constexpr std::size_t kShown = 2000;
    std::size_t n = std::min(a.text.size(), kShown);
    while (n > 0 && n < a.text.size() && (static_cast<unsigned char>(a.text[n]) & 0xC0) == 0x80) {
        --n;
    }
    return a.text.substr(0, n) + (n < a.text.size() ? std::format("\n… and {} bytes more", a.text.size() - n) : "");
}

auto clipboard_write_run(const security::Permit& permit, std::string_view json) -> Outcome {
    args::ClipboardWrite a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(a, json)) {
        return failure("clipboard.write", core::Error{core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json)});
    }
    if (permit.risk() != security::Risk::Clipboard) {
        return failure("clipboard.write", core::Error{core::Code::Policy, "a clipboard write needs its own permission"});
    }
    if (auto done = os::current().set_clipboard(a.text); !done) {
        return failure("clipboard.write", done.error());
    }
    return envelope("clipboard.write", out::Copied{a.text.size()});
}

// system.open

namespace {

// "https:", "ms-settings:": a scheme of two letters or more ("C:" is a drive).
auto is_url(std::string_view s) -> bool {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos || colon < 2) {
        return false;
    }
    return std::ranges::all_of(s.substr(0, colon), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    });
}

} // namespace

auto open_schema() -> std::string {
    auto s = glz::write_json_schema<args::Open>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto open_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    args::Open a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    if (a.target.empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"target\" is empty");
    }
    auto r = pathless("system.open", security::Risk::Execute, wd);
    r.opens = true;
    if (is_url(a.target)) {
        r.command = a.target;
    } else {
        auto w = core::utf8::to_wide(a.target);
        if (!w) {
            return core::make_error(core::Code::InvalidArgument, "arguments: \"target\" is not UTF-8");
        }
        r.paths.push_back(std::move(*w));
    }
    return r;
}

auto open_run(const security::Permit& permit, std::string_view) -> Outcome {
    if (permit.risk() != security::Risk::Execute || !permit.request().opens) {
        return failure("system.open", core::Error{core::Code::Policy, "opening needs its own permission"});
    }
    std::wstring target;
    if (!permit.paths().empty()) {
        // Looked up again: what was decided must still be what opens (a document swapped for a
        // program after the decision would run it).
        const auto& decided = permit.paths()[0];
        const auto now = security::paths::canonical(decided.logical.empty() ? decided.path : decided.logical, decided.path);
        if (!now || CompareStringOrdinal(now->path.c_str(), -1, decided.path.c_str(), -1, TRUE) != CSTR_EQUAL) {
            return failure("system.open", core::Error{core::Code::Policy, "it is not what the permission was given for"});
        }
        target = now->path;
    } else {
        auto w = core::utf8::to_wide(permit.request().command);
        if (!w) {
            return failure("system.open", core::Error{core::Code::InvalidArgument, "not UTF-8"});
        }
        target = std::move(*w);
    }
    if (auto done = os::current().open(target); !done) {
        return failure("system.open", done.error());
    }
    return envelope("system.open", out::Opened{utf8(target)});
}

} // namespace tools::sys
