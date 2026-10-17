#pragma once

#include <optional>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"

// color-model@1 (SPEC.md COLOR-03, document.pixel): what a page holds at a point, in its own colour model — `genko
// inspect --pixel` and the status bar's values under the cursor.

namespace genko::render {

// The pixel of the page at a point (mm) as the page is drawn (at `dpi`), or of one layer's own pixels there (`layer`:
// its id; the raster stretched over the page): {"page", "mode", "depth", "values" (in the page's space: RGB 0..1 of its
// profile, CMYK inks 0..1, Lab L a b, grey k), "stored" (the samples as kept: 8 and 16 bits integers, 32 bits floats),
// "alpha", "srgb" [r, g, b] (as the screen shows it)}; a layer with no pixels there: its values null, alpha 0.
// core::Error("key") for a point off the page or an unknown layer.
core::Json pixel_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, const std::optional<std::string>& layer,
                    int dpi);

}  // namespace genko::render
