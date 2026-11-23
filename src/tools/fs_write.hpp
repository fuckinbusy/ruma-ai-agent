#pragma once

// The changing file tools' functions, for the table in all.cpp.

#include "tools/tool.hpp"

namespace tools::fs {

auto write_schema() -> std::string;
auto write_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto write_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto write_preview(std::string_view args, const security::Request& request) -> std::string;

auto mkdir_schema() -> std::string;
auto mkdir_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto mkdir_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto move_schema() -> std::string;
auto move_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto move_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto move_preview(std::string_view args, const security::Request& request) -> std::string;
auto move_narrow(std::string_view args, std::span<const std::size_t> keep) -> core::Result<std::string>;

auto copy_schema() -> std::string;
auto copy_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto copy_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto copy_preview(std::string_view args, const security::Request& request) -> std::string;
auto copy_narrow(std::string_view args, std::span<const std::size_t> keep) -> core::Result<std::string>;

} // namespace tools::fs
