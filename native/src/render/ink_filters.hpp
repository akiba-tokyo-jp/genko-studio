#pragma once

#include <string>
#include <string_view>

#include "core/adjust.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// color-mode@1, the third step (SPEC.md COLOR-02, 群C): filter_raster on a paint layer of a CMYK or Lab page, every
// kind of filters.KINDS but a plugin (which works the picture the screen shows, render/ink_edit's lending).
//
// What moves or blurs pixels works each ink by itself, at the raster's depth: the samples weighted by their alpha (a
// line blurred over nothing keeps its inks, its alpha fades), so a line on K alone stays on K alone. What changes
// brightness or colour does it as inks do: darker is more K (Lab: less L), lighter less ink; a colour laid in (noise,
// rain, a line drawing, what despeckle leaves) takes the page's rules. The corrections (levels, curve, hue …) work the
// colours the inks mean and go back by the rules (render/ink correct): what they leave as it was keeps its inks.
//
// Sizes in pixels are the 8-bit layer's at the working 200 dpi, scaled to the raster's resolution.

namespace genko::render::ink_filters {

// The raster filtered (GKIR bytes, its space, depth and size). The settings read and refused as the 8-bit filter
// reads them; `area`: only inside it (filters.within), on the page.
std::string apply(const core::Page& page, const core::InkPixels& raster, std::string_view kind, const core::Json& params,
                  const core::Json* area);

// PC2: a correction of the C++ build's own (core/adjust.hpp's AdjustFunction) on the raster: its records on the plates
// directly when it has them for the page's space (design 4.2), else the colours the inks mean through it and back by
// the rules (where a pixel is: its place in the raster); `area` as apply.
std::string apply_function(const core::Page& page, const core::InkPixels& raster, const core::AdjustFunction& f, const core::Json* area);

}  // namespace genko::render::ink_filters
