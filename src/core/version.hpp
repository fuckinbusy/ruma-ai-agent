#pragma once

#include <string_view>

namespace core {

// Product version: project(VERSION) in CMakeLists.txt, through the generated wa_version.h.
auto version() noexcept -> std::string_view;

} // namespace core
