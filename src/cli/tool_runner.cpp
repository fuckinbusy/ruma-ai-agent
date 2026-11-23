// tool-runner — one tool call from the console, through the same policy, questions and audit as
// the agent (tools::call). For trying tools by hand and for tests.
//
//   tool-runner --list
//   tool-runner --verify-audit <file>
//   tool-runner <tool> <json args> [--yes] [--wd <dir>] [--fence <dir>] [--dry-run] [--audit <file>]
//
// A question is asked on the console (y/N; no answer is no). --yes answers yes to questions, never
// to a hard refusal. --fence <dir>: anything changed outside <dir> ends the run (exit code 3)
// instead. --dry-run: changes go to a Recorder, which only lists them. The result's JSON goes to
// stdout, the decision to stderr; the exit code is the result's: 0 done, 2 failed, 3 refused,
// 4 an audit whose chain is broken.

#include "app/agent_tools.hpp"
#include "cli/console.hpp"
#include "app/config.hpp"
#include "core/utf8.hpp"
#include "core/crash.hpp"
#include "core/win.hpp"
#include "security/audit.hpp"
#include "tools/call.hpp"
#include "tools/os.hpp"
#include "tools/tool.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using cli::data_dir;
using cli::say;
using cli::utf8;

auto usage() -> int {
    say("usage: tool-runner --list\n"
        "       tool-runner --verify-audit <file>\n"
        "       tool-runner <tool> <json args> [--yes] [--wd <dir>] [--fence <dir>] [--dry-run] [--audit <file>]\n"
        "       any of them [--config <config.json>] (default: the agent's own)");
    return 2;
}



// The agent's own settings: config.json's advanced flags and limits, and whether this process
// is elevated — the same Settings the agent's tools are called with. A file that is not there,
// or cannot be read, gives the defaults (the safe ones).
auto settings_from(const std::filesystem::path& file) -> security::Settings {
    app::config::Config config;
    if (auto loaded = app::config::load(file)) {
        for (const auto& p : loaded->problems) {
            say("[config] line " + std::to_string(p.line) + ": " + p.message);
        }
        config = std::move(loaded->config);
    } else {
        say("[config] not read, the defaults hold: " + loaded.error().message);
    }
    return app::agent::settings_of(config.advanced, config.limits, config.access, app::agent::process_elevated());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    core::crash::quiet_failures(); // a failed check: stderr and exit code 3, never a dialog on the desktop
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    std::filesystem::path config_file = data_dir() / L"config.json";
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == L"--config") {
            if (i + 1 >= a.size()) {
                return usage();
            }
            config_file = a[i + 1];
            a.erase(a.begin() + static_cast<std::ptrdiff_t>(i), a.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            break;
        }
    }
    if (a.empty()) {
        return usage();
    }
    const security::Settings settings = settings_from(config_file);
    if (a[0] == L"--list") {
        for (const auto* t : tools::available(settings)) {
            std::printf("%.*s (%.*s)\n  %.*s\n  %s\n", static_cast<int>(t->name.size()), t->name.data(),
                        static_cast<int>(security::risk_name(t->risk).size()), security::risk_name(t->risk).data(),
                        static_cast<int>(t->description.size()), t->description.data(), t->schema().c_str());
        }
        return 0;
    }
    if (a[0] == L"--verify-audit") {
        if (a.size() != 2) {
            return usage();
        }
        const auto ok = security::verify_audit(a[1]);
        if (!ok) {
            say("broken: " + ok.error().message);
            return 4;
        }
        std::printf("ok: %zu records\n", *ok);
        return 0;
    }
    if (a.size() < 2) {
        return usage();
    }
    const std::string tool = utf8(a[0]);
    const std::string args = utf8(a[1]);
    bool yes = false;
    bool dry = false;
    std::filesystem::path wd = std::filesystem::current_path();
    std::filesystem::path audit_file = data_dir() / L"audit.jsonl";
    std::optional<std::filesystem::path> fence;
    for (std::size_t i = 2; i < a.size(); ++i) {
        if (a[i] == L"--yes") {
            yes = true;
        } else if (a[i] == L"--dry-run") {
            dry = true;
        } else if (a[i] == L"--wd" && i + 1 < a.size()) {
            wd = a[++i];
        } else if (a[i] == L"--fence" && i + 1 < a.size()) {
            fence = a[++i];
        } else if (a[i] == L"--audit" && i + 1 < a.size()) {
            audit_file = a[++i];
        } else {
            return usage();
        }
    }
    if (fence) {
        tools::os::fence::enable(*fence);
    }
    tools::os::Recorder recorder;
    if (dry) {
        tools::os::use(&recorder);
    }
    auto audit = security::AuditLog::open(audit_file);
    if (!audit) {
        say("the audit cannot be written: " + audit.error().message);
        return 2;
    }

    const auto confirm = cli::confirm(yes);
    security::Session session;
    const auto called = tools::call(tool, args, wd, settings, session, confirm, &*audit, 0);
    say("[policy] " + std::string{security::decision_name(called.ruling.decision)} + ": " + called.ruling.reason);
    if (dry) {
        say("[dry-run] backend calls: " + std::to_string(recorder.calls().size()));
        for (const auto& c : recorder.calls()) {
            say("  " + c);
        }
        tools::os::use(nullptr);
    }
    std::fwrite(called.outcome.json.data(), 1, called.outcome.json.size(), stdout);
    std::fputc('\n', stdout);
    return called.outcome.code;
}
