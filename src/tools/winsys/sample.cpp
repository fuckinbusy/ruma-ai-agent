#include "tools/winsys/sample.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <thread>

#pragma comment(lib, "pdh.lib")

namespace tools::winsys {
namespace {

auto filetime(const FILETIME& f) -> std::uint64_t {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

auto cpu_time(HANDLE h) -> std::optional<std::uint64_t> {
    FILETIME c{}, x{}, k{}, u{};
    if (!GetProcessTimes(h, &c, &x, &k, &u)) {
        return std::nullopt;
    }
    return filetime(k) + filetime(u);
}

// PDH's GPU Engine counters, a value per engine instance "pid_1234_luid_…_eng_0_engtype_3D".
class GpuCounters {
public:
    GpuCounters() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
            counter_ = nullptr;
        }
        PdhCollectQueryData(query_);
    }
    ~GpuCounters() {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
        }
    }
    GpuCounters(const GpuCounters&) = delete;
    GpuCounters& operator=(const GpuCounters&) = delete;

    // Each process's busiest engine, percent, since the last call.
    auto read() -> std::map<std::uint32_t, float> {
        std::map<std::uint32_t, float> out;
        if (query_ == nullptr || counter_ == nullptr || PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return out;
        }
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) {
            return out;
        }
        buf_.resize(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) {
            return out;
        }
        for (DWORD i = 0; i < count; ++i) {
            const std::wstring_view name{items[i].szName};
            if (!name.starts_with(L"pid_")) {
                continue;
            }
            const auto pid = static_cast<std::uint32_t>(std::wcstoul(name.data() + 4, nullptr, 10));
            auto& v = out[pid];
            v = std::max(v, static_cast<float>(std::clamp(items[i].FmtValue.doubleValue, 0.0, 100.0)));
        }
        return out;
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    std::vector<std::byte> buf_;
};

auto with_windows() -> std::set<std::uint32_t> {
    std::set<std::uint32_t> pids;
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            if (IsWindowVisible(w) && GetWindow(w, GW_OWNER) == nullptr) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                reinterpret_cast<std::set<std::uint32_t>*>(p)->insert(pid);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pids));
    return pids;
}

struct Watched {
    Sample s;
    core::win::KernelHandle handle;
    std::uint64_t last = 0;
};

} // namespace

auto sample_processes(std::chrono::seconds window, std::stop_token stop, const std::function<void(int second, int of)>& tick)
    -> std::vector<Sample> {
    const auto seconds = std::clamp<std::int64_t>(window.count(), 1, 60);
    std::vector<Watched> watched;
    {
        const core::win::FileHandle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
        if (!snap) {
            return {};
        }
        PROCESSENTRY32W e{sizeof(e)};
        for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e)) {
            Watched w;
            w.s.pid = e.th32ProcessID;
            w.s.name = core::utf8::to_utf8(e.szExeFile).value_or("");
            w.handle = core::win::KernelHandle{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)};
            if (w.handle) {
                w.last = cpu_time(w.handle.get()).value_or(0);
            }
            watched.push_back(std::move(w));
        }
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const double processors = std::max<DWORD>(1, si.dwNumberOfProcessors);
    GpuCounters gpu;
    LARGE_INTEGER f{}, t0{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (std::int64_t second = 0; second < seconds && !stop.stop_requested(); ++second) {
        for (int i = 0; i < 10 && !stop.stop_requested(); ++i) { // a stop is heard within 100 ms
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        const double elapsed = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e7 / static_cast<double>(f.QuadPart);
        t0 = t1;
        const auto engines = gpu.read();
        for (auto& w : watched) {
            if (!w.handle) {
                continue;
            }
            const auto now = cpu_time(w.handle.get());
            if (!now) {
                continue;
            }
            const double used = static_cast<double>(*now - w.last);
            w.last = *now;
            w.s.cpu.push_back(static_cast<float>(std::clamp(100.0 * used / (elapsed * processors), 0.0, 100.0)));
            const auto g = engines.find(w.s.pid);
            w.s.gpu.push_back(g == engines.end() ? 0.0f : g->second);
        }
        if (tick) {
            tick(static_cast<int>(second + 1), static_cast<int>(seconds));
        }
    }
    const auto windowed = with_windows();
    std::vector<Sample> out;
    for (auto& w : watched) {
        if (w.handle) {
            PROCESS_MEMORY_COUNTERS mc{};
            if (GetProcessMemoryInfo(w.handle.get(), &mc, sizeof(mc))) {
                w.s.memory = mc.WorkingSetSize;
            }
            wchar_t path[MAX_PATH * 2]{};
            DWORD size = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(w.handle.get(), 0, path, &size)) {
                w.s.path.assign(path, size);
            }
        }
        w.s.windowless = !windowed.contains(w.s.pid);
        out.push_back(std::move(w.s));
    }
    const auto mean = [](const Sample& s) {
        return s.cpu.empty() ? 0.0f : std::accumulate(s.cpu.begin(), s.cpu.end(), 0.0f) / static_cast<float>(s.cpu.size());
    };
    std::ranges::stable_sort(out, [&](const Sample& a, const Sample& b) {
        const float ma = mean(a), mb = mean(b);
        return ma != mb ? ma > mb : a.memory > b.memory;
    });
    out.resize(std::min<std::size_t>(out.size(), 50));
    return out;
}

} // namespace tools::winsys
