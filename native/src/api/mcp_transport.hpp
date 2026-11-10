#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The transports of `genko mcp` (the MCP SDK's stdio_server and Streamable HTTP, run by MCPServer.run(transport)).

namespace genko::api::mcp {

// Bytes as Python's UTF-8 decoder with errors="replace" reads them: each malformed sequence (its maximal subpart) as one
// U+FFFD.
std::string utf8_replace(std::string_view bytes);

// stdio_server: one connection over stdin and stdout. While it serves, fd 0 reads the null device and fd 1 goes to
// stderr, so nothing else in the process (a library, a child program) can write into the protocol; each line of input
// is one message (Python's universal newlines: "\n", "\r\n" or "\r"; a line longer than kMaxLine is dropped, which
// Python would read whole), each answer one line of output. Returns at the end of input, once every request has been
// answered and every background job has ended. Exit code 0.
inline constexpr std::size_t kMaxLine = 64ULL * 1024 * 1024;
int serve_stdio(const std::filesystem::path& root, const std::string& actor);

// Streamable HTTP (MCPServer.run("streamable-http"): uvicorn on host:port, the endpoint /mcp): sessions of the handshake
// era (Mcp-Session-Id, SSE answers, the GET stream, DELETE), single POST exchanges of the 2026 era, the Host and Origin
// checks (always on: the SDK checks them only on a loopback host), the 4 MiB body limit. `port` 0 takes a free port.
// The address it listens on is written to stderr ("genko mcp http://127.0.0.1:<port>/mcp root=<root>"). Runs until
// SIGINT / SIGTERM (or the console's Ctrl+C), then lets the requests that run end.
struct HttpOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8000;
    std::string path = "/mcp";
};
int serve_streamable_http(const std::filesystem::path& root, const std::string& actor, const HttpOptions& options);

}  // namespace genko::api::mcp
