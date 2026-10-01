#pragma once

#include <string>
#include <string_view>

#include "core/json.hpp"

// The state of a book (schema-v4 §4.1): project.json without "revision" and "writer", as canonical JSON (keys
// sorted, no spaces), kept as an asset with the suffix .state.json. The same content always gives the same ref;
// Undo and Redo compare states, never revisions.

namespace genko::storage {

inline constexpr std::string_view kStateSuffix = ".state.json";
inline constexpr std::string_view kSnapshotSuffix = ".project.json";  // (a v3 journal's project.json snapshots)
inline constexpr std::string_view kOpsSuffix = ".ops.json";

// The payload without revision and writer.
core::Json state_of(const core::Json& payload);
// Its bytes (canonical JSON) and its ref ("sha256:…").
std::string state_text(const core::Json& payload);
std::string state_ref(const core::Json& payload);

}  // namespace genko::storage
