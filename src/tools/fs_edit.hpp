#pragma once

// fs.edit (phase 12 § 1): exact pieces of a text file replaced, all or none, the file's encoding,
// line breaks, attributes and creation time kept; its card shows the diff.

#include "tools/tool.hpp"

namespace tools::fs {

inline constexpr std::uint64_t kMaxEdited = 10ull * 1024 * 1024; // larger files are refused

auto edit_schema() -> std::string;
auto edit_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto edit_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto edit_preview(std::string_view args, const security::Request& request) -> std::string;

// fs.revert: an edit taken back — the file gets the copy of what it held. The agent's own (Undo),
// never offered to the model; the copy must be in the undo folder, and a file changed since the
// edit (its hash is not `after`) is asked about with the diff of what the revert takes away.
void set_undo_root(std::filesystem::path root); // the agent: <data>\undo; tests: their own folder
[[nodiscard]] auto undo_root() -> std::filesystem::path;
// The hash an edit keeps of the file right after it ("0123456789abcdef").
[[nodiscard]] auto content_hash(std::string_view bytes) -> std::string;

auto revert_schema() -> std::string;
auto revert_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto revert_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto revert_preview(std::string_view args, const security::Request& request) -> std::string;

} // namespace tools::fs
