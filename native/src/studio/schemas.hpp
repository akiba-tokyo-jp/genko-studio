#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// The studio's input schemas and their checker (Python's genko/studio/schemas.py, jsonschema_lite.py and issues.py):
// bible@1, script@1 and name_plan@1, kept conservative so any LLM can emit them as tool arguments (no recursion, no
// numeric or length limits, every property required and optional values nullable), and a small JSON Schema checker for
// the subset they use (type — an integer is not a number's bool —, properties, required, additionalProperties false,
// items, enum, const, anyOf) whose findings carry Python's Japanese words.

namespace genko::studio {

// issues.Issue: one finding about agent input. `path` is a JSON pointer into that input.
struct Issue {
    std::string code;
    std::string severity;  // "error" blocks a commit, "warning" is reported only
    std::string path;
    std::string message;
    std::string hint;

    core::Json to_json() const;  // asdict(issue): {"code", "severity", "path", "message", "hint"}
};

bool has_errors(const std::vector<Issue>& issues);

// schemas.SCHEMAS[name] ("bible@1", "script@1", "name_plan@1"); core::Error("key") for another name.
const core::Json& schema(std::string_view name);

// schemas.fill_nulls: a copy of `data` with every missing nullable property set to null.
core::Json fill_nulls(const core::Json& data, const core::Json& schema);

// jsonschema_lite.validate: the findings, in Python's order and words.
std::vector<Issue> validate(const core::Json& data, const core::Json& schema, const std::string& path = {});

// jsonschema_lite.conservative_problems: why a schema is not usable as tool input by every LLM (empty when fine).
std::vector<std::string> conservative_problems(const core::Json& schema, const std::string& path = {});

}  // namespace genko::studio
