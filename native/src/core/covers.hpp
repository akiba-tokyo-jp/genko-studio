#pragma once

#include "core/json.hpp"
#include "core/model.hpp"

// Covers (表紙・裏表紙・カバー・帯): pages marked page.extra["cover"] = {"kind": front | back | jacket | obi,
// "spine_mm", "flap_mm", "height_mm"} (Python's genko/covers.py).

namespace genko::core {

// The paper of a cover (covers.spec_for): the book's own for the front and back; for a jacket (カバー) or an obi
// (帯), one wide sheet with both covers, the spine and the flaps, with the book's bleed and paper allowance.
PageSpec spec_for(const PageSpec& book, const Json& cover);

// covers.cover_of: the page's cover entry when it is one (kind front, back, jacket or obi), else null.
const Json* cover_of(const Page& page);

// covers.is_cover
inline bool is_cover(const Page& page) { return cover_of(page) != nullptr; }

}  // namespace genko::core
