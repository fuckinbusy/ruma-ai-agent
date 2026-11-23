#include "app/model_agent.hpp"

#include "ai/agent.hpp"
#include "ai/chat.hpp"
#include "ai/prompt.hpp"
#include "ai/schema.hpp"
#include "app/memory.hpp"
#include "app/present.hpp"
#include "app/report.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"
#include "net/net.hpp"
#include "net/proxy.hpp"
#include "security/secrets.hpp"
#include "tools/os.hpp"
#include "tools/tool.hpp"
#include "tools/winsys/check.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <tuple>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace app::model {

namespace {

// The end of each request: "(Local time: Tuesday 2026-10-13 14:05)" — the model's "today" and
// "older than a week" (05-AI-AGENT: what changes goes after the prompt, which stays cached).
auto local_time() -> std::string {
    static constexpr std::array<std::string_view, 7> kDays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("(Local time: {} {:04}-{:02}-{:02} {:02}:{:02})", kDays[t.wDayOfWeek % 7], t.wYear, t.wMonth, t.wDay, t.wHour,
                       t.wMinute);
}

// "2026-10-27": a remembered entry's date.
auto today() -> std::string {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay);
}

constexpr std::string_view kWatch = "watch.add";
constexpr std::string_view kWatchDescription =
    "Watch the computer in the background and tell the user when memory, the processor or the disk goes over a "
    "percent. It goes on after this answer, until the user switches it off.";
constexpr std::string_view kWatchSchema =
    R"({"type":"object","properties":{"what":{"type":"string","enum":["memory","cpu","disk"],"description":"What to watch."},)"
    R"("above":{"type":"number","minimum":1,"maximum":99,"description":"Tell the user when it goes over this percent."}},)"
    R"("required":["what","above"],"additionalProperties":false})";
constexpr std::string_view kTerminal = "PowerShell \xC2\xB7 agent";

auto utf8(const std::filesystem::path& p) -> std::string {
    return core::utf8::to_utf8(p.wstring()).value_or(std::string{});
}

// The user's folders for the system prompt (they do not change: they stay in DeepSeek's cache).
auto folders_of(const agent::Host& host) -> std::string {
    std::string lines = "Desktop: " + utf8(host.desktop) + "\n";
    if (!host.downloads.empty()) {
        lines += "Downloads: " + utf8(host.downloads) + "\n";
    }
    if (!host.documents.empty()) { // the host's: a copy's run names none of the user's folders
        lines += "Documents: " + utf8(host.documents) + "\n";
    }
    return lines;
}

// A count the model gave (a job's number, an entry's id, `since`): a whole number from 0 to 2^53,
// or none — never a 1.5 cut to 1 or a -1 cast to a huge one (final review J2).
// How the provider stands after a failed turn: by the error's kind (ai::chat sets it), never its
// words. A network failure with no kind is the provider out of reach; any other refusal came from
// a provider that answered.
auto trouble_of(const core::Error& e) -> events::Trouble {
    if (e.kind == "no_key") {
        return events::Trouble::NoKey;
    }
    if (e.kind == "key_rejected") {
        return events::Trouble::KeyRejected;
    }
    if (e.kind == "balance_empty") {
        return events::Trouble::Balance;
    }
    if (e.kind == "busy") {
        return events::Trouble::Busy;
    }
    if (e.code == core::Code::Net || e.code == core::Code::Timeout) {
        return events::Trouble::Offline;
    }
    return events::Trouble::None;
}

auto count_of(glz::generic& v) -> std::optional<std::uint64_t> {
    if (!v.is_number()) {
        return std::nullopt;
    }
    const double n = v.get_number();
    if (!(n >= 0.0 && n <= 9007199254740992.0 && n == std::floor(n))) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(n);
}

} // namespace

struct Agent::State {
    // One task: its conversation, what the screens showed, what the user said to it.
    struct Job {
        std::uint64_t task = 0;
        std::vector<ai::Message> history;
        std::optional<present::Task> view;
        std::vector<events::Answer> answers;   // under mutex
        std::vector<events::Reply> replies;    // under mutex: to ui.ask's questions (phase 12 § 4)
        std::uint64_t open_question = 0;       // under mutex: the question waiting for its reply (0: none)
        std::deque<std::string> refines;       // under mutex: typed while it works
        std::optional<std::string> pressed;    // under mutex: "continue" / "stop" for "go on?"
        bool cancelled = false;                // under mutex
        bool running = false;                  // under mutex
        bool answered = false;                 // under mutex: this run's answer is complete
        bool begun = false;                    // in the journal
        std::atomic<bool> paused{false};
        std::atomic<bool> answer_now{false};   // "Answer now" pressed (phase 12 § 9): the loop takes it
        std::string last_request;              // for Retry
        bool said = false;                     // the task's thread: its words (or a report) are on screen
        std::vector<ai::Image> asked_images;   // the user's pasted or dropped pictures, for the next request
        // "Only for the next request" (design/ui_model): this task's own model and thinking (empty:
        // the setting); its Retry goes with them too.
        std::string model_once;
        std::string thinking_once;
        std::atomic<bool> heard{false};        // a tool's result is in the history: show.text copies on a click only
        std::chrono::steady_clock::time_point ended{};
        std::optional<std::uint64_t> watch;    // a watcher on the monitor
        struct Watched {
            monitor::Metric metric = monitor::Metric::Memory;
            float above = 90.0f;
            std::string name; // "memory", "the processor"
        };
        std::optional<Watched> watched;        // what it watches: switched off, it can be switched on again
        std::optional<events::Failed> failure; // its thread's: posted after the run is marked over
        std::optional<events::ProviderState> trouble; // posted right after the failure
        std::stop_token stop;                  // its thread's
        std::jthread thread;
    };

    Post post;
    std::optional<agent::Tools> tools;
    Settings settings; // under mutex
    ai::Provider given;
    std::vector<ai::ToolSpec> specs;
    std::string system;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::atomic<std::uint64_t> questions{0}; // ui.ask's ids
    // Phase 12 § 6: what the agent remembers (data\memory.json; under mutex) and the user's rules
    // (data\agent.md, read for every request); the prompt's folders, for its rebuilding.
    std::optional<memory::Memory> memory;
    std::filesystem::path rules_file;
    // The user's private windows (phase 13a § 2.1): data\private.json, window.look's context.
    std::filesystem::path private_file;
    std::optional<tools::winsys::Context> windows;
    std::string folders;
    // Check deeper (phase 13a § 6): the helper, run on its own thread by the user's press only.
    std::function<core::Result<tools::winsys::reader::Data>()> read_deeper;
    std::atomic<bool> deepening{false};
    std::mutex deeper_mutex; // the helper's last read, for startup.list on a tool's thread
    std::shared_ptr<const tools::winsys::reader::Data> deeper_data;

    // The system prompt as it is now: the rules and the memory read anew.
    auto system_now() -> std::string {
        std::string remembered;
        {
            const std::scoped_lock lock{mutex};
            remembered = memory ? memory->prompt_lines() : std::string{};
        }
        return ai::system_prompt(folders, rules_file.empty() ? std::string{} : memory::read_rules(rules_file), remembered);
    }
    std::vector<std::unique_ptr<Job>> jobs; // under mutex; a job stays while the agent lives
    std::uint64_t last_task_asked = 0;      // under mutex: the conversation a follow-up continues
    std::mutex net_mutex;
    net::StatePtr net; // made at the first request that needs DeepSeek
    std::jthread deeper; // Check deeper's: the last member, so it ends first, while what it uses is here

    auto job_of(std::uint64_t task) -> Job* {
        const auto it = std::ranges::find(jobs, task, [](const auto& j) { return j->task; });
        return it == jobs.end() ? nullptr : it->get();
    }

    void cancel(Job& job) {
        job.cancelled = true;
        job.thread.request_stop();
        tools->jobs().stop_task(job.task); // its background commands end with it (phase 12 § 8)
        wake.notify_all();
    }

    // A background command ended (phase 12 § 8; on its thread): the sheet is told; unless it was
    // stopped, the model gets a note first in its next turn — or, the task over, a turn of its own.
    void job_finished(std::uint64_t task, const tools::jobs::State& st) {
        post({task, events::JobDone{.job = st.id, .command = st.command, .code = st.code, .stopped = st.stopped, .timed_out = st.timed_out}});
        if (st.stopped) {
            return; // the model's shell.stop, or the task cancelled: nothing goes on
        }
        const std::string note = std::format("[background job {} (`{}`) {}]", st.id, st.command,
                                             st.timed_out ? std::string{"ended after 2 hours"} : std::format("finished: exit {}", st.code));
        const std::scoped_lock lock{mutex};
        Job* job = job_of(task);
        if (job == nullptr || job->cancelled) {
            return;
        }
        if (job->running) {
            job->refines.push_back(note);
            wake.notify_all();
        } else {
            start(*job, note, false);
        }
    }

    // Starts `request` on the job's thread (the last one is over: joined at once).
    void start(Job& job, std::string request, bool retry) {
        if (job.thread.joinable()) {
            job.thread.join();
        }
        job.running = true;
        job.answered = false;
        job.cancelled = false;
        job.pressed.reset();
        job.answer_now = false; // a press for an earlier run is not this one's
        job.thread = std::jthread{[this, &job, request = std::move(request), retry](std::stop_token stop) {
            job.stop = stop;
            work(job, request, retry);
            {
                const std::scoped_lock lock{mutex};
                job.running = false;
                job.ended = std::chrono::steady_clock::now();
            }
            // The failure last, the run already over: its Retry, pressed the moment it arrives,
            // starts the turn again (start() joins this thread, which needs no lock any more).
            if (auto failed = std::exchange(job.failure, std::nullopt)) {
                post({job.task, std::move(*failed)});
            }
            if (auto trouble = std::exchange(job.trouble, std::nullopt)) {
                post({job.task, std::move(*trouble)});
            }
        }};
    }

    // ui.ask (phase 12 § 4): the question on the sheet, the task waiting for its reply; a stop, or
    // a new message of the user's, is no answer.
    auto ask_user(Job& job, events::Question q) -> events::Reply {
        const auto id = q.id;
        {
            const std::scoped_lock lock{mutex};
            job.open_question = id;
        }
        post({job.task, std::move(q)});
        std::unique_lock lock{mutex};
        events::Reply reply{.task = job.task, .id = id, .chosen = {}, .other = {}, .answered = false};
        wake.wait(lock, job.stop, [&] {
            if (job.cancelled) {
                return true;
            }
            const auto it = std::ranges::find(job.replies, id, &events::Reply::id);
            if (it == job.replies.end()) {
                return false;
            }
            reply = *it;
            job.replies.erase(it);
            return true;
        });
        job.open_question = 0;
        return reply;
    }

    auto ask(Job& job, const events::Permission& p) -> std::optional<events::Answer> {
        post({job.task, p});
        std::unique_lock lock{mutex};
        std::optional<events::Answer> choice;
        wake.wait(lock, job.stop, [&] {
            if (job.cancelled) {
                return true;
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
        return choice;
    }

    // "Go on?": the question in the answer, two buttons, the user's press (a stop is a no).
    auto go_on(Job& job, ai::agent::Ask about, double usd) -> bool {
        const std::string question = about == ai::agent::Ask::Budget ? std::format("\n\nThis task has spent ${:.4f}, its budget. Go on?", usd)
                                     : about == ai::agent::Ask::Repeat
                                         ? std::string{"\n\nIt keeps repeating the same steps. Go on?"}
                                         : std::string{"\n\nThis has taken many steps. Go on?"};
        post({job.task, events::Delta{question}});
        post({job.task, events::Actions{{{"continue", "Go on", true}, {"stop", "Stop", false}}}});
        std::unique_lock lock{mutex};
        wake.wait(lock, job.stop, [&] { return job.cancelled || job.pressed.has_value(); });
        const bool yes = job.pressed == std::optional<std::string>{"continue"};
        job.pressed.reset();
        return yes && !job.cancelled;
    }

    // `by`'s side of the network with its key from the keys file; none: no key for it (or no network).
    auto provider(const ai::catalog::Provider& by) -> std::optional<ai::Provider> {
        if (given) {
            return given;
        }
        Settings s;
        {
            const std::scoped_lock lock{mutex};
            s = settings;
        }
        auto key = security::load_key(by.id, s.secrets, ai::catalog::default_provider().id);
        if (!key) {
            return std::nullopt;
        }
        const std::scoped_lock lock{net_mutex};
        if (!net) {
            auto made = net::follows_system(s.proxy) ? net::init({.proxy = {}, .system = [] { return net::system_settings(); }})
                                                     : net::init({.proxy = net::settings_of(s.proxy, net::system_settings()), .system = {}});
            if (!made) {
                WA_ERROR("ai", "no network: {}", made.error().message);
                return std::nullopt;
            }
            net = std::move(*made);
        }
        auto shared = std::make_shared<security::SecureString>(std::move(*key));
        return ai::chat::provider(*net, by, [shared] { return std::string{shared->view()}; });
    }

    auto watch(Job& job, const ai::ToolCall& call) -> ai::agent::Result {
        glz::generic j;
        auto* meter = tools->host().monitor;
        if (glz::read_json(j, call.arguments) || !j.is_object() || meter == nullptr) {
            return {.content = R"({"error":"the computer cannot be watched now"})", .ok = false};
        }
        const std::string what = j.contains("what") && j["what"].is_string() ? j["what"].get_string() : "memory";
        const float above = j.contains("above") && j["above"].is_number() ? static_cast<float>(j["above"].get_number()) : 90.0f;
        const auto metric = what == "cpu" ? monitor::Metric::Cpu : what == "disk" ? monitor::Metric::Disk : monitor::Metric::Memory;
        const std::string name = what == "cpu" ? "the processor" : what == "disk" ? "the disk" : "memory";
        {
            // Under the lock the switch takes (the UI's Toggle): the job's watch is the two threads'.
            const std::scoped_lock lock{mutex};
            job.watched = Job::Watched{.metric = metric, .above = above, .name = name};
            start_watch(job, *meter);
        }
        return {.content = std::format(R"({{"watching":"{}","above":{:.0f}}})", what, above), .ok = true};
    }

    // The job's watch put on the monitor (again, after a switch off: the same watch).
    void start_watch(Job& job, monitor::Monitor& meter) {
        const auto& w = *job.watched;
        std::string title = w.name;
        title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        if (job.watch) {
            meter.unwatch(*job.watch);
        }
        job.watch = meter.watch(w.metric, w.above, [this, task = job.task, title, above = w.above](float v) {
            post({task, events::Alert{std::format("{} is at {:.0f}%", title, v), std::format("over the {:.0f}% you set", above)}});
        });
        post({job.task, events::Watching{"Watching " + w.name, std::format("I'll tell you if it is over {:.0f}%", w.above), true}});
    }

    // A translation card (phase 8): drawn from the call's fields; show.text's translation goes to
    // the clipboard without a question (the user asked for it; reading the clipboard still asks) —
    // unless a tool's result is in the conversation: a file or a title may carry words aimed at the
    // model, so then the card's Copy puts it there.
    auto show(Job& job, const ai::ToolCall& call) -> ai::agent::Result {
        static constexpr std::string_view kRefused = R"({"error":"the card's fields are not all there"})";
        if (call.name == present::kShowWord) {
            auto w = present::word_of(call.arguments);
            if (!w) {
                return {.content = std::string{kRefused}, .ok = false};
            }
            post({job.task, std::move(*w)});
        } else if (call.name == present::kShowPhrase) {
            auto p = present::phrase_of(call.arguments);
            if (!p) {
                return {.content = std::string{kRefused}, .ok = false};
            }
            post({job.task, std::move(*p)});
        } else {
            auto t = present::text_of(call.arguments);
            if (!t) {
                return {.content = std::string{kRefused}, .ok = false};
            }
            if (!job.heard) {
                t->copied = tools::os::current().set_clipboard(present::copied_text(*t)).has_value();
            }
            WA_INFO("agent", "translation shown, {} on the clipboard", t->copied ? "put" : job.heard ? "left for Copy" : "not put");
            const bool copied = t->copied;
            post({job.task, std::move(*t)});
            if (!copied) { // so the model does not say it is there
                return {.content = R"({"shown":true,"on_clipboard":false})", .ok = true};
            }
        }
        return {.content = R"({"shown":true})", .ok = true};
    }

    void switch_watch(Job& job, bool on) {
        auto* meter = tools->host().monitor;
        if (meter == nullptr) {
            return;
        }
        if (on) { // switched on again: the same watch as it was set
            if (!job.watch && job.watched) {
                start_watch(job, *meter);
            }
            return;
        }
        if (!job.watch) {
            return;
        }
        meter->unwatch(*job.watch);
        job.watch.reset();
        post({job.task, events::Watching{"Watching stopped", "switched off", false}});
    }

    void undo(Job& job, std::uint64_t target, std::optional<std::size_t> change) {
        const auto task = job.task;
        post({task, events::Thinking{"Taking the changes back"}});
        const tools::Control control{.stop = job.stop, .paused = &job.paused, .progress = {}, .output = {}};
        const auto u = tools->undo(task, target, change, [&](const events::Permission& p) { return ask(job, p); }, control);
        if (job.stop.stop_requested()) {
            return;
        }
        const auto changes = [](std::size_t n) { return std::format("{} {}", n, n == 1 ? "change" : "changes"); };
        if (u.done == 0 && u.failed == 0 && !u.refused) {
            post({task, events::Failed{"There is nothing to undo", {}}});
        } else if (u.done == 0 && u.refused) {
            post({task, events::Finished{"Nothing changed", "you said no", {}}});
        } else if (u.failed == 0 && !u.refused) {
            post({task, events::Finished{"Undone", changes(u.done) + " taken back", {}}});
        } else {
            post({task, events::Finished{"Partly undone", std::format("{} taken back; {}", changes(u.done),
                                                                      u.refused ? std::string{"then you said no"} : u.first_error),
                                         {}}});
        }
    }

    // One request of the user, on the job's thread.
    void work(Job& job, const std::string& request, bool retry) {
        const auto task = job.task;
        if (request.starts_with("\x01undo:")) { // an undo, not a request (see send)
            std::uint64_t target = task;
            const auto digits = std::string_view{request}.substr(6);
            std::from_chars(digits.data(), digits.data() + digits.size(), target);
            undo(job, target, std::nullopt);
            return;
        }
        if (!job.begun) {
            tools->begin(task, request);
            job.begun = true;
        } else {
            tools->heard(task, request); // the user's words: a web page they name opens unasked
        }
        if (!job.view) {
            job.view.emplace(task, request, &tools->host());
        }
        job.last_request = request;
        post({task, events::Thinking{"Thinking"}});
        Settings s;
        {
            const std::scoped_lock lock{mutex};
            s = settings;
        }
        // The model first: its provider is the one asked, with that provider's key.
        const std::string chosen_id = job.model_once.empty() ? s.model : job.model_once;
        const ai::catalog::Model* const model = ai::catalog::model(chosen_id);
        if (model == nullptr) { // config.json edited by hand, a model dropped: said, never another one quietly
            post({task, events::Failed{std::format("No such model: {}", chosen_id), {}}});
            return;
        }
        const ai::catalog::Provider& by = ai::catalog::provider_of(*model);
        auto chosen = provider(by);
        if (!chosen) {
            post({task, events::Failed{ai::chat::no_key(by), {{"set_key", "Add key", true}}}});
            post({task, events::ProviderState{std::string{by.id}, events::Trouble::NoKey}});
            return;
        }
        ai::agent::Setup setup;
        setup.provider = std::move(*chosen);
        setup.tools = specs;
        setup.system = system;
        if (memory) {
            setup.system_of = [this] { return system_now(); }; // the rules and the memory as they are now
        }
        setup.model = model;
        // Until roles are set by the user (stage 4c): a picture goes to the first model of the same
        // provider that sees, the summary to its cheapest.
        const auto others = ai::catalog::beside(*model);
        setup.vision_model = others.vision;
        setup.summary_model = others.summary;
        setup.thinking = job.thinking_once.empty() ? s.thinking : thinking_of(job.thinking_once);
        setup.limits.budget_usd = s.budget_usd;
        setup.now = [] { return std::chrono::system_clock::now(); };
        setup.context = [] { return local_time(); };
        setup.take_refine = [this, &job]() -> std::optional<std::string> {
            std::string text;
            {
                const std::scoped_lock lock{mutex};
                if (job.refines.empty()) {
                    return std::nullopt;
                }
                text = std::move(job.refines.front());
                job.refines.pop_front();
            }
            tools->heard(job.task, text); // on this thread, as the calls it may name a page for
            return text;
        };
        setup.execute = [this, &job](const ai::ToolCall& call) -> ai::agent::Result {
            // Paused (or the terminal taken over): the next call waits for Resume or a stop.
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, job.stop, [&] { return job.cancelled || !job.paused; });
            }
            if (job.stop.stop_requested()) {
                return {.content = R"({"error":"stopped by the user"})", .ok = false};
            }
            if (call.name == kWatch) {
                return watch(job, call);
            }
            if (call.name == present::kShowWord || call.name == present::kShowPhrase || call.name == present::kShowText) {
                return show(job, call);
            }
            if (call.name == present::kMemoryAdd || call.name == present::kMemoryForget) {
                // Phase 12 § 6: never asked, but seen (a line with Undo); never a secret.
                glz::generic a;
                if (glz::read_json(a, call.arguments) || !a.is_object()) {
                    return {.content = R"({"error":"arguments are not a JSON object"})", .ok = false};
                }
                const std::scoped_lock lock{mutex};
                if (!memory) {
                    return {.content = R"({"error":"there is no memory here"})", .ok = false};
                }
                if (call.name == present::kMemoryForget) {
                    const auto id = a.contains("id") ? count_of(a["id"]) : std::nullopt;
                    const bool gone = id && memory->forget(*id);
                    if (gone) {
                        (void)memory->save();
                    }
                    return {.content = gone ? R"({"forgotten":true})" : R"({"forgotten":false,"error":"no such entry"})", .ok = gone};
                }
                const std::string text = a.contains("text") && a["text"].is_string() ? a["text"].get_string() : std::string{};
                const auto added = memory->add(text, job.task, today());
                if (!added) {
                    const auto why = core::json::write(added.error().message);
                    return {.content = R"({"error":)" + (why ? *why : std::string{"\"\""}) + "}", .ok = false};
                }
                if (auto saved = memory->save(); !saved) {
                    WA_ERROR("memory", "not saved: {}", saved.error().message);
                }
                post({job.task, events::Remembered{.id = added->id, .text = added->text}});
                return {.content = std::format(R"({{"remembered":{}}})", added->id), .ok = true};
            }
            if (call.name == present::kPrivateAdd) {
                // Phase 13a § 2.1: a window never to look at, at the user's word; seen with Undo.
                glz::generic a;
                if (glz::read_json(a, call.arguments) || !a.is_object()) {
                    return {.content = R"({"error":"arguments are not a JSON object"})", .ok = false};
                }
                const auto field = [&](const char* k) { return a.contains(k) && a[k].is_string() ? a[k].get_string() : std::string{}; };
                if (private_file.empty()) {
                    return {.content = R"({"error":"there is no private windows list here"})", .ok = false};
                }
                const auto added = tools::winsys::add_private(private_file, field("program"), field("words"), today());
                if (!added) {
                    const auto why = core::json::write(added.error().message);
                    return {.content = R"({"error":)" + (why ? *why : std::string{"\"\""}) + "}", .ok = false};
                }
                post({job.task, events::Remembered{.id = added->id, .text = "never look at " + tools::winsys::describe(*added),
                                                   .private_window = true}});
                return {.content = std::format(R"({{"added":{}}})", added->id), .ok = true};
            }
            if (call.name == present::kShellJob || call.name == present::kShellStop) {
                // A background command of this task (phase 12 § 8): read or ended, never asked.
                glz::generic a;
                const bool parsed = !glz::read_json(a, call.arguments) && a.is_object() && a.contains("job");
                const auto job_id = parsed ? count_of(a["job"]) : std::nullopt;
                const auto since = parsed && a.contains("since") && !a["since"].is_null() ? count_of(a["since"]) : std::optional<std::uint64_t>{0};
                if (!job_id || !since) {
                    return {.content = R"({"error":"give {\"job\":N}, N and since whole numbers of 0 or more"})", .ok = false};
                }
                const auto id = *job_id;
                if (call.name == present::kShellStop) {
                    if (auto stopped = tools->jobs().stop(job.task, id); !stopped) {
                        const auto why = core::json::write(stopped.error().message);
                        return {.content = R"({"error":)" + (why ? *why : std::string{"\"\""}) + "}", .ok = false};
                    }
                    return {.content = std::format(R"({{"job":{},"stopping":true}})", id), .ok = true};
                }
                const auto state = tools->jobs().read(job.task, id, static_cast<std::size_t>(*since));
                if (!state) {
                    const auto why = core::json::write(state.error().message);
                    return {.content = R"({"error":)" + (why ? *why : std::string{"\"\""}) + "}", .ok = false};
                }
                return {.content = present::job_json(*state), .ok = true};
            }
            if (call.name == present::kAsk) {
                auto q = present::question_of(call.arguments);
                if (!q) {
                    const auto why = core::json::write(q.error().message);
                    return {.content = R"({"error":)" + (why ? *why : std::string{R"("bad question")"}) + "}", .ok = false};
                }
                q->id = ++questions;
                const auto reply = ask_user(job, std::move(*q));
                return {.content = present::reply_json(reply), .ok = true};
            }
            if (call.name == present::kPlanUpdate) {
                // The model's plan, as a checklist over the steps (phase 12 § 3): nothing runs.
                auto plan = present::checklist_of(call.arguments);
                if (!plan) {
                    const auto why = core::json::write(plan.error().message);
                    return {.content = R"({"error":)" + (why ? *why : std::string{R"("bad plan")"}) + "}", .ok = false};
                }
                post({job.task, std::move(*plan)});
                return {.content = R"({"shown":true})", .ok = true};
            }
            if (call.name == present::kSuggest) {
                // The model's next prompts, as chips under the answer: nothing runs.
                auto chips = present::suggestions(call.arguments);
                if (!chips) {
                    return {.content = R"({"error":"at most 3 prompts, each 1 to 60 characters"})", .ok = false};
                }
                post({job.task, std::move(*chips)});
                return {.content = R"({"shown":true})", .ok = true};
            }
            tools::Control control{.stop = job.stop, .paused = &job.paused, .progress = {}, .output = {}};
            control.progress = [this, &job, call](const tools::ProgressInfo& p) {
                for (auto& e : job.view->progress(call, p)) {
                    post({job.task, std::move(e)});
                }
            };
            control.output = [this, &job, call](std::string_view text) {
                for (auto& e : job.view->output(call, text)) {
                    post({job.task, std::move(e)});
                }
            };
            const auto called = tools->run(job.task, call.name, call.arguments,
                                           [this, &job](const events::Permission& p) { return ask(job, p); }, control);
            job.heard = true; // whatever it gave goes back to the model
            ai::agent::Result result{.content = called.outcome.json, .ok = called.outcome.code == 0, .images = {}};
            for (auto& e : job.view->finished(call, called)) {
                // A report made by this call: its sentence and tiles to the model, which begins its
                // answer with them in the user's language (the program writes no words of its own).
                if (const auto* report = std::get_if<events::Report>(&e)) {
                    result.content = report::with_report_summary(result.content, *report);
                }
                post({job.task, std::move(e)});
            }
            for (const auto& image : called.outcome.images) { // to the model, and to the window as a thumbnail
                result.images.push_back({.mime = image.mime, .data = image.bytes, .label = image.label});
                post({job.task, present::picture_event(image)});
            }
            return result;
        };
        // Each turn's words start a paragraph after what is on screen; what the turn said is kept
        // for a failure, so Retry can take it off the answer.
        std::string turn_said;
        std::string all_said; // every turn's words: the paths named before the last turn count too
        bool fresh = false;
        setup.events.on_turn = [&turn_said, &fresh] {
            turn_said.clear();
            fresh = true;
        };
        setup.events.on_text = [this, task, &job, &turn_said, &all_said, &fresh](std::string_view text) {
            std::string words = fresh && job.said ? "\n\n" : "";
            fresh = false;
            words += text;
            job.said = true;
            turn_said += words;
            all_said += words;
            post({task, events::Delta{std::move(words)}});
        };
        std::string thought;
        auto last_thought = std::chrono::steady_clock::time_point{};
        setup.events.on_reasoning = [this, task, &thought, &last_thought](std::string_view text) {
            thought += text;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_thought < std::chrono::milliseconds{150}) {
                return; // the line follows the thought, not every word of it
            }
            last_thought = now;
            const auto line = thought.substr(thought.find_last_of('\n') == std::string::npos ? 0 : thought.find_last_of('\n') + 1);
            post({task, events::Thinking{line.size() > 90 ? "\xE2\x80\xA6" + line.substr(line.size() - 90) : line}});
        };
        setup.events.on_tool_started = [this, &job](const ai::ToolCall& call) {
            for (auto& e : job.view->started(call)) {
                post({job.task, std::move(e)});
            }
        };
        setup.events.ask_continue = [this, &job](ai::agent::Ask about, double usd) { return go_on(job, about, usd); };
        setup.answer_now = &job.answer_now;
        // Phase 12 § 10: how full the history is; its old part summarized, said quietly.
        setup.events.on_context = [this, task](int percent) { post({task, events::Context{percent}}); };
        setup.events.on_compacted = [this, task] { post({task, events::Thinking{"Earlier messages summarized"}}); };
        setup.events.on_repeat = [this, task](std::string_view) { // the model is told; the user, quietly
            post({task, events::Thinking{"Repeating itself \xE2\x80\x94 told to stop"}});
        };
        const ai::catalog::Model* saw = nullptr; // a request went to this model for a picture: the cost line says so
        setup.events.on_vision_model = [&saw](const ai::catalog::Model& m) { saw = &m; };
        // Each turn's usage as the API gave it: the log the interface's cost line is checked against.
        setup.events.on_usage = [task](const ai::Usage& u, double usd) {
            WA_INFO("ai", "task {} usage: prompt {} (cache hit {}, miss {}), completion {} (reasoning {}); so far ${:.6f}", task, u.prompt,
                    u.cache_hit, u.cache_miss, u.completion, u.reasoning, usd);
        };

        // Retry goes on from the history as it is (the failed turn was not kept): the request is
        // not asked again after its tools ran, and a refine taken before the failure stays.
        const bool go_on_from_history = retry && !job.history.empty();
        const auto out = go_on_from_history ? ai::agent::resume(job.history, setup, job.stop)
                                            : ai::agent::run(job.history, request, std::exchange(job.asked_images, {}), setup, job.stop);
        switch (out.end) {
        case ai::agent::End::Done:
            WA_INFO("ai", "task {} done: prompt {} (cache hit {}), completion {}; ${:.6f}", task, out.usage.prompt, out.usage.cache_hit,
                    out.usage.completion, out.usd);
            {
                // Before the answer's last events go out: a question asked the moment they arrive
                // is a follow-up (running is cleared later, after this thread's last lock).
                const std::scoped_lock lock{mutex};
                job.answered = true;
                job.ended = std::chrono::steady_clock::now();
            }
            for (auto& e : job.view->answered(out.text, out.usage, out.usd, all_said)) {
                if (auto* spent = std::get_if<events::Spent>(&e)) {
                    spent->vision = saw != nullptr;
                    // design/ui_model: the answer says what it was made with (Flash for a picture).
                    spent->model = saw != nullptr ? std::string{saw->id} : chosen_id;
                    spent->thinking = std::string{thinking_id(setup.thinking)};
                    spent->chosen = chosen_id;
                }
                post({task, std::move(e)});
            }
            post({task, events::ProviderState{std::string{by.id}, events::Trouble::None}});
            break;
        case ai::agent::End::Failed: {
            const events::Trouble trouble = trouble_of(out.error);
            const bool keyless = trouble == events::Trouble::NoKey || trouble == events::Trouble::KeyRejected;
            job.trouble = events::ProviderState{std::string{by.id}, trouble};
            job.failure = events::Failed{out.error.message,
                                         keyless ? std::vector<events::Action>{{"set_key", "Add key", true}}
                                                 : std::vector<events::Action>{{"retry", "Retry", true}},
                                         turn_said};
            break;
        }
        case ai::agent::End::Stopped:
            post({task, events::Finished{"Stopped", "as you chose", {}}});
            break;
        case ai::agent::End::Cancelled: break; // the user's stop: the sheet says it already
        }
    }
};

Agent::Agent(Post post, agent::Host host, Settings settings, ai::Provider provider) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.post = std::move(post);
    s.settings = std::move(settings);
    s.given = std::move(provider);
    s.folders = folders_of(host);
    s.system = ai::system_prompt(s.folders);
    if (!host.journal.empty()) { // the agent's data folder (phase 12 § 6)
        const auto data = host.journal.parent_path();
        s.memory = memory::Memory::load(data / "memory.json");
        s.rules_file = data / "agent.md";
        s.private_file = data / "private.json";
        s.windows.emplace(tools::winsys::Context{.registry = host.registry, .folders = host.folders,
                                                 .private_file = s.private_file, .read_tasks = &tools::winsys::tasks,
                                                 .read_wmi = &tools::winsys::wmi_subscriptions, .quarantine = data / "quarantine"});
        s.windows->check_state = std::make_shared<tools::winsys::CheckState>(); // a stopped check continues from its step
        s.windows->check_sources = host.check_sources;
        s.windows->deeper_data = [&s] {
            const std::scoped_lock kept{s.deeper_mutex};
            return s.deeper_data;
        };
        tools::winsys::use_context(&*s.windows);
    }
    for (const auto* t : tools::available(host.settings)) {
        if (auto strict = ai::strict_schema(t->schema())) {
            s.specs.push_back({.name = std::string{t->name}, .description = std::string{t->description}, .schema = std::move(*strict)});
        } else {
            WA_ERROR("ai", "{}: its schema cannot be made strict: {}", t->name, strict.error().message);
        }
    }
    if (host.monitor != nullptr) {
        s.specs.push_back({.name = std::string{kWatch}, .description = std::string{kWatchDescription}, .schema = std::string{kWatchSchema}});
    }
    for (const auto& [name, description, schema] :
         {std::tuple{present::kShellJob, present::kShellJobDescription, present::kShellJobSchema},
          std::tuple{present::kShellStop, present::kShellStopDescription, present::kShellStopSchema},
          std::tuple{present::kMemoryAdd, present::kMemoryAddDescription, present::kMemoryAddSchema},
          std::tuple{present::kMemoryForget, present::kMemoryForgetDescription, present::kMemoryForgetSchema}}) {
        s.specs.push_back({.name = std::string{name}, .description = std::string{description}, .schema = std::string{schema}});
    }
    s.specs.push_back({.name = std::string{present::kPrivateAdd},
                       .description = std::string{present::kPrivateAddDescription},
                       .schema = std::string{present::kPrivateAddSchema}});
    s.specs.push_back({.name = std::string{present::kAsk},
                       .description = std::string{present::kAskDescription},
                       .schema = std::string{present::kAskSchema}});
    s.specs.push_back({.name = std::string{present::kPlanUpdate},
                       .description = std::string{present::kPlanUpdateDescription},
                       .schema = std::string{present::kPlanUpdateSchema}});
    s.specs.push_back({.name = std::string{present::kSuggest},
                       .description = std::string{present::kSuggestDescription},
                       .schema = std::string{present::kSuggestSchema}});
    for (const auto& [name, description, schema] :
         {std::tuple{present::kShowWord, present::kShowWordDescription, present::kShowWordSchema},
          std::tuple{present::kShowPhrase, present::kShowPhraseDescription, present::kShowPhraseSchema},
          std::tuple{present::kShowText, present::kShowTextDescription, present::kShowTextSchema}}) {
        s.specs.push_back({.name = std::string{name}, .description = std::string{description}, .schema = std::string{schema}});
    }
    s.read_deeper = host.read_deeper;
    s.tools.emplace(std::move(host));
    s.tools->jobs().on_finished([&s](std::uint64_t task, const tools::jobs::State& st) { s.job_finished(task, st); });
}

Agent::~Agent() {
    auto& s = *state_;
    if (s.windows) {
        tools::winsys::use_context(nullptr); // its context goes with it
    }
    std::vector<State::Job*> jobs;
    {
        const std::scoped_lock lock{s.mutex};
        for (auto& j : s.jobs) {
            j->cancelled = true;
            j->thread.request_stop();
            jobs.push_back(j.get());
        }
    }
    s.wake.notify_all();
    // The background commands end now, their last on_finished done, while what it uses is here
    // (phase 12 § 8); every job is cancelled above, so none starts a turn.
    s.tools->jobs().end();
    for (auto* j : jobs) {
        if (j->thread.joinable()) {
            j->thread.join();
        }
        if (j->watch && s.tools->host().monitor != nullptr) {
            s.tools->host().monitor->unwatch(*j->watch);
        }
    }
}

auto Agent::last_task() const -> std::uint64_t {
    return state_->tools->last_task();
}

auto Agent::memory_entries() const -> std::vector<memory::Entry> {
    const std::scoped_lock lock{state_->mutex};
    return state_->memory ? state_->memory->entries() : std::vector<memory::Entry>{};
}

auto Agent::private_rules() const -> std::vector<tools::winsys::PrivateRule> {
    return tools::winsys::load_private(state_->private_file);
}

auto Agent::rules_file() const -> std::filesystem::path {
    const auto& file = state_->rules_file;
    std::error_code ec;
    if (!file.empty() && !std::filesystem::exists(file, ec)) { // a start for the user, never over theirs
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream{file, std::ios::binary} << "# Your rules for the agent, in your own words: one per line.\n"
                                                 "# For example: Answer in Russian. Never touch my Downloads folder.\n";
    }
    return file;
}

void Agent::set_settings(const Settings& settings) {
    const std::scoped_lock lock{state_->mutex};
    state_->settings = settings;
}

void Agent::send(events::Command command) {
    auto& s = *state_;
    const std::scoped_lock lock{s.mutex};
    if (const auto* levels = std::get_if<events::Levels>(&command)) {
        s.tools->set_levels(*levels);
        return;
    }
    const auto task = std::visit([](const auto& x) -> std::uint64_t {
        if constexpr (requires { x.task; }) {
            return x.task;
        } else {
            return 0;
        }
    }, command);
    State::Job* job = s.job_of(task);
    const auto made = [&]() -> State::Job& {
        if (job == nullptr) {
            job = s.jobs.emplace_back(std::make_unique<State::Job>()).get();
            job->task = task;
        }
        return *job;
    };

    if (const auto* start = std::get_if<events::Start>(&command)) {
        if (auto* replaced = start->replaces != 0 ? s.job_of(start->replaces) : nullptr) {
            s.cancel(*replaced); // its question's answer is a no
        }
        auto& j = made();
        const bool new_job = j.history.empty();
        bool continues = false;
        // A follow-up: the last task's conversation, if it ended a short while ago — unless the
        // user said "New chat".
        if (const auto* before = s.job_of(s.last_task_asked);
            !start->fresh && before != nullptr && before != &j && j.history.empty() && (!before->running || before->answered) &&
            !before->cancelled && std::chrono::steady_clock::now() - before->ended <= s.settings.follow_up) {
            j.history = before->history;
            // The request's own model when it names one ("only for the next request"), else the setting.
            if (const auto* next = ai::catalog::model(start->model.empty() ? s.settings.model : start->model);
                next == nullptr || next->vision != ai::catalog::Tri::Yes) {
                ai::agent::thin_images(j.history, 0, 0); // a model without vision gets them as words
            }
            j.heard = before->heard.load();
            continues = true;
            if (before->view) { // its screens' facts too: what it read of the machine
                j.view.emplace(task, start->text, &s.tools->host());
                j.view->carry(*before->view);
            }
        }
        if (new_job) {
            s.post({task, events::Conversation{continues}}); // a new one ends the sheet's session
        }
        s.last_task_asked = task;
        if (j.running) {
            j.refines.push_back(start->text); // never waits for a running request here (the lock is held)
        } else {
            for (const auto& image : start->images) { // in the request itself: the user's own act, no question
                j.asked_images.push_back({.mime = image.mime, .data = image.bytes, .label = image.label});
            }
            j.model_once = start->model;
            j.thinking_once = start->thinking;
            s.start(j, start->text, false);
        }
    } else if (std::holds_alternative<events::Cancel>(command)) {
        if (job != nullptr) {
            s.cancel(*job);
        }
    } else if (const auto* answer = std::get_if<events::Answer>(&command)) {
        if (job != nullptr) {
            job->answers.push_back(*answer);
            s.wake.notify_all();
        }
    } else if (const auto* add = std::get_if<events::AddPrivate>(&command)) {
        // The user's own field (phase 13a § 2.1): kept at once; the shell shows the list again.
        if (!s.private_file.empty()) {
            if (auto added = tools::winsys::add_private(s.private_file, add->program, add->words, today()); !added) {
                WA_WARN("app", "private window not added: {}", added.error().message);
            }
        }
    } else if (const auto* forget = std::get_if<events::Forget>(&command)) {
        if (forget->private_window) { // a private windows rule (phase 13a): only the user takes one back
            if (!s.private_file.empty()) {
                (void)tools::winsys::forget_private(s.private_file, forget->id);
            }
        } else if (s.memory) { // the line's Undo, the list's Forget, "Forget everything" (phase 12 § 6)
            if (forget->all) {
                s.memory->clear();
            } else {
                (void)s.memory->forget(forget->id);
            }
            if (auto saved = s.memory->save(); !saved) {
                WA_ERROR("memory", "not saved: {}", saved.error().message);
            }
        }
    } else if (const auto* reply = std::get_if<events::Reply>(&command)) {
        if (job != nullptr) {
            job->replies.push_back(*reply);
            s.wake.notify_all();
        }
    } else if (const auto* refine = std::get_if<events::Refine>(&command)) {
        auto& j = made();
        if (j.open_question != 0) { // a new message instead of the reply: no answer, and the message goes on
            j.replies.push_back({.task = task, .id = j.open_question, .chosen = {}, .other = {}, .answered = false});
            s.wake.notify_all();
        }
        if (j.running) {
            j.refines.push_back(refine->text);
        } else {
            s.start(j, refine->text, false);
        }
    } else if (std::holds_alternative<events::Pause>(command) || std::holds_alternative<events::Resume>(command)) {
        if (job != nullptr) {
            job->paused = std::holds_alternative<events::Pause>(command); // under the lock: a waiting call sees it
            s.wake.notify_all();
        }
    } else if (const auto* toggle = std::get_if<events::Toggle>(&command)) {
        if (job != nullptr) {
            s.switch_watch(*job, toggle->on);
        }
    } else if (const auto* undo = std::get_if<events::Undo>(&command)) {
        auto& j = made();
        if (!j.running) {
            s.start(j, std::format("\x01undo:{}", undo->target), false);
        }
    } else if (const auto* act = std::get_if<events::Act>(&command)) {
        if (job == nullptr) {
            return;
        }
        if (act->action == "continue" || act->action == "stop") {
            job->pressed = act->action;
            s.wake.notify_all();
        } else if (act->action == "answer_now") {
            job->answer_now = true; // the loop cuts the turn under way and asks for the answer
        } else if (act->action == "retry" && !job->running) {
            s.start(*job, job->last_request, true);
        } else if (act->action == "undo" && !job->running) {
            s.start(*job, std::format("\x01undo:{}", task), false);
        } else if (act->action == "take_over") {
            job->paused = true;
            if (auto r = s.tools->take_over(task); !r) {
                s.post({task, events::Thinking{"The terminal could not be opened: " + r.error().message}});
            } else {
                s.post({task, events::Thinking{"Paused: the terminal is yours"}});
            }
        } else if (act->action.starts_with("close:") && job->view) {
            // A button is the user's line to the model: "Close OBS (pid 1234)."
            std::uint32_t pid = 0;
            const auto digits = std::string_view{act->action}.substr(6);
            std::from_chars(digits.data(), digits.data() + digits.size(), pid);
            const auto name = job->view->program(pid);
            const std::string line = std::format("Close {} (pid {}).", name.value_or("the program"), pid);
            if (job->running) {
                job->refines.push_back(line);
            } else {
                s.start(*job, line, false);
            }
        } else if ((act->action.starts_with("finding:") || act->action.starts_with("check_from:")) && job->view) {
            // The threat report's buttons (phase 13a): a finding's action, or the check on from its step.
            std::optional<std::string> line;
            if (act->action.starts_with("check_from:")) {
                line = std::format("Continue the threat check from step {}.", act->action.substr(11));
            } else {
                std::uint32_t row = 0, which = 0;
                const auto rest = std::string_view{act->action}.substr(8);
                const auto colon = rest.find(':');
                if (colon != std::string_view::npos) {
                    std::from_chars(rest.data(), rest.data() + colon, row);
                    std::from_chars(rest.data() + colon + 1, rest.data() + rest.size(), which);
                    line = job->view->finding(row, which);
                }
            }
            if (line) {
                if (job->running) {
                    job->refines.push_back(*line);
                } else {
                    s.start(*job, *line, false);
                }
            }
        } else if (act->action.starts_with("deeper:") && job->view && !job->running && s.read_deeper && s.windows &&
                   s.windows->check_state && !s.deepening.exchange(true)) {
            // Check deeper (phase 13a § 6): the user's press, never the model's, on its own thread (the
            // UAC prompt waits for the user there). The Startup tab's: its set read again with the
            // helper's boot log; the threat report's: a copy of the finished check deepened.
            std::uint32_t id = 0;
            const auto digits = std::string_view{act->action}.substr(7);
            std::from_chars(digits.data(), digits.data() + digits.size(), id);
            const bool startup = id != 0 && job->view->startup_set_id() == id;
            std::optional<tools::winsys::CheckState> state;
            if (!startup) {
                const auto& kept = *s.windows->check_state;
                if (kept.stages.size() == tools::winsys::kStages.size() &&
                    std::ranges::all_of(kept.stages, [](const auto& st) { return st.state == tools::winsys::StageState::Done; })) {
                    state = kept;
                }
            }
            if (!startup && !state) {
                s.deepening = false;
                s.post({task, job->view->inside("Check deeper", "It needs a finished threat check first", true)});
            } else {
                const auto sources = state ? (s.windows->check_sources ? *s.windows->check_sources : tools::winsys::real_sources())
                                           : tools::winsys::Sources{};
                s.deeper = std::jthread{[&s, job, task, id, startup, state = std::move(state), sources](std::stop_token) mutable {
                    const auto data = s.read_deeper();
                    if (data) { // kept for startup.list from now on (its Startup tab's timing)
                        const std::scoped_lock kept{s.deeper_mutex};
                        s.deeper_data = std::make_shared<const tools::winsys::reader::Data>(*data);
                    }
                    std::optional<tools::winsys::CheckReport> report;
                    std::optional<tools::Called> listed;
                    if (data && state) {
                        report = tools::winsys::deepen(sources, *state, *data);
                    } else if (data) {
                        listed = s.tools->run(task, "startup.list", "{}", [](const events::Permission&) { return std::nullopt; });
                    }
                    const std::scoped_lock lock{s.mutex};
                    if (!data) {
                        s.post({task, job->view->inside("Check deeper", "Nothing more was read: " + data.error().message, true)});
                    } else if (report) {
                        if (auto deeper = job->view->deepened(*report, id)) {
                            s.post({task, events::Data{std::move(*deeper), false}});
                            s.post({task, job->view->inside("Checked deeper with administrator rights",
                                                            std::format("{} places in all; {} not read even so", report->places_checked,
                                                                        report->not_read.size()),
                                                            false)});
                        }
                    } else if (listed) {
                        if (auto again = job->view->startup_again(*listed, id)) {
                            s.post({task, events::Data{std::move(*again), false}});
                            s.post({task, job->view->inside("Checked deeper with administrator rights",
                                                            data->events_read ? "the boot log read: each program's delay"
                                                                              : "the boot log could not be read even so",
                                                            !data->events_read)});
                        }
                    }
                    s.deepening = false;
                }};
            }
        } else if (act->action.starts_with("uninstall:") && job->view) {
            // The Programs tab's Uninstall… (phase 13a): the user's line, naming the program and its version.
            std::uint32_t row = 0;
            const auto digits = std::string_view{act->action}.substr(10);
            std::from_chars(digits.data(), digits.data() + digits.size(), row);
            if (const auto name = job->view->app(row)) {
                const std::string line = std::format("Uninstall {}.", *name);
                if (job->running) {
                    job->refines.push_back(line);
                } else {
                    s.start(*job, line, false);
                }
            }
        }
    }
}

} // namespace app::model
