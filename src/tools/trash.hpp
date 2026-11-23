#pragma once

// fs.trash and fs.restore, for the table in all.cpp.

#include "tools/tool.hpp"

namespace tools::trash {

auto schema() -> std::string;
// {"paths":[…]} with only the paths `keep` names (fs.trash, fs.restore, fs.delete_permanent).
auto narrow(std::string_view args, std::span<const std::size_t> keep) -> core::Result<std::string>;
auto trash_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto trash_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto trash_preview(std::string_view args, const security::Request& request) -> std::string;
auto restore_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto restore_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto delete_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto delete_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto delete_preview(std::string_view args, const security::Request& request) -> std::string;

} // namespace tools::trash
