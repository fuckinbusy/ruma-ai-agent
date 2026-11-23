#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/winsys/check.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>

#include <chrono>
#include <format>
#include <memory>
#include <optional>

// system.check (phase 13a § 5): the threat check's eight steps, their progress on the step, the
// Stop button; the model gets the verdict, the findings with their reasons and facts (secrets
// masked), what was not read; the panel the whole report.

namespace tools::winsys::args {

struct Check {
    std::optional<int> from_step; // 2–8: on from a stopped check; 1 or null: afresh
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct CheckFact {
    std::string key, value;
    bool read_only = false;
};
struct CheckFinding {
    std::string level, rule, title;
    std::vector<std::string> why;
    std::vector<CheckFact> facts;
    std::vector<std::string> actions;
};
struct CheckStage {
    std::string name, state;
    std::size_t places = 0;
};
struct StoppedAt {
    int at = 0, of = 0;
};
struct CheckOut {
    std::string verdict;
    std::optional<StoppedAt> stopped;
    std::size_t places_checked = 0;
    std::vector<std::string> not_read;
    bool defender_on = false;
    std::vector<CheckStage> stages;
    std::vector<CheckFinding> findings;
    std::string note = "The levels come from fixed rules: you may call something worth a look beyond them and say why; you may "
                       "never lower one. Say the verdict as it is: never \"no viruses\". Offer only the actions a finding lists.";
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

constexpr std::size_t kMaxFindings = 60;

auto read_args(std::string_view json) -> core::Result<args::Check> {
    args::Check a{};
    if (!json.empty() && json != "null") {
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
            return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
        }
    }
    if (a.from_step && (*a.from_step < 1 || *a.from_step > static_cast<int>(kStages.size()))) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"from_step\" is 1 to 8");
    }
    return a;
}

auto for_model(const CheckReport& r) -> out::CheckOut {
    out::CheckOut o;
    o.verdict = r.verdict;
    if (r.stopped) {
        o.stopped = out::StoppedAt{r.stopped->at, r.stopped->of};
    }
    o.places_checked = r.places_checked;
    for (const auto& n : r.not_read) {
        o.not_read.push_back(core::utf8::to_utf8(n).value_or("?"));
    }
    o.defender_on = r.defender_on;
    for (const auto& s : r.stages) {
        o.stages.push_back({s.name, s.state == StageState::Done ? "done" : s.state == StageState::Stopped ? "stopped" : "not reached", s.places});
    }
    for (std::size_t i = 0; i < std::min(kMaxFindings, r.findings.size()); ++i) {
        const auto& f = r.findings[i];
        // The reasons quote commands, queries, hosts lines, proxies: masked as the facts are (rule 6).
        const auto mask = [](const std::string& s) { return masked(core::utf8::to_wide(s).value_or(L"")); };
        out::CheckFinding x{.level = f.level == Level::Suspicious ? "suspicious" : f.level == Level::WorthALook ? "worth a look" : "fine",
                            .rule = f.rule, .title = mask(f.title), .why = {}, .facts = {}, .actions = f.actions};
        for (const auto& w : f.why) {
            x.why.push_back(mask(w));
        }
        for (const auto& fact : f.facts) {
            x.facts.push_back({fact.key, masked(core::utf8::to_wide(fact.value).value_or(L"")), fact.read_only});
        }
        o.findings.push_back(std::move(x));
    }
    return o;
}

} // namespace

auto check_schema() -> std::string {
    auto s = glz::write_json_schema<args::Check>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto check_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    if (auto a = read_args(json); !a) {
        return std::unexpected{a.error()};
    }
    return security::Request{.tool = "system.check", .risk = security::Risk::Read, .working_dir = wd};
}

auto check_run(const security::Permit&, std::string_view json) -> Outcome {
    const auto a = read_args(json);
    if (!a) {
        return failure("system.check", a.error());
    }
    const auto& ctx = context();
    const Sources sources = ctx.check_sources ? *ctx.check_sources : real_sources();
    CheckState local;
    CheckState& state = ctx.check_state ? *ctx.check_state : local;
    const auto steps = kStages.size();
    // In thousandths of a step: the bar moves through the load while idle too.
    auto report = run_check(sources, state, a->from_step.value_or(1), control().stop, [&](const CheckProgress& p) {
        const auto done = static_cast<std::uint64_t>((p.step - 1 + p.within) * 1000);
        const std::string what = std::format("{} of {} \xC2\xB7 {}", p.step, steps, p.name);
        tools::report(done, steps * 1000, core::utf8::to_wide(what).value_or(L""), 0,
                      p.left ? std::optional{std::chrono::seconds{*p.left}} : std::nullopt);
    });
    auto data = core::json::write(for_model(report));
    Outcome o{0, wrap("system.check", L"", data ? *data : std::string{R"("?")"}, false)};
    o.check = std::make_shared<const CheckReport>(std::move(report));
    return o;
}

} // namespace tools::winsys
