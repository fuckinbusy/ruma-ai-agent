#pragma once

// app::memory — what the agent remembers about the user and the user's own rules (phase 12 § 6).
// data\memory.json: facts and preferences, at most 50 entries of 200 characters, each with its date
// and the task it came from; never what looks like a secret (core::secret_like). data\agent.md:
// the user's rules as they wrote them, read for every request, at most 4 KB. Both go into the
// system prompt. Not thread-safe: its owner locks.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace app::memory {

inline constexpr std::size_t kMaxEntries = 50;
inline constexpr std::size_t kMaxChars = 200;
inline constexpr std::size_t kMaxRules = 4096;

struct Entry {
    std::uint64_t id = 0;
    std::string text;
    std::string date; // "2026-10-27"
    std::uint64_t task = 0;
};

class Memory {
public:
    // What `file` holds; none or broken: empty (a broken file stays, for the user to see).
    static auto load(std::filesystem::path file) -> Memory;
    // A new entry, or the same text's own; refused with why: empty, too long, full, secret-like.
    auto add(std::string_view text, std::uint64_t task, std::string date) -> core::Result<Entry>;
    auto forget(std::uint64_t id) -> bool;
    void clear();
    [[nodiscard]] auto save() const -> core::Result<void>;
    [[nodiscard]] auto entries() const noexcept -> const std::vector<Entry>& { return entries_; }
    // "- text (date)\n" each: the prompt's "What you know about the user".
    [[nodiscard]] auto prompt_lines() const -> std::string;

private:
    std::filesystem::path file_;
    std::vector<Entry> entries_;
    std::uint64_t last_ = 0;
};

// The user's rules from `file` (agent.md): trailing blank lines off; past 4 KB cut with a mark.
auto read_rules(const std::filesystem::path& file) -> std::string;

} // namespace app::memory
