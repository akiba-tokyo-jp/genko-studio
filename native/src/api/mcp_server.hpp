#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/mcp_tools.hpp"
#include "core/json.hpp"

// The MCP protocol of `genko mcp`, as the MCP SDK 2.2 (Python's mcp>=2,<3, uv.lock) serves an MCPServer: JSON-RPC 2.0
// messages; the handshake era (initialize, then requests; protocol 2024-11-05 … 2025-11-25) and the 2026-07-28 era
// (server/discover, and each request carrying its own envelope in params._meta); the methods MCPServer serves (ping,
// server/discover, tools/list, tools/call, resources/list, resources/templates/list, resources/read, prompts/list,
// prompts/get, subscriptions/listen) with the SDK's checks of their params, results in each version's shape, and its
// errors (code, message and data); requests served side by side, notifications/cancelled (the request is not answered:
// a tool that already runs finishes, as a Python tool running on its worker thread does, and its answer is dropped).
// The transports (stdio, Streamable HTTP) hand messages in and take them out (api/mcp_transport.hpp).

namespace genko::api::mcp {

inline constexpr std::string_view kModernVersion = "2026-07-28";
inline constexpr std::string_view kLatestHandshake = "2025-11-25";
inline constexpr std::string_view kDefaultNegotiated = "2025-03-26";  // (a connection's version before initialize)

bool handshake_version(std::string_view version);

// A JSON-RPC error (ErrorData): code, message and data (absent when nothing).
struct RpcError {
    int code = 0;
    std::string message;
    std::optional<Json> data;

    Json to_json() const;
};

// A decoded inbound message and the JSON pointers where its parser had to stand in for numbers Python would take
// (NaN and the infinities: null; integers past 64 bits: the nearest float).
struct Inbound {
    Json body;
    std::vector<std::string> odd_numbers;
};

// The JSON text as the SDK reads a message off the wire (pydantic's JSON: NaN and Infinity are numbers there): the
// value, or nothing for text that is not JSON (or nested deeper than the SDK reads).
std::optional<Inbound> decode(std::string_view text);

// What a decoded body is (JSONRPCMessage's union: a request needs a strict int or str id, a notification is the rest
// with a method; a response or an error answers a request of ours).
enum class Shape { Request, Notification, Response, Invalid };
Shape shape_of(const Json& body);

// The JSON of an outgoing message, as the SDK writes it (no spaces, UTF-8).
std::string encode(const Json& message);

// The server: the tools and resources, and each request's handling for a connection in some protocol version.
class Engine {
public:
    Engine(fs::path root, std::string actor);

    const Tools& tools() const { return tools_; }

    // The result of `method` (its params as sent) for a connection at `version` (born ready in the 2026 era): the
    // wire form, or RpcError. `initialized`: whether the handshake era's connection has been initialized.
    Json handle(const std::string& method, const Json& params, const std::string& version, bool initialized,
                const std::vector<std::string>& odd_numbers) const;

    // initialize: the result (in the shape of the connection's version before it) and the negotiated version. RpcError
    // for params the SDK refuses.
    std::pair<Json, std::string> initialize(const Json& params, const std::string& version) const;

private:
    Tools tools_;
};

// The 2026 era's checks of a request's envelope (classify_inbound_request): nothing when it passes, else the error.
// `headers` (lower-case names) for HTTP, nothing for stdio.
std::optional<RpcError> classify_modern(const Json& body, const std::map<std::string, std::string>* headers);
// The routing headers sent twice (HTTP only).
bool has_modern_envelope(const Json& params);

// One request of the 2026 era whose envelope passed classify_modern (serve_one): the JSON-RPC reply, a response or an
// error (a handler's other failures are "Internal server error" there).
Json serve_one(const Engine& engine, const Json& id, const std::string& method, const Json& params,
               const std::vector<std::string>& odd_numbers);

// The HTTP status the 2026 era's Streamable HTTP answers an error code with (ERROR_CODE_HTTP_STATUS; 200 for others).
int http_status_for(int code);

// A thread a request runs on. The process runs at most kRequestThreads of them at once (stdio, the HTTP sessions and the
// 2026 era's POSTs together); one more waits for one to end, so a client sending requests faster than they end, or
// leaving them behind, cannot make the server start threads without end.
constexpr int kRequestThreads = 64;
class RequestPermit {
public:
    RequestPermit();  // (waits for one)
    ~RequestPermit();
    RequestPermit(const RequestPermit&) = delete;
    RequestPermit& operator=(const RequestPermit&) = delete;
    // One within `wait_ms`, or nothing.
    static std::shared_ptr<RequestPermit> within(int wait_ms);

private:
    struct Taken {};
    explicit RequestPermit(Taken) {}
};

// One duplex connection (stdio; a Streamable HTTP session of the handshake era): what the client sends goes in through
// receive() on the reader's thread, what the server answers goes out through `send` (from any thread, one at a time).
// The era is fixed by the client's first request (stdio: dual; an HTTP session: the handshake era only).
class Connection {
public:
    struct Options {
        bool dual_era = true;  // false: the handshake era only (Streamable HTTP sessions)
        // A request that ends without an answer (cancelled): the 2025-era Streamable HTTP ends its stream with
        // -32800 "Request cancelled"; stdio writes nothing.
        bool answer_cancelled = false;
    };
    // `send(message, related)`: an outgoing message, and the request id it answers (nothing for none).
    using Send = std::function<void(const Json& message, const std::optional<Json>& related)>;

    Connection(const Engine& engine, Send send, Options options);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // One message off the wire (already decoded), or a line that was not a message (ignored, as the SDK drops it).
    void receive(Inbound message);
    // The end of input: requests still running finish (their answers are not written, as the SDK's are not), then it
    // returns.
    void close();
    // Whether the connection has been initialized (the handshake era).
    bool initialized() const;
    // Whether requests of it are running or waiting for a thread.
    bool busy() const;

private:
    struct InFlight {
        Json id;
        bool cancelled = false;
    };

    void dispatch_request(Inbound message, const std::string& era);
    void dispatch_notification(const Json& body);
    void run_request(Json id, std::string method, Json params, std::vector<std::string> odd, std::shared_ptr<InFlight> flight,
                     std::string era);
    void finish(const std::shared_ptr<InFlight>& flight, const Json& reply);

    const Engine& engine_;
    Send send_;
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::string era_;                       // "", "legacy" or "modern"
    std::vector<Inbound> lead_;              // (frames before the first request, replayed once the era is known)
    std::string version_;                    // the handshake era's negotiated version
    bool initialize_accepted_ = false;
    bool closing_ = false;
    std::map<std::string, std::shared_ptr<InFlight>> in_flight_;  // by coerced id
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers_;  // (each request's thread; joined once it ends, and all of them at close)
    int running_ = 0;
};

// The key requests correlate by (coerce_request_id: "7" and 7 are one id).
std::string id_key(const Json& id);

}  // namespace genko::api::mcp
