#pragma once

// shell.run and process.kill, for the table in all.cpp.

#include "tools/tool.hpp"

namespace tools::shell {

auto run_schema() -> std::string;
auto run_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto run_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto kill_schema() -> std::string;
auto kill_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto kill_run(const security::Permit& permit, std::string_view args) -> Outcome;

} // namespace tools::shell
