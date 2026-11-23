#include "net/http.hpp"

#include "core/log.hpp"
#include "core/mem/mem.hpp"
#include "net/curl.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <variant>

namespace net {
namespace {

constexpr std::size_t kErrorBody = 64 * 1024; // what a 4xx or 5xx body may tell, at most

using Clock = std::chrono::steady_clock;

// One request on the network thread: its curl handle and what it has said so far.
struct Transfer {
    RequestId id = 0;
    Request request;
    Sink sink;
    curl::Easy easy;
    curl::Slist headers;
    std::string proxy; // the one this request goes through; empty: direct
    Response response;
    bool body_began = false; // on_headers said, on_data may have been (no repeat after this)
    std::array<char, CURL_ERROR_SIZE> error{};
    Clock::time_point started;
    int attempt = 0;                          // tries made after the first
    bool in_multi = false;                    // with curl now (not waiting for another try)
    // The request went out on the connection now in use: curl says so just before it sends, and a
    // connection curl begins afresh (its own second go when a kept one died) has not carried it.
    bool request_out = false;
    std::optional<Clock::time_point> retry_at; // the next try
};

struct StartCmd {
    RequestId id;
    Request request;
    Sink sink;
};
struct CancelCmd {
    RequestId id;
};
using Command = std::variant<StartCmd, CancelCmd>;

// A URL or a proxy for the log and the errors: without its query and its "user:password@" (they
// may carry what is not the log's business).
auto shown(std::string_view url) -> std::string {
    url = url.substr(0, url.find('?'));
    const auto scheme = url.find("://");
    const std::size_t from = scheme == std::string_view::npos ? 0 : scheme + 3;
    const std::string_view authority = url.substr(from, url.find('/', from) - from);
    const auto at = authority.rfind('@');
    if (at == std::string_view::npos) {
        return std::string{url};
    }
    return std::string{url.substr(0, from)} + std::string{url.substr(from + at + 1)};
}

auto on_write(char* p, std::size_t size, std::size_t count, void* user) -> std::size_t {
    auto& t = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    long status = 0;
    curl_easy_getinfo(t.easy.get(), CURLINFO_RESPONSE_CODE, &status);
    t.response.status = static_cast<int>(status);
    if (status >= 200 && status < 300) {
        if (!t.body_began) {
            t.body_began = true;
            if (t.sink.on_headers) {
                t.sink.on_headers(t.response);
            }
        }
        if (t.sink.on_data) {
            t.sink.on_data({reinterpret_cast<const std::byte*>(p), bytes});
        }
    } else { // up to kErrorBody: once there, nothing more is kept
        t.response.error_body.append(p, std::min(bytes, kErrorBody - t.response.error_body.size()));
    }
    return bytes;
}

auto on_header(char* p, std::size_t size, std::size_t count, void* user) -> std::size_t {
    auto& t = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    std::string_view line{p, bytes};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.starts_with("HTTP/")) {
        t.response.headers.clear(); // a new response (after 100 Continue or a redirect)
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        t.response.headers.emplace_back(std::string{line.substr(0, colon)}, std::string{value});
    }
    return bytes;
}

// The connection is there and the request goes out now.
auto on_prereq(void* user, char* /*remote_ip*/, char* /*local_ip*/, int /*remote_port*/, int /*local_port*/) -> int {
    static_cast<Transfer*>(user)->request_out = true;
    return CURL_PREREQFUNC_OK;
}

// A new connection begins: the request has not gone out on it yet.
auto on_socket(void* user, curl_socket_t /*s*/, curlsocktype purpose) -> int {
    if (purpose == CURLSOCKTYPE_IPCXN) {
        static_cast<Transfer*>(user)->request_out = false;
    }
    return CURL_SOCKOPT_OK;
}

} // namespace

struct State {
    Config config;
    std::mutex mutex;
    std::condition_variable cv;    // the thread's wait before curl is loaded
    std::vector<Command> commands; // under mutex
    bool stopping = false;         // under mutex
    CURLM* multi = nullptr;        // under mutex: set once the thread made it, cleared before it goes
    std::atomic<RequestId> next{1};
    std::atomic<std::uint64_t> wakeups{0};
    std::jthread thread;

    // Under mutex: the thread wakes from curl_multi_poll, or from its first wait.
    void wake() {
        if (multi != nullptr) {
            curl_multi_wakeup(multi);
        } else {
            cv.notify_all();
        }
    }
};

namespace {

void tell_done(Transfer& t, core::Result<Response> result) {
    if (t.sink.on_done) {
        t.sink.on_done(std::move(result));
    }
}

class Loop {
public:
    explicit Loop(State& s) : s_(s) {}

    void run() {
        // What the thread holds (bodies, events being read, the transfers) shows under its own tag.
        const core::mem::TagScope tag{core::mem::tag_id("net/http")};
        // Nothing of curl before the first command: libcurl.dll is delay-loaded. Closed with no
        // request at all: it never is.
        {
            std::unique_lock lock{s_.mutex};
            s_.cv.wait(lock, [&] { return s_.stopping || !s_.commands.empty(); });
            if (s_.commands.empty()) {
                return;
            }
        }
        if (auto ready = curl::global(); !ready) {
            refuse_all(ready.error());
            return;
        }
        curl::Multi multi{curl_multi_init()};
        if (!multi) {
            refuse_all(core::Error{.code = core::Code::Net, .message = "curl_multi_init failed"});
            return;
        }
        multi_ = multi.get();
        {
            const std::scoped_lock lock{s_.mutex};
            s_.multi = multi_;
        }
        std::vector<Command> batch;
        for (;;) {
            bool stop = false;
            {
                const std::scoped_lock lock{s_.mutex};
                batch.swap(s_.commands);
                stop = s_.stopping;
            }
            for (auto& c : batch) {
                if (auto* start = std::get_if<StartCmd>(&c)) {
                    begin(std::move(*start)); // when leaving, it is cancelled just below with the rest
                } else {
                    finish(std::get<CancelCmd>(c).id, cancelled());
                }
            }
            batch.clear();
            if (stop) {
                while (!transfers_.empty()) {
                    finish(transfers_.begin()->first, cancelled());
                }
                break;
            }
            retry_due();
            int running = 0;
            curl_multi_perform(multi_, &running);
            int left = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi_, &left)) {
                if (msg->msg == CURLMSG_DONE) {
                    complete(msg->easy_handle, msg->data.result);
                }
            }
            curl_multi_poll(multi_, nullptr, 0, sleep_ms(), nullptr);
            s_.wakeups.fetch_add(1, std::memory_order_relaxed);
        }
        {
            const std::scoped_lock lock{s_.mutex};
            s_.multi = nullptr; // no wakeup reaches it once it is gone
        }
    }

private:
    static auto cancelled() -> core::Result<Response> {
        return core::make_error(core::Code::Cancelled, "the request was cancelled");
    }

    // curl could not be had: every request, now and later, is told so.
    void refuse_all(const core::Error& e) {
        WA_ERROR("net", "no network: {}", e.message);
        std::unique_lock lock{s_.mutex};
        for (;;) {
            std::vector<Command> batch;
            batch.swap(s_.commands);
            const bool stop = s_.stopping;
            lock.unlock();
            for (auto& c : batch) {
                if (auto* start = std::get_if<StartCmd>(&c); start != nullptr && start->sink.on_done) {
                    start->sink.on_done(std::unexpected{e});
                }
            }
            lock.lock();
            if (stop) {
                return;
            }
            s_.cv.wait(lock, [&] { return s_.stopping || !s_.commands.empty(); });
        }
    }

    void begin(StartCmd cmd) {
        auto t = std::make_unique<Transfer>();
        t->id = cmd.id;
        t->request = std::move(cmd.request);
        t->sink = std::move(cmd.sink);
        t->started = Clock::now();
        t->easy.reset(curl_easy_init());
        if (!t->easy) {
            tell_done(*t, core::make_error(core::Code::Net, "curl_easy_init failed"));
            return;
        }
        CURL* e = t->easy.get();
        const Request& r = t->request;
        curl_easy_setopt(e, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(e, CURLOPT_PRIVATE, t.get());
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t->error.data());
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, t.get());
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, t.get());
        curl_easy_setopt(e, CURLOPT_PREREQFUNCTION, on_prereq);
        curl_easy_setopt(e, CURLOPT_PREREQDATA, t.get());
        curl_easy_setopt(e, CURLOPT_SOCKOPTFUNCTION, on_socket);
        curl_easy_setopt(e, CURLOPT_SOCKOPTDATA, t.get());
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(r.connect_timeout.count()));
        // "Silence": under one byte a second for that long. curl counts it in whole seconds.
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, static_cast<long>(std::max<long long>(1, (r.idle_timeout.count() + 999) / 1000)));
        for (const auto& [name, value] : r.headers) {
            const std::string line = name + ": " + value;
            curl_slist* grown = curl_slist_append(t->headers.get(), line.c_str());
            if (grown == nullptr) {
                tell_done(*t, core::make_error(core::Code::Net, "out of memory for the request's headers"));
                return;
            }
            (void)t->headers.release();
            t->headers.reset(grown);
        }
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->headers.get());
        if (r.method == "POST") {
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, r.body.data());
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        } else if (r.method != "GET") {
            curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, r.method.c_str());
            if (!r.body.empty()) {
                curl_easy_setopt(e, CURLOPT_POSTFIELDS, r.body.data());
                curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
            }
        }
        t->proxy = proxy_for(r.url, s_.config.system ? s_.config.system() : s_.config.proxy); // the system's as it is now
        curl_easy_setopt(e, CURLOPT_PROXY, t->proxy.c_str()); // "": direct, whatever the environment says
        WA_INFO("net", "{} {}{}", r.method, shown(r.url), t->proxy.empty() ? std::string{} : " via proxy " + shown(t->proxy));
        for (const auto& [name, value] : r.headers) {
            WA_DEBUG("net", "  {}: {}", name, value); // the logger masks "Authorization: Bearer …"
        }
        if (curl_multi_add_handle(multi_, e) != CURLM_OK) {
            tell_done(*t, core::make_error(core::Code::Net, "curl_multi_add_handle failed"));
            return;
        }
        t->in_multi = true;
        const RequestId id = t->id;
        transfers_.emplace(id, std::move(t));
    }

    // A request that ended by itself.
    void complete(CURL* easy, CURLcode code) {
        Transfer* raw = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &raw);
        if (raw == nullptr) {
            return;
        }
        auto it = transfers_.find(raw->id);
        if (it == transfers_.end()) {
            return;
        }
        Transfer& t = *it->second;
        curl_multi_remove_handle(multi_, easy);
        t.in_multi = false;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t.started).count();
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_OK && (status == 429 || (status >= 500 && status <= 599)) && again(t, retry_after_of(t.response))) {
            WA_INFO("net", "{} {} -> {}: try {} of {} in {} ms", t.request.method, shown(t.request.url), status, t.attempt + 1,
                    t.request.retries + 1, std::chrono::duration_cast<std::chrono::milliseconds>(*t.retry_at - Clock::now()).count());
            return;
        }
        // A connection, DNS, TLS or proxy that failed before the request went out: another try.
        // Once the server may have the request (a byte of it sent), never: a POST tried again could
        // make, and bill, a second answer; a server that went silent would cost another idle period.
        // Bytes sent on a kept connection that died with no answer don't count once curl began a
        // fresh one that failed: curl sent the request again itself, and that go never left.
        long sent = 0;
        curl_easy_getinfo(easy, CURLINFO_REQUEST_SIZE, &sent);
        const bool early = status == 0 && (sent == 0 || !t.request_out) &&
                           (code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                            code == CURLE_COULDNT_RESOLVE_PROXY || code == CURLE_SSL_CONNECT_ERROR ||
                            code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING ||
                            code == CURLE_OPERATION_TIMEDOUT);
        if (code != CURLE_OK && early && again(t, std::nullopt)) {
            WA_INFO("net", "{} {} failed ({}): try {} of {}", t.request.method, shown(t.request.url), curl_easy_strerror(code),
                    t.attempt + 1, t.request.retries + 1);
            return;
        }
        auto owned = std::move(it->second);
        transfers_.erase(it);
        if (code == CURLE_OK) {
            owned->response.status = static_cast<int>(status);
            long version = 0;
            curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &version);
            owned->response.http_version = version == CURL_HTTP_VERSION_1_0   ? 10
                                           : version == CURL_HTTP_VERSION_1_1 ? 11
                                           : version == CURL_HTTP_VERSION_2_0 ? 20
                                           : version == CURL_HTTP_VERSION_3   ? 30
                                                                              : 0;
            if (!owned->body_began && owned->sink.on_headers) {
                owned->sink.on_headers(owned->response); // a 4xx, a 5xx or an empty body
            }
            WA_INFO("net", "{} {} -> {} in {} ms", owned->request.method, shown(owned->request.url), status, ms);
            tell_done(*owned, std::move(owned->response));
            return;
        }
        const std::string detail = owned->error[0] != '\0' ? std::string{owned->error.data()} : std::string{curl_easy_strerror(code)};
        const std::string via = owned->proxy.empty() ? std::string{} : " (via proxy " + shown(owned->proxy) + ")";
        const bool timeout = code == CURLE_OPERATION_TIMEDOUT;
        const auto error = core::make_error(timeout ? core::Code::Timeout : core::Code::Net, detail + via);
        WA_WARN("net", "{} {} failed after {} ms: {}{}", owned->request.method, shown(owned->request.url), ms, detail, via);
        tell_done(*owned, error);
    }

    // Another try of `t` later, if tries are left. Only a 429, a 5xx or no status at all comes
    // here, so nothing of the body has reached the Sink (it gets a 2xx body only). Its response so
    // far is forgotten; the handle goes back in at retry_at.
    auto again(Transfer& t, std::optional<std::chrono::seconds> retry_after) -> bool {
        if (t.attempt >= t.request.retries) {
            return false;
        }
        ++t.attempt;
        t.request_out = false;
        t.response = {};
        t.error[0] = '\0';
        t.retry_at = Clock::now() + backoff(t.attempt, retry_after);
        return true;
    }

    // Retry-After in seconds (an HTTP date is not read: the doubling wait instead).
    static auto retry_after_of(const Response& r) -> std::optional<std::chrono::seconds> {
        for (const auto& [name, value] : r.headers) {
            if (name.size() == 11 && _strnicmp(name.c_str(), "Retry-After", 11) == 0) {
                int seconds = -1;
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (ec == std::errc{} && seconds >= 0) {
                    return std::chrono::seconds{seconds};
                }
            }
        }
        return std::nullopt;
    }

    // The tries whose wait is over go back to curl.
    void retry_due() {
        const auto now = Clock::now();
        for (auto& [id, t] : transfers_) {
            if (t->retry_at && *t->retry_at <= now) {
                t->retry_at.reset();
                if (curl_multi_add_handle(multi_, t->easy.get()) == CURLM_OK) {
                    t->in_multi = true;
                }
            }
        }
    }

    // How long the thread may sleep: until curl wants it, a try is due or (nothing going on) a
    // command wakes it — 0 % CPU at rest.
    auto sleep_ms() const -> int {
        long curl_ms = -1;
        curl_multi_timeout(multi_, &curl_ms);
        long wait = 24L * 3600 * 1000;
        const auto now = Clock::now();
        for (const auto& [id, t] : transfers_) {
            if (t->in_multi) {
                wait = std::min<long>(wait, curl_ms < 0 ? 1000 : std::min<long>(curl_ms, 1000));
            } else if (t->retry_at) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*t->retry_at - now).count();
                wait = std::min<long>(wait, static_cast<long>(std::max<long long>(0, left)));
            }
        }
        return static_cast<int>(wait);
    }

    // A request ended from outside (cancel, leaving): told once, then gone.
    void finish(RequestId id, core::Result<Response> result) {
        auto it = transfers_.find(id);
        if (it == transfers_.end()) {
            return; // done already, or never known
        }
        auto t = std::move(it->second);
        transfers_.erase(it);
        curl_multi_remove_handle(multi_, t->easy.get()); // waiting for another try: not in, curl says OK
        tell_done(*t, std::move(result));
    }

    State& s_;
    CURLM* multi_ = nullptr;
    std::map<RequestId, std::unique_ptr<Transfer>> transfers_;
};

} // namespace

void StateDeleter::operator()(State* s) const noexcept {
    if (s == nullptr) {
        return;
    }
    {
        const std::scoped_lock lock{s->mutex};
        s->stopping = true;
        s->wake();
    }
    if (s->thread.joinable()) {
        s->thread.join();
    }
    std::unique_ptr<State> owned{s}; // the thread is gone: nothing uses it any more
}

auto init(const Config& config) -> std::expected<StatePtr, core::Error> {
    auto s = std::make_unique<State>();
    s->config = config;
    s->thread = std::jthread{[state = s.get()] { Loop{*state}.run(); }};
    return StatePtr{s.release()};
}

auto start(State& state, Request request, Sink sink) -> RequestId {
    const RequestId id = state.next.fetch_add(1, std::memory_order_relaxed);
    const std::scoped_lock lock{state.mutex};
    state.commands.emplace_back(StartCmd{id, std::move(request), std::move(sink)});
    state.wake();
    return id;
}

void cancel(State& state, RequestId id) {
    const std::scoped_lock lock{state.mutex};
    state.commands.emplace_back(CancelCmd{id});
    state.wake();
}

auto backoff(int attempt, std::optional<std::chrono::seconds> retry_after) -> std::chrono::milliseconds {
    if (retry_after) {
        return std::min<std::chrono::milliseconds>(*retry_after, std::chrono::seconds{30});
    }
    return std::chrono::milliseconds{500} * (1 << std::clamp(attempt - 1, 0, 6));
}

auto wakeups(const State& state) -> std::uint64_t {
    return state.wakeups.load(std::memory_order_relaxed);
}

} // namespace net
