#pragma once

#include <string_view>

#include "core/json.hpp"

namespace genko::core {

// The public list of ops (`genko schema`): native/src/core/data/ops_schema.json, embedded at build time. It is the
// contract of the ops: when an op's arguments change, change this file in the same commit.
std::string_view ops_schema_text();
const Json& ops_schema();

}  // namespace genko::core
