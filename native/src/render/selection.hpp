#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/fill.hpp"
#include "render/image.hpp"

// Working on part of a layer (Python's genko/selection.py): an area (a polygon, or a mask over a box) and what lies in
// it — pen lines, fills and pasted pixels (patches), the layer's own pixels — lifted, moved, scaled, turned, deleted,
// pasted. And the richer areas of genko/selops.py (rect, ellipse, layer, color, all, saved, union, intersect,
// subtract, invert, grow_mm, feather_mm) turned into a mask or a plain area.
//
// Matrices are affine [a, b, c, d, e, f]: x' = a·x + c·y + e, y' = b·x + d·y + f (mm).

namespace genko::render::selection {

inline constexpr int kWorkingDpi = 200;  // raster.WORKING_DPI
inline constexpr int kSelDpi = 200;      // selops.SEL_DPI

using Matrix = std::array<double, 6>;
inline constexpr Matrix kIdentity{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};

// An area as a mask ("L") and where its corner is, in pixels at a resolution.
struct AreaMask {
    Image mask;
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
};

// selection.area_mask(area, dpi): {"poly"} drawn, or {"mask": {"box", "png"}} read and resized (NEAREST). `what` names
// the op's key in the errors of this build ("<what> must be a finite number", "<what> is too large").
AreaMask area_mask(const core::Json& area, int dpi = fills::kFillDpi, std::string_view what = "area");

// base64.b64decode(value) of a str (Python's errors for other values and for text that is not ASCII).
std::string b64decode(const core::Json& value);

// Image.open(bytes) and load(), with the errors Python's apply_ops lets through as Python names them
// (PIL.UnidentifiedImageError, OSError).
Image open_picture(const std::string& bytes);

// An area asked about point by point (selection.contains): its polygon and, for a mask area, its mask at FILL_DPI are
// made the first time they are needed, with Python's errors then.
class AreaTest {
public:
    explicit AreaTest(const core::Json& area, std::string_view what = "area") : area_(area), what_(what) {}
    // selection.contains(area, x, y)
    bool contains(double x, double y);
    // selection.stroke_inside: most of the line's points are in the area.
    bool stroke_inside(const core::Stroke& stroke);
    // area_mask(area) at FILL_DPI (the patches' resolution), made once.
    const AreaMask& fill_mask();

private:
    const core::Json& area_;
    std::string what_;
    std::optional<std::vector<std::pair<double, double>>> poly_;
    std::optional<AreaMask> mask_;
};

// selection.transform_stroke: the line's points through m (a new stroke: a new id, its width scaled by √|det m|; its
// kind, colour and opacity kept).
core::Stroke transform_stroke(const core::Stroke& stroke, const Matrix& m);

// selection._invert: OpError-free ValueError "the transform squashes the selection flat" (PyValueError).
Matrix invert(const Matrix& m);

// A patch's picture at FILL_DPI and its corner in pixels (selection._patch_px).
struct PatchPicture {
    Image image;
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
};
PatchPicture patch_px(const core::Patch& patch, int dpi = fills::kFillDpi);

// selection._to_patch: a patch from a page-aligned picture ("L" mask or "RGBA") cropped to what it shows, with the
// template's keys (but png, asset, box and id), a new id and its box; nothing when it shows nothing.
std::optional<core::Patch> to_patch(const Image& image, std::int64_t x0, std::int64_t y0, const core::Patch& templ,
                                    int dpi = fills::kFillDpi);

// selection.transform_patch
std::optional<core::Patch> transform_patch(const core::Patch& patch, const Matrix& m, Resample resample = Resample::Bilinear);

// What lies in an area: the lines and the patches lifted from a layer (or copied, to paste).
struct Items {
    std::vector<core::StrokePtr> strokes;
    std::vector<core::Patch> patches;
};

// selection.lift: what lies in the area taken off the layer (the layer keeps the rest; its own pixels in the area
// become an image patch).
Items lift(core::Layer& layer, const core::Json& area, const core::Page& page);

// selection.drop: lifted (or copied) items put back on the layer through m; fresh ids for a paste.
void drop(core::Layer& layer, Items items, const Matrix& m, bool fresh_ids = false, Resample resample = Resample::Bilinear);

// selection.drop_warped: through a free transform (render/warp.hpp): lines point by point, pixels piece by piece.
using WarpFunction = std::function<std::pair<double, double>(double, double)>;
void drop_warped(core::Layer& layer, Items items, const WarpFunction& go, Resample resample = Resample::Bilinear);

// selection.items_from_json: copied items as an op carries them ({"strokes", "patches": [{…, "png": base64}]}).
Items items_from_json(const core::Json& data);

// selection.area_bbox: (x, y, w, h) of a poly; the mask's box as it is (any length) of a mask area.
std::vector<double> area_bbox(const core::Json& area);

// --- selops -------------------------------------------------------------------------------------------------------

// selops.to_mask: the area as a page-sized mask ("L", 255 inside) at `dpi`, any kind. Throws OpError with AreaError's
// words.
Image to_mask(const core::Json& area, const core::Page& page, const core::Document* episode, int dpi = kSelDpi);

// selops.resolve: any area to a plain one ({"poly"} or {"mask"}) the drawing ops take. OpError "the area is empty".
core::Json resolve(const core::Json& area, const core::Page& page, const core::Document* episode);

// selops.from_mask: a page-sized mask back to {"mask": {"box", "png"}} (cropped to what it covers); nothing when empty.
std::optional<core::Json> from_mask(const Image& mask, int dpi = kSelDpi);

// selops.rect_poly / ellipse_poly
core::Json rect_poly(const core::Json& box);
core::Json ellipse_poly(const core::Json& box, int n = 72);

}  // namespace genko::render::selection
