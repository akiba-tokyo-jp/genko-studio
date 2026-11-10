#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small HTTP/1.1 server for `genko serve` and `genko mcp --transport streamable-http`: one request per connection
// (every response closes it), each connection on its own thread, with bounds Python's servers do not have (header size
// and count, the body's size, read timeouts, connections at once and from one address). A request can be refused from
// its head before its body is read (admit). After a response the server stops writing and reads, for a moment, what
// the client still sends, so that a client cut short (an early 413, 431, 503) gets the answer rather than a reset. Qt's
// QTcpSocket in blocking mode (QtNetwork, LGPL); headless.

namespace genko::api::http {

struct Request {
    std::string method;  // "GET"
    std::string target;  // the request-target as sent ("/v1/inspect?path=a%20b")
    std::string version;  // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;  // names in lower case, in order
    std::string body;
    std::string peer;       // the client's address
    std::string principal;  // what admit found out about the client (genko serve: the token's actor)

    // The first header of that name (lower case), as Python's message.get and starlette's Headers.get read it.
    std::optional<std::string> header(std::string_view name) const;
};

// How a handler answers (on the connection's thread).
class Responder {
public:
    virtual ~Responder() = default;
    // A whole response (Content-Length set, the connection closed after it).
    virtual void send(int status, const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body) = 0;
    // The status line and headers of a response whose body follows in pieces until the connection closes.
    virtual bool start(int status, const std::vector<std::pair<std::string, std::string>>& headers) = 0;
    // A piece of the body (false when the client went away).
    virtual bool write(std::string_view piece) = 0;
    // Whether the client closed its side, waiting up to `wait_ms` to find out.
    virtual bool gone(int wait_ms) = 0;
    virtual bool started() const = 0;
};

using Handler = std::function<void(const Request&, Responder&)>;

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Limits {
    std::size_t max_header_bytes = 64 * 1024;  // the request line and headers together
    std::size_t max_headers = 100;
    std::size_t max_body = 64ULL * 1024 * 1024;
    int header_timeout_ms = 10000;    // the request line and headers within this from the connection's start
    int idle_timeout_ms = 30000;      // no bytes from the client for this long while it sends a request: the connection ends
    int request_timeout_ms = 300000;  // the whole request (line, headers and body) within this
    int max_connections = 64;
    int max_connections_per_peer = 32;  // from one client address at once (the rest: 503)
    int linger_ms = 1000;               // after a response, what the client still sends is read and dropped this long
    // How a body without Content-Length is read: chunked (uvicorn: Transfer-Encoding "chunked" alone, never with a
    // Content-Length) or not at all (Python's http.server ignores Transfer-Encoding).
    bool chunked_bodies = false;
    // The words of a 400 for a request that is not HTTP (uvicorn: "Invalid HTTP request received.").
    std::string bad_request = "Bad request syntax";
    // The answer to a body past max_body (before it is read).
    std::function<void(Responder&)> too_large;
    // Once the head is read and the body's size is known to be allowed, before the body is read: false when it
    // answered the request itself (the body is then never read). It may set the request's principal.
    std::function<bool(Request& head, Responder&)> admit;
    // Headers added to every response once the head is read (genko serve: CORS for an allowed Origin).
    std::function<void(const Request& head, int status, Headers& headers)> decorate;
};

// The status line's words (http.HTTPStatus phrases).
std::string_view reason_phrase(int status);

class Server {
public:
    Server(Handler handler, Limits limits);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listen on host:port (port 0: a free one). The error's words when it cannot.
    std::optional<std::string> listen(const std::string& host, std::uint16_t port);
    std::uint16_t port() const;
    // Serve until `stop` is set, then wait for the connections being served to end (a request still arriving is not
    // waited for).
    void run(const std::atomic<bool>& stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The process's stop flag, set by SIGINT and SIGTERM (Ctrl+C / Ctrl+Break on Windows) once install_stop_handlers ran.
std::atomic<bool>& stop_flag();
void install_stop_handlers();

// %XX decoded (urllib.parse.unquote: UTF-8, malformed sequences replaced by U+FFFD).
std::string percent_decode(std::string_view text, bool plus_as_space = false);

}  // namespace genko::api::http
