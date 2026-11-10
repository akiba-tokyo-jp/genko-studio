#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// The tools and resources `genko mcp` serves (Python's genko/mcp/server.py on MCPServer of the MCP SDK 2): the 41 agent
// tools with their descriptions and input schemas, the arguments checked as the SDK's pydantic model checks them (the
// same coercions, and on failure the same "Error executing tool …: N validation errors for <tool>Arguments …" text),
// each tool handed to StudioService with Python's positional mapping, and Python's call(): the conversation's name
// (session), the book-in-use warning, the tool log, the next work item on a write's reply and the refusal's wording.
// Approving is a person's act (HumanService): no tool here reaches it, and the actor is always an agent (ai:<name>).

namespace genko::api::mcp {

namespace fs = std::filesystem;
using core::Json;

// The server's instructions (MCPServer(instructions=…)).
std::string_view instructions();

enum class Kind { Str, Int, Float, Bool, Dict, List, ListInt, ListFloat, ListStr, ListDict };

struct ToolParam {
    std::string_view name;
    Kind kind;
    bool nullable;               // `X | None`
    std::optional<Json> fallback;  // nothing: required
};

struct ToolSpec {
    std::string_view name;
    std::string_view description;  // the tool function's docstring, as Python keeps it
    std::vector<ToolParam> params;  // the function's parameters (session last)
};

// The 41 tools in Python's order of registration.
const std::vector<ToolSpec>& tool_specs();
const ToolSpec* find_tool(std::string_view name);

// The tool's input schema as Python publishes it (pydantic's schema of the arguments, through slim_schema).
Json input_schema(const ToolSpec& tool);

// FuncMetadata.validate_arguments: the arguments pre-parsed (a JSON text in a string for a parameter that is not a plain
// str) and checked as pydantic's lax model checks them. Either every parameter's value (the defaults for those not
// given) in parameter order, or the ValidationError's text ("1 validation error for statusArguments\nproject\n  Field
// required [type=missing, …]\n    For further information visit https://errors.pydantic.dev/2.13/v/missing").
struct Checked {
    std::optional<Json> values;
    std::string error;
    // Where the arguments hold a number this build cannot carry on, which Python would take (NaN, an infinity, or an
    // integer past 64 bits, given as such or as text): the JSON pointer of the first one (this build refuses the call).
    std::optional<std::string> unsupported;
};
Checked check_arguments(const ToolSpec& tool, const Json& arguments);

// The JSON pointer of the first non-finite float in a value (nothing when there is none).
std::optional<std::string> unsupported_number(const Json& value);

// What a tool call answers: the content blocks of CallToolResult and isError.
struct CallOutcome {
    Json content = Json::array();
    bool is_error = false;
};

// One server's tools: the root and the agent (ai:<name>) the server was started with.
class Tools {
public:
    // ApplyError (Python's words) for an actor that is not ai:<name>.
    Tools(fs::path root, std::string actor);

    const fs::path& root() const { return root_; }
    const std::string& actor() const { return actor_; }

    // tools/call: the tool by name with its arguments (a JSON object), as MCPServer's call_tool runs it. `unsupported`:
    // where the request's arguments held a number this build cannot carry (NaN, an infinity, an integer past 64 bits;
    // its parser left null there): the call is refused once the tool is known.
    CallOutcome call(std::string_view name, const Json& arguments, const std::optional<std::string>& unsupported = std::nullopt) const;

private:
    fs::path root_;
    std::string actor_;
};

// --- resources -------------------------------------------------------------------------------------------------------

struct ResourceSpec {
    std::string_view uri;
    std::string_view name;
    std::string_view description;
    std::string_view mime_type;
};
const std::vector<ResourceSpec>& resource_specs();
// The resource's text, or nothing for a URI that is not one of them.
std::optional<std::string> read_resource(std::string_view uri);

}  // namespace genko::api::mcp
