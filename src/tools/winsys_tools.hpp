#pragma once

// The Windows tools of phase 13a, for the table in all.cpp: what starts with Windows, the
// services (looked at only), the scheduled tasks, a file's signature. Read only, never asked.

#include "tools/tool.hpp"

namespace tools::winsys {

auto empty_schema() -> std::string;
auto startup_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto startup_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto services_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto services_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto tasks_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto tasks_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto signature_schema() -> std::string;
auto signature_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto signature_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto sample_schema() -> std::string;
auto sample_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto sample_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto connections_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto connections_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto events_schema() -> std::string;
auto events_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto events_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto apps_schema() -> std::string;
auto apps_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto apps_run(const security::Permit& permit, std::string_view args) -> Outcome;

auto look_schema() -> std::string;
auto look_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto look_run(const security::Permit& permit, std::string_view args) -> Outcome;

// startup.disable (task 5) and its way back, the agent's own: startup.enable, task.enable, wmi.restore.
auto disable_schema() -> std::string;
auto disable_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto disable_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto enable_schema() -> std::string;
auto enable_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto enable_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto task_enable_schema() -> std::string;
auto task_enable_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto task_enable_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto wmi_restore_schema() -> std::string;
auto wmi_restore_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto wmi_restore_run(const security::Permit& permit, std::string_view args) -> Outcome;

// disk.usage (task 6): what takes the space under a folder.
auto disk_schema() -> std::string;
auto disk_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto disk_run(const security::Permit& permit, std::string_view args) -> Outcome;
// What the model gets of a count: below a folder of secrets or of the agent's own (by `roots`), no names.
auto disk_for_model(const DiskScan& s, const security::paths::Roots& roots) -> std::string;

// app.uninstall (task 7): a program removed by its own uninstaller.
auto uninstall_schema() -> std::string;
auto uninstall_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto uninstall_run(const security::Permit& permit, std::string_view args) -> Outcome;
// Several versions offered on the card (06b): the call with the one picked (`keep`: one index).
auto uninstall_narrow(std::string_view args, std::span<const std::size_t> keep) -> core::Result<std::string>;

// system.check (task 8): the threat check.
auto check_schema() -> std::string;
auto check_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto check_run(const security::Permit& permit, std::string_view args) -> Outcome;

// file.quarantine (task 9), its way back file.release (the agent's own) and quarantine.delete.
auto quarantine_schema() -> std::string;
auto quarantine_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto quarantine_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto release_schema() -> std::string;
auto release_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto release_run(const security::Permit& permit, std::string_view args) -> Outcome;
auto quarantine_delete_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto quarantine_delete_run(const security::Permit& permit, std::string_view args) -> Outcome;
// defender.scan (task 9c): Microsoft Defender's own verdict on one file.
auto defender_scan_schema() -> std::string;
auto defender_scan_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto defender_scan_run(const security::Permit& permit, std::string_view args) -> Outcome;

// For the model: what looks like a password or a token, masked (phase 13a rule 6).
[[nodiscard]] auto masked(std::wstring_view text) -> std::string;

} // namespace tools::winsys
