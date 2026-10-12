#pragma once

#include <QStringList>

// The commands that serve Genko to other programs (Python's `genko mcp`, `genko serve` and `genko token`):
//   genko mcp --root R [--agent ai:<name>] [--transport stdio|streamable-http] [--host H] [--port P]
//       the MCP server (api/mcp_tools.hpp): stdio (Python's only transport) by default; streamable-http as the MCP
//       SDK's MCPServer.run("streamable-http") serves it (127.0.0.1:8000, /mcp)
//   genko serve --root R [--host 127.0.0.1] [--port 8765] [--allow-origin O]… [--allow-host H]…
//       the headless HTTP API (api/headless_server.hpp)
//   genko token add --actor human:<name>|ai:<name> | list | revoke --id <prefix>
//       the HTTP tokens (tokens.json in the config folder)

namespace genko::api {

int mcp_command(const QStringList& args);
int serve_command(const QStringList& args);
int token_command(const QStringList& args, bool ascii);

}  // namespace genko::api
