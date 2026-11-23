#include "app/keycheck.hpp"

#include "core/log.hpp"
#include "net/http.hpp"
#include "net/proxy.hpp"

#include <chrono>
#include <format>
#include <future>
#include <memory>

namespace app::keycheck {

auto looks_right(std::string_view key) -> bool {
    const auto& by = ai::catalog::default_provider(); // the form of its keys: how they start, how long at least
    const std::string_view kPrefix = by.key_prefix;
    if (!key.starts_with(kPrefix) || key.size() < kPrefix.size() + by.key_min)
        return false;
    for (const char c : key.substr(kPrefix.size())) {
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum)
            return false;
    }
    return true;
}

auto check(std::string_view key, std::string_view proxy, std::string_view host) -> Result {
    const auto unreachable = [](std::string_view why) {
        return Result{Verdict::Unreachable, "Couldn't check the key: " + std::string{why} + ". Try again."};
    };
    const auto& by = ai::catalog::provider_of(ai::catalog::default_model()); // one provider until keys are per provider in the interface (stage 4d)
    // A network thread of its own, for this one request (the agent's is made at its first task).
    auto made = net::follows_system(proxy) ? net::init({.proxy = {}, .system = [] { return net::system_settings(); }})
                                           : net::init({.proxy = net::settings_of(proxy, net::system_settings()), .system = {}});
    if (!made)
        return unreachable(made.error().message);
    auto done = std::make_shared<std::promise<core::Result<net::Response>>>();
    auto answer = done->get_future();
    net::Request request{.method = "GET",
                         .url = std::string{host.empty() ? by.host : host} + "/models",
                         .headers = {{"Authorization", "Bearer " + std::string{key}}, {"Accept", "application/json"}},
                         .connect_timeout = std::chrono::milliseconds{10'000},
                         .idle_timeout = std::chrono::milliseconds{15'000},
                         .retries = 0};
    (void)net::start(*made.value(), std::move(request),
                     {.on_headers = {}, .on_data = {}, .on_done = [done](core::Result<net::Response> r) { done->set_value(std::move(r)); }});
    const auto r = answer.get();
    if (!r)
        return unreachable(r.error().message);
    if (r->status >= 200 && r->status < 300) {
        WA_INFO("app", "{} key accepted", by.name);
        return {Verdict::Accepted, {}};
    }
    WA_INFO("app", "{} key check: HTTP {}", by.name, r->status);
    if (r->status == 401 || r->status == 403)
        return {Verdict::Rejected, std::format("{} doesn't accept this key", by.name)};
    return unreachable(ai::chat::error_of(by, *r).message);
}

} // namespace app::keycheck
