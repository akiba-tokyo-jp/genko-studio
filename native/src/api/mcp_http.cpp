// Streamable HTTP for `genko mcp --transport streamable-http` (the MCP SDK's StreamableHTTPSessionManager with its
// defaults: stateful sessions, SSE answers, no event store; the 2026 era's single exchanges; uvicorn on host:port).

#include <QCoreApplication>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include <QRandomGenerator>

#include "api/http_server.hpp"
#include "api/mcp_server.hpp"
#include "api/mcp_transport.hpp"
#include "core/base64.hpp"
#include "core/paths.hpp"
#include "service/jobs.hpp"

namespace genko::api::mcp {

namespace {

using Clock = std::chrono::steady_clock;
using Headers = std::vector<std::pair<std::string, std::string>>;

constexpr std::size_t kMaxBody = 4ULL * 1024 * 1024;  // (DEFAULT_MAX_REQUEST_BODY_SIZE)
constexpr auto kSessionIdle = std::chrono::minutes(30);  // (DEFAULT_SESSION_IDLE_TIMEOUT)
constexpr std::size_t kMaxSessions = 10000;               // (DEFAULT_MAX_SESSIONS)
constexpr int kPingMs = 15000;                            // (sse-starlette's ping; the 2026 entry's deferral and ping)
constexpr int kTick = 200;                                // (how often a waiting stream looks at its client)

std::string error_body(int code, const std::string& message) {
    return encode(Json::object({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", Json::object({{"code", code}, {"message", message}})}}));
}

// json.dumps(body, separators=(",", ":")): the 2026 entry's bodies (ASCII, \\uXXXX for the rest).
std::string ascii_body(const Json& body) {
    core::DumpOptions options;
    options.item_separator = ",";
    options.key_separator = ":";
    options.ensure_ascii = true;
    return core::dump(body, options);
}

std::string sse_event(const std::string& data) { return "event: message\r\ndata: " + data + "\r\n\r\n"; }

std::string ping_comment() {
    // (sse-starlette: ": ping - <the time in UTC>")
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[64];
    std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d.%06lld+00:00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<long long>(micros));
    return std::string(": ping - ") + text + "\r\n\r\n";
}

// check_accept_headers: (accepts JSON, accepts SSE), with */*, application/* and text/*.
std::pair<bool, bool> accepts(const http::Request& request) {
    const std::string accept = request.header("accept").value_or("");
    bool json = false;
    bool sse = false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = accept.find(',', start);
        std::string type = accept.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        type = type.substr(0, type.find(';'));
        const std::size_t a = type.find_first_not_of(" \t");
        const std::size_t b = type.find_last_not_of(" \t");
        type = a == std::string::npos ? std::string() : type.substr(a, b - a + 1);
        for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (type == "*/*") json = sse = true;
        if (type == "application/json" || type == "application/*") json = true;
        if (type == "text/event-stream" || type == "text/*") sse = true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return {json, sse};
}

bool loopback(const std::string& host) { return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]"; }

// One session of the handshake era.
struct Session {
    std::string id;
    std::unique_ptr<Connection> connection;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, std::deque<Json>> streams;  // a POST's messages by its request's id; "" the GET stream
    std::set<std::string> open;                       // the streams someone waits on
    bool terminated = false;
    bool get_open = false;
    int in_flight = 0;  // (HTTP requests being served on it)
    Clock::time_point idle_since = Clock::now();
};

class Transport {
public:
    Transport(const std::filesystem::path& root, const std::string& actor, const HttpOptions& options)
        : engine_(root, actor), options_(options) {
        if (loopback(options.host)) {
            hosts_ = {"127.0.0.1:*", "localhost:*", "[::1]:*"};
            origins_ = {"http://127.0.0.1:*", "http://localhost:*", "http://[::1]:*"};
        }
    }
    ~Transport() { retire_all(); }

    void bound(std::uint16_t port) {
        port_ = port;
        // (a host that is not a loopback name: only that host and port, where the SDK checks nothing — an IPv6
        // address in brackets, as a client writes it; a wildcard address (every interface) has no one name the clients
        // use, so there, as in the SDK, the Host is not checked)
        if (loopback(options_.host)) return;
        if (options_.host.empty() || options_.host == "0.0.0.0" || options_.host == "::" || options_.host == "[::]") {
            check_host_ = false;
            return;
        }
        const bool v6 = options_.host.find(':') != std::string::npos && options_.host.front() != '[';
        hosts_.push_back((v6 ? "[" + options_.host + "]" : options_.host) + ":" + std::to_string(port));
    }

    void handle(const http::Request& request, http::Responder& r) {
        const std::size_t question = request.target.find('?');
        const std::string route = http::percent_decode(request.target.substr(0, question));  // (scope["path"]: decoded)
        if (route != options_.path) {
            // Starlette's redirect_slashes: the path without its trailing slashes (or with one) that names the endpoint
            // is where a 307 sends the client, built from its Host header, the query kept (before any other check)
            std::string other = route;
            if (!other.empty() && other.back() == '/') {
                while (!other.empty() && other.back() == '/') other.pop_back();
            } else {
                other += '/';
            }
            if (route != "/" && other == options_.path) {
                const std::string authority = request.header("host").value_or(options_.host + ":" + std::to_string(port_));
                const std::string query = question == std::string::npos ? std::string() : request.target.substr(question);
                r.send(307, {{"location", "http://" + authority + options_.path + (query.size() > 1 ? query : std::string())}}, "");
                return;
            }
            r.send(404, {{"content-type", "text/plain; charset=utf-8"}}, "Not Found");
            return;
        }
        reap();
        const auto version = request.header("mcp-protocol-version");
        if (version && !handshake_version(*version)) {
            modern(request, r);
            return;
        }
        const auto sid = request.header("mcp-session-id");
        std::shared_ptr<Session> session;
        if (sid) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = sessions_.find(*sid);
            if (found == sessions_.end()) {
                r.send(404, {{"content-type", "application/json"}}, error_body(-32600, "Session not found"));
                return;
            }
            session = found->second;
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sessions_.size() >= kMaxSessions) {
                r.send(503, {{"content-type", "application/json"}}, error_body(-32603, "Too many open sessions"));
                return;
            }
            session = std::make_shared<Session>();
            char hex[33];
            for (int i = 0; i < 4; ++i) std::snprintf(hex + i * 8, 9, "%08x", QRandomGenerator::system()->generate());
            session->id = hex;
            Session* raw = session.get();
            session->connection = std::make_unique<Connection>(
                engine_,
                [raw](const Json& message, const std::optional<Json>& related) {
                    std::lock_guard<std::mutex> guard(raw->mutex);
                    const std::string key = related ? id_key(*related) : std::string();
                    if (!raw->open.contains(key)) return;  // (no one waits on it: dropped, as the SDK's router drops it)
                    raw->streams[key].push_back(message);
                    raw->changed.notify_all();
                },
                Connection::Options{false, true});
            sessions_[session->id] = session;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            ++session->in_flight;
        }
        const int status = serve(*session, request, r, !sid.has_value());
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            --session->in_flight;
            session->idle_since = Clock::now();
        }
        if (!sid && status >= 400) discard(session);  // (an opening request that was refused opens nothing)
        if (session->terminated) discard(session);
    }

    void stop() { retire_all(); }

private:
    // TransportSecurityMiddleware.validate_request: the Content-Type of a POST, then Host, then Origin.
    bool secure(const http::Request& request, http::Responder& r) {
        // (starlette's Response(words, status): no Content-Type)
        const auto text = [&r](int status, const char* words) { r.send(status, {}, words); };
        if (request.method == "POST") {
            std::string type = request.header("content-type").value_or("");
            for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!request.header("content-type") || type.rfind("application/json", 0) != 0) {
                text(400, "Invalid Content-Type header");
                return false;
            }
        }
        const std::string host = request.header("host").value_or("");
        bool host_ok = false;
        for (const std::string& allowed : hosts_) {
            if (host == allowed || (allowed.ends_with(":*") && host.rfind(allowed.substr(0, allowed.size() - 1), 0) == 0)) host_ok = true;
        }
        if (check_host_ && (host.empty() || !host_ok)) {
            text(421, "Invalid Host header");
            return false;
        }
        const auto origin = request.header("origin");
        if (origin && !origin->empty()) {
            bool origin_ok = false;
            for (const std::string& allowed : origins_) {
                if (*origin == allowed || (allowed.ends_with(":*") && origin->rfind(allowed.substr(0, allowed.size() - 1), 0) == 0)) origin_ok = true;
            }
            if (!origin_ok) {
                text(403, "Invalid Origin header");
                return false;
            }
        }
        return true;
    }

    // StreamableHTTPServerTransport.handle_request for one session: the status it answered with.
    int serve(Session& session, const http::Request& request, http::Responder& r, bool opening) {
        int status = 200;
        const auto error = [&](int http_status, int code, const std::string& message, Headers extra = {}) {
            Headers headers = {{"Content-Type", "application/json"}};
            for (auto& h : extra) headers.push_back(std::move(h));
            headers.emplace_back("mcp-session-id", session.id);
            r.send(http_status, headers, error_body(code, message));
            status = http_status;
        };
        if (!secure(request, r)) return 400;
        {
            std::lock_guard<std::mutex> lock(session.mutex);
            if (session.terminated) {
                error(404, -32600, "Not Found: Session has been terminated");
                return status;
            }
        }
        const auto session_header_ok = [&]() {
            const auto sid = request.header("mcp-session-id");
            if (!sid || sid->empty()) {
                error(400, -32600, "Bad Request: Missing session ID");
                return false;
            }
            if (*sid != session.id) {
                error(404, -32600, "Not Found: Invalid or expired session ID");
                return false;
            }
            return true;
        };
        if (request.method == "POST") {
            const auto [json, sse] = accepts(request);
            if (!(json && sse)) {
                error(406, -32600, "Not Acceptable: Client must accept both application/json and text/event-stream");
                return status;
            }
            bool type_ok = false;
            {
                const std::string type = request.header("content-type").value_or("");
                const std::string first = type.substr(0, type.find(';'));
                std::size_t start = 0;
                for (;;) {
                    const std::size_t comma = first.find(',', start);
                    std::string part = first.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    const std::size_t a = part.find_first_not_of(" \t");
                    const std::size_t b = part.find_last_not_of(" \t");
                    part = a == std::string::npos ? std::string() : part.substr(a, b - a + 1);
                    if (part == "application/json") type_ok = true;
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            if (!type_ok) {
                error(415, -32600, "Unsupported Media Type: Content-Type must be application/json");
                return status;
            }
            auto decoded = decode(request.body);
            if (!decoded) {
                error(400, -32700, "Parse error: the body is not JSON");
                return status;
            }
            const Shape shape = shape_of(decoded->body);
            if (shape == Shape::Invalid) {
                error(400, -32602, "Validation error: the body is not a JSON-RPC message");
                return status;
            }
            const bool initialize = shape == Shape::Request && decoded->body["method"] == Json("initialize");
            if (initialize) {
                const auto sid = request.header("mcp-session-id");
                if (sid && !sid->empty() && *sid != session.id) {
                    error(404, -32600, "Not Found: Invalid or expired session ID");
                    return status;
                }
            } else if (!session_header_ok()) {
                return status;
            }
            if (shape != Shape::Request) {
                r.send(202, {{"Content-Type", "application/json"}, {"mcp-session-id", session.id}}, "");
                session.connection->receive(std::move(*decoded));
                return 202;
            }
            const std::string key = id_key(decoded->body["id"]);
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                session.open.insert(key);
                session.streams[key].clear();
            }
            r.start(200, {{"Cache-Control", "no-cache, no-transform"},
                          {"Connection", "keep-alive"},
                          {"Content-Type", "text/event-stream"},
                          {"mcp-session-id", session.id},
                          {"X-Accel-Buffering", "no"}});
            session.connection->receive(std::move(*decoded));
            stream(session, key, r, true);
            return 200;
        }
        if (request.method == "GET") {
            const auto [json, sse] = accepts(request);
            (void)json;
            if (!sse) {
                error(406, -32600, "Not Acceptable: Client must accept text/event-stream");
                return status;
            }
            if (!session_header_ok()) return status;
            if (request.header("last-event-id")) {
                // (no event store: the SDK's replay answers nothing, and its server says so)
                r.send(500, {{"content-type", "text/plain; charset=utf-8"}}, "Internal Server Error");
                return 500;
            }
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                if (session.get_open) {
                    error(409, -32600, "Conflict: Only one SSE stream is allowed per session");
                    return status;
                }
                session.get_open = true;
                session.open.insert("");
            }
            r.start(200, {{"Cache-Control", "no-cache, no-transform"},
                          {"Connection", "keep-alive"},
                          {"Content-Type", "text/event-stream"},
                          {"mcp-session-id", session.id},
                          {"X-Accel-Buffering", "no"}});
            stream(session, "", r, false);
            std::lock_guard<std::mutex> lock(session.mutex);
            session.get_open = false;
            session.open.erase("");
            session.streams.erase("");
            return 200;
        }
        if (request.method == "DELETE") {
            if (!session_header_ok()) return status;
            {
                std::lock_guard<std::mutex> lock(session.mutex);
                session.terminated = true;
                session.changed.notify_all();
            }
            r.send(200, {{"Content-Type", "application/json"}, {"mcp-session-id", session.id}}, "");
            return 200;
        }
        error(405, -32600, "Method Not Allowed", {{"Allow", "GET, POST, DELETE"}});
        return status;
    }

    // An SSE stream: the messages for `key` as they come (the POST's ends with its answer), a ping every 15 s, until
    // the client goes, the session ends or the server stops.
    void stream(Session& session, const std::string& key, http::Responder& r, bool until_answer) {
        auto last_ping = Clock::now();
        for (;;) {
            std::deque<Json> ready;
            bool ended = false;
            {
                std::unique_lock<std::mutex> lock(session.mutex);
                session.changed.wait_for(lock, std::chrono::milliseconds(kTick), [&] {
                    return !session.streams[key].empty() || session.terminated || http::stop_flag().load();
                });
                ready.swap(session.streams[key]);
                ended = session.terminated || http::stop_flag().load();
            }
            for (const Json& message : ready) {
                if (!r.write(sse_event(encode(message)))) {
                    forget(session, key);
                    return;
                }
                if (until_answer && (message.contains("result") || message.contains("error"))) {
                    forget(session, key);
                    return;
                }
            }
            if (ended) {
                forget(session, key);
                return;
            }
            if (Clock::now() - last_ping >= std::chrono::milliseconds(kPingMs)) {
                if (!r.write(ping_comment())) {
                    forget(session, key);
                    return;
                }
                last_ping = Clock::now();
            }
            if (r.gone(0)) {
                forget(session, key);
                return;
            }
        }
    }

    void forget(Session& session, const std::string& key) {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.open.erase(key);
        session.streams.erase(key);
    }

    // handle_modern_request: one 2026-era POST.
    void modern(const http::Request& request, http::Responder& r) {
        if (!secure(request, r)) return;
        if (request.method != "POST") {
            r.send(405, {{"Allow", "POST"}}, "");
            return;
        }
        const auto [json, sse] = accepts(request);
        if (!json || !sse) {
            r.send(406, {}, "");
            return;
        }
        const auto write = [&r](const Json& message) {
            int status = 200;
            Json body = message;
            if (message.contains("error")) {
                status = http_status_for(message["error"]["code"].get<int>());
                if (message["id"].is_null()) {  // (exclude_none drops the id, and the entry puts it back last)
                    body = Json::object({{"jsonrpc", "2.0"}, {"error", message["error"]}, {"id", nullptr}});
                }
            }
            r.send(status, {{"content-type", "application/json"}}, ascii_body(body));
        };
        const auto decoded = decode(request.body);
        if (!decoded) {
            write(Json::object({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", Json::object({{"code", -32700}, {"message", "Parse error"}})}}));
            return;
        }
        const Json& body = decoded->body;
        const Json invalid_body = Json::object(
            {{"jsonrpc", "2.0"},
             {"id", nullptr},
             {"error", Json::object({{"code", -32600}, {"message", "Body must be a single JSON-RPC request or notification object"}})}});
        if (body.is_object() && !body.contains("id")) {
            if (shape_of(body) != Shape::Notification) {
                write(invalid_body);
                return;
            }
            const std::string requested = request.header("mcp-protocol-version").value_or("");
            if (requested != kModernVersion) {
                write(Json::object({{"jsonrpc", "2.0"},
                                    {"id", nullptr},
                                    {"error", RpcError{-32022, "Unsupported protocol version",
                                                       Json::object({{"supported", Json::array({std::string(kModernVersion)})}, {"requested", requested}})}
                                                  .to_json()}}));
                return;
            }
            r.send(202, {}, "");  // (acknowledged and dropped: the 2026 era has no notifications from a client over HTTP)
            return;
        }
        if (shape_of(body) != Shape::Request) {
            write(invalid_body);
            return;
        }
        const Json id = body["id"];
        const std::string method = body["method"].get<std::string>();
        static const std::set<std::string> kRouting{"mcp-protocol-version", "mcp-method", "mcp-name"};
        std::set<std::string> seen;
        for (const auto& [name, value] : request.headers) {
            (void)value;
            if (!kRouting.contains(name)) continue;
            if (!seen.insert(name).second) {
                write(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", RpcError{-32020, name + " header appears more than once", std::nullopt}.to_json()}}));
                return;
            }
        }
        std::map<std::string, std::string> headers;
        for (const auto& [name, value] : request.headers) {
            if (headers.contains(name)) continue;
            std::string v = value;
            if (name == "mcp-name" && v.rfind("=?base64?", 0) == 0 && v.size() >= 11 && v.ends_with("?=")) {
                const std::string payload = v.substr(9, v.size() - 11);
                try {
                    const std::string decoded_name = core::a2b_base64(payload);
                    v = core::b64encode(decoded_name) == payload ? decoded_name : std::string("\x01invalid");
                } catch (const std::exception&) {
                    v = std::string("\x01invalid");
                }
            }
            headers[name] = v;
        }
        if (auto rejection = classify_modern(body, &headers)) {
            write(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", rejection->to_json()}}));
            return;
        }
        const Json params = body.contains("params") ? body["params"] : Json();
        // the handler beside this thread: its answer as JSON when it comes within 15 s, else SSE with pings
        struct Slot {
            std::mutex mutex;
            std::condition_variable done;
            std::optional<Json> reply;
        };
        auto slot = std::make_shared<Slot>();
        const std::vector<std::string> odd = decoded->odd_numbers;
        // (a thread for it once one of the kRequestThreads is free; a client that left before then is not served:
        // its work never starts. Work that started runs to its end — a write is never cut in the middle — and its
        // answer is dropped when the client left.)
        std::shared_ptr<RequestPermit> permit;
        while (!(permit = RequestPermit::within(kTick))) {
            if (r.gone(0) || http::stop_flag().load()) return;
        }
        start_work([this, slot, id, method, params, odd, permit]() mutable {
            Json reply = method == "subscriptions/listen" ? Json() : serve_one(engine_, id, method, params, odd);
            permit.reset();
            if (method == "subscriptions/listen") return;  // (a listen stream never ends from this side)
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->reply = std::move(reply);
            slot->done.notify_all();
        });
        const auto wait_reply = [&slot](int ms) {
            std::unique_lock<std::mutex> lock(slot->mutex);
            slot->done.wait_for(lock, std::chrono::milliseconds(ms), [&slot] { return slot->reply.has_value(); });
            return slot->reply;
        };
        const auto deferral = Clock::now() + std::chrono::milliseconds(kPingMs);
        while (Clock::now() < deferral) {
            if (auto reply = wait_reply(kTick)) {
                write(*reply);
                return;
            }
            if (r.gone(0) || http::stop_flag().load()) return;  // (the client closed: the handler's answer is dropped)
        }
        r.start(200, {{"content-type", "text/event-stream"}, {"cache-control", "no-cache, no-transform"}, {"connection", "keep-alive"},
                      {"x-accel-buffering", "no"}});
        auto last_ping = Clock::now() - std::chrono::milliseconds(kPingMs);
        for (;;) {
            if (Clock::now() - last_ping >= std::chrono::milliseconds(kPingMs)) {
                if (!r.write(": ping\r\n\r\n")) return;
                last_ping = Clock::now();
            }
            if (auto reply = wait_reply(kTick)) {
                r.write(sse_event(ascii_body(*reply)));
                return;
            }
            if (r.gone(0) || http::stop_flag().load()) return;
        }
    }

    // Work that may outlive its HTTP request (a 2026-era handler whose client went away): joined when it ends.
    void start_work(std::function<void()> work) {
        std::lock_guard<std::mutex> lock(work_mutex_);
        auto done = std::make_shared<std::atomic<bool>>(false);
        works_.push_back({std::thread([work = std::move(work), done] {
                              try {
                                  work();
                              } catch (const std::exception& error) {
                                  std::fprintf(stderr, "genko mcp: %s\n", error.what());
                              }
                              done->store(true);
                          }),
                          done});
    }

    void reap() {
        std::vector<std::thread> ended;
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            auto keep = std::partition(works_.begin(), works_.end(), [](const Work& w) { return !w.done->load(); });
            for (auto it = keep; it != works_.end(); ++it) ended.push_back(std::move(it->thread));
            works_.erase(keep, works_.end());
        }
        for (std::thread& t : ended) t.join();
        // sessions with nothing in flight for 30 minutes end, as the SDK's idle timeout ends them
        std::vector<std::shared_ptr<Session>> idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, session] : sessions_) {
                std::lock_guard<std::mutex> inner(session->mutex);
                if (session->in_flight == 0 && Clock::now() - session->idle_since > kSessionIdle) idle.push_back(session);
            }
        }
        for (const auto& session : idle) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->terminated = true;
            }
            discard(session);
        }
    }

    // The session forgotten (its id answers 404 from now on); its requests still running end on a thread of their own.
    void discard(const std::shared_ptr<Session>& session) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = sessions_.find(session->id);
            if (found == sessions_.end() || found->second != session) return;
            sessions_.erase(found);
        }
        if (!session->connection->busy()) {
            session->connection->close();  // (nothing of it runs: closed here, no thread for it)
            return;
        }
        start_work([session] { session->connection->close(); });  // (at most as many as the requests running)
    }

    void retire_all() {
        std::vector<std::shared_ptr<Session>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, session] : sessions_) all.push_back(session);
        }
        for (const auto& session : all) discard(session);
        std::vector<Work> works;
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            works.swap(works_);
        }
        for (Work& w : works) w.thread.join();
    }

    struct Work {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    Engine engine_;
    HttpOptions options_;
    std::uint16_t port_ = 0;
    std::vector<std::string> hosts_;
    bool check_host_ = true;
    std::vector<std::string> origins_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex work_mutex_;
    std::vector<Work> works_;
};

}  // namespace

int serve_streamable_http(const std::filesystem::path& root, const std::string& actor, const HttpOptions& options) {
    http::install_stop_handlers();
    Transport transport(root, actor, options);
    http::Limits limits;
    limits.max_body = kMaxBody;
    limits.chunked_bodies = true;
    limits.bad_request = "Invalid HTTP request received.";  // (uvicorn's words for a request h11 cannot read)
    limits.too_large = [](http::Responder& r) { r.send(413, {}, "Request body too large"); };
    http::Server server([&transport](const http::Request& request, http::Responder& r) { transport.handle(request, r); }, limits);
    if (const auto error = server.listen(options.host, options.port)) {
        std::fprintf(stderr, "genko mcp: %s\n", error->c_str());
        return 1;
    }
    transport.bound(server.port());
    const std::string host = options.host.find(':') != std::string::npos && options.host.front() != '[' ? "[" + options.host + "]" : options.host;
    std::fprintf(stderr, "genko mcp http://%s:%u%s root=%s\n", host.c_str(), static_cast<unsigned>(server.port()), options.path.c_str(),
                 core::path_to_utf8(root).c_str());
    std::fflush(stderr);
    server.run(http::stop_flag());
    transport.stop();
    service::jobs::wait_all();
    return 0;
}

}  // namespace genko::api::mcp
