// Python's genko/fill.py (px, region, dilate, region_mask, mask_patch, polygon_patch) and selection.area_mask.

#include "render/fill_patches.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/limits.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/draw.hpp"
#include "render/png.hpp"

namespace genko::render::tone_fills {

namespace {

using core::Json;
using core::py_max;
using core::py_min;

constexpr std::int64_t kMaxPatchPixels = core::limits::kPatchPixels;

int to_int_px(double v) {
    const std::int64_t n = core::py_trunc_held(v);
    if (n > core::limits::kFillCoordinate || n < -core::limits::kFillCoordinate) throw core::Error("value", "the area is far too large");
    return static_cast<int>(n);
}

// fill.dilate: True pixels grown by r (a square), one axis and then the other.
std::vector<char> dilate(const std::vector<char>& arr, int w, int h, int r) {
    if (r <= 0) return arr;
    std::vector<char> out = arr;
    for (int axis = 0; axis < 2; ++axis) {
        std::vector<char> grown = out;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]) continue;
                for (int k = -r; k <= r; ++k) {
                    const int yy = axis == 0 ? y + k : y;
                    const int xx = axis == 0 ? x : x + k;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    grown[static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)] = 1;
                }
            }
        }
        out = std::move(grown);
    }
    return out;
}

// fill.region: the pixels connected to `seed` through free pixels (4-neighbour).
std::vector<char> region(const std::vector<char>& free, int w, int h, Point seed) {
    std::vector<char> filled(free.size(), 0);
    const auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    if (seed.x < 0 || seed.x >= w || seed.y < 0 || seed.y >= h || !free[at(seed.x, seed.y)]) return filled;
    std::vector<Point> stack{seed};
    while (!stack.empty()) {
        const Point p = stack.back();
        stack.pop_back();
        if (filled[at(p.x, p.y)] || !free[at(p.x, p.y)]) continue;
        int left = p.x;
        while (left > 0 && free[at(left - 1, p.y)]) --left;
        int right = p.x;
        while (right + 1 < w && free[at(right + 1, p.y)]) ++right;
        for (int x = left; x <= right; ++x) filled[at(x, p.y)] = 1;
        for (const int ny : {p.y - 1, p.y + 1}) {
            if (ny < 0 || ny >= h) continue;
            bool open = false;
            for (int x = left; x <= right; ++x) {
                const bool now = free[at(x, ny)] && !filled[at(x, ny)];
                if (now && !open) stack.push_back(Point{x, ny});
                open = now;
            }
        }
    }
    return filled;
}

}  // namespace

int px(double mm, int dpi) { return to_int_px(std::nearbyint(mm / 25.4 * dpi)); }

std::optional<core::Patch> mask_patch(const Image& mask, int dpi, const std::vector<std::int64_t>& rgb, double opacity,
                                      Point offset_px) {
    const auto box = mask.getbbox();
    if (!box) return std::nullopt;
    const Image crop = mask.crop(*box);
    const double mm = 25.4 / dpi;
    const int x0 = box->x0 + offset_px.x;
    const int y0 = box->y0 + offset_px.y;
    core::Patch patch;
    patch.attrs = Json::object();
    patch.attrs["id"] = core::new_id();
    patch.attrs["box"] = Json::array({core::py_round(x0 * mm, 3), core::py_round(y0 * mm, 3), core::py_round(crop.width() * mm, 3),
                                      core::py_round(crop.height() * mm, 3)});
    patch.attrs["mode"] = "mask";
    Json colour = Json::array();
    for (const auto v : rgb) colour.push_back(v);
    patch.attrs["rgb"] = std::move(colour);
    patch.attrs["opacity"] = opacity;
    patch.png = std::make_shared<const std::string>(write_png(crop));
    return patch;
}

std::optional<core::Patch> polygon_patch(const std::vector<std::array<double, 2>>& points_mm, const std::vector<std::int64_t>& rgb,
                                         double opacity, int dpi) {
    if (points_mm.size() < 3) return std::nullopt;
    double min_x = points_mm.front()[0], min_y = points_mm.front()[1], max_x = min_x, max_y = min_y;
    for (const auto& p : points_mm) {
        min_x = py_min(min_x, p[0]);
        min_y = py_min(min_y, p[1]);
        max_x = py_max(max_x, p[0]);
        max_y = py_max(max_y, p[1]);
    }
    const int x0 = px(min_x, dpi);
    const int y0 = px(min_y, dpi);
    const std::int64_t w = static_cast<std::int64_t>(px(max_x, dpi)) - x0 + 2;
    const std::int64_t h = static_cast<std::int64_t>(px(max_y, dpi)) - y0 + 2;
    if (w < 2 || h < 2) return std::nullopt;
    if (w * h > kMaxPatchPixels) throw core::Error("value", "the area is too large to fill");
    Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
    std::vector<PointD> pts;
    for (const auto& p : points_mm) {
        pts.push_back(PointD{static_cast<double>(px(p[0], dpi) - x0), static_cast<double>(px(p[1], dpi) - y0)});
    }
    Draw(mask).polygon(pts, Ink(255));
    return mask_patch(mask, dpi, rgb, opacity, Point{x0, y0});
}

std::optional<Image> region_mask(const Image& reference, Point at, int gap_px, int threshold, int expand_px,
                                 std::optional<Box> window) {
    const Image grey = reference.mode() == "L" ? reference : reference.convert("L");
    const Box win = window.value_or(Box{0, 0, grey.width(), grey.height()});
    const Image crop = grey.crop(win);
    const int w = crop.width();
    const int h = crop.height();
    if (w <= 0 || h <= 0) core::raise_index_error("index 0 is out of bounds for axis 0 with size 0");
    const std::string pixels = crop.tobytes();
    std::vector<char> walls(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) walls[i] = static_cast<unsigned char>(pixels[i]) < threshold ? 1 : 0;
    // the window's edge is a wall too (a fill never leaves its panel's box)
    for (int x = 0; x < w; ++x) {
        walls[static_cast<std::size_t>(x)] = 1;
        walls[static_cast<std::size_t>(h - 1) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 1;
    }
    for (int y = 0; y < h; ++y) {
        walls[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)] = 1;
        walls[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(w - 1)] = 1;
    }
    std::vector<char> free = dilate(walls, w, h, gap_px);
    for (auto& v : free) v = v ? 0 : 1;
    std::vector<char> filled = region(free, w, h, Point{at.x - win.x0, at.y - win.y0});
    if (std::none_of(filled.begin(), filled.end(), [](char v) { return v != 0; })) return std::nullopt;
    filled = dilate(filled, w, h, expand_px + gap_px);  // under the lines (and into the closed gaps)
    std::string data(filled.size(), '\0');
    for (std::size_t i = 0; i < filled.size(); ++i) data[i] = filled[i] ? static_cast<char>(255) : '\0';
    Image page = Image::create("L", grey.size(), Ink(0));
    page.paste(Image::frombytes("L", Size{w, h}, data), Point{win.x0, win.y0});
    return page;
}

std::pair<Image, Point> area_mask(const Json& area, int dpi) {
    const auto mm2px = [&](double v) { return v / 25.4 * dpi; };
    const Json poly = core::py_get(area, "poly");
    if (core::py_truthy(poly)) {
        std::vector<std::array<double, 2>> pts;
        for (const Json& item : core::iterate(poly)) {
            const std::vector<Json> pair = core::iterate(item);
            if (pair.size() != 2) {
                throw core::PyValueError(pair.size() > 2 ? "too many values to unpack (expected 2)"
                                                         : "not enough values to unpack (expected 2, got " + std::to_string(pair.size()) + ")");
            }
            pts.push_back({mm2px(core::to_float(pair[0])), mm2px(core::to_float(pair[1]))});
        }
        double min_x = pts.front()[0], min_y = pts.front()[1], max_x = min_x, max_y = min_y;
        for (const auto& p : pts) {
            min_x = py_min(min_x, p[0]);
            min_y = py_min(min_y, p[1]);
            max_x = py_max(max_x, p[0]);
            max_y = py_max(max_y, p[1]);
        }
        const int x0 = to_int_px(min_x) - 1;
        const int y0 = to_int_px(min_y) - 1;
        const int x1 = to_int_px(max_x) + 2;
        const int y1 = to_int_px(max_y) + 2;
        if (static_cast<std::int64_t>(std::max(1, x1 - x0)) * std::max(1, y1 - y0) > kMaxPatchPixels) {
            throw core::Error("value", "the area is too large");
        }
        Image mask = Image::create("L", Size{std::max(1, x1 - x0), std::max(1, y1 - y0)}, Ink(0));
        std::vector<PointD> local;
        for (const auto& p : pts) local.push_back(PointD{p[0] - x0, p[1] - y0});
        Draw(mask).polygon(local, Ink(255));
        return {mask, Point{x0, y0}};
    }
    const Json spec = core::py_or(core::py_get(area, "mask"), Json::object());
    if (!spec.is_object() || !spec.contains("box")) throw core::OpKeyError("'box'");
    const std::vector<Json> box = core::iterate(spec["box"]);
    if (box.size() != 4) {
        throw core::PyValueError(box.size() > 4 ? "too many values to unpack (expected 4)"
                                                : "not enough values to unpack (expected 4, got " + std::to_string(box.size()) + ")");
    }
    const double x = core::to_float(box[0]), y = core::to_float(box[1]), w = core::to_float(box[2]), h = core::to_float(box[3]);
    if (!spec.contains("png")) throw core::OpKeyError("'png'");
    const Json& png = spec["png"];
    if (!png.is_string()) throw core::PyTypeError("argument should be a bytes-like object or ASCII string, not '" + core::py_type_name(png) + "'");
    std::string data;
    try {
        data = core::a2b_base64(png.get_ref<const std::string&>());
    } catch (const core::Error& error) {
        throw core::PyValueError(error.what());
    }
    const Image image = open_image(data, kPillowOpenLimits).convert("L");
    const int pw = std::max(1, to_int_px(std::nearbyint(mm2px(w))));
    const int ph = std::max(1, to_int_px(std::nearbyint(mm2px(h))));
    if (static_cast<std::int64_t>(pw) * ph > kMaxPatchPixels) throw core::Error("value", "the area is too large");
    return {image.resize(Size{pw, ph}, Resample::Nearest),
            Point{to_int_px(std::nearbyint(mm2px(x))), to_int_px(std::nearbyint(mm2px(y)))}};
}

}  // namespace genko::render::tone_fills
