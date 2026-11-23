#pragma once

// tools::winsys — the user's own list of private windows (phase 13a § 2.1): a program, or words in
// a window's title, the agent never looks at. data\private.json, kept like the agent's memory: the
// user adds and forgets in Settings › Rules and memory, the agent adds when asked (private.add).

#include "core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tools::winsys {

struct PrivateRule {
    std::uint64_t id = 0; // its own, for Forget and Undo
    std::string program; // "Telegram.exe"; empty: any program
    std::string words;   // in the title, any case; empty: any title
    std::string date;    // "2026-10-28"
};

// What `file` holds; none or broken: empty.
[[nodiscard]] auto load_private(const std::filesystem::path& file) -> std::vector<PrivateRule>;
[[nodiscard]] auto save_private(const std::filesystem::path& file, const std::vector<PrivateRule>& rules) -> core::Result<void>;
// A rule added to `file` (its id the next one), or the same rule already there; refused: neither a
// program nor words, or past 50 rules.
[[nodiscard]] auto add_private(const std::filesystem::path& file, std::string program, std::string words, std::string date)
    -> core::Result<PrivateRule>;
// The rule `id` gone from `file`; false when there was none.
auto forget_private(const std::filesystem::path& file, std::uint64_t id) -> bool;
// "Windows with “Bank” in the title", "Telegram.exe", "Telegram.exe windows with “chat”".
[[nodiscard]] auto describe(const PrivateRule& r) -> std::string;

} // namespace tools::winsys
