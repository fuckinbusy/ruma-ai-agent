#pragma once

// The machine-state tools' functions, for the table in all.cpp: none of them names a file.

#include "tools/tool.hpp"

namespace tools::sys {

auto no_args_schema() -> std::string;

auto info_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto info_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto processes_schema() -> std::string;
auto processes_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto processes_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto window_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto window_run(const security::Permit& permit, std::string_view args) -> Outcome;
// window.active's answer for the window `w` (an HWND): a sensitive one gives only that it is
// (phase 13a rule 7: not even its title).
auto window_of(void* w) -> Outcome;

auto clipboard_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto clipboard_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto clipboard_write_schema() -> std::string;
auto clipboard_write_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto clipboard_write_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto clipboard_write_preview(std::string_view args, const security::Request& request) -> std::string;

auto open_schema() -> std::string;
auto open_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto open_run(const security::Permit& permit, std::string_view args) -> Outcome;

} // namespace tools::sys
