#include "reader/collect.hpp"

#include "tools/winsys/readonly_com.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <winevt.h>

#include <array>
#include <format>
#include <string>
#include <vector>

namespace reader {
namespace {

namespace rd = tools::winsys::reader;

auto utf8(std::wstring_view s) -> std::string {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

auto unix_of(const FILETIME& ft) -> std::int64_t {
    const auto ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return ticks == 0 ? 0 : static_cast<std::int64_t>(ticks / 10'000'000ULL) - 11'644'473'600LL;
}

auto unix_of(std::uint64_t filetime) -> std::int64_t {
    return unix_of(FILETIME{.dwLowDateTime = static_cast<DWORD>(filetime), .dwHighDateTime = static_cast<DWORD>(filetime >> 32)});
}

// A CIM datetime "20261028093000.000000+000", seconds since 1970 (the offset ignored).
auto cim_time(const std::wstring& s) -> std::int64_t {
    if (s.size() < 14) {
        return 0;
    }
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(_wtoi(s.substr(0, 4).c_str()));
    st.wMonth = static_cast<WORD>(_wtoi(s.substr(4, 2).c_str()));
    st.wDay = static_cast<WORD>(_wtoi(s.substr(6, 2).c_str()));
    st.wHour = static_cast<WORD>(_wtoi(s.substr(8, 2).c_str()));
    st.wMinute = static_cast<WORD>(_wtoi(s.substr(10, 2).c_str()));
    st.wSecond = static_cast<WORD>(_wtoi(s.substr(12, 2).c_str()));
    FILETIME ft{};
    return SystemTimeToFileTime(&st, &ft) ? unix_of(ft) : 0;
}

struct Evt {
    EVT_HANDLE h = nullptr;
    explicit Evt(EVT_HANDLE x = nullptr) : h(x) {}
    Evt(const Evt&) = delete;
    auto operator=(const Evt&) -> Evt& = delete;
    ~Evt() {
        if (h != nullptr) {
            EvtClose(h);
        }
    }
};

auto number_of(const EVT_VARIANT& v) -> std::uint64_t {
    switch (v.Type) {
    case EvtVarTypeUInt32: return v.UInt32Val;
    case EvtVarTypeUInt64: return v.UInt64Val;
    case EvtVarTypeInt32: return v.Int32Val < 0 ? 0 : static_cast<std::uint64_t>(v.Int32Val);
    case EvtVarTypeInt64: return v.Int64Val < 0 ? 0 : static_cast<std::uint64_t>(v.Int64Val);
    case EvtVarTypeUInt16: return v.UInt16Val;
    default: return 0;
    }
}

auto text_of(const EVT_VARIANT& v) -> std::wstring {
    return v.Type == EvtVarTypeString && v.StringVal != nullptr ? std::wstring{v.StringVal} : std::wstring{};
}

// The newest `most` events of `query` in the Diagnostics-Performance log, `paths` rendered from each.
template <class F>
auto each_event(const wchar_t* query, std::vector<const wchar_t*> paths, std::size_t most, F&& f) -> DWORD {
    const Evt q{EvtQuery(nullptr, L"Microsoft-Windows-Diagnostics-Performance/Operational", query,
                         EvtQueryChannelPath | EvtQueryReverseDirection)};
    if (q.h == nullptr) {
        return GetLastError();
    }
    const Evt ctx{EvtCreateRenderContext(static_cast<DWORD>(paths.size()), paths.data(), EvtRenderContextValues)};
    if (ctx.h == nullptr) {
        return GetLastError();
    }
    std::vector<std::byte> buf(16 * 1024);
    std::size_t seen = 0;
    while (seen < most) {
        std::array<EVT_HANDLE, 16> got{};
        DWORD n = 0;
        if (!EvtNext(q.h, static_cast<DWORD>(got.size()), got.data(), 2000, 0, &n)) {
            const DWORD e = GetLastError();
            return e == ERROR_NO_MORE_ITEMS ? ERROR_SUCCESS : e;
        }
        for (DWORD i = 0; i < n; ++i) {
            const Evt ev{got[i]};
            DWORD used = 0, count = 0;
            if (!EvtRender(ctx.h, ev.h, EvtRenderEventValues, static_cast<DWORD>(buf.size()), buf.data(), &used, &count)) {
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                    continue;
                }
                buf.resize(used);
                if (!EvtRender(ctx.h, ev.h, EvtRenderEventValues, static_cast<DWORD>(buf.size()), buf.data(), &used, &count)) {
                    continue;
                }
            }
            if (count == paths.size() && seen < most) {
                f(reinterpret_cast<const EVT_VARIANT*>(buf.data()));
                ++seen;
            }
        }
    }
    return ERROR_SUCCESS;
}

void read_events(rd::Data& d) {
    const DWORD boots = each_event(L"*[System[(EventID=100)]]",
                                   {L"Event/System/TimeCreated/@SystemTime", L"Event/EventData/Data[@Name='BootTime']"}, 20,
                                   [&](const EVT_VARIANT* v) {
                                       d.boots.push_back({.at = v[0].Type == EvtVarTypeFileTime ? unix_of(v[0].FileTimeVal) : 0,
                                                          .boot_ms = static_cast<std::uint32_t>(number_of(v[1]))});
                                   });
    const DWORD slow = each_event(L"*[System[(EventID=101)]]",
                                  {L"Event/System/TimeCreated/@SystemTime", L"Event/EventData/Data[@Name='FriendlyName']",
                                   L"Event/EventData/Data[@Name='Name']", L"Event/EventData/Data[@Name='TotalTime']",
                                   L"Event/EventData/Data[@Name='DegradationTime']"},
                                  100, [&](const EVT_VARIANT* v) {
                                      d.slow_starts.push_back({.name = utf8(text_of(v[1])),
                                                               .file = utf8(text_of(v[2])),
                                                               .total_ms = static_cast<std::uint32_t>(number_of(v[3])),
                                                               .degradation_ms = static_cast<std::uint32_t>(number_of(v[4])),
                                                               .at = v[0].Type == EvtVarTypeFileTime ? unix_of(v[0].FileTimeVal) : 0});
                                  });
    d.events_read = boots == ERROR_SUCCESS && slow == ERROR_SUCCESS;
    if (!d.events_read) {
        d.not_read.push_back(std::format("The boot log (Diagnostics-Performance): error {}", boots != ERROR_SUCCESS ? boots : slow));
    }
}

void read_threats(rd::Data& d) {
    namespace ro = tools::winsys::readonly;
    constexpr const wchar_t* ns = L"ROOT\\Microsoft\\Windows\\Defender";
    std::vector<std::pair<std::int64_t, std::string>> names;
    const bool named = ro::select(ns, L"SELECT ThreatID, ThreatName FROM MSFT_MpThreat", [&](const ro::Row& r) {
        names.emplace_back(r.number(L"ThreatID"), utf8(r.text(L"ThreatName")));
    });
    d.threats_read = named && ro::select(ns, L"SELECT ThreatID, Resources, ActionSuccess, InitialDetectionTime FROM MSFT_MpThreatDetection",
                                         [&](const ro::Row& r) {
                                             rd::Threat t;
                                             const auto id = r.number(L"ThreatID");
                                             for (const auto& [k, v] : names) {
                                                 if (k == id) {
                                                     t.name = v;
                                                 }
                                             }
                                             const auto res = r.texts(L"Resources");
                                             t.path = res.empty() ? std::string{} : utf8(res.front());
                                             t.resolved = r.flag(L"ActionSuccess");
                                             t.at = cim_time(r.text(L"InitialDetectionTime"));
                                             d.threats.push_back(std::move(t));
                                         });
    if (!d.threats_read) {
        d.not_read.emplace_back("Microsoft Defender's history of threats");
    }
}

// SYSTEM and the service accounts: what the user's own list of tasks does not show.
auto system_account(std::wstring a) -> bool {
    for (auto& c : a) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return a == L"system" || a == L"s-1-5-18" || a == L"s-1-5-19" || a == L"s-1-5-20" || a.ends_with(L"\\system") ||
           a.find(L"local service") != std::wstring::npos || a.find(L"network service") != std::wstring::npos || a == L"localsystem";
}

void read_tasks(rd::Data& d) {
    std::vector<tools::winsys::readonly::TaskRow> rows;
    std::vector<std::wstring> not_read;
    d.tasks_read = tools::winsys::readonly::tasks(rows, not_read);
    for (const auto& r : rows) {
        if (system_account(r.account)) {
            d.tasks.push_back({.path = utf8(r.path), .account = utf8(r.account), .command = utf8(r.command), .enabled = r.enabled});
        }
    }
    if (!d.tasks_read) {
        d.not_read.emplace_back("The Task Scheduler");
    }
    for (const auto& f : not_read) {
        d.not_read.push_back("Task folder " + utf8(f));
    }
}

void read_processes(rd::Data& d) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        d.not_read.emplace_back("The list of processes");
        return;
    }
    PROCESSENTRY32W e{.dwSize = sizeof(PROCESSENTRY32W)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        rd::Process p{.pid = e.th32ProcessID, .name = utf8(e.szExeFile), .path = {}};
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID); h != nullptr) {
            std::array<wchar_t, 32768> path{};
            DWORD n = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(h, 0, path.data(), &n)) {
                p.path = utf8({path.data(), n});
            }
            CloseHandle(h);
        }
        d.processes.push_back(std::move(p));
    }
    CloseHandle(snap);
}

// A folder's size: every file under it, links not followed; `budget` entries at most.
void measure(const std::wstring& dir, rd::Folder& f, std::size_t& budget, bool top) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (!top) {
            ++f.not_read;
        }
        return;
    }
    do {
        if (budget == 0) {
            break;
        }
        --budget;
        const std::wstring_view name{fd.cFileName};
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                measure(dir + L"\\" + std::wstring{name}, f, budget, false);
            }
        } else {
            f.bytes += (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            ++f.files;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

auto env(const wchar_t* name) -> std::wstring {
    std::array<wchar_t, MAX_PATH> v{};
    const DWORD n = GetEnvironmentVariableW(name, v.data(), static_cast<DWORD>(v.size()));
    return n > 0 && n < v.size() ? std::wstring{v.data(), n} : std::wstring{};
}

void read_folders(rd::Data& d) {
    const auto win = env(L"SystemRoot");
    const auto pd = env(L"ProgramData");
    const std::array<std::wstring, 6> dirs{win + L"\\Temp", win + L"\\Prefetch", win + L"\\ServiceProfiles", win + L"\\System32\\LogFiles",
                                           win + L"\\System32\\config", pd + L"\\Microsoft\\Windows Defender"};
    std::size_t budget = 400'000;
    for (const auto& dir : dirs) {
        rd::Folder f{.path = utf8(dir)};
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        measure(dir, f, budget, true);
        d.folders.push_back(std::move(f));
    }
}

// Defender's exclusions: a folder, a kind of file or a program it never scans (administrators only).
void read_exclusions(rd::Data& d) {
    bool any_denied = false;
    for (const wchar_t* kind : {L"Paths", L"Extensions", L"Processes", L"IpAddresses"}) {
        HKEY key = nullptr;
        const auto sub = std::wstring{L"SOFTWARE\\Microsoft\\Windows Defender\\Exclusions\\"} + kind;
        const LSTATUS s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (s == ERROR_ACCESS_DENIED) {
            any_denied = true;
            continue;
        }
        if (s != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD i = 0;; ++i) {
            std::array<wchar_t, 16384> name{};
            DWORD n = static_cast<DWORD>(name.size());
            if (RegEnumValueW(key, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            d.exclusions.push_back(utf8(kind) + ": " + utf8({name.data(), n}));
        }
        RegCloseKey(key);
    }
    d.exclusions_read = !any_denied;
    if (any_denied) {
        d.not_read.emplace_back("Microsoft Defender's exclusions");
    }
}

auto elevated() -> bool {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated != 0;
    CloseHandle(token);
    return yes;
}

} // namespace

auto collect() -> rd::Data {
    rd::Data d;
    d.elevated = elevated();
    read_events(d);
    read_threats(d);
    read_tasks(d);
    read_processes(d);
    read_folders(d);
    read_exclusions(d);
    return d;
}

} // namespace reader
