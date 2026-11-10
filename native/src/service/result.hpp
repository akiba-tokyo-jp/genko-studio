#pragma once

#include <string>
#include <vector>

#include "core/json.hpp"
#include "studio/schemas.hpp"

// What a studio tool answers (Python's genko/studio/service.py: ToolResult, fail, wording_error, and issues.error /
// warning): ok, the tool's own data, the issues found in what the agent sent (errors first), the small PNG previews
// for the model to look at and the same previews as files. MCP, HTTP and `genko studio` all answer with to_dict().

namespace genko::service {

using core::Json;
using studio::Issue;

// issues.error / issues.warning
Issue error(std::string code, std::string path, std::string message, std::string hint = {});
Issue warning(std::string code, std::string path, std::string message, std::string hint = {});
bool has_errors(const std::vector<Issue>& issues);

struct ToolResult {
    bool ok = true;
    Json data = Json::object();
    std::vector<Issue> issues;
    std::vector<std::string> images;  // PNG bytes
    std::vector<std::string> files;   // paths (as Python's str(path))

    ToolResult() = default;
    ToolResult(bool is_ok, Json with = Json::object(), std::vector<Issue> found = {}, std::vector<std::string> pngs = {},
               std::vector<std::string> paths = {});

    // {"ok", **data, "issues": [errors first, then warnings, each in its order], "files"} — keys in Python's order (a
    // key of data's named ok, issues or files keeps its place, its value replaced as the dict literal replaces it).
    Json to_dict() const;
};

// service.wording_error: an op's refusal in Japanese where the wording is known (the op's place in the list kept).
std::string wording_error(const std::string& message);

// service.fail: one failure said once: "error" in full (with how the op is written, when that helps), the issue's
// message without that part, and "detail" only as the short original when the wording changed it.
ToolResult fail(const std::string& message, const std::string& code = "error", const std::string& path = "/");

}  // namespace genko::service
