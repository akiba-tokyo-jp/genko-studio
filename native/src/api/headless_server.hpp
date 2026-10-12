#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "api/http_server.hpp"
#include "core/json.hpp"

// `genko serve`: the headless HTTP JSON API (Python's genko/server.py). Every route but /health needs
// `Authorization: Bearer <token>` (tokens.json decides the actor); the Host header must name this server (DNS
// rebinding), a browser's Origin must be allowed, bodies are JSON (images for /v1/assets) of at most 64 MiB, and every
// path in a request stays inside --root (symbolic links followed before the check). Routes: GET /health, /schema,
// /openapi.json, /v1/inspect, /v1/pages/{n}.png, /v1/pages/{n}/frames/{id}.png, /v1/spreads/{a}-{b}.png,
// /v1/requests/{id}/files/{name}, /v1/jobs/{id}; POST /v1/new, /v1/apply, /v1/export, /v1/assets. Changes go through
// the ops under the book's lock, as the actor of the token; a person's approvals stay with HumanService, which no route
// reaches.

namespace genko::api::headless {

using core::Json;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8765;
    std::filesystem::path root;
    std::vector<std::string> allow_origins;
    std::vector<std::string> allow_hosts;
};

// One request routed (handle_request with the guard of _Handler): the status, the body, and the headers to add.
struct Reply {
    int status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class Api {
public:
    // The tokens are read from tokens.json (again whenever the file changes: a revoked token stops working at once).
    explicit Api(Options options);
    void bound(std::uint16_t port);
    Reply serve(const http::Request& request);

private:
    // handle_request after the guard: the route's reply, Python's exceptions turned into its statuses.
    Reply route_request(const std::string& method, const std::string& route, const std::map<std::string, std::string>& query,
                        const std::string& body, const std::string& actor);
    Reply routes(const std::string& method, const std::string& route, const std::map<std::string, std::string>& query, const Json& data,
                 const std::vector<std::string>& odd_numbers, const std::string& actor);

    Options options_;
    std::uint16_t port_ = 0;
};

// The command: tokens made (human:owner) when there are none, the address on stderr, served until SIGINT / SIGTERM.
int serve(const Options& options);

}  // namespace genko::api::headless
