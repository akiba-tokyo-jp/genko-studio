#pragma once

#include <string_view>

// How deep a JSON text goes, counted before it is read (core/json.cpp). nlohmann reads any depth, but its copies, value()
// and comparisons go down it by recursion: a text deeper than its reader allows is never parsed.

namespace genko::core {

// How deep the brackets of a JSON text go ([ and { inside one another, those inside strings not counted); `limit`: the
// count stops there (service::control::nesting for the control channel's lines).
int json_nesting(std::string_view text, int limit);

// (PC6, the review's P3-2) The depth of this computer's own files and settings read with nlohmann — actions.json,
// session.json, the notation table, the measurement's data points — as deep as MCP's requests (api/mcp_server's 255):
// deeper is read as a file that is not JSON.
inline constexpr int kMaxFileDepth = 255;
inline bool too_deep(std::string_view text, int max_depth = kMaxFileDepth) { return json_nesting(text, max_depth + 1) > max_depth; }

}  // namespace genko::core
