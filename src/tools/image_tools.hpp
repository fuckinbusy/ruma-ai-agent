#pragma once

// The picture tools' functions, for the table in all.cpp (phase 9).

#include "tools/tool.hpp"

namespace tools::pictures {

auto view_schema() -> std::string;
auto view_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto view_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto ocr_schema() -> std::string;
auto ocr_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto ocr_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto convert_schema() -> std::string;
auto convert_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto convert_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto capture_schema() -> std::string;
auto capture_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto capture_prepare(std::string_view args) -> core::Result<Prepared>;
auto capture_prepare_again(std::string_view args, const Retake& retake) -> core::Result<Prepared>;
auto capture_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto capture_run_prepared(const security::Permit& permit, std::string_view args, Prepared&& prepared) -> Outcome;

} // namespace tools::pictures
