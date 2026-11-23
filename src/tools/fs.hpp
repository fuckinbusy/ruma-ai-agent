#pragma once

// The file tools' functions, for the table in all.cpp.

#include "tools/tool.hpp"

#include <optional>

namespace tools::fs {

auto list_schema() -> std::string;
auto list_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto list_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto read_schema() -> std::string;
auto read_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto read_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto stat_schema() -> std::string;
auto stat_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto stat_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto search_schema() -> std::string;
auto search_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto search_run(const security::Permit& permit, std::string_view args) -> Outcome;

// A text file's bytes as UTF-8, its encoding guessed as fs.read guesses it (BOM, UTF-8, else
// Windows-1251); nullopt for a binary file (doc.read's TXT, MD and CSV).
auto text_of(std::string_view bytes) -> std::optional<std::string>;

} // namespace tools::fs
