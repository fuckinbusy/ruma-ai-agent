#pragma once

// net — the agent's network (01-ARCHITECTURE § net): one thread that waits in curl_multi_poll,
// commands from any thread through a queue and curl_multi_wakeup, the receivers' callbacks on
// that thread. A module with state: init() starts it, the owner's destructor ends it.

#include "core/error.hpp"
#include "net/proxy.hpp"

#include <functional>
#include <memory>

namespace net {

struct Config {
    ProxySettings proxy; // a fixed proxy (network.proxy names one); none: direct
    // The system's settings, read again for each request (set: `proxy` is not used) - a proxy a VPN
    // turns off or on is followed without a restart.
    std::function<ProxySettings()> system;
};

struct State;
struct StateDeleter {
    // Wakes the thread, tells every request still going that it is cancelled (once), joins.
    void operator()(State* s) const noexcept;
};
using StatePtr = std::unique_ptr<State, StateDeleter>;

// Starts the network thread. curl is not touched (libcurl.dll is delay-loaded): the first
// request loads it, on that thread.
auto init(const Config& config) -> std::expected<StatePtr, core::Error>;

} // namespace net
