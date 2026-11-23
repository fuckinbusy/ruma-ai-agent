#include "tools/winsys/events.hpp"

#include "core/utf8.hpp"

#include <windows.h>
#include <winevt.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>

#pragma comment(lib, "wevtapi.lib")

namespace tools::winsys {
namespace {

constexpr std::size_t kMaxGroups = 50;
constexpr std::size_t kMaxRead = 2000;

// An event log handle, closed when it goes.
class Evt {
public:
    explicit Evt(EVT_HANDLE h) : h_{h} {}
    ~Evt() {
        if (h_ != nullptr) {
            EvtClose(h_);
        }
    }
    Evt(const Evt&) = delete;
    Evt& operator=(const Evt&) = delete;
    [[nodiscard]] auto get() const -> EVT_HANDLE { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    EVT_HANDLE h_;
};

auto message_of(EVT_HANDLE event, const std::wstring& provider, std::map<std::wstring, EVT_HANDLE>& publishers) -> std::string {
    auto [it, fresh] = publishers.try_emplace(provider, nullptr);
    if (fresh) {
        it->second = EvtOpenPublisherMetadata(nullptr, provider.c_str(), nullptr, 0, 0);
    }
    if (it->second == nullptr) {
        return {};
    }
    DWORD used = 0;
    EvtFormatMessage(it->second, event, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
    if (used == 0) {
        return {};
    }
    std::wstring text(used, L'\0');
    if (!EvtFormatMessage(it->second, event, 0, 0, nullptr, EvtFormatMessageEvent, used, text.data(), &used)) {
        return {};
    }
    text.resize(wcsnlen(text.c_str(), text.size()));
    if (text.size() > 400) {
        text.resize(400);
    }
    return core::utf8::to_utf8(text).value_or("");
}

void read_channel(const wchar_t* channel, const std::wstring& xpath, std::vector<RawEvent>& out, EventReport& report) {
    const Evt query{EvtQuery(nullptr, channel, xpath.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection)};
    if (!query) {
        report.not_read.push_back(core::utf8::to_utf8(channel).value_or("?"));
        return;
    }
    report.read = true;
    const Evt context{EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem)};
    std::map<std::wstring, EVT_HANDLE> publishers;
    EVT_HANDLE batch[64];
    DWORD got = 0;
    std::vector<std::byte> values;
    while (out.size() < kMaxRead && EvtNext(query.get(), 64, batch, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; ++i) {
            const Evt event{batch[i]};
            DWORD used = 0, count = 0;
            EvtRender(context.get(), event.get(), EvtRenderEventValues, 0, nullptr, &used, &count);
            values.assign(used, std::byte{0});
            if (!EvtRender(context.get(), event.get(), EvtRenderEventValues, used, values.data(), &used, &count)) {
                continue;
            }
            const auto* v = reinterpret_cast<const EVT_VARIANT*>(values.data());
            const std::wstring provider = v[EvtSystemProviderName].StringVal != nullptr ? v[EvtSystemProviderName].StringVal : L"";
            const auto ticks = v[EvtSystemTimeCreated].FileTimeVal;
            out.push_back({.provider = core::utf8::to_utf8(provider).value_or(""),
                           .id = v[EvtSystemEventID].UInt16Val,
                           .level = v[EvtSystemLevel].ByteVal,
                           .at = static_cast<std::int64_t>(ticks / 10'000'000ULL) - 11'644'473'600LL,
                           .message = message_of(event.get(), provider, publishers)});
        }
    }
    for (auto& [name, h] : publishers) {
        if (h != nullptr) {
            EvtClose(h);
        }
    }
}

} // namespace

auto summarize(std::span<const RawEvent> raw, int days, std::int64_t now) -> EventReport {
    EventReport r;
    r.read = true;
    const std::int64_t since = now - static_cast<std::int64_t>(days) * 86'400;
    std::map<std::pair<std::string, std::uint32_t>, EventGroup> groups;
    for (const auto& e : raw) {
        if (e.at < since) {
            continue;
        }
        ++r.total;
        auto& g = groups[{e.provider, e.id}];
        if (g.count == 0 || e.at > g.last) {
            g.provider = e.provider;
            g.id = e.id;
            g.level = e.level;
            g.last = e.at;
            g.message = e.message;
        }
        ++g.count;
    }
    for (auto& [key, g] : groups) {
        r.groups.push_back(std::move(g));
    }
    std::ranges::stable_sort(r.groups, [](const EventGroup& a, const EventGroup& b) { return a.count != b.count ? a.count > b.count : a.last > b.last; });
    if (r.groups.size() > kMaxGroups) {
        r.groups.resize(kMaxGroups);
    }
    return r;
}

auto crashes(int days) -> EventReport {
    days = std::clamp(days, 1, 30);
    const auto ms = static_cast<std::uint64_t>(days) * 86'400'000ULL;
    const auto when = std::format(L" and System[TimeCreated[timediff(@SystemTime) <= {}]]", ms);
    const std::wstring system =
        L"*[System[(Provider[@Name='Service Control Manager'] and (EventID=7031 or EventID=7034)) or "
        L"(Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41) or (Provider[@Name='EventLog'] and EventID=6008)]" + when + L"]";
    const std::wstring application =
        L"*[System[(Provider[@Name='Application Error'] and EventID=1000) or (Provider[@Name='Application Hang'] and EventID=1002) or "
        L"(Provider[@Name='Windows Error Reporting'] and EventID=1001)]" + when + L"]";
    std::vector<RawEvent> raw;
    EventReport probe;
    read_channel(L"System", system, raw, probe);
    read_channel(L"Application", application, raw, probe);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto r = summarize(raw, days, now);
    r.read = probe.read;
    r.not_read = std::move(probe.not_read);
    return r;
}

} // namespace tools::winsys
