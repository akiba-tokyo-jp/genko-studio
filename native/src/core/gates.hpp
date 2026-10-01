#pragma once

#include "core/json.hpp"

// Approvals in a project.json payload (Python's io._gates and io.gate_changes): each page's name_ok and art_ok, each
// character's locked, and the number of export approvals that are not revoked.

namespace genko::core {

// The approval state of a payload: {"page:<id>:name_ok": bool, "page:<id>:art_ok": bool,
// "character:<id>:locked": bool, "export_approvals": int}, keys sorted. A null payload has none.
Json gates_of(const Json* payload);

// What changed between two payloads, as Python lists it: [{"what": key, "from": old value or null, "to": new
// value}, …] in key order, for the keys of `after` whose value differs from `before` (a missing key counts as false,
// or 0 for export_approvals). `before` may be null (a new book).
Json gate_changes(const Json* before, const Json& after);

}  // namespace genko::core
