#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/model.hpp"
#include "render/image.hpp"

// Placed pictures (a layer of kind "placed": art adopted from a candidate, a picture put on the page with place_asset):
// the size of an asset's picture, which the studio ops fit the picture with (core/studio.hpp), and the picture drawn
// on the page (Python's render._placed_raster and _finish_placed, with _face_mask and _clothes_masks).

namespace genko::render {

// studio_ops._asset_size's picture: Image.open(assets/<ab>/<sha256>.png of the book `book`).size, nothing when there is
// no such file (or `ref` is no asset ref). Python's errors as it raises them: core::PyUncaught UnidentifiedImageError
// ("cannot identify image file <_io.BytesIO object>") and DecompressionBombError; NotYetPorted("image_format") for TIFF,
// WebP and PSD. A JPEG, BMP or GIF whose pixels cannot be decoded, or a file larger than storage::kAssetMaxBytes, is
// refused with core::OpError "the asset's picture cannot be read (…)" (C++ only: Python reads its size from the header
// and the page cannot be drawn later).
std::optional<std::pair<std::int64_t, std::int64_t>> asset_picture_size(const std::filesystem::path& book, const std::string& ref);

// render._placed_raster(layer, page, episode, size, dpi, mode, finish) over the box `area` of the page (an "RGBA"
// picture of the area): the asset's picture resampled (LANCZOS) into its placement, only the part inside its clip box
// (clip_box: the panel, the panel run out to the bleed, or the page) and the page, cut to the panel's shape (a slanted,
// rounded or curved panel), and with `finish` the monochrome finish of a mono page (finish_placed). Nothing when Python
// has nothing: no book folder, no asset or placement, no such file, nothing of it on the page (then the area is not
// looked at: a part with none of the picture is a clear picture). `dots`: not render._NO_DOTS. The asset's errors as
// Image.open raises them (core::Error "unidentified_image", "image_too_large", "format"; NotYetPorted("image_format")).
// A ref that is not "sha256:" + 64 hex digits is no file (Python would look outside assets/ for some).
std::optional<Image> placed_raster(const core::Layer& layer, const core::Page& page, const core::Document* episode, Size size,
                                   int dpi, std::string_view mode, bool finish, bool dots, const Box& area);

// The pictures placed_raster remembers between tiles (decoded assets, finished art), forgotten (clear_render_caches).
void clear_placed_caches();

// render._finish_placed(fitted, layer, page, episode, dpi, mode, origin): a placed (or painted) picture on a monochrome
// page in print or proof: extracted line art (source.to "ink") as pure black lines, other art grey with the finish of
// studio.style.finish and the layer's own (screentone.finish_gray: tones as dots in print, flat greys in a proof; the
// panel's reported faces lightened, the clothes the bible prints black or white so). `origin`: where the picture's
// top-left is on the page (the dots line up across layers). Unchanged in another mode, on a colour page, and for a
// draft or name layer.
Image finish_placed(const Image& fitted, const core::Layer& layer, const core::Page& page, const core::Document* episode, int dpi,
                    std::string_view mode, bool dots, Point origin);

}  // namespace genko::render
