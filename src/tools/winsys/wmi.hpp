#pragma once

// tools::winsys::wmi_subscriptions — permanent WMI event subscriptions (root\subscription): a
// filter bound to a consumer that runs a command or a script. Read with ExecQuery only; the
// namespace usually needs administrator rights, then it is named in not_read.

#include <string>
#include <vector>

namespace tools::winsys {

struct WmiSubscription {
    std::string filter;         // the __EventFilter's name
    std::string query;          // its WQL query
    std::string consumer;       // the consumer's name
    std::string consumer_class; // CommandLineEventConsumer, ActiveScriptEventConsumer, …
    std::wstring command;       // CommandLineTemplate or ExecutablePath; a script's first line
};

struct WmiList {
    bool read = false;
    std::vector<WmiSubscription> subscriptions;
    std::vector<std::wstring> not_read;
};

[[nodiscard]] auto wmi_subscriptions() -> WmiList;

} // namespace tools::winsys
