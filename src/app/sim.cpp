#include "app/sim.hpp"

#include "app/report.hpp"
#include "core/json.hpp"
#include "core/utf8.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <chrono>
#include <format>
#include <limits>
#include <span>

namespace app::sim {
namespace {

// A scenario step. Actions are written "id:Label;id2:Label 2", a '*' after the id marks the
// primary button: "close*:Yes, close;later:Not now".
struct Step {
    enum Kind : std::uint8_t { Think, Say, Offer, Finish } kind;
    std::string_view text;    // Think: the step line; Say: answer text; Finish: the title
    double seconds = 0;       // Think: how long
    std::string_view detail;  // Finish: the detail line
    std::string_view actions; // Offer, Finish
};

struct Script {
    std::string_view scenario;
    std::string_view entry; // "" = the request itself, otherwise the pressed button's id
    std::span<const Step> steps;
};

// ---- the mockups' scenarios -------------------------------------------------------------------

constexpr Step kSlow[] = {
    {Step::Think, "Analyzing the load", 1.4},
    {Step::Think, "Reading the process list", 0.9},
    {Step::Say, "The biggest resource users right now:\n"
                "- **chrome.exe** \xE2\x80\x94 2.4 GB RAM, 18% CPU\n"
                "- **Blender 5.1** \xE2\x80\x94 1.9 GB, rendering in the background\n"
                "- **OBS Studio** \xE2\x80\x94 11% GPU without recording\n"
                "Close OBS and unload the idle Chrome tabs?"},
    {Step::Offer, {}, 0, {}, "close*:Yes, close;later:Not now"},
};
constexpr Step kSlowClose[] = {
    {Step::Think, "Closing OBS Studio", 1.2},
    {Step::Think, "Unloading 14 idle tabs", 0.8},
    {Step::Finish, "OBS closed", 0, "freed 1.1 GB of RAM", "undo:Undo"},
};
constexpr Step kSlowLater[] = {
    {Step::Finish, "Nothing changed", 0, "ask again any time"},
};
constexpr Step kSlowUndo[] = {
    {Step::Think, "Starting OBS Studio again", 1.0},
    {Step::Finish, "OBS is back", 0, "as it was before"},
};

constexpr Step kDesktop[] = {
    {Step::Think, "Scanning the desktop", 1.2},
    {Step::Think, "Grouping 23 files by type", 1.0},
    {Step::Say, "I put your desktop in order:\n"
                "- **Documents** \xE2\x80\x94 9 files\n"
                "- **Images** \xE2\x80\x94 8 files\n"
                "- **Installers** \xE2\x80\x94 6 files\n"
                "Shortcuts stayed where they were."},
    {Step::Offer, {}, 0, {}, "keep*:Looks good;undo:Undo"},
};
constexpr Step kDesktopKeep[] = {
    {Step::Finish, "Desktop sorted", 0, "23 files in 3 folders", "undo:Undo"},
};
constexpr Step kDesktopUndo[] = {
    {Step::Think, "Moving the files back", 1.0},
    {Step::Finish, "Desktop restored", 0, "23 files back in place"},
};

constexpr Step kTranslate[] = {
    {Step::Think, "Reading the clipboard", 0.8},
    {Step::Say, "**Original** (Chinese): \xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\n"
                "**Translation**: Hello, world\n"
                "**Pinyin**: n\xC7\x90 h\xC7\x8Eo, sh\xC3\xAC ji\xC3\xA8"},
    {Step::Offer, {}, 0, {}, "copy*:Copy;style:Another style"},
};
constexpr Step kTranslateCopy[] = {
    {Step::Finish, "Copied", 0, "the translation is on the clipboard"},
};
constexpr Step kTranslateStyle[] = {
    {Step::Think, "Rewriting", 0.8},
    {Step::Say, "\n**Casual**: Hey there, world!"},
    {Step::Offer, {}, 0, {}, "copy*:Copy"},
};

constexpr Step kChat[] = {
    {Step::Think, "Thinking it over", 0.9},
    {Step::Say, "This is the simulated agent of phase 3. It knows three requests so far:\n"
                "- \"Why is my computer slow?\"\n"
                "- \"Sort my desktop\"\n"
                "- \"Translate the clipboard\"\n"
                "The real model connects in phase 7."},
    {Step::Offer},
};

constexpr Script kScripts[] = {
    {"slow", "", kSlow},           {"slow", "close", kSlowClose},
    {"slow", "later", kSlowLater}, {"slow", "undo", kSlowUndo},
    {"desktop", "", kDesktop},     {"desktop", "keep", kDesktopKeep},
    {"desktop", "undo", kDesktopUndo},
    {"translate", "", kTranslate}, {"translate", "copy", kTranslateCopy},
    {"translate", "style", kTranslateStyle},
    {"chat", "", kChat},
};

struct Keywords {
    std::string_view scenario;
    std::string_view words[8];
};
constexpr Keywords kKeywords[] = {
    {"duplicates", {"duplicate"}},
    {"terminal", {"in the terminal", "count to"}},
    {"cleanup", {"temp files", "old installers", "installers"}}, // before "desktop": "clean up" alone sorts it
    {"watch", {"watch", "warn me", "tell me if", "tell me when", "notify me", "alert me"}}, // before "slow": "watch memory"
    {"folder", {"folder"}}, // before "desktop": "a folder on the desktop" is a folder
    {"space", {"space", "drive c", "disk c", "storage"}}, // before "slow": "memory" is not space
    {"slow", {"slow", "lag", "freez", "performance", "memory", "ram", "cpu", "hot"}},
    {"desktop", {"desktop", "tidy", "sort", "organi", "clean up"}},
    {"translate", {"translat", "clipboard", "chinese"}},
};

auto parse_actions(std::string_view text) -> std::vector<events::Action> {
    std::vector<events::Action> out;
    while (!text.empty()) {
        const auto semicolon = text.find(';');
        const std::string_view item = text.substr(0, semicolon);
        const auto colon = item.find(':');
        std::string_view id = item.substr(0, colon);
        const bool primary = !id.empty() && id.back() == '*';
        if (primary) {
            id.remove_suffix(1);
        }
        out.push_back({std::string{id},
                       std::string{colon == std::string_view::npos ? id : item.substr(colon + 1)},
                       primary});
        if (semicolon == std::string_view::npos) {
            break;
        }
        text.remove_prefix(semicolon + 1);
    }
    return out;
}

} // namespace

auto Player::scenario_for(std::string_view request) -> std::string_view {
    std::string lower{request};
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& k : kKeywords) {
        for (const auto word : k.words) {
            if (!word.empty() && lower.find(word) != std::string::npos) {
                return k.scenario;
            }
        }
    }
    return "chat";
}

void Player::schedule(std::uint64_t task, std::string_view entry, double now) {
    const Script* script = nullptr;
    for (const auto& s : kScripts) {
        if (s.scenario == scenario_of(task) && s.entry == entry) {
            script = &s;
        }
    }
    if (script == nullptr) {
        queue_.push_back({now, task, events::Failed{"The simulated agent cannot do that yet"}});
        return;
    }
    std::uniform_real_distribution<double> pace{0.04, 0.11}; // the mockup's 40 + random * 70 ms
    double t = now;
    for (const Step& step : script->steps) {
        switch (step.kind) {
        case Step::Think:
            queue_.push_back({t, task, events::Thinking{std::string{step.text}}});
            t += step.seconds / speed_;
            break;
        case Step::Say: {
            // Tokens as the mockup cuts them: leading spaces, or a word with its trailing spaces
            // (a line break ends a token and adds the mockup's .12s pause).
            std::string_view rest = step.text;
            while (!rest.empty()) {
                std::size_t n = 0;
                if (rest.front() == ' ') {
                    while (n < rest.size() && rest[n] == ' ') {
                        ++n;
                    }
                } else {
                    while (n < rest.size() && rest[n] != ' ' && rest[n] != '\n') {
                        ++n;
                    }
                    while (n < rest.size() && (rest[n] == ' ' || rest[n] == '\n')) {
                        ++n;
                    }
                }
                const std::string_view token = rest.substr(0, n);
                queue_.push_back({t, task, events::Delta{std::string{token}}});
                t += pace(random_) / speed_;
                if (token.back() == '\n') {
                    t += 0.12 / speed_;
                }
                rest.remove_prefix(n);
            }
            break;
        }
        case Step::Offer:
            queue_.push_back({t, task, events::Actions{parse_actions(step.actions)}});
            break;
        case Step::Finish:
            queue_.push_back({t, task, events::Finished{std::string{step.text}, std::string{step.detail},
                                                  parse_actions(step.actions)}});
            break;
        }
    }
}

void Player::command(const events::Command& command, double now) {
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, events::Start>) {
                // Beside the tasks already playing: they go on (the sheet keeps them in the background).
                drop(c.task);
                std::erase_if(scenarios_, [&](const auto& s) { return s.first == c.task; });
                scenarios_.emplace_back(c.task, std::string{scenario_for(c.text)});
                schedule(c.task, "", now);
            } else if constexpr (std::is_same_v<T, events::Cancel>) {
                drop(c.task);
            } else if constexpr (std::is_same_v<T, events::Act>) {
                if (!scenario_of(c.task).empty()) {
                    drop(c.task);
                    schedule(c.task, c.action, now);
                }
            } else if constexpr (std::is_same_v<T, events::Refine>) {
                // The scripts cannot change course: they say they heard it (the model will, phase 7).
                if (!scenario_of(c.task).empty()) {
                    queue_.push_front({now, c.task, events::Thinking{"Noted: " + c.text}});
                }
            }
        },
        command);
}

void Player::drop(std::uint64_t task) {
    std::erase_if(queue_, [&](const Due& d) { return d.task == task; });
}

auto Player::scenario_of(std::uint64_t task) const -> std::string_view {
    for (const auto& [t, s] : scenarios_) {
        if (t == task) {
            return s;
        }
    }
    return {};
}

auto Player::advance(double now, std::vector<events::Event>& out) -> double {
    out.clear();
    std::ranges::stable_sort(queue_, {}, &Due::time); // the tasks' events interleaved by when they are due
    while (!queue_.empty() && queue_.front().time <= now) {
        out.push_back({queue_.front().task, std::move(queue_.front().event)});
        queue_.pop_front();
    }
    return queue_.empty() ? std::numeric_limits<double>::infinity() : queue_.front().time;
}

Agent::Agent(Post post, double speed, std::optional<agent::Host> host)
    : post_(std::move(post)), speed_(speed),
      player_(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()), speed) {
    if (host) {
        tools_.emplace(std::move(*host)); // in place: the tools hold their locks
    }
    thread_ = std::jthread{[this](std::stop_token stop) { run(stop); }}; // after everything it uses
}

Agent::~Agent() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // The tasks' threads: stopped (a question is no answer), then waited for, before what they use goes.
    std::vector<Job*> jobs;
    {
        std::scoped_lock lock{mutex_};
        for (auto& j : jobs_) {
            j->thread.request_stop();
            jobs.push_back(j.get());
        }
    }
    wake_.notify_all();
    for (auto* j : jobs) {
        if (j->thread.joinable()) {
            j->thread.join();
        }
        if (j->watch && tools_ && tools_->host().monitor != nullptr) {
            tools_->host().monitor->unwatch(*j->watch);
        }
    }
}

auto Agent::last_task() const -> std::uint64_t {
    return tools_ ? tools_->last_task() : 0;
}

void Agent::send(events::Command command) {
    {
        std::scoped_lock lock{mutex_};
        inbox_.push_back(std::move(command));
    }
    wake_.notify_all();
}

auto Agent::job_of(std::uint64_t task) -> Job* {
    for (auto& j : jobs_) {
        if (j->task == task) {
            return j.get();
        }
    }
    return nullptr;
}

auto Agent::Job::control() -> tools::Control {
    return {.stop = stop, .paused = &paused, .progress = {}, .output = {}};
}

void Agent::start(Job& job, events::Command command) {
    if (job.thread.joinable()) {
        job.thread.join(); // its last program is over (the user could press its button)
    }
    job.cancelled = false;
    job.thread = std::jthread{[this, &job, command = std::move(command)](std::stop_token stop) {
        job.stop = stop;
        run_program(job, command);
    }};
}

void Agent::stop_job(Job& job) {
    {
        std::scoped_lock lock{mutex_};
        job.cancelled = true;
    }
    job.thread.request_stop();
    wake_.notify_all();
}

void Agent::run(std::stop_token stop) {
    using clock = std::chrono::steady_clock;
    const auto start_time = clock::now();
    const auto seconds = [&] { return std::chrono::duration<double>(clock::now() - start_time).count(); };
    std::vector<events::Command> commands;
    std::vector<events::Event> batch;
    double next = std::numeric_limits<double>::infinity();
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock{mutex_};
            const auto ready = [&] { return !inbox_.empty(); };
            if (next == std::numeric_limits<double>::infinity()) {
                wake_.wait(lock, stop, ready);
            } else {
                const auto deadline = start_time + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(next));
                wake_.wait_until(lock, stop, deadline, ready);
            }
            commands.swap(inbox_);
            inbox_.clear();
        }
        if (stop.stop_requested()) {
            break;
        }
        for (auto& c : commands) {
            dispatch(std::move(c), seconds());
        }
        commands.clear();
        next = player_.advance(seconds(), batch);
        for (auto& e : batch) {
            post_(std::move(e));
        }
    }
}

// One command, on the agent's thread: a tool task gets its own thread; answers, stops and pauses
// go to the task they name; the rest is the Player's.
void Agent::dispatch(events::Command c, double now) {
    if (const auto* levels = std::get_if<events::Levels>(&c)) {
        if (tools_) {
            tools_->set_levels(*levels); // from the next call on
        }
        return;
    }
    Job* job = nullptr;
    const auto task = std::visit([](const auto& x) -> std::uint64_t {
        if constexpr (requires { x.task; }) {
            return x.task;
        } else {
            return 0;
        }
    }, c);
    {
        std::scoped_lock lock{mutex_};
        job = job_of(task);
    }
    if (const auto* s = std::get_if<events::Start>(&c); s != nullptr && s->replaces != 0) {
        // The request replaced a task waiting for an answer: that answer is a no.
        Job* replaced = nullptr;
        {
            std::scoped_lock lock{mutex_};
            replaced = job_of(s->replaces);
        }
        if (replaced != nullptr) {
            stop_job(*replaced);
        }
        player_.command(events::Cancel{s->replaces}, now);
    }
    if (std::holds_alternative<events::Answer>(c)) {
        if (job != nullptr) {
            {
                std::scoped_lock lock{mutex_};
                job->answers.push_back(std::get<events::Answer>(c));
            }
            wake_.notify_all();
        }
        return;
    }
    if (std::holds_alternative<events::Cancel>(c)) {
        if (job != nullptr) {
            stop_job(*job);
        }
        player_.command(c, now);
        return;
    }
    if (std::holds_alternative<events::Pause>(c) || std::holds_alternative<events::Resume>(c)) {
        if (job != nullptr) {
            job->paused = std::holds_alternative<events::Pause>(c);
        }
        return;
    }
    if (const auto* a = std::get_if<events::Act>(&c); a != nullptr && a->action == "take_over") {
        // The user's own terminal; the task waits for them (Resume lets it go on).
        if (job != nullptr && tools_) {
            job->paused = true;
            if (auto r = tools_->take_over(a->task); !r) {
                post_({a->task, events::Thinking{"The terminal could not be opened: " + r.error().message}});
            } else {
                post_({a->task, events::Thinking{"Paused: the terminal is yours"}});
            }
        }
        return;
    }
    if (const auto* t = std::get_if<events::Toggle>(&c)) {
        if (job != nullptr && job->scenario == "watch") {
            switch_watch(*job, t->on);
        }
        return;
    }
    if (const auto* a = std::get_if<events::Act>(&c); a != nullptr && a->action == "undo" && tools_ &&
                                                      !tools_->undoable(a->task) && player_.scenario_of(a->task).empty()) {
        // A done bar or a notification left over: the journal has nothing of it to take back.
        post_({a->task, events::Finished{"Nothing to undo", "it was undone already, or nothing it did can be taken back"}});
        return;
    }
    if (!is_program(c, job)) {
        player_.command(c, now);
        return;
    }
    if (job == nullptr) {
        std::scoped_lock lock{mutex_};
        job = jobs_.emplace_back(std::make_unique<Job>()).get();
        job->task = task;
    }
    start(*job, std::move(c));
}

auto Agent::is_program(const events::Command& c, const Job* job) const -> bool {
    if (const auto* s = std::get_if<events::Start>(&c); s != nullptr && Player::scenario_for(s->text) == "duplicates") {
        return true; // played on a thread of its own, like the tools' tasks (it has a plan and a terminal)
    }
    if (!tools_) {
        return false;
    }
    if (const auto* s = std::get_if<events::Start>(&c)) {
        const auto scenario = Player::scenario_for(s->text);
        if (scenario == "cleanup") {
            return !tools_->host().temp.empty() && !tools_->host().downloads.empty();
        }
        return scenario == "folder" || scenario == "slow" || scenario == "space" || scenario == "terminal" ||
               (scenario == "watch" && tools_->host().monitor != nullptr) ||
               (scenario == "desktop" && tools_->host().sort_desktop);
    }
    if (std::holds_alternative<events::Undo>(c)) {
        return true;
    }
    if (const auto* act = std::get_if<events::Act>(&c)) {
        if (job != nullptr && job->scenario == "slow" && act->action.starts_with("close:")) {
            return true; // Close in the report
        }
        return act->action == "undo" && tools_->undoable(act->task); // the done bar's Undo, by the journal
    }
    return false;
}

void Agent::run_program(Job& job, const events::Command& c) {
    if (const auto* s = std::get_if<events::Start>(&c)) {
        if (tools_) {
            tools_->begin(s->task, s->text);
        }
        job.scenario = std::string{Player::scenario_for(s->text)};
        if (job.scenario == "slow") {
            slow(job);
        } else if (job.scenario == "space") {
            space(job);
        } else if (job.scenario == "desktop") {
            sort_desktop(job);
        } else if (job.scenario == "watch") {
            watch(job, s->text);
        } else if (job.scenario == "terminal") {
            count(job, s->text);
        } else if (job.scenario == "cleanup") {
            cleanup(job);
        } else if (job.scenario == "duplicates") {
            duplicates(job);
        } else {
            make_folder(job, s->text);
        }
    } else if (const auto* a = std::get_if<events::Act>(&c)) {
        if (a->action.starts_with("close:")) {
            std::uint32_t pid = 0;
            const auto digits = std::string_view{a->action}.substr(6);
            std::from_chars(digits.data(), digits.data() + digits.size(), pid);
            close_program(job, pid);
        } else {
            undo(job, job.task, std::nullopt);
        }
    } else if (const auto* u = std::get_if<events::Undo>(&c)) {
        job.scenario = "undo";
        undo(job, u->target, u->change);
    }
}

void Agent::pause(Job& job, double seconds) {
    std::unique_lock lock{mutex_};
    wake_.wait_for(lock, job.stop, std::chrono::duration<double>(seconds / speed_), [&] { return job.cancelled; });
}

auto Agent::ask(Job& job, const events::Permission& p) -> std::optional<events::Answer> {
    post_({job.task, p});
    std::unique_lock lock{mutex_};
    std::optional<events::Answer> choice;
    wake_.wait(lock, job.stop, [&] {
        if (job.cancelled) {
            return true; // stopped, or replaced by a new request: no answer
        }
        for (auto it = job.answers.begin(); it != job.answers.end(); ++it) {
            if (it->id == p.id) {
                choice = *it;
                job.answers.erase(it);
                return true;
            }
        }
        return false;
    });
    if (!choice) {
        job.cancelled = true;
    }
    return choice;
}

auto Agent::call(Job& job, std::string_view name, std::string_view args, std::function<void(const tools::ProgressInfo&)> progress,
                 std::function<void(std::string_view)> output) -> tools::Called {
    auto control = job.control();
    control.progress = std::move(progress);
    if (name == "shell.run") {
        // Every command in the agent's PowerShell tab: its line, then what it prints as it comes.
        glz::generic a;
        std::string line = "PS " + core::utf8::to_utf8(tools_->host().desktop.wstring()).value_or(std::string{}) + ">";
        if (!glz::read_json(a, args) && a.contains("command") && a["command"].is_string()) {
            line += " " + a["command"].get_string();
        }
        post_({job.task, events::Output{.tab = 1, .tab_label = "PowerShell \xC2\xB7 agent", .text = line + "\n", .command = true, .step = 0}});
        control.output = output ? std::move(output) : [this, task = job.task](std::string_view text) {
            post_({task, events::Output{.tab = 1, .tab_label = "PowerShell \xC2\xB7 agent", .text = std::string{text}, .command = false, .step = 0}});
        };
    }
    return tools_->run(job.task, name, args, [&](const events::Permission& p) { return ask(job, p); }, control);
}

auto Agent::over(const Job& job) -> bool {
    std::scoped_lock lock{mutex_};
    return job.cancelled || job.stop.stop_requested();
}

// ---- tool scenarios (with a Host) ----------------------------------------------------------------

namespace {

auto json_string(std::string_view s) -> std::string {
    auto j = core::json::write(s);
    return j ? std::move(*j) : std::string{"\"\""};
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

// "Create a folder Reports on the desktop", "make a new folder called Photos 2026" -> the name.
auto folder_name(std::string_view request) -> std::string {
    std::string lower{request};
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto at = lower.find("folder");
    std::string_view rest = at == std::string::npos ? std::string_view{} : request.substr(at + 6);
    std::string_view rest_lower = at == std::string::npos ? std::string_view{} : std::string_view{lower}.substr(at + 6);
    const auto trim = [&] {
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"' || rest.front() == '\'')) {
            rest.remove_prefix(1);
            rest_lower.remove_prefix(1);
        }
    };
    trim();
    for (std::string_view lead : {"called ", "named "}) {
        if (rest_lower.starts_with(lead)) {
            rest.remove_prefix(lead.size());
            rest_lower.remove_prefix(lead.size());
            trim();
        }
    }
    std::size_t end = rest.size();
    for (std::string_view tail : {" on the desktop", " on desktop", " on my desktop", " in ", "\"", "'", "?", "!"}) {
        end = std::min(end, rest_lower.find(tail));
    }
    std::string name{rest.substr(0, end)};
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name.empty() ? std::string{"New folder"} : name;
}

auto valid_name(std::string_view name) -> bool {
    return !name.empty() && name.size() < 200 && name.find_first_of("\\/:*?\"<>|") == std::string_view::npos &&
           name != "." && name != "..";
}

} // namespace

void Agent::slow(Job& job, std::string said) {
    const auto task = job.task;
    post_({task, events::Thinking{"Reading the computer's numbers"}});
    auto* monitor = tools_->host().monitor;
    if (monitor != nullptr) {
        monitor->sample(true); // the graphics card once: its rate needs a second reading, below
    }
    const auto info = call(job, "system.info", "{}"); // reads ask only if the user said so
    const auto list = call(job, "process.list", R"({"sort":"memory","top":1000})");
    if (over(job)) {
        return;
    }
    glz::generic i, l;
    if (info.outcome.code != 0 || list.outcome.code != 0 || glz::read_json(i, info.outcome.json) ||
        glz::read_json(l, list.outcome.json) || !i.contains("data") || !l.contains("data")) {
        post_({task, events::Failed{"The computer's numbers could not be read"}});
        return;
    }
    std::vector<report::Proc> procs;
    for (const auto& p : l["data"]["processes"].get_array()) {
        procs.push_back({.pid = static_cast<std::uint32_t>(p["pid"].get_number()),
                         .name = p["name"].get_string(),
                         .memory = static_cast<std::uint64_t>(p["memory"].get_number()),
                         .cpu = p["cpu_percent"].get_number(),
                         .readable = p["readable"].is_boolean() ? p["readable"].get_boolean() : true});
    }
    const auto count = static_cast<std::size_t>(l["data"]["total"].get_number());
    monitor::Snapshot minute;
    monitor::Temperature temperature;
    if (monitor != nullptr) {
        monitor->sample(true); // the graphics card's second reading: its rate
        minute = monitor->snapshot();
        temperature = monitor->temperature();
    } else {
        temperature = monitor::machine(tools_->host().settings.agent_elevated)->temperature();
    }
    auto r = report::make_report(report::machine_from_json(info.outcome.json).value_or(report::Machine{}), procs, count, minute, temperature, GetCurrentProcessId());
    // No model here: its sentence is the answer's words, after what it said (the report puts none in).
    post_({task, events::Delta{said.empty() ? r.headline : said + " " + r.headline}});
    job.closable.clear();
    for (const auto& p : r.processes) {
        if (p.closable) {
            job.closable.emplace_back(p.pid, p.name);
        }
    }
    post_({task, std::move(r)});
    post_({task, events::Actions{}}); // the turn's end, as the real agent says it (phase 10: "it works" ends here)
}

void Agent::close_program(Job& job, std::uint32_t pid) {
    const auto task = job.task;
    const auto offered = std::ranges::find(job.closable, pid, &std::pair<std::uint32_t, std::string>::first);
    if (offered == job.closable.end()) {
        post_({task, events::Failed{"That program was not offered to close"}});
        return;
    }
    const std::string name = offered->second;
    post_({task, events::Thinking{"Closing " + name}});
    const auto called = call(job, "process.kill", std::format(R"({{"pid":{}}})", pid));
    if (over(job)) {
        return;
    }
    const std::string said = called.outcome.code == 0 ? "Closed " + name + "."
                             : called.asked && !called.allowed ? std::string{"Nothing changed: you said no."}
                                                                : name + " was not closed: " + called.ruling.reason + ".";
    pause(job, 0.5); // the memory it held comes back
    slow(job, said);
}

void Agent::space(Job& job) {
    const auto task = job.task;
    post_({task, events::Thinking{"Reading the drive"}});
    const auto started = std::chrono::steady_clock::now();
    const auto info = call(job, "system.info", "{}");
    if (over(job)) {
        return;
    }
    glz::generic i;
    if (info.outcome.code != 0 || glz::read_json(i, info.outcome.json) || !i.contains("data")) {
        post_({task, events::Failed{"The drive could not be read"}});
        return;
    }
    const auto m = report::machine_from_json(info.outcome.json).value_or(report::Machine{});
    if (m.disk_total == 0) {
        post_({task, events::Failed{"Windows' drive could not be read"}});
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    post_({task, report::disk_quick(m, seconds)});
    post_({task, events::Actions{}}); // the turn's end
}

void Agent::make_folder(Job& job, std::string_view request) {
    const auto task = job.task;
    const std::string name = folder_name(request);
    if (!valid_name(name)) {
        post_({task, events::Failed{"A folder cannot be called \"" + name + "\""}});
        return;
    }
    post_({task, events::Thinking{"Creating the folder"}});
    const auto path = tools_->host().desktop / std::filesystem::path{core::utf8::to_wide(name).value_or(L"New folder")};
    const auto called = call(job, "fs.mkdir", R"({"path":)" + json_string(utf8(path.wstring())) + "}");
    if (over(job)) {
        return; // the user stopped it: the UI has moved on
    }
    if (called.outcome.code == 0) {
        post_({task, events::Finished{"Folder created", name + " on the desktop", {{"undo", "Undo", false}}}});
    } else if (called.asked && called.ruling.decision == security::Decision::Ask) {
        post_({task, events::Finished{"Nothing changed", "the folder was not created"}});
    } else {
        post_({task, events::Failed{called.ruling.reason.empty() ? "The folder could not be created" : called.ruling.reason}});
    }
}

namespace {

// Where a desktop file goes by its type; nullptr: it stays (shortcuts, what is not known).
auto group_of(std::wstring_view name) -> const char* {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos) {
        return nullptr;
    }
    std::wstring ext{name.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    static constexpr std::pair<const wchar_t*, const char*> kGroups[] = {
        {L"pdf", "Documents"}, {L"doc", "Documents"},  {L"docx", "Documents"}, {L"txt", "Documents"},
        {L"rtf", "Documents"}, {L"odt", "Documents"},  {L"xls", "Documents"},  {L"xlsx", "Documents"},
        {L"csv", "Documents"}, {L"ppt", "Documents"},  {L"pptx", "Documents"}, {L"md", "Documents"},
        {L"jpg", "Images"},    {L"jpeg", "Images"},    {L"png", "Images"},     {L"gif", "Images"},
        {L"bmp", "Images"},    {L"webp", "Images"},    {L"heic", "Images"},    {L"svg", "Images"},
        {L"exe", "Installers"}, {L"msi", "Installers"}, {L"msix", "Installers"}, {L"appx", "Installers"},
        {L"zip", "Archives"},  {L"rar", "Archives"},   {L"7z", "Archives"},    {L"gz", "Archives"}};
    for (const auto& [e, g] : kGroups) {
        if (ext == e) {
            return g;
        }
    }
    return nullptr;
}

} // namespace

void Agent::sort_desktop(Job& job) {
    const auto task = job.task;
    const auto& desktop = tools_->host().desktop;
    post_({task, events::Thinking{"Scanning the desktop"}});
    const auto listed = call(job, "fs.list", R"({"path":)" + json_string(utf8(desktop.wstring())) + "}");
    glz::generic l;
    if (listed.outcome.code != 0 || glz::read_json(l, listed.outcome.json) || !l.contains("data")) {
        post_({task, events::Failed{"The desktop could not be read"}});
        return;
    }
    // By group, in a fixed order: the files that go there.
    constexpr const char* kOrder[] = {"Documents", "Images", "Installers", "Archives"};
    std::vector<std::vector<std::wstring>> groups(std::size(kOrder));
    std::size_t files = 0;
    for (const auto& e : l["data"]["entries"].get_array()) {
        if (e["type"].get_string() != "file") {
            continue;
        }
        const auto name = core::utf8::to_wide(e["name"].get_string()).value_or(L"");
        if (const char* g = group_of(name)) {
            for (std::size_t i = 0; i < std::size(kOrder); ++i) {
                if (std::string_view{kOrder[i]} == g) {
                    groups[i].push_back(name);
                    ++files;
                }
            }
        }
    }
    if (files == 0) {
        post_({task, events::Finished{"Nothing to sort", "no documents, images, installers or archives here"}});
        return;
    }
    std::string plan = std::format("I'll put {} files into folders by type:\n", files);
    for (std::size_t i = 0; i < std::size(kOrder); ++i) {
        if (!groups[i].empty()) {
            plan += std::format("- **{}** \xE2\x80\x94 {} files\n", kOrder[i], groups[i].size());
        }
    }
    plan += "Shortcuts and everything else stay where they are.";
    post_({task, events::Delta{plan}});

    // Where each file's icon stands now: an undo puts it back there (the user's check: the files
    // came back in the first free cells).
    {
        std::vector<std::string> paths;
        for (const auto& group : groups) {
            for (const auto& name : group) {
                paths.push_back(utf8((desktop / name).wstring()));
            }
        }
        tools_->remember_spots(task, paths);
    }
    std::size_t done_before = 0; // in the groups already moved
    const auto progress = [&](std::size_t done) { // the bar's line, file by file
        post_({task, events::Progress{static_cast<float>(done_before + done) / static_cast<float>(files)}});
    };
    std::size_t moved = 0;
    std::string stopped; // why it stopped early
    for (std::size_t i = 0; i < std::size(kOrder) && stopped.empty(); ++i) {
        if (groups[i].empty()) {
            continue;
        }
        const auto folder = desktop / kOrder[i];
        progress(0);
        const auto made = call(job, "fs.mkdir", R"({"path":)" + json_string(utf8(folder.wstring())) + "}");
        if (over(job)) {
            return;
        }
        if (made.outcome.code != 0) {
            stopped = made.asked ? "you said no" : made.ruling.reason;
            break;
        }
        std::string moves = R"({"moves":[)";
        for (std::size_t k = 0; k < groups[i].size(); ++k) {
            moves += std::format(R"({}{{"from":{},"to":{}}})", k == 0 ? "" : ",",
                                 json_string(utf8((desktop / groups[i][k]).wstring())),
                                 json_string(utf8((folder / groups[i][k]).wstring())));
        }
        moves += "]}";
        const auto r = call(job, "fs.move", moves, [&](const tools::ProgressInfo& p) { progress(static_cast<std::size_t>(p.done)); });
        if (over(job)) {
            return;
        }
        if (r.outcome.code != 0) {
            stopped = r.asked ? "you said no" : r.ruling.reason;
            break;
        }
        moved += groups[i].size(); // the journal has each move, for Undo
        done_before += groups[i].size();
    }
    if (moved == 0) {
        post_({task, events::Finished{"Nothing changed", stopped.empty() ? "the desktop stays as it was" : stopped}});
    } else if (!stopped.empty()) {
        post_({task, events::Finished{"Partly sorted", std::format("{} files moved, then {}", moved, stopped),
                                      {{"undo", "Undo", false}}}});
    } else {
        post_({task, events::Finished{"Desktop sorted", std::format("{} files in folders by type", moved),
                                      {{"undo", "Undo", false}}}});
    }
}

void Agent::undo(Job& job, std::uint64_t target, std::optional<std::size_t> change) {
    const auto task = job.task;
    post_({task, events::Thinking{"Taking the changes back"}});
    const auto u = tools_->undo(task, target, change, [&](const events::Permission& p) { return ask(job, p); }, job.control());
    if (over(job)) {
        return;
    }
    const auto changes = [](std::size_t n) { return std::format("{} {}", n, n == 1 ? "change" : "changes"); };
    if (u.done == 0 && u.failed == 0 && !u.refused) {
        post_({task, events::Failed{"There is nothing to undo"}});
    } else if (u.done == 0 && u.refused) {
        post_({task, events::Finished{"Nothing changed", "you said no"}});
    } else if (u.failed == 0 && !u.refused) {
        post_({task, events::Finished{"Undone", changes(u.done) + " taken back"}});
    } else if (u.done == 0) {
        post_({task, events::Finished{"Not undone", u.first_error}});
    } else {
        post_({task, events::Finished{"Partly undone", std::format("{} taken back; {}", changes(u.done),
                                                                   u.refused ? std::string{"then you said no"} : u.first_error)}});
    }
}

void Agent::watch(Job& job, std::string_view request) {
    // What to watch (memory unless the processor or the disk is named) and its line (the first
    // number said, else 90 %).
    std::string lower{request};
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("gpu") != std::string::npos || lower.find("graphics") != std::string::npos || lower.find("video") != std::string::npos) {
        // The card is read only while the report is on screen (~1 ms a reading): not all the time.
        post_({job.task, events::Failed{"I can watch memory, the processor or the disk; the graphics card only while the report is open"}});
        return;
    }
    if (lower.find("processor") != std::string::npos || lower.find("cpu") != std::string::npos) {
        job.metric = monitor::Metric::Cpu;
        job.what = "the processor";
    } else if (lower.find("disk") != std::string::npos || lower.find("drive") != std::string::npos) {
        job.metric = monitor::Metric::Disk;
        job.what = "the disk";
    } else {
        job.metric = monitor::Metric::Memory;
        job.what = "memory";
    }
    job.above = 90.0f;
    if (const auto at = lower.find_first_of("0123456789"); at != std::string::npos) {
        float n = 0;
        const auto [end, ec] = std::from_chars(lower.data() + at, lower.data() + lower.size(), n);
        if (ec == std::errc{} && n > 0.0f && n < 100.0f) {
            job.above = n;
        }
    }
    switch_watch(job, true);
}

void Agent::switch_watch(Job& job, bool on) {
    auto* meter = tools_->host().monitor;
    if (meter == nullptr) {
        return;
    }
    if (on && !job.watch) {
        auto what = job.what;
        what[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[0])));
        job.watch = meter->watch(job.metric, job.above, [this, task = job.task, what, above = job.above](float v) {
            post_({task, events::Alert{std::format("{} is at {:.0f}%", what, v), std::format("over the {:.0f}% you set", above)}});
        });
    } else if (!on && job.watch) {
        meter->unwatch(*job.watch);
        job.watch.reset();
    }
    post_({job.task, events::Watching{"Watching " + job.what, std::format("I'll tell you if it is over {:.0f}%", job.above), on}});
}


void Agent::wait_paused(Job& job) {
    while (job.paused && !over(job)) {
        pause(job, 0.1 * speed_); // a tenth of a second, whatever the speed
    }
}

void Agent::count(Job& job, std::string_view request) {
    const auto task = job.task;
    int n = 5;
    if (const auto at = request.find_first_of("123456789"); at != std::string_view::npos) {
        std::from_chars(request.data() + at, request.data() + request.size(), n);
        n = std::clamp(n, 1, 300); // at most a minute and a half
    }
    // The one form the tests' fence lets run: it prints the numbers slowly.
    const std::string command = std::format("1..{} | % {{ Write-Output $_; Start-Sleep -m 300 }}", n);
    const std::string label = "PowerShell \xC2\xB7 agent";
    events::Plan plan{.items = {{std::format("Count to {}", n), events::PlanState::Run, {}}}};
    post_({task, plan});
    post_({task, events::Thinking{"Counting in the terminal"}});
    const auto called = call(job, "shell.run", R"({"command":)" + json_string(command) + "}", {}, [&](std::string_view text) {
        post_({task, events::Output{.tab = 1, .tab_label = label, .text = std::string{text}, .command = false, .step = 0}});
    });
    if (over(job)) {
        return;
    }
    wait_paused(job); // the user took it over: the task waits for them
    if (over(job)) {
        return;
    }
    plan.items[0].state = events::PlanState::Done;
    post_({task, plan});
    if (called.outcome.code == 0) {
        post_({task, events::Finished{std::format("Counted to {}", n), "in the PowerShell tab"}});
    } else if (called.asked && !called.allowed) {
        post_({task, events::Finished{"Nothing ran", "you said no"}});
    } else {
        post_({task, events::Failed{called.ruling.reason.empty() ? "The command did not run" : called.ruling.reason}});
    }
}


namespace {

auto size_text(std::uint64_t bytes) -> std::string {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024 ? std::format("{:.1f} GB", mb / 1024) : mb >= 1 ? std::format("{:.0f} MB", mb) : std::format("{} KB", (bytes + 1023) / 1024);
}

// "2026-08-10T09:00:00Z" for `days` before now (fs.list's modified, compared as text). Written
// from integers: chrono's "{:%Y…}" goes through std::locale, whose cache stays for the process.
auto days_ago_utc(int days) -> std::string {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - std::chrono::hours{24 * days});
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{t - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()), hms.hours().count(), hms.minutes().count(), hms.seconds().count());
}

auto installer(std::string_view name) -> bool {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string ext{name.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "exe" || ext == "msi" || ext == "msix";
}

} // namespace

void Agent::cleanup(Job& job) {
    const auto task = job.task;
    const auto& host = tools_->host();
    struct Found {
        std::string path;
        std::string name;
        std::uint64_t size = 0;
    };
    const auto list = [&](const std::filesystem::path& dir, bool recursive) -> std::optional<std::vector<Found>> {
        const auto r = call(job, "fs.list", std::format(R"({{"path":{},"recursive":{}}})", json_string(utf8(dir.wstring())), recursive));
        glz::generic l;
        if (r.outcome.code != 0 || glz::read_json(l, r.outcome.json) || !l.contains("data")) {
            return std::nullopt;
        }
        std::vector<Found> out;
        for (const auto& e : l["data"]["entries"].get_array()) {
            if (e["type"].get_string() == "file") {
                const auto name = e["name"].get_string();
                if (recursive || e["modified"].get_string() < days_ago_utc(30)) {
                    out.push_back({utf8((dir / core::utf8::to_wide(name).value_or(L"")).wstring()), name,
                                   static_cast<std::uint64_t>(e["size"].get_number())});
                }
            }
        }
        return out;
    };
    post_({task, events::Delta{"First I'll look at what can safely go, then clean up. I'll delete the installers only after your OK."}});
    // 1, 2: what there is.
    post_({task, events::Step{.id = 1, .kind = events::Kind::Read, .state = events::StepState::Run, .title = "Scanning the temp folders",
                              .detail = utf8(host.temp.wstring())}});
    const auto temp = list(host.temp, true);
    if (over(job)) {
        return;
    }
    std::uint64_t temp_bytes = 0;
    for (const auto& f : temp.value_or(std::vector<Found>{})) {
        temp_bytes += f.size;
    }
    post_({task, events::Step{.id = 1, .kind = events::Kind::Read, .state = events::StepState::Done, .title = "Scanned the temp folders",
                              .detail = utf8(host.temp.wstring()),
                              .result = std::format("{} files \xC2\xB7 {}", temp ? temp->size() : 0, size_text(temp_bytes))}});
    auto old = list(host.downloads, false);
    if (over(job)) {
        return;
    }
    std::vector<Found> installers;
    std::uint64_t installer_bytes = 0;
    for (auto& f : old.value_or(std::vector<Found>{})) {
        if (installer(f.name)) {
            installer_bytes += f.size;
            installers.push_back(std::move(f));
        }
    }
    events::Step found{.id = 2, .kind = events::Kind::Read, .state = events::StepState::Done, .title = "Found installers older than 30 days",
                       .detail = utf8(host.downloads.wstring()),
                       .result = std::format("{} files \xC2\xB7 {}", installers.size(), size_text(installer_bytes))};
    for (std::size_t i = 0; i < installers.size() && i < 2; ++i) {
        found.files.push_back({installers[i].name, {}, size_text(installers[i].size)});
    }
    found.more_files = installers.size() > 2 ? static_cast<std::uint32_t>(installers.size() - 2) : 0;
    post_({task, found});
    post_({task, events::Step{.id = 4, .kind = events::Kind::Delete, .state = events::StepState::Wait,
                              .title = std::format("Delete {} installers", installers.size()), .detail = "from Downloads",
                              .tag = "will ask permission"}});
    // 3, 4: to the Recycle Bin, each after its card.
    std::uint64_t freed = 0;
    std::size_t in_use = 0;
    std::string stopped;
    // `skip`: go on past a file another program holds open (a real %TEMP% always has some).
    const auto trash = [&](std::uint32_t id, const std::string& title, const std::vector<Found>& files, bool skip) {
        if (files.empty()) {
            return;
        }
        std::string paths = R"({"paths":[)";
        for (std::size_t i = 0; i < files.size(); ++i) {
            paths += (i == 0 ? "" : ",") + json_string(files[i].path);
        }
        paths += skip ? R"(],"skip_failed":true})" : "]}";
        post_({task, events::Step{.id = id, .kind = events::Kind::Delete, .state = events::StepState::Run, .title = title, .progress = 0}});
        const auto r = call(job, "fs.trash", paths, [&](const tools::ProgressInfo& p) {
            const float f = p.total == 0 ? 0.0f : static_cast<float>(p.done) / static_cast<float>(p.total);
            post_({task, events::Step{.id = id, .kind = events::Kind::Delete, .state = events::StepState::Run, .title = title,
                                      .progress = f, .progress_text = std::format("{} of {}", p.done, p.total)}});
            post_({task, events::Progress{f}});
        });
        std::uint64_t gone = 0;
        for (const auto& e : r.outcome.effects) {
            const auto path = utf8(e.to);
            for (const auto& f : files) {
                if (f.path == path) {
                    gone += f.size;
                }
            }
        }
        freed += gone;
        std::size_t skipped = 0;
        if (glz::generic o; r.outcome.code == 0 && !glz::read_json(o, r.outcome.json) && o.contains("data") &&
                            o["data"].contains("skipped")) {
            skipped = o["data"]["skipped"].get_array().size();
        }
        in_use += skipped;
        post_({task, events::Step{.id = id, .kind = events::Kind::Delete, .state = events::StepState::Done, .title = title,
                                  .result = std::format("{} files \xC2\xB7 {}", r.outcome.effects.size(), size_text(gone)),
                                  .notes = skipped == 0 ? std::vector<std::string>{}
                                                        : std::vector{std::format("Skipped {} files \xE2\x80\x94 in use", skipped)}}});
        if (r.outcome.code != 0 && stopped.empty()) {
            stopped = r.asked && !r.allowed ? "you said no" : r.ruling.reason;
        }
    };
    trash(3, "Deleting temp files to the Recycle Bin", temp.value_or(std::vector<Found>{}), true);
    if (over(job)) {
        return;
    }
    trash(4, std::format("Deleting {} installers to the Recycle Bin", installers.size()), installers, false);
    if (over(job)) {
        return;
    }
    const std::uint64_t whole = temp_bytes + installer_bytes;
    post_({task, events::Summary{.value = size_text(freed), .of = "/ " + size_text(whole), .label = "freed",
                                 .fraction = whole == 0 ? 1.0f : static_cast<float>(freed) / static_cast<float>(whole),
                                 .facts = {{"Deleting to", "Recycle Bin"}, {"Can be undone", "yes, 30 days", events::Tone::Ok}}}});
    post_({task, events::Progress{1.0f}});
    if (freed == 0 && !stopped.empty()) {
        post_({task, events::Finished{"Nothing changed", stopped}});
    } else if (freed == 0 && !tools_->undoable(task)) {
        post_({task, events::Finished{"Nothing to clean up", "no temp files, and no installers older than 30 days in Downloads"}});
    } else {
        const std::string left = in_use == 0 ? std::string{} : std::format("; {} in use, left where they were", in_use);
        post_({task, events::Finished{"Cleaned up", std::format("{} freed; the files are in the Recycle Bin{}", size_text(freed), left),
                                      {{"undo", "Undo", false}}}});
    }
}

void Agent::duplicates(Job& job) {
    const auto task = job.task;
    const std::string ps = "PowerShell \xC2\xB7 agent";
    const auto out = [&](std::string text, bool command, int step) {
        post_({task, events::Output{.tab = 1, .tab_label = ps, .text = std::move(text), .command = command, .step = step}});
    };
    events::Plan plan{.items = {{"Count the files", events::PlanState::Run, {}},
                                {"Group by size", events::PlanState::Wait, {}},
                                {"Hash the candidates", events::PlanState::Wait, {}},
                                {"Create the \xE2\x80\x9C" "Duplicates\xE2\x80\x9D folder", events::PlanState::Wait, {}},
                                {"Move the copies (will ask)", events::PlanState::Wait, {}}}};
    post_({task, events::Delta{"I'll compare the files by their contents (a hash), not by name \xE2\x80\x94 so renamed copies are found "
                               "too. The original is the oldest file in each group. (Played: no file is read.)"}});
    post_({task, plan});
    const auto step = [&](std::size_t i, std::string note) {
        plan.items[i].state = events::PlanState::Done;
        plan.items[i].note = std::move(note);
        if (i + 1 < plan.items.size()) {
            plan.items[i + 1].state = events::PlanState::Run;
        }
        post_({task, plan});
    };
    out("PS C:\\Users\\you> Get-ChildItem ~\\Pictures -Recurse -File -Include *.jpg,*.png,*.heic | Measure-Object\n", true, 0);
    pause(job, 0.6);
    out("\nCount    : 4812\n\n", false, 0);
    step(0, "4 812");
    out("PS C:\\Users\\you> $groups = Get-ChildItem ~\\Pictures -Recurse -File | Group-Object Length | Where Count -gt 1\n", true, 1);
    pause(job, 0.6);
    out("PS C:\\Users\\you> $groups.Count\n", true, 1);
    out("387  # groups of the same size \xE2\x86\x92 1 203 candidates\n\n", false, 1);
    step(1, {});
    out("PS C:\\Users\\you> python .agent\\hash.py --files candidates.json --algo xxh3\n", true, 2);
    for (int i = 1; i <= 4 && !over(job); ++i) {
        pause(job, 0.4);
        wait_paused(job);
        plan.items[2].note = std::format("{}%", i * 25);
        post_({task, plan});
        out(std::format("  match  IMG_{}.jpg  \xE2\x89\xA1  IMG_{} (1).jpg\n", 2040 + i, 2040 + i), false, 2);
    }
    if (over(job)) {
        return;
    }
    out("  found so far: 212 groups \xC2\xB7 1.8 GB\n", false, 2);
    step(2, {});
    post_({task, events::Finished{"212 groups of duplicates", "1.8 GB could be freed (played: nothing was moved)"}});
}

} // namespace app::sim
