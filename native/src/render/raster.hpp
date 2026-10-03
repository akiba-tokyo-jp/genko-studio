#pragma once

#include <string>
#include <string_view>

#include "core/geometry.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// A paint layer's pixels (Python's genko/raster.py and the ops' _bake_vectors and layer_pixels): kept as a PNG at
// WORKING_DPI, its pen lines and fills drawn into it when a tool works on pixels, the eraser on it (hard, soft or
// rough).

namespace genko::render::raster {

inline constexpr int kWorkingDpi = 200;

// raster.ensure_raster: the layer's pixels ("RGBA"), or a clear page at dpi when it has none.
Image ensure_raster(const core::Page& page, const core::Layer& layer, int dpi = kWorkingDpi);

// raster.save_raster: the picture as the layer's PNG, its relpath "pages/<NNN>/<role>.png".
void save_raster(const core::Page& page, core::Layer& layer, const Image& image);

// f"pages/{page.index:03d}" (PyValueError "Unknown format code 'd' for object of type 'float'" for a float index).
std::string page_folder(const core::Page& page);

// ops._bake_vectors: the layer's fills (patches) and pen lines drawn into its pixels as they show on the page; the
// layer is a paint layer with no lines or fills after it.
void bake_vectors(const core::Page& page, core::Layer& layer);

// ops.layer_pixels: the layer's pixels as it shows (its lines and fills drawn in on a copy: the layer is not changed).
Image layer_pixels(const core::Page& page, const core::Layer& layer);

// raster.erase_raster: the eraser along `points` (mm) on the layer's pixels: "" (hard, a clean edge), "soft" (the edge
// fades out) or "rough" (a grain left behind; its random grain seeded with `seed`, Python's f"{layer.id}{points[0]}").
void erase_raster(const core::Page& page, core::Layer& layer, const core::PenPoints& points, double width_mm, int dpi,
                  std::string_view texture, const std::string& seed);

// repr(points[0]) of the eraser's points as Python passes them (a tuple of floats), "" when there are none.
std::string first_point_repr(const core::PenPoints& points);

}  // namespace genko::render::raster
