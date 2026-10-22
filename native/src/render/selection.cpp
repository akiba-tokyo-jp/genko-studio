#include "render/selection.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <string>
#include <utility>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/color_model.hpp"
#include "core/channels.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/area_kinds.hpp"
#include "render/channels.hpp"
#include "render/draw.hpp"
#include "render/layer_masks.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/path_trace.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/select_color.hpp"
#include "render/shape_parts.hpp"
#include "render/warp.hpp"

namespace genko::render::selection {

using core::Json;

namespace {

double mm2px(double v, int dpi) { return v / 25.4 * dpi; }

// round(x) as a whole double, kept within the corners a picture may have
double rounded_corner(double v, std::string_view what) {
    const double r = core::py_round_whole(v);
    limits::check_coordinate(r, what);
    return r;
}

[[noreturn]] void no_get(const Json& value) {
    throw core::PyUncaught("AttributeError", "'" + core::py_type_name(value) + "' object has no attribute 'get'");
}

// patch.get(key) of a patch as Python holds it (its keys, then the ones added after it was read)
const Json* patch_get(const core::Patch& patch, std::string_view key) {
    if (const Json* v = core::get(patch.attrs, key)) return v;
    return core::get(patch.after_asset, key);
}

// patch[key] = value: in its place when the patch has the key, else after its other keys (after "asset" when it has
// one, as Python's dict has it by then)
void patch_set(core::Patch& patch, const std::string& key, Json value) {
    if (patch.attrs.contains(key)) {
        patch.attrs[key] = std::move(value);
    } else if (patch.after_asset.contains(key) || patch.asset) {
        patch.after_asset[key] = std::move(value);
    } else {
        patch.attrs[key] = std::move(value);
    }
}

// for x, y in poly: (float(x), float(y)) — each point unpacked, then converted
std::vector<std::pair<double, double>> poly_points(const Json& poly, std::string_view what) {
    std::vector<std::pair<double, double>> out;
    const std::vector<Json> items = core::iterate(poly);
    limits::check_count(static_cast<double>(items.size()), static_cast<double>(limits::kPoints), what);
    for (const Json& item : items) {
        const std::vector<Json> xy = core::unpack_values(item, 2);
        const double x = core::finite_float(xy[0], what);
        const double y = core::finite_float(xy[1], what);
        out.emplace_back(x, y);
    }
    return out;
}

// _patch_image: the patch's picture, "L" for a mask patch, "RGBA" for an image
Image patch_image(const core::Patch& patch) {
    if (!patch.png) throw core::OpKeyError("'png'");
    const Image opened = open_picture(*patch.png);
    const Json* mode = patch_get(patch, "mode");
    const bool mask = mode == nullptr || *mode == Json("mask");
    return opened.convert(mask ? "L" : "RGBA");
}

}  // namespace

std::string b64decode(const Json& value) {
    if (!value.is_string()) {
        throw core::PyTypeError("argument should be a bytes-like object or ASCII string, not '" + core::py_type_name(value) + "'");
    }
    const std::string& text = value.get_ref<const std::string&>();
    if (std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
        throw core::PyValueError("string argument should contain only ASCII characters");
    }
    try {
        return core::a2b_base64(text);
    } catch (const core::PyValueError&) {
        throw;
    } catch (const core::Error& error) {
        throw core::PyValueError(error.what());  // (binascii.Error is a ValueError)
    }
}

Image open_picture(const std::string& bytes) {
    try {
        return open_image(bytes, kPillowOpenLimits);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() == "unidentified_image") {
            throw core::PyUncaught("PIL.UnidentifiedImageError", "cannot identify image file <_io.BytesIO object>");
        }
        if (error.code() == "image_too_large") throw core::PyUncaught("PIL.Image.DecompressionBombError", error.what());
        if (error.code() == "format") throw core::PyUncaught("OSError", error.what());
        throw;
    }
}

namespace {

std::atomic<int> g_grey_area_stretch{static_cast<int>(Resample::Bilinear)};

}  // namespace

Resample grey_area_stretch() { return static_cast<Resample>(g_grey_area_stretch.load()); }

void set_grey_area_stretch(Resample resample) { g_grey_area_stretch.store(static_cast<int>(resample)); }

AreaMask area_mask(const Json& area, int dpi, std::string_view what) {
    if (!area.is_object()) no_get(area);
    if (core::truthy_at(area, "poly")) {
        std::vector<std::pair<double, double>> pts;
        for (const auto& [x, y] : poly_points(area["poly"], what)) pts.emplace_back(mm2px(x, dpi), mm2px(y, dpi));
        double min_x = pts[0].first, min_y = pts[0].second, max_x = pts[0].first, max_y = pts[0].second;
        for (const auto& [x, y] : pts) {
            min_x = core::py_min(min_x, x);
            min_y = core::py_min(min_y, y);
            max_x = core::py_max(max_x, x);
            max_y = core::py_max(max_y, y);
        }
        const double x0 = core::py_trunc(min_x) - 1;
        const double y0 = core::py_trunc(min_y) - 1;
        const double x1 = core::py_trunc(max_x) + 2;
        const double y1 = core::py_trunc(max_y) + 2;
        limits::check_coordinate(x0, what);
        limits::check_coordinate(y0, what);
        const double w = std::max(1.0, x1 - x0);
        const double h = std::max(1.0, y1 - y0);
        limits::check_sides(w, h, what);
        Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
        std::vector<PointD> corners;
        corners.reserve(pts.size());
        for (const auto& [x, y] : pts) corners.push_back(PointD{x - x0, y - y0});
        Draw(mask).polygon(corners, Ink(255));
        return AreaMask{std::move(mask), static_cast<std::int64_t>(x0), static_cast<std::int64_t>(y0)};
    }
    const Json* given = core::get(area, "mask");
    const Json spec = given != nullptr && core::py_truthy(*given) ? *given : Json::object();
    const Json box_value = core::subscript(spec, "box");
    const std::vector<double> box = core::unpack_floats(box_value, 4);
    for (const double v : box) {
        if (!std::isfinite(v)) throw core::OpError(std::string(what) + " must be a finite number");
    }
    const std::string data = b64decode(core::subscript(spec, "png"));
    const Image image = open_picture(data).convert("L");
    const double w = std::max(1.0, core::py_round_whole(mm2px(box[2], dpi)));
    const double h = std::max(1.0, core::py_round_whole(mm2px(box[3], dpi)));
    limits::check_sides(w, h, what);
    // (PC3 B-0: a grey picture — an anti-aliased or feathered selection's — stretched bilinear, its grey edge staying
    // smooth at the patches' 300 dpi and a precise page's finer grid; a two-valued one stays hard, as Photoshop keeps a
    // selection made without anti-aliasing, pixel for pixel what it was)
    bool grey = false;
    for (const char v : image.tobytes()) {
        const auto u = static_cast<unsigned char>(v);
        if (u != 0 && u != 255) {
            grey = true;
            break;
        }
    }
    Image sized = image.resize(Size{static_cast<int>(w), static_cast<int>(h)}, grey ? grey_area_stretch() : Resample::Nearest);
    const double x0 = rounded_corner(mm2px(box[0], dpi), what);
    const double y0 = rounded_corner(mm2px(box[1], dpi), what);
    return AreaMask{std::move(sized), static_cast<std::int64_t>(x0), static_cast<std::int64_t>(y0)};
}

// --- AreaTest -----------------------------------------------------------------------------------------------------

const AreaMask& AreaTest::fill_mask() {
    if (!mask_) mask_ = area_mask(area_, fills::kFillDpi, what_);
    return *mask_;
}

bool AreaTest::contains(double x, double y) {
    if (!area_.is_object()) no_get(area_);
    if (core::truthy_at(area_, "poly")) {
        if (!poly_) poly_ = poly_points(area_["poly"], what_);
        const auto& pts = *poly_;
        bool inside = false;
        std::size_t j = pts.size() - 1;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto [xi, yi] = pts[i];
            const auto [xj, yj] = pts[j];
            if ((yi > y) != (yj > y)) {
                const double dy = (yj - yi) != 0.0 ? (yj - yi) : 1e-9;
                if (x < (xj - xi) * (y - yi) / dy + xi) inside = !inside;
            }
            j = i;
        }
        return inside;
    }
    const AreaMask& m = fill_mask();
    const double px = core::py_round_whole(mm2px(x, fills::kFillDpi)) - static_cast<double>(m.x0);
    const double py = core::py_round_whole(mm2px(y, fills::kFillDpi)) - static_cast<double>(m.y0);
    if (!(0 <= px && px < m.mask.width() && 0 <= py && py < m.mask.height())) return false;
    return m.mask.getpixel(static_cast<int>(px), static_cast<int>(py))[0] > 127;
}

bool AreaTest::stroke_inside(const core::Stroke& stroke) {
    if (stroke.points.empty()) return false;
    std::size_t n = 0;
    for (const core::PointF& p : stroke.points) {
        if (contains(p.x, p.y)) ++n;
    }
    return n * 2 >= stroke.points.size();
}

// --- lines and patches through a matrix ----------------------------------------------------------------------------

core::Stroke transform_stroke(const core::Stroke& stroke, const Matrix& m) {
    const auto [a, b, c, d, e, f] = m;
    core::PenPoints points;
    // zip(stroke.points, stroke.pressure or [None] * len(stroke.points)): as many as the shorter has
    const bool with_pressure = !stroke.pressure.empty();
    const std::size_t n = with_pressure ? std::min(stroke.points.size(), stroke.pressure.size()) : stroke.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double x = stroke.points[i].x;
        const double y = stroke.points[i].y;
        core::PenPoint p{a * x + c * y + e, b * x + d * y + f, std::nullopt};
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw core::OpError("the transform is too far off the page");
        if (with_pressure) p.p = stroke.pressure[i];
        points.push_back(p);
    }
    core::Stroke out = core::coerce_stroke(points);
    out.width_mm = core::py_round(stroke.width_mm * std::sqrt(std::fabs(a * d - b * c)), 4);
    if (!std::isfinite(out.width_mm)) throw core::OpError("the transform makes a line too wide");
    out.kind = stroke.kind;
    out.rgb = stroke.rgb;
    out.color_rgb = stroke.color_rgb;
    out.opacity = stroke.opacity;
    core::carry_inputs(stroke, out);  // (brushes@1, PC4 E1: its flow, 背景色 and pen inputs, point for point)
    return out;
}

Matrix invert(const Matrix& m) {
    const auto [a, b, c, d, e, f] = m;
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-12) throw core::PyValueError("the transform squashes the selection flat");
    const double ia = d / det;
    const double ib = -b / det;
    const double ic = -c / det;
    const double id = a / det;
    return Matrix{ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

PatchPicture patch_px(const core::Patch& patch, int dpi) {
    const Json* box_value = patch_get(patch, "box");
    if (box_value == nullptr) throw core::OpKeyError("'box'");
    const std::vector<double> box = core::unpack_floats(*box_value, 4);
    const double w = std::max(1.0, core::py_round_whole(mm2px(box[2], dpi)));
    const double h = std::max(1.0, core::py_round_whole(mm2px(box[3], dpi)));
    limits::check_sides(w, h, "a patch");
    Image image = patch_image(patch).resize(Size{static_cast<int>(w), static_cast<int>(h)}, Resample::Lanczos);
    const double x0 = rounded_corner(mm2px(box[0], dpi), "a patch");
    const double y0 = rounded_corner(mm2px(box[1], dpi), "a patch");
    return PatchPicture{std::move(image), static_cast<std::int64_t>(x0), static_cast<std::int64_t>(y0)};
}

std::optional<core::Patch> to_patch(const Image& image, std::int64_t x0, std::int64_t y0, const core::Patch& templ, int dpi) {
    const Image alpha = image.mode() == "L" ? image : image.getchannel(3);
    const auto box = alpha.getbbox();
    if (!box) return std::nullopt;
    const Image crop = image.crop(*box);
    core::Patch out;
    out.png = fills::png_bytes(crop);
    const auto keep = [](const std::string& key) { return key != "png" && key != "asset" && key != "box" && key != "id"; };
    for (const auto& [key, value] : templ.attrs.items()) {
        if (keep(key)) out.attrs[key] = value;
    }
    for (const auto& [key, value] : templ.after_asset.items()) {
        if (keep(key)) out.attrs[key] = value;
    }
    const double mm = 25.4 / dpi;
    out.attrs["id"] = core::new_id();
    out.attrs["box"] = Json::array({core::py_round(static_cast<double>(x0 + box->x0) * mm, 3),
                                    core::py_round(static_cast<double>(y0 + box->y0) * mm, 3),
                                    core::py_round(crop.width() * mm, 3), core::py_round(crop.height() * mm, 3)});
    return out;
}

std::optional<core::Patch> transform_patch(const core::Patch& patch, const Matrix& m, Resample resample) {
    const PatchPicture pic = patch_px(patch);
    const double ox = static_cast<double>(pic.x0);
    const double oy = static_cast<double>(pic.y0);
    const double w = pic.image.width();
    const double h = pic.image.height();
    const double scale = fills::kFillDpi / 25.4;
    const auto [a, b, c, d, e, f] = m;
    const auto page_px = [&](double x, double y) {
        const double mx = x / scale;
        const double my = y / scale;
        return std::pair<double, double>{(a * mx + c * my + e) * scale, (b * mx + d * my + f) * scale};
    };
    const std::pair<double, double> moved[4] = {page_px(ox, oy), page_px(ox + w, oy), page_px(ox, oy + h), page_px(ox + w, oy + h)};
    double min_x = moved[0].first, min_y = moved[0].second, max_x = moved[0].first, max_y = moved[0].second;
    for (const auto& [x, y] : moved) {
        min_x = core::py_min(min_x, x);
        min_y = core::py_min(min_y, y);
        max_x = core::py_max(max_x, x);
        max_y = core::py_max(max_y, y);
    }
    const double nx0 = std::floor(min_x);
    const double ny0 = std::floor(min_y);
    const double nx1 = std::ceil(max_x);
    const double ny1 = std::ceil(max_y);
    const Matrix inv = invert(m);
    const auto back = [&](double x, double y) {
        const double mx = x / scale;
        const double my = y / scale;
        return std::pair<double, double>{(inv[0] * mx + inv[2] * my + inv[4]) * scale, (inv[1] * mx + inv[3] * my + inv[5]) * scale};
    };
    limits::check_coordinate(nx0, "the transform");
    limits::check_coordinate(ny0, "the transform");
    // PIL's affine maps output (x, y) → input: back(x + nx0, y + ny0) − (ox, oy) as coefficients
    const auto p00 = back(nx0, ny0);
    const auto p10 = back(nx0 + 1, ny0);
    const auto p01 = back(nx0, ny0 + 1);
    const double coeffs[6] = {p10.first - p00.first, p01.first - p00.first, p00.first - ox,
                              p10.second - p00.second, p01.second - p00.second, p00.second - oy};
    const double sw = std::max(1.0, nx1 - nx0);
    const double sh = std::max(1.0, ny1 - ny0);
    limits::check_sides(sw, sh, "the transform");
    // libImaging's nearest path casts inverse-mapped coordinates before checking image bounds.
    // An affine function reaches its extrema at the output rectangle's corners. Include the extra
    // end coordinate used by the incremental scale loop, and validate coefficients before any C cast.
    for (const double value : coeffs) limits::check_coordinate(value, "the inverse transform");
    for (const double x : {0.0, sw + 1.0}) {
        for (const double y : {0.0, sh + 1.0}) {
            const double bx = coeffs[0] * x + coeffs[1] * y + coeffs[2];
            const double by = coeffs[3] * x + coeffs[4] * y + coeffs[5];
            limits::check_coordinate(bx, "the inverse transform");
            limits::check_coordinate(by, "the inverse transform");
        }
    }
    const Image out = pic.image.transform(Size{static_cast<int>(sw), static_cast<int>(sh)}, TransformMethod::Affine, coeffs, resample);
    return to_patch(out, static_cast<std::int64_t>(nx0), static_cast<std::int64_t>(ny0), patch);
}

// --- lift and drop -------------------------------------------------------------------------------------------------

namespace {

// selection._split_patch: (the part outside the area, the part inside)
std::pair<std::optional<core::Patch>, std::optional<core::Patch>> split_patch(const core::Patch& patch, AreaTest& area) {
    const PatchPicture pic = patch_px(patch);
    const AreaMask& m = area.fill_mask();
    Image local = Image::create("L", pic.image.size(), Ink(0));
    const double dx = static_cast<double>(m.x0 - pic.x0);
    const double dy = static_cast<double>(m.y0 - pic.y0);
    limits::check_coordinate(dx, "the area");
    limits::check_coordinate(dy, "the area");
    local.paste(m.mask, Point{static_cast<int>(dx), static_cast<int>(dy)});
    const bool mask_mode = pic.image.mode() == "L";
    const Image alpha = mask_mode ? pic.image : pic.image.getchannel(3);
    const Image inside_alpha = chops::multiply(alpha, local);
    const Image outside_alpha = chops::subtract(alpha, inside_alpha);
    const auto with_alpha = [&](const Image& a) {
        if (mask_mode) return a;
        Image out = pic.image.copy();
        out.putalpha(a);
        return out;
    };
    auto outside = to_patch(with_alpha(outside_alpha), pic.x0, pic.y0, patch);
    auto inside = to_patch(with_alpha(inside_alpha), pic.x0, pic.y0, patch);
    return {std::move(outside), std::move(inside)};
}

// pen-shapes@1 (PC3 C-b): an area's outline as a path in mm — its polygon, or its mask's outline traced at the
// selection's 200 dpi within half a pixel.
core::vector::Path area_outline(const Json& area) {
    namespace vec = core::vector;
    if (area.is_object() && core::truthy_at(area, "poly")) {
        vec::Subpath s;
        for (const auto& [x, y] : poly_points(area["poly"], "area")) s.knots.push_back(vec::Knot::corner(x, y));
        vec::Path out;
        if (s.knots.size() >= 3) out.subpaths.push_back(std::move(s));
        return out;
    }
    const AreaMask m = area_mask(area, kSelDpi);
    const std::string bytes = m.mask.tobytes();
    std::vector<float> values(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    const int x0 = static_cast<int>(m.x0), y0 = static_cast<int>(m.y0);
    const double k = 25.4 / kSelDpi;
    return path_trace::trace_mask(values, path_raster::PixelBox{x0, y0, x0 + m.mask.width(), y0 + m.mask.height()}, 0.0f, 0.5)
        .transformed({k, 0.0, 0.0, k, 0.0, 0.0});
}

// The layer's shape parts that stay, those lifted added to `in`: a part wholly inside the area goes whole, one wholly
// outside stays, one the area's edge crosses is cut in two along it (render/shape_parts cut, baked: the lifted half a new
// id); an open or inverted part, which has no inside to cut, goes whole when most of its anchors are inside.
std::vector<core::vector::ShapePart> lift_parts(const core::Layer& layer, const Json& area, AreaTest& test, Items& in) {
    namespace vec = core::vector;
    constexpr double kSliver = 0.002;  // (mm²: about a twelfth of a pixel at 200 dpi — nothing of it on that side)
    std::vector<vec::ShapePart> kept;
    std::optional<vec::Path> outline;
    std::set<std::string> ids;
    for (const vec::ShapePart& part : layer.shapes->items) ids.insert(part.id);
    for (const vec::ShapePart& part : layer.shapes->items) {
        if (part.path.empty()) {
            kept.push_back(part);
            continue;
        }
        if (!shape_parts::closed(part.path) || part.path.inverted) {
            std::size_t inside = 0, all = 0;
            for (const vec::Subpath& s : part.path.subpaths) {
                for (const vec::Knot& k : s.knots) {
                    ++all;
                    if (test.contains(k.anchor.x, k.anchor.y)) ++inside;
                }
            }
            (all > 0 && inside * 2 >= all ? in.shapes : kept).push_back(part);
            continue;
        }
        if (!outline) outline = area_outline(area);
        const auto [inner, outer] = shape_parts::shares(part.path, *outline);
        if (outer <= kSliver) {
            in.shapes.push_back(part);
            continue;
        }
        if (inner <= kSliver) {
            kept.push_back(part);
            continue;
        }
        vec::ShapePart lifted = part, left = part;
        lifted.path = shape_parts::cut(part.path, *outline, true);
        left.path = shape_parts::cut(part.path, *outline, false);
        lifted.live.reset();
        left.live.reset();
        do {
            lifted.id = core::new_id();
        } while (ids.contains(lifted.id));
        ids.insert(lifted.id);
        if (!lifted.path.empty()) in.shapes.push_back(vec::part_from_json(vec::to_json(lifted)));
        if (!left.path.empty()) kept.push_back(vec::part_from_json(vec::to_json(left)));
    }
    return kept;
}

}  // namespace

Items lift(core::Layer& layer, const Json& area, const core::Page& page) {
    // (color-model@1: an RGB page of a profile's paint pixels are its values: the picture lifted says so)
    const bool page_values = core::holds_page_values(page, layer);
    // (PC3 B2: a precise layer's own pixels are lifted as a piece that keeps their samples, color_edit::lift_piece; its
    // lines and pictures here)
    AreaTest test(area);
    Items in;
    std::vector<core::StrokePtr> strokes_out;
    for (const core::StrokePtr& stroke : layer.strokes->items) {
        (test.stroke_inside(*stroke) ? in.strokes : strokes_out).push_back(stroke);
    }
    std::vector<core::Patch> patches_out;
    for (const core::Patch& patch : layer.patches) {
        if (!patch.png || patch.png->empty()) {
            patches_out.push_back(patch);
            continue;
        }
        auto [outside, inside] = split_patch(patch, test);
        if (outside) patches_out.push_back(std::move(*outside));
        if (inside) in.patches.push_back(std::move(*inside));
    }
    if (layer.raster_png && !layer.raster_png->empty()) {
        const Image raster = open_picture(*layer.raster_png).convert("RGBA");
        // (PC4 F-b: at the layer's pixels' own resolution on a page of grid "finest" — an opened picture's —, as coverage)
        const int dpi = raster::dpi_across(page, raster.width());
        const AreaMask m = area_mask(area, dpi);
        Image local = Image::create("L", raster.size(), Ink(0));
        local.paste(m.mask, Point{static_cast<int>(m.x0), static_cast<int>(m.y0)});
        const Image raster_alpha = raster.getchannel(3);
        const Image inside_alpha = chops::multiply(raster_alpha, local);
        if (inside_alpha.getbbox()) {
            Image piece = raster.copy();
            piece.putalpha(inside_alpha);
            Image kept = raster.copy();
            kept.putalpha(chops::subtract(raster_alpha, inside_alpha));
            layer.raster_png = fills::png_bytes(kept);
            core::Patch templ;
            templ.attrs["mode"] = "image";
            templ.attrs["opacity"] = 1.0;
            if (page_values) templ.attrs["rgb_profile"] = page.rgb->profile;  // (color_edit::kPatchProfileKey)
            if (auto patch = to_patch(piece, 0, 0, templ, dpi)) in.patches.push_back(std::move(*patch));
        }
    }
    layer.strokes = core::make_strokes(std::move(strokes_out));
    layer.patches = std::move(patches_out);
    if (shape_parts::any(layer)) layer.shapes = core::vector::make_shapes(lift_parts(layer, area, test, in));
    return in;
}

namespace {

// pen-shapes@1 (PC3 C-b): the parts put on the layer (each through `move`): a pen layer's parts, an id it has already
// given a new one; on a layer that is not a pen layer, their pictures as patches.
void drop_parts(core::Layer& layer, std::vector<core::vector::ShapePart> parts, bool fresh_ids, const core::Page* page, const core::Document* book,
                const std::function<core::vector::ShapePart(const core::vector::ShapePart&)>& move) {
    namespace vec = core::vector;
    if (parts.empty()) return;
    std::vector<vec::ShapePart> out = layer.shapes->items;
    std::set<std::string> ids;
    for (const vec::ShapePart& p : out) ids.insert(p.id);
    for (const vec::ShapePart& part : parts) {
        vec::ShapePart moved = vec::part_from_json(vec::to_json(move(part)));
        if (layer.kind != core::LayerKind::Strokes) {
            if (book == nullptr && shape_parts::has_pattern(moved)) {
                throw core::Error("value", "a shape part filled with a pattern is drawn with its book's pattern (the book was not given)");
            }
            if (auto patch = shape_parts::as_patch(page, moved, book)) {
                if (fresh_ids) patch_set(*patch, "id", core::new_id());
                layer.patches.push_back(std::move(*patch));
            }
            continue;
        }
        if (fresh_ids || ids.contains(moved.id)) {
            do {
                moved.id = core::new_id();
            } while (ids.contains(moved.id));
        }
        ids.insert(moved.id);
        out.push_back(std::move(moved));
    }
    if (layer.kind == core::LayerKind::Strokes) {
        if (out.size() > vec::kShapePartsMax) throw core::OpError("a pen layer holds at most " + std::to_string(vec::kShapePartsMax) + " shape parts");
        layer.shapes = vec::make_shapes(std::move(out));
    }
}

}  // namespace

void drop(core::Layer& layer, Items items, const Matrix& m, bool fresh_ids, Resample resample, const core::Page* page, const core::Document* book) {
    const bool identity = m == kIdentity;  // (-0.0 is 0.0 here, as in Python's tuple comparison)
    drop_parts(layer, std::move(items.shapes), fresh_ids, page, book, [&](const core::vector::ShapePart& part) {
        return identity ? part : shape_parts::transformed(part, {m[0], m[1], m[2], m[3], m[4], m[5]}, true);
    });
    std::vector<core::StrokePtr> strokes = layer.strokes->items;
    for (const core::StrokePtr& stroke : items.strokes) {
        if (!identity) {
            core::Stroke moved = transform_stroke(*stroke, m);
            moved.id = fresh_ids ? core::new_id() : stroke->id;
            strokes.push_back(std::make_shared<const core::Stroke>(std::move(moved)));
        } else if (fresh_ids) {
            core::Stroke moved = *stroke;
            moved.id = core::new_id();
            strokes.push_back(std::make_shared<const core::Stroke>(std::move(moved)));
        } else {
            strokes.push_back(stroke);
        }
    }
    layer.strokes = core::make_strokes(std::move(strokes));
    for (core::Patch& patch : items.patches) {
        std::optional<core::Patch> moved = identity ? std::optional<core::Patch>(patch) : transform_patch(patch, m, resample);
        if (!moved) continue;
        if (fresh_ids) patch_set(*moved, "id", core::new_id());
        layer.patches.push_back(std::move(*moved));
    }
}

void drop_warped(core::Layer& layer, Items items, const WarpFunction& go, Resample resample, const core::Page* page, const core::Document* book) {
    drop_parts(layer, std::move(items.shapes), false, page, book, [&](const core::vector::ShapePart& part) {
        core::vector::ShapePart bent = part;
        bent.path = shape_parts::bent(part.path, go);
        bent.live.reset();
        return bent;
    });
    std::vector<core::StrokePtr> strokes = layer.strokes->items;
    for (const core::StrokePtr& stroke : items.strokes) {
        strokes.push_back(std::make_shared<const core::Stroke>(warp::warp_stroke(*stroke, go)));
    }
    layer.strokes = core::make_strokes(std::move(strokes));
    for (const core::Patch& patch : items.patches) {
        const PatchPicture pic = patch_px(patch);
        const auto warped = warp::warp_image(pic.image, pic.x0, pic.y0, go, fills::kFillDpi, 14, resample);
        if (!warped) continue;
        if (auto moved = to_patch(warped->image, warped->x0, warped->y0, patch)) layer.patches.push_back(std::move(*moved));
    }
}

Items items_from_json(const Json& data) {
    if (!data.is_object()) no_get(data);
    Items items;
    for (const Json& s : core::iterate(core::get_or(data, "strokes", Json::array()))) {
        core::Stroke stroke = core::coerce_stroke(s);
        const auto finite = [](double value) {
            if (!std::isfinite(value)) throw core::OpError("stroke must contain only finite numbers");
        };
        for (const auto& p : stroke.points) { finite(p.x); finite(p.y); }
        for (const double value : stroke.pressure) finite(value);
        for (const double value : stroke.rotation) finite(value);
        finite(stroke.width_mm);
        finite(stroke.opacity);
        finite(stroke.pressure_opacity);
        // Validate the full packed arrays, including an unmatched last coordinate discarded by coerce_stroke.
        if (s.is_object() && s.contains("xy")) {
            for (const double value : core::unpack_doubles(s["xy"].get<std::string>())) finite(value);
        }
        limits::check_count(static_cast<double>(stroke.points.size()), static_cast<double>(limits::kPoints), "stroke points");
        items.strokes.push_back(std::make_shared<const core::Stroke>(std::move(stroke)));
    }
    for (const Json& p : core::iterate(core::get_or(data, "patches", Json::array()))) {
        if (!p.is_object()) no_get(p);
        if (!core::truthy_at(p, "png")) continue;
        core::Patch patch;
        patch.png = std::make_shared<const std::string>(b64decode(p["png"]));
        bool after = false;  // (the keys after "asset": a dict keeps them there)
        for (const auto& [key, value] : p.items()) {
            if (key == "png") continue;
            if (key == "asset") {
                after = true;
                patch.asset = value.is_string() ? value.get<std::string>() : core::py_str(value);
                continue;
            }
            (after ? patch.after_asset : patch.attrs)[key] = value;
        }
        items.patches.push_back(std::move(patch));
    }
    // (pen-shapes@1, PC3 C-b: a pen layer's shape parts, as a layer keeps them)
    if (const Json* shapes = core::get(data, "shapes"); shapes != nullptr && !shapes->is_null()) {
        if (!shapes->is_array()) throw core::OpError("items' shapes is a list of shape parts");
        for (const Json& item : *shapes) {
            try {
                items.shapes.push_back(core::vector::part_from_json(item));
            } catch (const core::Error& error) {
                throw core::OpError(std::string("items' shapes: ") + error.what());
            }
        }
    }
    // (patterns@1, PC4 E3: the patterns the parts name, carried from another book — paste puts them into its book)
    if (const Json* patterns = core::get(data, "patterns"); patterns != nullptr && !patterns->is_null()) {
        if (!patterns->is_array()) throw core::OpError("items' patterns is a list of patterns");
        items.patterns = *patterns;
    }
    return items;
}

std::vector<double> area_bbox(const Json& area) {
    if (!area.is_object()) no_get(area);
    if (core::truthy_at(area, "poly")) {
        std::vector<double> xs;
        std::vector<double> ys;
        for (const Json& p : core::iterate(area["poly"])) xs.push_back(core::finite_float(core::subscript(p, 0), "area"));
        for (const Json& p : core::iterate(area["poly"])) ys.push_back(core::finite_float(core::subscript(p, 1), "area"));
        if (xs.empty()) throw core::PyValueError("min() iterable argument is empty");
        double min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
        for (const double x : xs) {
            min_x = core::py_min(min_x, x);
            max_x = core::py_max(max_x, x);
        }
        for (const double y : ys) {
            min_y = core::py_min(min_y, y);
            max_y = core::py_max(max_y, y);
        }
        return {min_x, min_y, max_x - min_x, max_y - min_y};
    }
    std::vector<double> out;
    const Json mask = core::subscript(area, "mask");
    for (const Json& v : core::iterate(core::subscript(mask, "box"))) out.push_back(core::finite_float(v, "area"));
    return out;
}

// --- selops ----------------------------------------------------------------------------------------------------------

namespace {

constexpr int kNesting = 64;  // areas in areas (union, saved …): deeper is refused (Python recurses until it fails)

// What making one area may take in all (selops has no bound: a union of thousands of page-sized masks, or saved areas
// that name others twice over level after level, run out of memory or time): the areas visited, the pixels of the
// masks made, and the pixels of those held at once.
struct Budget {
    std::int64_t areas = 0;
    double work = 0;
    double held = 0;
    void visit(double pixels) {
        if (++areas > limits::kAreaParts) {
            throw core::OpError("the area is made of too many areas (at most " + std::to_string(limits::kAreaParts) + ")");
        }
        spend(pixels);
    }
    void spend(double pixels) {
        work += pixels;
        if (work > limits::kAreaWork) {
            throw core::OpError("the area takes too much work (more than " + core::py_float_repr(limits::kAreaWork) + " pixels of masks)");
        }
    }
};

// A mask held while others are made (the running result of a union, an intersection or a subtraction), counted while
// it is held.
class Held {
public:
    explicit Held(Budget& budget) : budget_(budget) {}
    ~Held() { budget_.held -= pixels_; }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    void hold(double pixels) {
        budget_.held += pixels;
        pixels_ += pixels;
        if (budget_.held > limits::kAreaHeld) {
            throw core::OpError("the area holds too many masks at once (more than " + core::py_float_repr(limits::kAreaHeld) + " pixels)");
        }
    }

private:
    Budget& budget_;
    double pixels_ = 0;
};

Size page_size(const core::Page& page, int dpi) {
    const double w = std::max(1.0, core::py_round_whole(mm2px(page.spec.width_mm.value(), dpi)));
    const double h = std::max(1.0, core::py_round_whole(mm2px(page.spec.height_mm.value(), dpi)));
    limits::check_sides(w, h, "the page");
    return Size{static_cast<int>(w), static_cast<int>(h)};
}

[[noreturn]] void area_error(const std::string& message) { throw core::OpError(message); }

Image to_mask_at(const Json& area, const core::Page& page, const core::Document* episode, int dpi, int depth, Budget& budget);

// selops._grow: grow (the blur reaches) or shrink (only what stays solid)
Image grow(const Image& mask, double amount_px) {
    if (std::fabs(amount_px) < 0.5) return mask;
    const double radius = std::fabs(amount_px);
    limits::check_count(radius, limits::kReach, "grow_mm");
    const Image blurred = mask.filter(Filter::gaussian_blur(radius / 1.5));
    if (amount_px > 0) return blurred.point([](int v) { return v > 12 ? 255 : 0; });
    return blurred.point([](int v) { return v > 243 ? 255 : 0; });
}

// ImageDraw.floodfill(image, xy, value) on an "L" picture with thresh 0: the pixels of the seed's value joined to it
// (4-neighbour) take the value
void floodfill_l(Image& image, int x, int y, int value) {
    const int w = image.width();
    const int h = image.height();
    std::string pixels = image.tobytes();
    const auto at = [&](int px, int py) -> unsigned char& {
        return reinterpret_cast<unsigned char&>(pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px)]);
    };
    const int background = at(x, y);
    if (std::abs(value - background) <= 0) return;
    std::vector<std::pair<int, int>> stack{{x, y}};
    at(x, y) = static_cast<unsigned char>(value);
    while (!stack.empty()) {
        const auto [cx, cy] = stack.back();
        stack.pop_back();
        const std::pair<int, int> next[4] = {{cx + 1, cy}, {cx - 1, cy}, {cx, cy + 1}, {cx, cy - 1}};
        for (const auto& [nx, ny] : next) {
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (at(nx, ny) != background) continue;
            at(nx, ny) = static_cast<unsigned char>(value);
            stack.emplace_back(nx, ny);
        }
    }
    image = Image::frombytes("L", image.size(), pixels);
}

// selops._colour_area: the colour found at a point of the page as seen
Image colour_area(const Json& spec, const core::Page& page, const core::Document* episode, int dpi, Size size) {
    // (PC3 B1: a layer's own colours, an anti-aliased edge, a precise or ink page's values — render/select_color)
    if (spec.is_object() && select_color::own_colour(spec, page)) return select_color::colour(spec, page, episode, size);
    RenderOptions options;
    options.mode = "proof";
    Image image = render_page(page, dpi, options, episode).image.resize(size);
    const double x = core::py_round_whole(mm2px(core::finite_float(core::subscript(spec, "x_mm"), "color"), dpi));
    const double y = core::py_round_whole(mm2px(core::finite_float(core::subscript(spec, "y_mm"), "color"), dpi));
    if (!(0 <= x && x < size.width && 0 <= y && y < size.height)) area_error("the colour point is off the page");
    if (!spec.is_object()) no_get(spec);
    const std::int64_t tolerance = std::max<std::int64_t>(0, std::min<std::int64_t>(255, core::to_int(core::get_or(spec, "tolerance", Json(24)))));
    const std::vector<double> target = image.getpixel(static_cast<int>(x), static_cast<int>(y));
    const std::vector<Image> bands = image.split();
    std::vector<Image> channels;
    for (std::size_t i = 0; i < bands.size() && i < target.size(); ++i) {
        channels.push_back(chops::difference(bands[i], Image::create("L", size, Ink(static_cast<int>(target[i])))));
    }
    const Image worst = chops::lighter(chops::lighter(channels[0], channels[1]), channels[2]);
    Image similar = worst.point([tolerance](int v) { return v <= tolerance ? 255 : 0; });
    if (!core::truthy_at(spec, "contiguous")) return similar;
    // only the similar colour joined to the clicked point
    floodfill_l(similar, static_cast<int>(x), static_cast<int>(y), 128);
    return similar.point([](int v) { return v == 128 ? 255 : 0; });
}

Image base_mask(const Json& area, const core::Page& page, const core::Document* episode, int dpi, Size size, int depth, Budget& budget) {
    if (core::truthy_at(area, "all")) return Image::create("L", size, Ink(255));
    if (core::truthy_at(area, "rect") || core::truthy_at(area, "ellipse") || core::truthy_at(area, "poly")) {
        // (PC3: "antialias": true, the edge grey — an ellipse then of 360 corners)
        const bool smooth = area_kinds::antialiased(area);
        const Json points = core::truthy_at(area, "poly")   ? area["poly"]
                            : core::truthy_at(area, "rect") ? rect_poly(area["rect"])
                                                            : ellipse_poly(area["ellipse"], smooth ? 360 : 72);
        if (core::length(points) < 3) area_error("an area needs at least three points");
        std::vector<PointD> corners;
        for (const auto& [x, y] : poly_points(points, "area")) corners.push_back(PointD{mm2px(x, dpi), mm2px(y, dpi)});
        if (smooth) {
            std::vector<std::pair<double, double>> pixels;
            for (const PointD& p : corners) pixels.emplace_back(p.x, p.y);
            return area_kinds::antialiased_polygon(pixels, size);
        }
        Image mask = Image::create("L", size, Ink(0));
        Draw(mask).polygon(corners, Ink(255));
        return mask;
    }
    if (core::truthy_at(area, "mask")) {
        const AreaMask part = area_mask(Json::object({{"mask", area["mask"]}}), dpi);
        Image mask = Image::create("L", size, Ink(0));
        mask.paste(part.mask, Point{static_cast<int>(part.x0), static_cast<int>(part.y0)});
        return mask;
    }
    if (core::truthy_at(area, "layer")) {
        const Json& wanted = area["layer"];
        const core::Layer* layer = nullptr;
        for (const core::Layer& item : page.layers) {
            if (wanted.is_string() && item.id == wanted.get_ref<const std::string&>()) {
                layer = &item;
                break;
            }
        }
        if (layer == nullptr) area_error("no layer " + core::py_str(wanted));
        const Image alpha = layer_image(page, *layer, dpi, episode).getchannel(3);
        return alpha.resize(size).point([](int v) { return v > 8 ? 255 : 0; });
    }
    if (core::truthy_at(area, "color")) return colour_area(area["color"], page, episode, dpi, size);
    if (core::truthy_at(area, "channel")) {  // (channels@1: a page's alpha or spot channel, its density; this build's own)
        const core::Channel* channel = area["channel"].is_string() ? core::find_channel(page, area["channel"].get<std::string>()) : nullptr;
        if (channel != nullptr) return channels::mask_of(*channel, size);
        // (PC3 B1, 選択範囲を読み込む: a colour plate of the page — its value as the panel shows the plate alone — or the
        // composite, the page's lightness; a channel's id or name comes first)
        if (area["channel"].is_string()) {
            const std::string name = area["channel"].get<std::string>();
            const auto plate = core::plate_index(page, name);
            if (plate || name == "composite") {
                const bool alone = core::plate_names(page).size() == 1;
                const channels::Plane plane = channels::page_plane(page, episode, plate && !alone ? plate : std::nullopt);
                return channels::mask_image(plane, size);
            }
        }
        area_error("no channel " + core::py_str(area["channel"]));
    }
    if (core::truthy_at(area, "saved")) {
        const Json* kept = core::get(page.extra, "saved_areas");
        const Json saved_areas = kept != nullptr && core::py_truthy(*kept) ? *kept : Json::object();
        if (!saved_areas.is_object()) no_get(saved_areas);
        const std::string name = core::py_str(area["saved"]);
        const Json* saved = core::get(saved_areas, name);
        if (saved == nullptr || saved->is_null()) area_error("no saved area " + core::py_str(area["saved"]));
        return to_mask_at(*saved, page, episode, dpi, depth + 1, budget);
    }
    for (const char* key : {"union", "intersect", "subtract"}) {
        if (!area.contains(key)) continue;
        // (each part made and laid on the running result in turn: the result and one part held, not all the parts)
        const Json& given = area[key];
        std::optional<Image> out;
        Held held(budget);
        for (const Json& item : core::iterate(core::py_truthy(given) ? given : Json::array())) {
            Image part = to_mask_at(item, page, episode, dpi, depth + 1, budget);
            if (!out) {
                out = std::move(part);
                held.hold(static_cast<double>(out->width()) * static_cast<double>(out->height()));
                continue;
            }
            budget.spend(static_cast<double>(out->width()) * static_cast<double>(out->height()));
            if (std::string_view(key) == "union") {
                out = chops::lighter(*out, part);
            } else if (std::string_view(key) == "intersect") {
                out = chops::darker(*out, part);
            } else {
                out = chops::subtract(*out, part);
            }
        }
        if (!out) area_error(std::string(key) + " needs areas");
        return std::move(*out);
    }
    // (PC3: the kinds this build adds, render/area_kinds)
    if (const area_kinds::Kind* kind = area_kinds::kind_of(area)) {
        const area_kinds::Context context{page, episode, dpi, size,
                                          [&](const Json& inner) { return to_mask_at(inner, page, episode, dpi, depth + 1, budget); }};
        return area_kinds::solve(*kind, area, context);
    }
    area_error("an area is poly, mask, rect, ellipse, layer, color, all, saved, union, intersect or subtract");
}

Image to_mask_at(const Json& area, const core::Page& page, const core::Document* episode, int dpi, int depth, Budget& budget) {
    if (depth > kNesting) throw core::OpError("the area is nested too deeply (saved areas in a loop?)");
    if (!area.is_object()) no_get(area);
    const Size size = page_size(page, dpi);
    budget.visit(static_cast<double>(size.width) * static_cast<double>(size.height));
    Image mask = base_mask(area, page, episode, dpi, size, depth, budget);
    // (PC3, render/area_kinds: the kind → matrix → grow_mm → expand_mm, contract_mm → border_mm → smooth_mm → invert →
    // feather_mm; an area without the new modifiers as before)
    const area_kinds::Context context{page, episode, dpi, size,
                                      [&](const Json& inner) { return to_mask_at(inner, page, episode, dpi, depth + 1, budget); }};
    mask = area_kinds::before_grow(std::move(mask), area, context);
    if (core::truthy_at(area, "grow_mm")) mask = grow(mask, mm2px(core::finite_float(area["grow_mm"], "grow_mm"), dpi));
    mask = area_kinds::after_grow(std::move(mask), area, context);
    if (core::truthy_at(area, "invert")) mask = chops::invert(mask);
    if (core::truthy_at(area, "feather_mm")) {
        const double radius = core::py_max(0.1, mm2px(core::finite_float(area["feather_mm"], "feather_mm"), dpi) / 2);
        limits::check_count(radius, limits::kReach, "feather_mm");
        mask = mask.filter(Filter::gaussian_blur(radius));
    }
    return mask;
}

bool only_key(const Json& area, std::string_view key) {
    for (const auto& [k, v] : area.items()) {
        if (k != key) return false;
    }
    return true;
}

}  // namespace

int grid_dpi(const core::Page& page) {
    const bool precise = page.rgb && page.rgb->depth > 8;
    const bool finest = page.spec.grid && *page.spec.grid == "finest";  // (PC4 F-b, image-grid@1)
    return page.color || precise || finest ? layer_masks::grid_dpi(page) : kSelDpi;
}

Image to_mask(const Json& area, const core::Page& page, const core::Document* episode, int dpi) {
    ImageAllocationBudget live(static_cast<std::uint64_t>(limits::kAreaHeld));
    Budget budget;
    return to_mask_at(area, page, episode, dpi, 0, budget);
}

Json rect_poly(const Json& box) {
    const std::vector<double> v = core::unpack_floats(box, 4);
    for (const double x : v) {
        if (!std::isfinite(x)) throw core::OpError("area must be a finite number");
    }
    const double x = v[0], y = v[1], w = v[2], h = v[3];
    return Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + h}), Json::array({x, y + h})});
}

Json ellipse_poly(const Json& box, int n) {
    const std::vector<double> v = core::unpack_floats(box, 4);
    for (const double x : v) {
        if (!std::isfinite(x)) throw core::OpError("area must be a finite number");
    }
    const double cx = v[0] + v[2] / 2;
    const double cy = v[1] + v[3] / 2;
    const double rx = v[2] / 2;
    const double ry = v[3] / 2;
    constexpr double kTau = 6.283185307179586;  // math.tau
    Json out = Json::array();
    for (int k = 0; k < n; ++k) {
        const double angle = kTau * k / n;
        out.push_back(Json::array({core::py_round(cx + rx * core::py_cos(angle), 3), core::py_round(cy + ry * core::py_sin(angle), 3)}));
    }
    return out;
}

std::optional<Json> from_mask(const Image& mask, int dpi) {
    const auto box = mask.getbbox();
    if (!box) return std::nullopt;
    const Image crop = mask.crop(*box);
    const double mm = 25.4 / dpi;
    Json spec = Json::object();
    spec["box"] = Json::array({core::py_round(box->x0 * mm, 3), core::py_round(box->y0 * mm, 3), core::py_round(crop.width() * mm, 3),
                               core::py_round(crop.height() * mm, 3)});
    spec["png"] = core::b64encode(fills::png_data(crop));
    const Json out = Json::object({{"mask", std::move(spec)}});
    return std::make_optional<Json>(out);
}

std::optional<Json> combine(const std::optional<Json>& current, const Json& area, std::string_view how, const core::Page& page,
                            const core::Document* episode) {
    if (!current || how == "replace") {
        const bool plain = area.is_object() && (core::py_truthy(area.value("poly", Json())) || core::py_truthy(area.value("mask", Json())));
        return plain ? area : resolve(area, page, episode);
    }
    const std::string key = how == "add" ? "union" : how == "subtract" ? "subtract" : how == "intersect" ? "intersect" : "";
    if (key.empty()) throw core::OpError("how must be replace, add, subtract or intersect");
    const int dpi = grid_dpi(page);
    return from_mask(to_mask(Json{{key, Json::array({*current, area})}}, page, episode, dpi), dpi);
}

std::optional<Json> stroke_area(const std::vector<std::array<double, 2>>& points, double width_mm) {
    if (points.empty()) return std::nullopt;
    double x_lo = points[0][0], x_hi = points[0][0], y_lo = points[0][1], y_hi = points[0][1];
    for (const auto& [x, y] : points) {
        x_lo = std::min(x_lo, x);
        x_hi = std::max(x_hi, x);
        y_lo = std::min(y_lo, y);
        y_hi = std::max(y_hi, y);
    }
    const double pad = width_mm;
    const double x0 = x_lo - pad, y0 = y_lo - pad;
    const double w = x_hi - x_lo + 2 * pad, h = y_hi - y_lo + 2 * pad;
    const double side_w = std::max(1.0, core::py_round_whole(mm2px(w, kSelDpi)));
    const double side_h = std::max(1.0, core::py_round_whole(mm2px(h, kSelDpi)));
    limits::check_sides(side_w, side_h, "the selection pen's line");
    Image mask = Image::create("L", Size{static_cast<int>(side_w), static_cast<int>(side_h)}, Ink(0));
    std::vector<PointD> pts;
    pts.reserve(points.size());
    for (const auto& [x, y] : points) pts.push_back(PointD{mm2px(x - x0, kSelDpi), mm2px(y - y0, kSelDpi)});
    const double r = std::max(1.0, mm2px(width_mm, kSelDpi) / 2);
    {
        Draw draw(mask);
        if (pts.size() > 1) draw.line(pts, Ink(255), std::max(1, static_cast<int>(core::py_round_whole(2 * r))), Joint::Curve);
        for (const PointD& end : {pts.front(), pts.back()}) draw.ellipse(BoxF{end.x - r, end.y - r, end.x + r, end.y + r}, Ink(255));
    }
    Json spec = Json::object();
    spec["box"] = Json::array({core::py_round(x0, 3), core::py_round(y0, 3), core::py_round(w, 3), core::py_round(h, 3)});
    spec["png"] = core::b64encode(fills::png_data(mask));
    return Json::object({{"mask", std::move(spec)}});
}

Json items_to_json(const Items& items) {
    Json strokes = Json::array();
    for (const core::StrokePtr& stroke : items.strokes) strokes.push_back(core::stroke_to_dict(*stroke));
    Json patches = Json::array();
    for (const core::Patch& patch : items.patches) {
        if (!patch.png || patch.png->empty()) continue;
        Json out = patch.attrs;  // (its keys but png and asset, in order; then the png)
        for (const auto& [key, value] : patch.after_asset.items()) out[key] = value;
        out.erase("png");
        out.erase("asset");
        out["png"] = core::b64encode(*patch.png);
        patches.push_back(std::move(out));
    }
    Json out{{"strokes", std::move(strokes)}, {"patches", std::move(patches)}};
    if (!items.shapes.empty()) {  // (pen-shapes@1, PC3 C-b)
        Json shapes = Json::array();
        for (const core::vector::ShapePart& part : items.shapes) shapes.push_back(core::vector::to_json(part));
        out["shapes"] = std::move(shapes);
    }
    if (items.patterns.is_array() && !items.patterns.empty()) out["patterns"] = items.patterns;  // (patterns@1, PC4 E3)
    return out;
}

Json resolve(const Json& area, const core::Page& page, const core::Document* episode) {
    if (only_key(area, "rect")) return Json::object({{"poly", rect_poly(area["rect"])}});
    if (only_key(area, "ellipse")) return Json::object({{"poly", ellipse_poly(area["ellipse"])}});
    const int dpi = grid_dpi(page);
    auto out = from_mask(to_mask(area, page, episode, dpi), dpi);
    if (!out) area_error("the area is empty");
    return std::move(*out);
}

}  // namespace genko::render::selection
