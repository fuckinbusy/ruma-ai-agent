#include "app/instance.hpp"

#include "core/agent_ids.hpp"

#include <string>

namespace app::instance {

static_assert(std::wstring_view{kName} == core::agent_ids::kMutex); // the installer looks for it

auto acquire(std::wstring_view name) -> core::Result<Instance> {
    const std::wstring n{name};
    core::win::KernelHandle mutex{CreateMutexW(nullptr, FALSE, n.c_str())};
    const DWORD error = GetLastError(); // read before anything else can change it
    if (!mutex) {
        return core::os_error(error, "CreateMutexW(single instance)");
    }
    return Instance{std::move(mutex), error != ERROR_ALREADY_EXISTS};
}

} // namespace app::instance
