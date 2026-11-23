// agent-cli — the agent in a console (05-AI-AGENT § The CLI harness).
//
//   agent-cli [--model M] [--thinking off|low|high|max] [--budget USD] [--yes] [--fence DIR]
//             [--wd DIR] [--desktop DIR] [--record FILE | --replay FILE] [--secrets FILE] [--image FILE] "request"
//   agent-cli --set-key [--secrets <file>]     the DeepSeek key from stdin (no echo) into DPAPI
//   agent-cli --probe [--raw] [--secrets <file>]   what phase 7 had to know of the live API
//
// A request runs the agent's loop (ai::agent) with every tool through tools::call: the policy, a
// y/N question on the console for a change (--yes answers yes, never to a hard refusal), the
// audit in the agent's data folder. The model's text goes to stdout as it comes, the steps as
// "> tool args" lines, then the usage and the price. --record writes the exchanges to a cassette
// (no key, the profile folder and the computer's name scrubbed); --replay plays one, no network.
// --fence DIR: anything changed outside DIR ends the run (exit code 3).
// --image FILE (png, jpg, gif, webp; repeatable): the picture as it is, in the request (phase 9).
// The key file is the agent's own (%LOCALAPPDATA%\Agent\secrets.bin) unless --secrets names
// another. The key is never printed, logged or put on a command line.
// Exit codes: 0 done, 2 failed (no key, the API said no, the network), 3 stopped or cancelled.
#include "ai/agent.hpp"
#include "ai/cassette.hpp"
#include "ai/chat.hpp"
#include "ai/prompt.hpp"
#include "ai/schema.hpp"
#include "app/agent_tools.hpp"
#include "app/config.hpp"
#include "cli/console.hpp"
#include "core/crash.hpp"
#include "core/win.hpp"
#include "net/net.hpp"
#include "net/proxy.hpp"
#include "security/audit.hpp"
#include "security/secrets.hpp"
#include "tools/call.hpp"
#include "tools/os.hpp"
#include "tools/tool.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <future>
#include <iterator>
#include <iostream>
#include <mutex>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

void say(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

using cli::data_dir;

auto usage() -> int {
    say("usage: agent-cli [--model M] [--thinking off|low|high|max] [--budget USD] [--yes] [--fence DIR]\n"
        "                 [--wd DIR] [--record FILE | --replay FILE] [--secrets FILE] [--image FILE] \"request\"\n"
        "       agent-cli --set-key [--secrets <file>]\n"
        "       agent-cli --probe [--raw] [--secrets <file>]");
    return 2;
}

// The model's words as they come: stdout, at once.
void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// The user's folders for the system prompt: "Desktop: C:\Users\…" lines.
// --desktop DIR: the model's desktop is DIR, and no other folder of the user is named (a live
// recording on a copy: the model never learns the real folders' paths).
auto profile_paths(const std::optional<std::filesystem::path>& desktop) -> std::string {
    if (desktop) {
        return "Desktop: " + cli::utf8(desktop->wstring()) + "\n";
    }
    std::string lines;
    const std::pair<const char*, const GUID*> folders[] = {
        {"Desktop", &FOLDERID_Desktop}, {"Documents", &FOLDERID_Documents}, {"Downloads", &FOLDERID_Downloads}};
    for (const auto& [name, id] : folders) {
        if (auto p = core::win::known_folder(*id)) {
            lines += std::string{name} + ": " + cli::utf8(p->wstring()) + "\n";
        }
    }
    return lines;
}

auto computer_name() -> std::string {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? cli::utf8(std::wstring_view{name, size}) : std::string{};
}

// Ctrl+C: the request stops (the stream cancelled, no tool after it).
std::stop_source* g_stop = nullptr;
auto WINAPI on_ctrl(DWORD kind) -> BOOL {
    if ((kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT) && g_stop != nullptr) {
        g_stop->request_stop();
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::string model{ai::catalog::default_model().id};
    ai::Thinking thinking = ai::Thinking::Low;
    double budget = 0.10;
    bool yes = false;
    std::optional<std::filesystem::path> fence, record, replay;
    std::filesystem::path wd = std::filesystem::current_path();
    std::filesystem::path secrets;
    std::string host; // --host: the tests' mock server; empty: the provider's own
    std::optional<std::filesystem::path> desktop;          // --desktop: the prompt's desktop
    std::vector<std::filesystem::path> images;             // --image: pictures in the request
    std::string request;
};

auto request(const Options& o) -> int {
    app::config::Config config;
    if (auto loaded = app::config::load(data_dir() / L"config.json")) {
        config = std::move(loaded->config);
    }
    const auto settings = app::agent::settings_of(config.advanced, config.limits, config.access, app::agent::process_elevated());
    if (o.fence) {
        tools::os::fence::enable(*o.fence);
    }
    std::error_code ec;
    std::filesystem::create_directories(data_dir(), ec);
    auto audit = security::AuditLog::open(data_dir() / L"audit.jsonl");
    if (!audit) {
        say("The audit cannot be written: " + audit.error().message);
        return 2;
    }
    std::vector<ai::ToolSpec> specs;
    for (const auto* t : tools::available(settings)) {
        auto strict = ai::strict_schema(t->schema());
        if (!strict) {
            say("A tool's schema could not be made strict: " + strict.error().message);
            return 2;
        }
        specs.push_back({.name = std::string{t->name}, .description = std::string{t->description}, .schema = std::move(*strict)});
    }

    net::StatePtr net;
    std::optional<security::SecureString> key;
    ai::Provider provider;
    if (o.replay) {
        auto played = ai::cassette::player(*o.replay);
        if (!played) {
            say("The recording cannot be played: " + played.error().message);
            return 2;
        }
        provider = std::move(*played);
    } else {
        auto loaded = security::load_key(ai::catalog::default_provider().id, o.secrets, ai::catalog::default_provider().id);
        if (!loaded) {
            say(std::format("No {} key here ({}): run agent-cli --set-key first.", ai::catalog::default_provider().name, loaded.error().message));
            return 2;
        }
        key = std::move(*loaded);
        auto made = net::init(net::follows_system(config.network.proxy) ? net::Config{.proxy = {}, .system = [] { return net::system_settings(); }}
                                                                       : net::Config{.proxy = net::settings_of(config.network.proxy, net::system_settings()), .system = {}});
        if (!made) {
            say("No network: " + made.error().message);
            return 2;
        }
        net = std::move(*made);
        ai::Tap tap;
        if (o.record) {
            const auto profile = core::win::known_folder(FOLDERID_Profile);
            auto recorder = ai::cassette::recorder(*o.record, profile ? cli::utf8(profile->wstring()) : std::string{}, computer_name());
            if (!recorder) {
                say("The recording cannot be written: " + recorder.error().message);
                return 2;
            }
            tap = std::move(*recorder);
        }
        provider = ai::chat::provider(*net, ai::catalog::provider_of(ai::catalog::default_model()), [&key] { return std::string{key->view()}; }, o.host, std::move(tap));
    }

    security::Session session;
    const auto confirm = cli::confirm(o.yes);
    ai::agent::Setup s;
    s.provider = std::move(provider);
    s.tools = specs;
    s.system = ai::system_prompt(profile_paths(o.desktop));
    s.model = ai::catalog::model(o.model);
    if (s.model == nullptr) {
        say("No such model: " + o.model);
        return 2;
    }
    const auto others = ai::catalog::beside(*s.model); // as the app: a picture to a model that sees, the summary to the cheapest
    s.vision_model = others.vision;
    s.summary_model = others.summary;
    s.thinking = o.thinking;
    s.limits.budget_usd = o.budget;
    s.now = [] { return std::chrono::system_clock::now(); };
    s.take_refine = [] { return std::optional<std::string>{}; };
    s.execute = [&](const ai::ToolCall& c) {
        const auto called = tools::call(c.name, c.arguments, o.wd, settings, session, confirm, &*audit, 0);
        if (called.outcome.code == 0) {
            say("  done");
        } else if (called.asked && !called.allowed) {
            say("  refused: you said no");
        } else if (called.outcome.code == 3) {
            say("  refused: " + called.ruling.reason);
        } else {
            say("  failed");
        }
        ai::agent::Result result{.content = called.outcome.json, .ok = called.outcome.code == 0, .images = {}};
        for (const auto& image : called.outcome.images) { // what image.view and the others showed, to the model
            result.images.push_back({.mime = image.mime, .data = image.bytes, .label = image.label});
        }
        return result;
    };
    s.events.on_text = [](std::string_view text) { print(text); };
    s.events.on_tool_started = [](const ai::ToolCall& c) {
        print("\n");
        say("> " + c.name + " " + c.arguments);
    };
    // --yes answers the tools' questions, never this one: money is asked every time (no answer: stop).
    s.events.ask_continue = [](ai::agent::Ask about, double usd) {
        print("\n");
        return cli::ask_yes(about == ai::agent::Ask::Budget ? std::format("This task has spent ${:.4f}. Go on?", usd)
                                                            : std::string{"The model has taken many turns. Go on?"},
                            false);
    };

    std::stop_source stop;
    g_stop = &stop;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::vector<ai::Image> images;
    for (const auto& file : o.images) {
        auto ext = file.extension().wstring();
        std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        const char* mime = ext == L".png"                     ? "image/png"
                           : ext == L".jpg" || ext == L".jpeg" ? "image/jpeg"
                           : ext == L".gif"                    ? "image/gif"
                           : ext == L".webp"                   ? "image/webp"
                                                               : nullptr;
        std::ifstream in{file, std::ios::binary};
        std::string data{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        if (mime == nullptr || (!in.good() && !in.eof()) || data.empty()) {
            say("not a picture this can send: " + cli::utf8(file.wstring()) + "\n");
            return 2;
        }
        images.push_back({.mime = mime, .data = std::move(data), .label = cli::utf8(file.filename().wstring())});
    }
    std::vector<ai::Message> history;
    const auto out = ai::agent::run(history, o.request, std::move(images), s, stop.get_token());
    SetConsoleCtrlHandler(on_ctrl, FALSE);
    g_stop = nullptr;

    print("\n");
    say(std::format("[usage] prompt {} (cache hit {}, miss {}), completion {} — ${:.6f}", out.usage.prompt, out.usage.cache_hit,
                    out.usage.cache_miss, out.usage.completion, out.usd));
    switch (out.end) {
    case ai::agent::End::Done: return 0;
    case ai::agent::End::Failed: say("Failed: " + out.error.message); return 2;
    case ai::agent::End::Stopped: say("Stopped."); return 3;
    case ai::agent::End::Cancelled: say("Cancelled."); return 3;
    }
    return 2;
}

auto thinking_of(std::wstring_view w) -> std::optional<ai::Thinking> {
    if (w == L"off") {
        return ai::Thinking::Off;
    }
    if (w == L"low") {
        return ai::Thinking::Low;
    }
    if (w == L"high") {
        return ai::Thinking::High;
    }
    if (w == L"max") {
        return ai::Thinking::Max;
    }
    return std::nullopt;
}

// One line of stdin, without echo when it is a console; the spaces around it are not the key's.
auto read_secret() -> std::string {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(in, &mode) != 0;
    if (console) {
        SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
        std::fputs(std::format("{} API key: ", ai::catalog::default_provider().name).c_str(), stderr);
    }
    std::string line;
    std::getline(std::cin, line);
    if (console) {
        SetConsoleMode(in, mode);
        std::fputc('\n', stderr);
    }
    const auto first = line.find_first_not_of(" \t\r\n");
    const auto last = line.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : line.substr(first, last - first + 1);
}

auto set_key(const std::filesystem::path& secrets) -> int {
    const std::string key = read_secret();
    if (key.empty()) {
        say("No key given: nothing saved.");
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(secrets.parent_path(), ec);
    if (auto stored = security::store_key(ai::catalog::default_provider().id, key, secrets, ai::catalog::default_provider().id); !stored) {
        say("The key could not be saved: " + stored.error().message);
        return 2;
    }
    say("Key saved (encrypted for this Windows user).");
    return 0;
}

// One turn, waited for: its message, its finish and how it ended.
struct Turn {
    ai::Assembler turn;
    core::Result<void> result;
};

auto ask(const ai::Provider& provider, const ai::ChatRequest& r) -> Turn {
    auto done = std::make_shared<std::promise<core::Result<void>>>();
    auto future = done->get_future();
    auto turn = std::make_shared<ai::Assembler>();
    (void)provider(r, ai::StreamSink{.on_delta = [turn](const ai::Delta& d) { turn->add(d); },
                                     .on_done = [done](core::Result<void> result) { done->set_value(std::move(result)); }});
    auto result = future.get();
    return Turn{.turn = *turn, .result = std::move(result)};
}

// A body sent as it is (the probe's own edits of the JSON), waited for: the status and the body.
auto post(net::State& net, std::string_view key, std::string_view path, std::string body) -> core::Result<net::Response> {
    auto done = std::make_shared<std::promise<core::Result<net::Response>>>();
    auto future = done->get_future();
    (void)net::start(net,
                     net::Request{.method = "POST",
                                  .url = std::string{ai::catalog::provider_of(ai::catalog::default_model()).host} + std::string{path},
                                  .headers = {{"Authorization", "Bearer " + std::string{key}}, {"Content-Type", "application/json"}},
                                  .body = std::move(body)},
                     net::Sink{.on_headers = {}, .on_data = {}, .on_done = [done](core::Result<net::Response> r) { done->set_value(std::move(r)); }});
    return future.get();
}

auto message(ai::Role role, std::string content) -> ai::Message {
    return ai::Message{.role = role, .content = std::move(content), .reasoning = {}, .tool_calls = {}, .tool_call_id = {}};
}

auto probe(const std::filesystem::path& secrets, bool raw) -> int {
    auto key = security::load_key(ai::catalog::default_provider().id, secrets, ai::catalog::default_provider().id);
    if (!key) {
        say(std::format("No {} key here ({}): run agent-cli --set-key first.", ai::catalog::default_provider().name, key.error().message));
        return 2;
    }
    app::config::Config config;
    if (auto loaded = app::config::load(data_dir() / L"config.json")) {
        config = std::move(loaded->config);
    }
    auto net = net::init(net::follows_system(config.network.proxy) ? net::Config{.proxy = {}, .system = [] { return net::system_settings(); }}
                                                                       : net::Config{.proxy = net::settings_of(config.network.proxy, net::system_settings()), .system = {}});
    if (!net) {
        say("No network: " + net.error().message);
        return 2;
    }
    const auto& by = ai::catalog::provider_of(ai::catalog::default_model());
    const auto provider = ai::chat::provider(**net, by, [&] { return std::string{key->view()}; });

    std::vector<ai::ToolSpec> specs;
    for (const auto& t : tools::all()) {
        auto strict = ai::strict_schema(t.schema());
        if (!strict) {
            say("A tool's schema could not be made strict: " + strict.error().message);
            return 2;
        }
        specs.push_back({.name = std::string{t.name}, .description = std::string{t.description}, .schema = std::move(*strict)});
    }
    int failures = 0;

    // 1. All 19 strict schemas under wire names, and nothing to say: is the request taken?
    ai::ChatRequest schemas;
    schemas.model = std::string{ai::catalog::default_model().id};
    schemas.thinking = ai::Thinking::Off;
    schemas.max_tokens = 1;
    schemas.messages = {message(ai::Role::User, "Say OK.")};
    schemas.tools = specs;
    const auto taken = ask(provider, schemas);
    say(taken.result ? "1. 19 strict schemas, names with '_': accepted"
                     : "1. 19 strict schemas, names with '_': REFUSED — " + taken.result.error().message);
    failures += taken.result ? 0 : 1;

    // 2. A name with a dot, as it is: does the API take it? (Ours go with '_' either way.)
    ai::ChatRequest dotted = schemas;
    dotted.tools = std::span{specs}.first(1);
    std::string body = ai::chat::request_json(by, dotted);
    if (const auto at = body.find(R"("name":"fs_list")"); at != std::string::npos) {
        body.replace(at, 16, R"("name":"fs.list")");
    }
    const auto dot = post(**net, key->view(), ai::chat::path_for(by, dotted), std::move(body));
    say(dot && dot->status == 200 ? "2. a name with '.': accepted"
                                  : "2. a name with '.': refused — " +
                                        (dot ? std::to_string(dot->status) + " " + dot->error_body : dot.error().message));

    // 3. A request that wants a tool, thinking high (low may not reason at all: then 4 and 5 below
    //    would be the same request): the call, as it comes.
    ai::ChatRequest wants;
    wants.model = std::string{ai::catalog::default_model().id};
    wants.thinking = ai::Thinking::High;
    wants.messages = {message(ai::Role::System, ai::system_prompt("Desktop: C:\\Users\\user\\Desktop")),
                      message(ai::Role::User, "List the files on my desktop.")};
    wants.tools = specs;
    // --raw: step 3's events as they came (the model's words only; no key, no header).
    const auto watched = raw ? ai::chat::provider(**net, by, [&] { return std::string{key->view()}; }, {},
                                                      ai::Tap{.request = {}, .event = [](std::string_view data) { say("   raw: " + std::string{data}); }, .answer = {}})
                             : provider;
    const auto called = ask(watched, wants);
    const auto& calls = called.turn.message().tool_calls;
    if (!called.result || calls.empty()) {
        say("3. a tool call: NONE — " + (called.result ? "the model answered in text" : called.result.error().message));
        return 2;
    }
    say("3. a tool call: " + calls.front().name + " " + calls.front().arguments + " (reasoning: " +
        std::to_string(called.turn.message().reasoning.size()) + " bytes)");

    // 4. The turn sent back with its reasoning and a tool's result; 5. the same without the reasoning.
    auto follow = wants;
    follow.messages.push_back(called.turn.message());
    for (const auto& c : calls) {
        auto result = message(ai::Role::Tool, R"({"source":"fs.list","data":{"entries":[]}})");
        result.tool_call_id = c.id;
        follow.messages.push_back(std::move(result));
    }
    follow.max_tokens = 200;
    const auto with = ask(provider, follow);
    say(with.result ? "4. reasoning sent back with the calls: accepted"
                    : "4. reasoning sent back with the calls: REFUSED — " + with.result.error().message);
    failures += with.result ? 0 : 1;
    follow.messages[2].reasoning.clear();
    const auto without = ask(provider, follow);
    say(without.result ? "5. the same without the reasoning: accepted (not required)"
                       : "5. the same without the reasoning: refused (required) — " + without.result.error().message);
    return failures == 0 ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    core::crash::quiet_failures(); // a failed check: stderr and exit code 3, never a dialog on the desktop
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    std::filesystem::path secrets = data_dir() / L"secrets.bin";
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == L"--secrets") {
            if (i + 1 >= a.size()) {
                return usage();
            }
            secrets = a[i + 1];
            a.erase(a.begin() + static_cast<std::ptrdiff_t>(i), a.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            break;
        }
    }
    if (a.size() == 2 && a[0] == L"--probe" && a[1] == L"--raw") {
        return probe(secrets, true);
    }
    if (a.size() == 1 && a[0] == L"--set-key") {
        return set_key(secrets);
    }
    if (a.size() == 1 && a[0] == L"--probe") {
        return probe(secrets, false);
    }
    Options o;
    o.secrets = secrets;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const bool more = i + 1 < a.size();
        if (a[i] == L"--yes") {
            o.yes = true;
        } else if (a[i] == L"--model" && more) {
            o.model = cli::utf8(a[++i]);
        } else if (a[i] == L"--thinking" && more) {
            const auto t = thinking_of(a[++i]);
            if (!t) {
                return usage();
            }
            o.thinking = *t;
        } else if (a[i] == L"--budget" && more) {
            o.budget = std::wcstod(a[++i].c_str(), nullptr);
        } else if (a[i] == L"--fence" && more) {
            o.fence = a[++i];
        } else if (a[i] == L"--wd" && more) {
            o.wd = a[++i];
        } else if (a[i] == L"--record" && more) {
            o.record = a[++i];
        } else if (a[i] == L"--replay" && more) {
            o.replay = a[++i];
        } else if (a[i] == L"--desktop" && more) {
            o.desktop = a[++i];
        } else if (a[i] == L"--image" && more) {
            o.images.emplace_back(a[++i]);
        } else if (a[i] == L"--host" && more) {
            o.host = cli::utf8(a[++i]);
        } else if (!a[i].starts_with(L"--") && o.request.empty()) {
            o.request = cli::utf8(a[i]);
        } else {
            return usage();
        }
    }
    if (o.request.empty() || (o.record && o.replay) || o.budget <= 0) {
        return usage();
    }
    return request(o);
}
