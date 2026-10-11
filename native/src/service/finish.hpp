#pragma once

#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "service/blocking.hpp"

// The page's finish (Python's genko/studio/finish.py): the reported faces kept clear of the lines, the effects, the
// marks, then the page set to finish.

namespace genko::service::finish {

// finish.regions_stale(panel): the faces were reported on art that is no longer the adopted one.
bool regions_stale(const core::Json& panel);
// finish.region_figures(panel): the faces and people reported in the adopted art, as the lettering's figures.
std::vector<Figure> region_figures(const core::Json& panel);
// finish.plan(episode, page): (ops, notes) — the finishing pass's ops and what each does, for the preview.
std::pair<core::Json, core::Json> plan(const core::Document& doc, const core::Page& page);

}  // namespace genko::service::finish
