#include "tools/call.hpp"

#include "core/log.hpp"
#include "security/permit.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <stdexcept>

namespace tools {
namespace {

constexpr std::size_t kMaxAuditedArgs = 4096; // fs.write's content need not be in the audit whole

auto audited_args(std::string_view args) -> std::string {
    if (args.size() <= kMaxAuditedArgs) {
        return std::string{args};
    }
    std::size_t n = kMaxAuditedArgs;
    while (n > 0 && (static_cast<unsigned char>(args[n]) & 0xC0) == 0x80) {
        --n;
    }
    return std::string{args.substr(0, n)} + "…";
}

// Every path of `now` was in the question the user answered (`asked`).
auto all_asked(const security::Ruling& now, const security::Ruling& asked) -> bool {
    return std::ranges::all_of(now.paths, [&](const security::paths::Canonical& p) {
        return std::ranges::any_of(asked.paths, [&](const security::paths::Canonical& q) { return q.path == p.path; });
    });
}

} // namespace

auto call(std::string_view name, std::string_view args, const std::filesystem::path& working_dir,
          const security::Settings& settings, security::Session& session, const Confirm& confirm, security::AuditLog* audit,
          std::uint64_t task, const Control& control) -> Called {
    const auto started = std::chrono::steady_clock::now();
    Called c;
    std::string audited_name{name}; // the call as it was made: narrowed, or turned permanent
    std::string made_args{args};
    const auto record = [&] {
        if (audit == nullptr) {
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const auto written = audit->append({.task_id = task,
                                            .tool = audited_name,
                                            .args = audited_args(made_args),
                                            .zone = c.ruling.zone ? std::string{security::paths::zone_name(*c.ruling.zone)} : std::string{},
                                            .decision = std::string{security::decision_name(c.ruling.decision)},
                                            .decided_by = c.asked ? "user" : c.ruling.by_session ? "session" : "policy",
                                            .duration_ms = ms,
                                            .result_code = c.outcome.code});
        if (!written) {
            // The call already happened: it cannot be taken back, but a gap in the audit is
            // never silent.
            WA_ERROR("tools", "{} is missing from the audit: {}", audited_name, written.error().message);
        }
    };
    const auto refuse = [&](std::string why) {
        c.outcome = failure(audited_name, core::Error{core::Code::Policy, std::move(why)});
        record();
        return c;
    };

    // An internal tool only for the agent's own calls: for the model it is no tool (final review E1).
    const Tool* tool = internal(name) && !control.agents_own ? nullptr : find(name, settings);
    if (tool == nullptr) {
        c.ruling.reason = "no such tool";
        c.outcome = failure(name, core::Error{core::Code::NotFound, "there is no tool named " + std::string{name}});
        record();
        return c;
    }
    auto request = tool->request(args, working_dir);
    if (!request) {
        c.ruling.reason = request.error().message;
        c.outcome = failure(name, request.error());
        record();
        return c;
    }
    // Made before the question, shown on it; dropped with this frame on a no.
    std::optional<Prepared> prepared;
    if (tool->prepare != nullptr) {
        auto made = tool->prepare(args);
        if (!made) {
            c.ruling.reason = made.error().message;
            c.outcome = failure(name, made.error());
            record();
            return c;
        }
        prepared = std::move(*made);
    }
    c.ruling = security::decide(*request, settings, session);
    bool confirmed = false;
    Consent consent;
    if (c.ruling.decision == security::Decision::Ask) {
        c.asked = true;
        const std::string preview = tool->preview != nullptr ? tool->preview(args, *request) : std::string{};
        if (confirm) {
            consent = confirm(c.ruling, preview, prepared ? &*prepared : nullptr);
            // "Take it again" (phase 10, screen 19): made anew, the same question asked with it — never a
            // yes by itself. "The same again" is the last way asked for; a failed one leaves the picture
            // before; twenty questions at most, then it is a no.
            std::optional<Retake> last;
            for (int again = 1; !consent.yes && consent.retake && again < 20; ++again) {
                Retake want = *consent.retake;
                if (want.what != Retake::What::Same) {
                    last = want;
                } else if (last) {
                    want = *last;
                }
                auto made = want.what == Retake::What::Same || tool->prepare_again == nullptr
                                ? (tool->prepare != nullptr ? tool->prepare(args) : core::Result<Prepared>{std::unexpected{core::Error{}}})
                                : tool->prepare_again(args, want);
                if (made) {
                    prepared = std::move(*made);
                }
                consent = confirm(c.ruling, preview, prepared ? &*prepared : nullptr);
            }
            if (!consent.yes) {
                consent = Consent{false};
            }
        }
        confirmed = consent.yes;
    }

    // The card's choices: only what was left ticked, or permanently instead of the Recycle Bin.
    // The call they make is decided again, and may name nothing the question did not.
    if (confirmed && (consent.permanent || !consent.keep.empty())) {
        const Tool* effective = tool;
        if (consent.permanent) {
            const Tool* forever = find("fs.delete_permanent", settings);
            if (tool->name != "fs.trash" || forever == nullptr) {
                return refuse("deleting permanently is not possible here (advanced.allow_permanent_delete is off)");
            }
            effective = forever;
            audited_name = std::string{forever->name};
        }
        if (!consent.keep.empty()) {
            if (effective->narrow == nullptr) {
                return refuse(std::string{effective->name} + " cannot leave anything out");
            }
            auto narrowed = effective->narrow(made_args, consent.keep);
            if (!narrowed) {
                return refuse("the choice names what was not asked about: " + narrowed.error().message);
            }
            made_args = std::move(*narrowed);
        }
        auto again = effective->request(made_args, working_dir);
        if (!again) {
            return refuse(again.error().message);
        }
        auto ruling = security::decide(*again, settings, session);
        // A call of no paths narrowed to one of the choices its card offered (phase 13a, 06b).
        const auto& offered = c.ruling.request.choices;
        const bool chosen = !offered.empty() && again->choices.empty() && consent.keep.size() == 1 && consent.keep.front() < offered.size() &&
                            offered[consent.keep.front()].name == again->entry && offered[consent.keep.front()].version == again->version &&
                            again->risk == c.ruling.request.risk;
        if (!chosen && (!all_asked(ruling, c.ruling) || ruling.paths.empty())) {
            return refuse("the choice names what was not asked about");
        }
        if (ruling.decision == security::Decision::Deny || ruling.decision == security::Decision::HardDeny) {
            c.ruling = std::move(ruling);
            return refuse(c.ruling.reason);
        }
        c.ruling = std::move(ruling);
        request = std::move(again);
        tool = effective;
    }

    auto permit = security::grant(c.ruling, *request, confirmed, session);
    if (!permit) {
        return refuse(c.asked && c.ruling.decision == security::Decision::Ask ? "the user said no: " + c.ruling.reason
                                                                               : c.ruling.reason);
    }
    c.allowed = true;
    {
        const ControlScope scope{control};
        try { // no input a tool is given ends the agent: what it could not hold is its failure
            c.outcome = prepared && tool->run_prepared != nullptr ? tool->run_prepared(*permit, made_args, std::move(*prepared))
                                                                  : tool->run(*permit, made_args);
        } catch (const std::bad_alloc&) {
            c.outcome = failure(name, core::Error{core::Code::Io, "not enough memory for this input"});
        } catch (const std::length_error&) {
            c.outcome = failure(name, core::Error{core::Code::InvalidArgument, "the input is too large"});
        }
    }
    record();
    return c;
}

} // namespace tools
