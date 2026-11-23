#pragma once

// tools::call — the whole way of one tool call, the same for the agent and for tool-runner:
//
//   find -> request -> security::decide -> (Ask: confirm, with the canonical paths and the
//   preview) -> security::grant -> run -> audit.
//
// `confirm` is the only door for a question; HardDeny and Deny never reach it. Every call that
// got as far as a name is audited, refused or not.

#include "security/audit.hpp"
#include "security/policy.hpp"
#include "tools/tool.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tools {

// The user's answer to a question: yes or no, and on the card's choices — only the objects left
// ticked (`keep`, indices into the call's list; empty: all), and "delete permanently" instead of
// the Recycle Bin (only where advanced mode has fs.delete_permanent). A bool is a plain yes/no.
struct Consent {
    bool yes = false;
    std::vector<std::size_t> keep;
    bool permanent = false;
    // Not an answer yet: make it again first and ask once more (phase 10, screen.capture).
    std::optional<Retake> retake;

    Consent(bool y = false) : yes(y) {} // NOLINT: a confirmer may answer with a bool
};

// `prepared`: what the tool made before asking (a picture of the screen), or nullptr.
using Confirm = std::function<Consent(const security::Ruling& ruling, std::string_view preview, const Prepared* prepared)>;

struct Called {
    Outcome outcome;
    security::Ruling ruling;
    bool asked = false;
    bool allowed = false; // by the policy or by the user
};

// `control`: stop, pause, progress and output of this call (tools::control() inside the tool).
// A narrowed or permanent consent is decided again on what it now names, and refused unless
// every path of it was in the question.
auto call(std::string_view name, std::string_view args, const std::filesystem::path& working_dir,
          const security::Settings& settings, security::Session& session, const Confirm& confirm, security::AuditLog* audit,
          std::uint64_t task, const Control& control = {}) -> Called;

} // namespace tools
