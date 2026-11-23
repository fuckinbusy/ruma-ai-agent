#include "app/monitor.hpp"

#include <windows.h>

#include <Wbemidl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace app::monitor {
namespace {

auto ticks(const FILETIME& t) -> std::uint64_t {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

auto percent(std::uint64_t part, std::uint64_t whole) -> std::optional<float> {
    if (whole == 0) {
        return std::nullopt;
    }
    return static_cast<float>(100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

// "C:\" — the drive Windows is on.
auto system_root() -> std::wstring {
    wchar_t dir[MAX_PATH + 1]{};
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH + 1);
    if (n < 3) {
        return L"C:\\";
    }
    return std::wstring{dir, 3};
}

// A BSTR for the length of a call.
class Bstr {
public:
    explicit Bstr(const wchar_t* s) : s_(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(s_); }
    Bstr(const Bstr&) = delete;
    auto operator=(const Bstr&) -> Bstr& = delete;
    [[nodiscard]] auto get() const noexcept -> BSTR { return s_; }

private:
    BSTR s_;
};

// ROOT\WMI MSAcpi_ThermalZoneTemperature: the hottest zone, from tenths of a kelvin. Many
// computers answer "not supported" even to an administrator (docs/spikes/phase-5.md).
auto wmi_query() -> WmiAnswer {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = init == S_OK || init == S_FALSE;
    WmiAnswer out{.hr = E_FAIL, .celsius = std::nullopt};
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWbemLocator> locator;
        ComPtr<IWbemServices> services;
        const Bstr ns{L"ROOT\\WMI"};
        out.hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
        if (SUCCEEDED(out.hr)) {
            out.hr = locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        }
        if (SUCCEEDED(out.hr)) {
            CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            const Bstr lang{L"WQL"};
            const Bstr query{L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"};
            ComPtr<IEnumWbemClassObject> rows;
            out.hr = services->ExecQuery(lang.get(), query.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
            if (SUCCEEDED(out.hr)) {
                for (;;) {
                    ComPtr<IWbemClassObject> row;
                    ULONG got = 0;
                    const HRESULT next = rows->Next(2000, 1, &row, &got);
                    if (FAILED(next)) {
                        out.hr = next; // "not supported" comes here: the query itself is accepted
                        break;
                    }
                    if (got == 0) {
                        break;
                    }
                    VARIANT value{};
                    if (SUCCEEDED(row->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr))) {
                        const double tenths = value.vt == VT_I4 ? value.lVal : value.vt == VT_UI4 ? value.ulVal : 0;
                        const auto c = static_cast<float>(tenths / 10.0 - 273.15);
                        if (tenths > 0 && c > 0.0f && c < 150.0f) {
                            out.celsius = out.celsius ? std::max(*out.celsius, c) : c;
                        }
                    }
                    VariantClear(&value);
                }
            }
        }
    }
    if (uninit) {
        CoUninitialize();
    }
    return out;
}

class Machine final : public Source {
public:
    explicit Machine(bool elevated) : elevated_(elevated), root_(system_root()) {}
    ~Machine() override {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
        }
    }
    Machine(const Machine&) = delete;
    auto operator=(const Machine&) -> Machine& = delete;

    auto read(bool gpu) -> Reading override {
        Reading r;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const std::uint64_t i = ticks(idle);
            const std::uint64_t t = ticks(kernel) + ticks(user); // kernel includes idle
            if (have_cpu_ && t > total_) {
                const double busy = static_cast<double>((t - total_) - std::min(t - total_, i - idle_));
                r.cpu = static_cast<float>(std::clamp(100.0 * busy / static_cast<double>(t - total_), 0.0, 100.0));
            }
            idle_ = i;
            total_ = t;
            have_cpu_ = true;
        }
        MEMORYSTATUSEX ms{sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) {
            r.memory_total = ms.ullTotalPhys;
            r.memory_used = ms.ullTotalPhys - std::min(ms.ullTotalPhys, ms.ullAvailPhys);
        }
        ULARGE_INTEGER total{}, free_bytes{};
        if (GetDiskFreeSpaceExW(root_.c_str(), nullptr, &total, &free_bytes)) {
            r.disk_total = total.QuadPart;
            r.disk_used = total.QuadPart - std::min(total.QuadPart, free_bytes.QuadPart);
        }
        if (gpu) {
            r.gpu = gpu_load();
        }
        return r;
    }

    auto temperature() -> Temperature override {
        return temperature_of(wmi_query(), elevated_); // asked whatever the rights: Windows says why not
    }

private:
    // The busiest adapter's 3D engines, summed over the processes using it (as Task Manager).
    auto gpu_load() -> std::optional<float> {
        if (failed_) {
            return std::nullopt;
        }
        if (query_ == nullptr) {
            if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS ||
                PdhAddEnglishCounterW(query_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
                failed_ = true;
                return std::nullopt;
            }
            PdhCollectQueryData(query_); // a rate needs two collections
            return std::nullopt;
        }
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA) {
            return 0.0f; // no 3D engine is in use
        }
        buffer_.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        // "pid_1234_luid_0x00000000_0x0000D1F2_phys_0_eng_3_engtype_3D": the adapter is the luid.
        adapters_.clear();
        for (DWORD k = 0; k < count; ++k) {
            const std::wstring_view name{items[k].szName};
            const auto from = name.find(L"luid_");
            const auto to = name.find(L"_eng_");
            const auto key = from == std::wstring_view::npos ? name : name.substr(from, to == std::wstring_view::npos ? name.npos : to - from);
            const double v = items[k].FmtValue.CStatus == ERROR_SUCCESS ? items[k].FmtValue.doubleValue : 0.0;
            auto it = std::ranges::find(adapters_, key, &std::pair<std::wstring_view, double>::first);
            if (it == adapters_.end()) {
                adapters_.emplace_back(key, v);
            } else {
                it->second += v;
            }
        }
        double busiest = 0;
        for (const auto& [key, v] : adapters_) {
            busiest = std::max(busiest, v);
        }
        return static_cast<float>(std::clamp(busiest, 0.0, 100.0));
    }

    bool elevated_;
    std::wstring root_;
    bool have_cpu_ = false;
    std::uint64_t idle_ = 0;
    std::uint64_t total_ = 0;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    bool failed_ = false;
    std::vector<unsigned char> buffer_;
    std::vector<std::pair<std::wstring_view, double>> adapters_;
};

} // namespace

auto wmi_temperature() -> WmiAnswer {
    return wmi_query();
}

auto temperature_of(const WmiAnswer& answer, bool elevated) -> Temperature {
    if (SUCCEEDED(answer.hr) && answer.celsius) {
        return {.state = TemperatureState::Value, .celsius = *answer.celsius};
    }
    const bool refused = answer.hr == WBEM_E_ACCESS_DENIED || answer.hr == E_ACCESSDENIED;
    return {.state = refused && !elevated ? TemperatureState::NeedsAdmin : TemperatureState::NotAvailable};
}

auto temperature_text(const Temperature& t) -> std::string {
    switch (t.state) {
    case TemperatureState::Value: return std::format("{:.0f} \xC2\xB0" "C", std::floor(t.celsius + 0.5f));
    case TemperatureState::NeedsAdmin: return "temp needs admin"; // whole in its tile
    case TemperatureState::NotAvailable: break;
    }
    return "no temperature";
}

auto machine(bool elevated) -> std::unique_ptr<Source> {
    return std::make_unique<Machine>(elevated);
}

void Monitor::Ring::push(float v) {
    if (size < points.size()) {
        points[size++] = v;
        return;
    }
    std::shift_left(points.begin(), points.end(), 1);
    points.back() = v;
}

Monitor::Monitor(std::unique_ptr<Source> source) : source_(std::move(source)) {}

Monitor::~Monitor() {
    stop();
}

void Monitor::start(std::function<void()> sampled, std::chrono::milliseconds every) {
    stop();
    thread_ = std::jthread{[this, sampled = std::move(sampled), every](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            sample();
            if (notify_ && sampled) {
                sampled();
            }
            std::unique_lock lock{mutex_};
            wake_.wait_for(lock, stop, every, [] { return false; });
        }
    }};
}

void Monitor::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Monitor::sample(bool gpu_too) {
    Reading r;
    {
        std::scoped_lock reading{read_mutex_}; // the agent may ask for a reading while the thread takes one
        r = source_->read(gpu_ || gpu_too);
    }
    std::vector<std::pair<std::function<void(float)>, float>> fired;
    {
        std::scoped_lock lock{mutex_};
        latest_ = r;
        ++samples_;
        std::array<std::optional<float>, 4> values{r.cpu, percent(r.memory_used, r.memory_total), r.gpu,
                                                   percent(r.disk_used, r.disk_total)};
        for (std::size_t m = 0; m < values.size(); ++m) {
            if (values[m]) {
                rings_[m].push(*values[m]);
            }
        }
        for (auto& w : watches_) {
            const auto& v = values[static_cast<std::size_t>(w.metric)];
            if (!v) {
                continue;
            }
            if (*v > w.above && !w.over) {
                w.over = true;
                fired.emplace_back(w.fire, *v);
            } else if (*v <= w.above) {
                w.over = false;
            }
        }
    }
    for (auto& [fire, value] : fired) {
        fire(value);
    }
}

auto Monitor::series(Metric m) const -> std::span<const float> {
    const auto& ring = rings_[static_cast<std::size_t>(m)];
    return {ring.points.data(), ring.size};
}

auto Monitor::snapshot() const -> Snapshot {
    std::scoped_lock lock{mutex_};
    Snapshot s{.latest = latest_, .series = {}, .samples = samples_};
    for (std::size_t m = 0; m < rings_.size(); ++m) {
        s.series[m].assign(rings_[m].points.begin(), rings_[m].points.begin() + static_cast<std::ptrdiff_t>(rings_[m].size));
    }
    return s;
}

auto Monitor::temperature() -> Temperature {
    return source_->temperature();
}

auto Monitor::watch(Metric metric, float above, std::function<void(float)> fire) -> std::uint64_t {
    std::scoped_lock lock{mutex_};
    const auto id = next_watch_++;
    watches_.push_back({.id = id, .metric = metric, .above = above, .over = false, .fire = std::move(fire)});
    return id;
}

void Monitor::unwatch(std::uint64_t id) {
    std::scoped_lock lock{mutex_};
    std::erase_if(watches_, [&](const Watch& w) { return w.id == id; });
}

} // namespace app::monitor
