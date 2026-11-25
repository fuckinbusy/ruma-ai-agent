#include "ai/agent.hpp"

#include "ai/compact.hpp"
#include "ai/cost.hpp"
#include "ai/chat.hpp"
#include "ai/repeats.hpp"
#include "ai/stream.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace ai::agent {

namespace {

constexpr std::string_view kGaveWay = "[result truncated]";
constexpr std::string_view kCut = "\n[truncated]";
constexpr std::string_view kGoOn = "Your answer was cut off. Go on from where it stopped; if you were calling a tool, call it again, whole.";

// What the provider says on its thread, for the caller's thread to take.
struct Channel {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::vector<Delta> deltas;
    std::optional<core::Result<void>> done;
};

auto said(Role role, std::string content) -> Message {
    return Message{.role = role, .content = std::move(content), .reasoning = {}, .tool_calls = {}, .tool_call_id = {}};
}

auto known(std::span<const ToolSpec> tools, std::string_view name) -> bool {
    return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; });
}

// Why `args` is not a JSON object; nothing when it is one.
auto broken(std::string_view args) -> std::optional<std::string> {
    glz::generic j;
    if (const auto error = glz::read_json(j, args)) {
        return glz::format_error(error, args);
    }
    if (!j.is_object()) {
        return std::string{"not an object"};
    }
    return std::nullopt;
}

// `text` cut to `limit` bytes (not in the middle of a UTF-8 character) and marked.
auto cut(std::string text, std::size_t limit) -> std::string {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t at = limit;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    text.resize(at);
    text += kCut;
    return text;
}

constexpr std::size_t kImageTokens = 1024; // DeepSeek's upper bound for one picture
constexpr std::size_t kImagesMost = 8;     // messages with pictures before the oldest give way…
constexpr std::size_t kImagesKept = 4;     // …down to these

auto tokens_of(const Message& m) -> std::size_t {
    std::size_t n = estimate_tokens(m.content) + estimate_tokens(m.reasoning) + kImageTokens * m.images.size();
    for (const auto& c : m.tool_calls) {
        n += estimate_tokens(c.arguments);
    }
    return n;
}

// The request: the prompt, then the history; beyond the limit, the oldest tool results give way
// (the prompt and the last four messages never do). The history itself stays whole.
struct Asked {
    ChatRequest request;
    const catalog::Model* model = nullptr; // the one it goes to
};
// The request to `to`: a model that does not see pictures gets them as words (the history keeps them).
auto request_of(const std::vector<Message>& history, const Setup& s, const catalog::Model* to) -> Asked {
    ChatRequest r;
    r.tools = s.tools;
    if (std::string system = s.system_of ? s.system_of() : s.system; !system.empty()) {
        r.messages.push_back(said(Role::System, std::move(system)));
    }
    r.messages.insert(r.messages.end(), history.begin(), history.end());
    if (to->vision != catalog::Tri::Yes) {
        thin_images(r.messages, 0, 0);
    }
    r.model = std::string{to->id};
    r.thinking = catalog::nearest_level(*to, s.thinking); // a level it does not take: the nearest it has
    std::size_t total = 0;
    for (const auto& m : r.messages) {
        total += tokens_of(m);
    }
    const std::size_t keep = r.messages.size() > 4 ? r.messages.size() - 4 : 0;
    for (std::size_t i = 0; i < keep && total > to->context_tokens; ++i) {
        auto& m = r.messages[i];
        if (m.role == Role::Tool && m.content != kGaveWay) {
            total -= estimate_tokens(m.content);
            m.content = kGaveWay;
            total += estimate_tokens(m.content);
        }
    }
    return {.request = std::move(r), .model = to};
}

// What has failed in this request (stage 4c): single models, and whole providers.
struct Out {
    std::vector<std::pair<const catalog::Model*, routing::Why>> models;
    std::vector<std::pair<std::string_view, routing::Why>> providers;

    [[nodiscard]] auto why(const catalog::Model* m) const -> std::optional<routing::Why> {
        for (const auto& [model, why] : models) {
            if (model == m) {
                return why;
            }
        }
        for (const auto& [provider, why] : providers) {
            if (provider == m->provider) {
                return why;
            }
        }
        return std::nullopt;
    }
    void add(const catalog::Model* m, routing::Why why) {
        if (routing::provider_wide(why)) {
            providers.emplace_back(m->provider, why);
        } else {
            models.emplace_back(m, why);
        }
    }
};

// The model a turn goes to: the first of its role's chain that can do what is needed and is not out;
// with none that can, the first of Chat's that is not out (a picture then goes as words). Null: all out.
struct Pick {
    const catalog::Model* to = nullptr;
    Route route;
    bool say = false; // not the chosen model, or pictures as words
};
auto pick(const std::vector<Message>& history, const Setup& s, const Out& out) -> Pick {
    const bool picture = std::ranges::any_of(history, [](const Message& m) { return !m.images.empty(); });
    const routing::Need need = routing::need_of(picture, s.thinking);
    const auto chain_of = [&s](routing::Need n) -> std::span<const catalog::Model* const> {
        const auto& c = n == routing::Need::Images ? s.chains.images : n == routing::Need::Hard ? s.chains.hard : s.chains.chat;
        return c.empty() ? std::span<const catalog::Model* const>{&s.model, 1} : std::span<const catalog::Model* const>{c};
    };
    const auto can = [need](const catalog::Model& m) {
        return need == routing::Need::Images ? m.vision == catalog::Tri::Yes
               : need == routing::Need::Hard ? m.thinking == catalog::Tri::Yes
                                             : true;
    };
    const catalog::Model* failed = nullptr; // the first of the walk that was out: why it went elsewhere
    routing::Why failed_why = routing::Why::Role;
    const auto walk = [&](routing::Need n, bool check) -> const catalog::Model* {
        for (const auto* m : chain_of(n)) {
            if (const auto why = out.why(m)) {
                if (failed == nullptr) {
                    failed = m;
                    failed_why = *why;
                }
                continue;
            }
            if (!check || can(*m)) {
                return m;
            }
        }
        return nullptr;
    };
    Pick p;
    p.to = walk(need, true);
    bool words_only = false;
    if (p.to == nullptr && need != routing::Need::Chat) {
        p.to = walk(routing::Need::Chat, false);
        words_only = p.to != nullptr && need == routing::Need::Images && p.to->vision != catalog::Tri::Yes;
    }
    if (p.to == nullptr) {
        return p;
    }
    p.route = {.to = p.to, .from = s.model, .why = routing::Why::Role, .need = need, .words_only = words_only};
    if (failed != nullptr) {
        p.route.from = failed;
        p.route.why = failed_why;
    } else if (need == routing::Need::Images && (words_only || s.model->vision != catalog::Tri::Yes)) {
        p.route.why = routing::Why::NoVision;
    } else if (need == routing::Need::Hard && s.model->thinking != catalog::Tri::Yes) {
        p.route.why = routing::Why::NoThinking;
    }
    p.say = p.to != s.model || words_only;
    return p;
}

auto failure_of(Finish f, std::string_view who) -> std::optional<std::string> {
    switch (f) {
    case Finish::ContentFilter: return std::format("{} withheld the answer (content filter)", who);
    case Finish::NoResources: return std::format("{} ran out of resources for this answer; try again later", who);
    case Finish::Aborted: return std::format("{} stopped the answer", who);
    default: return std::nullopt;
    }
}

// A request whose words nobody sees as they come (the summary, phase 12 § 10): its message and
// usage once it ended well; a failure, a cut or a stop: why.
auto ask_once(const ChatRequest& request, const catalog::Model& to, const Setup& s, std::stop_token stop)
    -> core::Result<std::pair<Message, Usage>> {
    auto channel = std::make_shared<Channel>();
    StreamSink sink{.on_delta =
                        [channel](const Delta& d) {
                            const std::scoped_lock lock{channel->mutex};
                            channel->deltas.push_back(d);
                            channel->cv.notify_all();
                        },
                    .on_done =
                        [channel](core::Result<void> r) {
                            const std::scoped_lock lock{channel->mutex};
                            channel->done = std::move(r);
                            channel->cv.notify_all();
                        }};
    Provider by = s.provider;
    if (s.provider_for) {
        auto got = s.provider_for(catalog::provider_of(to));
        if (!got) {
            return std::unexpected{got.error()};
        }
        by = std::move(*got);
    }
    const Stream stream = by(request, std::move(sink));
    const std::stop_callback on_stop{stop, [&stream] {
                                         if (stream.cancel) {
                                             stream.cancel();
                                         }
                                     }};
    Assembler turn;
    std::optional<core::Result<void>> done;
    while (!done) {
        std::vector<Delta> got;
        {
            std::unique_lock lock{channel->mutex};
            channel->cv.wait(lock, stop, [&] { return !channel->deltas.empty() || channel->done.has_value(); });
            got.swap(channel->deltas);
            done = channel->done;
        }
        for (const auto& d : got) {
            turn.add(d);
        }
        if (stop.stop_requested()) {
            return core::make_error(core::Code::Cancelled, "stopped");
        }
    }
    if (!*done) {
        return std::unexpected{done->error()};
    }
    if (turn.finish() != Finish::Stop || turn.message().content.empty()) {
        return core::make_error(core::Code::Api, "the summary did not come whole");
    }
    return std::pair{turn.message(), turn.usage().value_or(Usage{})};
}

} // namespace

auto estimate_tokens(std::string_view text) -> std::size_t {
    const auto ascii = static_cast<std::size_t>(std::ranges::count_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    const auto other = text.size() - ascii;
    return static_cast<std::size_t>(std::ceil(static_cast<double>(ascii) / 4.0 + static_cast<double>(other) / 3.5));
}

auto run(std::vector<Message>& history, std::string_view request, const Setup& s, std::stop_token stop) -> Outcome {
    return run(history, request, {}, s, std::move(stop));
}

auto run(std::vector<Message>& history, std::string_view request, std::vector<Image> images, const Setup& s, std::stop_token stop)
    -> Outcome {
    std::string content{request};
    if (const std::string extra = s.context ? s.context() : std::string{}; !extra.empty()) {
        content += "\n\n" + extra;
    }
    auto& asked = history.emplace_back(said(Role::User, std::move(content)));
    asked.images = std::move(images);
    if (!asked.images.empty()) {
        thin_images(history, kImagesMost, kImagesKept);
    }
    const std::size_t kept = history.size();
    auto out = resume(history, s, std::move(stop));
    // No key for any model of the chain: nothing was sent, and there is no Retry for it — the user
    // asks again once a key is in, so the request is not kept (it would be there twice).
    if (out.end == End::Failed && out.error.kind == "no_key" && history.size() == kept) {
        history.pop_back();
    }
    return out;
}

auto thin_images(std::vector<Message>& history, std::size_t most, std::size_t keep) -> std::size_t {
    auto with = static_cast<std::size_t>(std::ranges::count_if(history, [](const Message& m) { return !m.images.empty(); }));
    if (with <= most) {
        return 0;
    }
    std::size_t dropped = 0;
    for (auto& m : history) {
        if (with <= keep) {
            break;
        }
        if (m.images.empty()) {
            continue;
        }
        for (const auto& image : m.images) {
            m.content += "\n[image: " + image.label + ", no longer attached]";
        }
        m.images.clear();
        --with;
        ++dropped;
    }
    return dropped;
}

auto resume(std::vector<Message>& history, const Setup& s, std::stop_token stop) -> Outcome {
    Outcome out;
    if (s.model == nullptr) {
        out.end = End::Failed;
        out.error = core::make_error(core::Code::InvalidArgument, "no model is set").error();
        return out;
    }
    const catalog::Model& summary_by = s.summary_model != nullptr ? *s.summary_model : *s.model;
    std::size_t last_context = s.model->context_tokens; // of the model the last turn went to
    double budget = s.limits.budget_usd;
    int turn_limit = s.limits.turns;
    int turns = 0;
    bool went_on = false; // one more try after a turn cut by length
    Repeats repeats;      // the loop guard (phase 12 § 9)
    std::size_t last_prompt = 0; // the last turn's prompt tokens: past 60 % of the limit, a summary (§ 10)
    bool answering = false; // "Answer now" pressed: every request from then on without thinking
    Out gone;               // stage 4c: what failed in this request, and is not asked again
    core::Error last_error = core::make_error(core::Code::InvalidArgument, "no model can take this request").error();
    const catalog::Model* last_model = s.model;
    std::vector<Route> said_routes; // each route said once a request
    const auto pressed = [&] { return s.answer_now != nullptr && s.answer_now->load(); };
    const auto cancelled = [&](std::string partial = {}) {
        out.end = End::Cancelled;
        out.text = std::move(partial);
        return out;
    };
    // "Go on?": false stops; a stop meanwhile cancels.
    const auto go_on = [&](Ask about) -> std::optional<Outcome> {
        const bool yes = s.events.ask_continue && s.events.ask_continue(about, out.usd);
        if (stop.stop_requested()) {
            return cancelled();
        }
        if (!yes) {
            out.end = End::Stopped;
            return out;
        }
        return std::nullopt;
    };

    for (;;) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        if (turns >= turn_limit) {
            if (auto ended = go_on(Ask::Turns)) {
                return *ended;
            }
            turn_limit += s.limits.turns;
        }
        if (s.answer_now != nullptr && s.answer_now->exchange(false)) {
            history.push_back(said(Role::User, std::string{kAnswerNow}));
            answering = true;
        }
        // Past 60 % of the limit by the last turn's prompt: the old part becomes a summary (phase 12
        // § 10), once — the next turn measures anew. A summary that does not come leaves it whole.
        if (static_cast<double>(last_prompt) > compact::kAt * static_cast<double>(last_context)) {
            last_prompt = 0;
            if (const auto at = compact::split(history); at > 0) {
                auto summary = ask_once(compact::summary_request(std::span{history}.first(at), summary_by), summary_by, s, stop);
                if (stop.stop_requested()) {
                    return cancelled();
                }
                if (summary) {
                    const auto& u = summary->second;
                    out.usage.prompt += u.prompt;
                    out.usage.completion += u.completion;
                    out.usage.cache_hit += u.cache_hit;
                    out.usage.cache_miss += u.cache_miss;
                    out.usage.reasoning += u.reasoning;
                    out.usd += cost_usd(summary_by, u, s.now ? s.now() : std::chrono::system_clock::now());
                    if (s.events.on_usage) {
                        s.events.on_usage(u, out.usd);
                    }
                    history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(at));
                    history.insert(history.begin(), compact::summary_message(std::move(summary->first.content)));
                    if (s.events.on_compacted) {
                        s.events.on_compacted();
                    }
                }
            }
        }
        auto channel = std::make_shared<Channel>();
        StreamSink sink{.on_delta =
                            [channel](const Delta& d) {
                                const std::scoped_lock lock{channel->mutex};
                                channel->deltas.push_back(d);
                                channel->cv.notify_all();
                            },
                        .on_done =
                            [channel](core::Result<void> r) {
                                const std::scoped_lock lock{channel->mutex};
                                channel->done = std::move(r);
                                channel->cv.notify_all();
                            }};
        if (s.events.on_turn) {
            s.events.on_turn();
        }
        // Stage 4c: the turn's model by its role, and that model's provider (its key); a provider
        // with no key here is out, and the chain moves on.
        Pick chosen;
        std::optional<Provider> by;
        while (!by) {
            chosen = pick(history, s, gone);
            if (chosen.to == nullptr) {
                out.end = End::Failed;
                out.error = last_error;
                out.model = last_model;
                return out;
            }
            if (!s.provider_for) {
                by = s.provider;
            } else if (auto got = s.provider_for(catalog::provider_of(*chosen.to))) {
                by = std::move(*got);
            } else {
                gone.add(chosen.to, routing::why_of(got.error()).value_or(routing::Why::NoKey));
                last_error = got.error();
                last_model = chosen.to;
            }
        }
        if (chosen.say && s.events.on_routed &&
            std::ranges::none_of(said_routes, [&](const Route& r) {
                return r.to == chosen.route.to && r.why == chosen.route.why && r.words_only == chosen.route.words_only;
            })) {
            said_routes.push_back(chosen.route);
            s.events.on_routed(chosen.route);
        }
        Asked going = request_of(history, s, chosen.to);
        out.model = going.model;
        ChatRequest& asked = going.request;
        if (answering) {
            asked.thinking = catalog::nearest_level(*going.model, Thinking::Off); // none, or the least it has
        }
        const Stream stream = (*by)(asked, std::move(sink));
        ++turns;
        const std::stop_callback on_stop{stop, [&stream] {
                                             if (stream.cancel) {
                                                 stream.cancel();
                                             }
                                         }};

        Assembler turn;
        std::optional<core::Result<void>> done;
        bool cut_short = false; // "Answer now" during the turn: it is dropped, the loop asks again
        bool got_any = false;   // a piece came: the user may have seen it, so a failure moves nowhere
        while (!done) {
            std::vector<Delta> got;
            {
                std::unique_lock lock{channel->mutex};
                const auto ready = [&] { return !channel->deltas.empty() || channel->done.has_value() || pressed(); };
                if (s.answer_now != nullptr) { // a press comes from another thread, not through this channel
                    channel->cv.wait_for(lock, stop, std::chrono::milliseconds{50}, ready);
                } else {
                    channel->cv.wait(lock, stop, ready);
                }
                got.swap(channel->deltas);
                done = channel->done;
            }
            for (const auto& d : got) {
                if (stop.stop_requested()) {
                    return cancelled(turn.message().content);
                }
                turn.add(d);
                // A piece the user can see: words, a thought or a call (DeepSeek's first chunk carries
                // only the role, an empty content).
                got_any = got_any || !d.content.empty() || !d.reasoning.empty() || d.tool_index >= 0;
                if (!d.reasoning.empty() && s.events.on_reasoning) {
                    s.events.on_reasoning(d.reasoning);
                }
                if (!d.content.empty() && s.events.on_text) {
                    s.events.on_text(d.content);
                }
            }
            if (stop.stop_requested()) {
                return cancelled(turn.message().content);
            }
            if (!done && pressed()) {
                if (stream.cancel) {
                    stream.cancel();
                }
                cut_short = true;
                break;
            }
        }
        if (cut_short) {
            continue; // not kept; the next request asks for the answer
        }
        if (!*done) {
            // Before its first piece, a failure that is the model's or its provider's (busy, no
            // answer, the key): that one is out for this request, the turn goes to the next.
            if (const auto why = routing::why_of(done->error()); why && !got_any) {
                gone.add(going.model, *why);
                last_error = done->error();
                last_model = going.model;
                --turns;
                continue;
            }
            out.end = End::Failed;
            out.error = done->error();
            out.text = turn.message().content;
            return out; // a turn that broke half-way is not kept
        }
        if (const auto& u = turn.usage()) {
            out.usage.prompt += u->prompt;
            out.usage.completion += u->completion;
            out.usage.cache_hit += u->cache_hit;
            out.usage.cache_miss += u->cache_miss;
            out.usage.reasoning += u->reasoning;
            out.usd += cost_usd(*going.model, *u, s.now ? s.now() : std::chrono::system_clock::now()); // the one that answered
            if (s.events.on_usage) {
                s.events.on_usage(*u, out.usd);
            }
            last_prompt = static_cast<std::size_t>(std::max(0, u->prompt));
            last_context = going.model->context_tokens;
            if (s.events.on_context && last_context > 0) {
                s.events.on_context(static_cast<int>(std::lround(100.0 * static_cast<double>(last_prompt) /
                                                                 static_cast<double>(last_context))));
            }
        }

        // A turn that did not end well is not kept, and none of its calls run (even a complete one).
        if (const auto why = failure_of(turn.finish(), catalog::provider_of(*going.model).name)) {
            out.end = End::Failed;
            out.error = core::make_error(core::Code::Api, *why).error();
            out.text = turn.message().content;
            return out;
        }
        if (turn.finish() == Finish::None) { // the stream closed with no finish_reason: cut off
            out.end = End::Failed;
            out.error = core::make_error(core::Code::Net, "The answer broke off before its end").error();
            out.text = turn.message().content;
            return out;
        }
        if (turn.finish() == Finish::Length) {
            if (went_on) {
                out.end = End::Failed;
                out.error = core::make_error(core::Code::Api, "The model's answer was cut off twice: too long").error();
                out.text = turn.message().content;
                return out;
            }
            went_on = true;
            Message partial = turn.message();
            partial.tool_calls.clear(); // a call cut in half is never run
            if (!partial.content.empty() || !partial.reasoning.empty()) {
                history.push_back(std::move(partial));
            }
            history.push_back(said(Role::User, std::string{kGoOn}));
        } else {
            const Message& answer = history.emplace_back(turn.message());
            if (answer.tool_calls.empty()) {
                if (auto refine = s.take_refine ? s.take_refine() : std::nullopt) {
                    history.push_back(said(Role::User, std::move(*refine)));
                } else {
                    out.end = End::Done;
                    out.text = answer.content;
                    return out;
                }
            } else {
                const auto calls = answer.tool_calls; // the history grows below
                Message shown = said(Role::User, {}); // the pictures the tools gave, after their results
                bool again = false;                   // the fourth repeat: "Go on?" once the turn's results are in
                for (const auto& c : calls) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    Result r;
                    if (!known(s.tools, c.name)) {
                        r = Result{.content = "unknown tool: " + chat::wire_name(c.name), .ok = false};
                    } else if (const auto why = broken(c.arguments)) {
                        r = Result{.content = "invalid arguments: " + *why, .ok = false};
                    } else {
                        if (s.events.on_tool_started) {
                            s.events.on_tool_started(c);
                        }
                        r = s.execute ? s.execute(c) : Result{.content = "no way to run tools", .ok = false};
                        if (stop.stop_requested()) {
                            return cancelled();
                        }
                        r.content = cut(std::move(r.content), s.limits.result_bytes);
                        if (s.events.on_tool_finished) {
                            s.events.on_tool_finished(c, r);
                        }
                        for (auto& image : r.images) {
                            shown.content += (shown.content.empty() ? "[" : " [") + c.name + ": " + image.label + "]";
                            shown.images.push_back(std::move(image));
                        }
                    }
                    // The loop guard: the third time, the model is told in the result; once more, the user is asked.
                    if (const int level = repeats.note(c.name, c.arguments); level == 3) {
                        const auto what = repeats.what();
                        r.content += std::format("\n\nYou are repeating {}. Stop and answer, or ask the user with ui.ask.", what);
                        if (s.events.on_repeat) {
                            s.events.on_repeat(what);
                        }
                    } else if (level >= 4) {
                        again = true;
                    }
                    Message result = said(Role::Tool, std::move(r.content));
                    result.tool_call_id = c.id;
                    history.push_back(std::move(result));
                }
                if (!shown.images.empty()) {
                    history.push_back(std::move(shown));
                    thin_images(history, kImagesMost, kImagesKept);
                }
                if (auto refine = s.take_refine ? s.take_refine() : std::nullopt) {
                    history.push_back(said(Role::User, std::move(*refine)));
                }
                if (again) {
                    if (auto ended = go_on(Ask::Repeat)) {
                        return *ended;
                    }
                    repeats.reset();
                }
            }
        }
        if (out.usd >= budget) {
            if (auto ended = go_on(Ask::Budget)) {
                return *ended;
            }
            budget += s.limits.budget_usd;
        }
    }
}

} // namespace ai::agent
