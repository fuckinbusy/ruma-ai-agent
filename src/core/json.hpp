#pragma once

// core::json — how the agent writes JSON with glaze. Included only by targets that link glaze
// (core itself does not).
//
// Control characters (ESC from a coloured PowerShell, BEL, NUL) are written as \u00XX. glaze's
// default does not escape them and writes other bytes in their place (two zeros for ESC): the
// text is not JSON. Found live in phase 7: a tool's coloured output stayed in the conversation,
// and every later request to DeepSeek was refused with a 400.

#include <glaze/glaze.hpp>

#include <utility>

namespace core::json {

struct Opts : glz::opts {
    bool escape_control_characters = true;
};

inline constexpr Opts kCompact{};
inline constexpr Opts kPretty{{.prettify = true}};

// As glz::write_json: the text, or glaze's error.
template <class T>
[[nodiscard]] auto write(T&& value) {
    return glz::write<kCompact>(std::forward<T>(value));
}

// Into `out`; as glz::write_json(value, out): an error context, true when it failed.
template <class T, class Buffer>
[[nodiscard]] auto write(T&& value, Buffer&& out) {
    return glz::write<kCompact>(std::forward<T>(value), std::forward<Buffer>(out));
}

// Indented, for files people read (config.json).
template <class T>
[[nodiscard]] auto write_pretty(T&& value) {
    return glz::write<kPretty>(std::forward<T>(value));
}

} // namespace core::json
