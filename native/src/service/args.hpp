#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/json.hpp"

// Python's binding of a call's arguments to a method's parameters (f(*positional, **kwargs)), with Python 3.12's
// TypeError for what does not bind: what MCP, HTTP and `genko studio call` hand a tool by name.

namespace genko::service {

using core::Json;

struct Param {
    const char* name;
    std::optional<Json> fallback;  // nothing: required
    bool keyword_only = false;
};

// The parameters' values (every one, in order: the default for those not given) as a JSON object. Throws
// core::PyTypeError with Python's words ("StudioService.render() missing 1 required positional argument: 'page'",
// "… got an unexpected keyword argument 'x'", "… got multiple values for argument 'project'", "… takes from 2 to 4
// positional arguments but 5 were given").
Json bind_arguments(std::string_view qualname, std::span<const Param> params, const Json& positional, const Json& kwargs);

}  // namespace genko::service
