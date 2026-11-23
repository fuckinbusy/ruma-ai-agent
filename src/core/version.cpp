#include "core/version.hpp"

#include "wa_version.h"

namespace core {

auto version() noexcept -> std::string_view {
    return WA_VERSION_TEXT;
}

} // namespace core
