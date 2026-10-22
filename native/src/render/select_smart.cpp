#include "render/select_smart.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/image_ops.hpp"
#include "render/matting.hpp"
#include "render/op_limits.hpp"
#include "render/segment/segment.hpp"
#include "render/selection_ext.hpp"

// PhotoCraft v0.2.0 crates/engine/src/smartselect_cmds.rs (lines 30-221) and select_extra_cmds.rs (lines 49-77), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::select_smart {

using core::Json;
using core::OpError;
using segment::Rect;
using segment::RgbImage;

namespace {

const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto it = value.find(key);
    return it == value.end() || it->is_null() ? nullptr : &*it;
}

double number(const Json& value, const char* key, double fallback, const std::string& what) {
    const Json* v = field(value, key);
    if (v == nullptr) return fallback;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(what + "." + key + " must be a finite number");
    return v->get<double>();
}

bool flag(const Json& value, const char* key, bool fallback, const std::string& what) {
    const Json* v = field(value, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(what + "." + key + " must be true or false");
    return v->get<bool>();
}

// The settings a kind takes: an object of those keys (`true` for none, where a kind needs none).
void only(const Json& value, const std::string& what, std::initializer_list<const char*> keys, bool bare) {
    if (bare && value.is_boolean() && value.get<bool>()) return;
    if (!value.is_object()) throw OpError(what + " must be an object of its settings");
    for (const auto& [key, v] : value.items()) {
        if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return key == k; })) throw OpError(what + " has no setting " + core::py_repr_str(key));
    }
}

const core::Layer* layer_of(const Json& value, const core::Page& page, const std::string& what) {
    const Json* v = field(value, "layer");
    if (v == nullptr) return nullptr;
    if (v->is_string()) {
        for (const core::Layer& layer : page.layers)
            if (layer.id == v->get_ref<const std::string&>()) return &layer;
    }
    throw OpError(what + ": no layer " + core::py_str(*v));
}

double px_of(double mm, int dpi) { return mm / 25.4 * dpi; }

// The colours read: the layer's own or the page as it shows, as RGB over mid grey on their own grid.
RgbImage source_of(const core::Document& doc, const core::Page& page, const core::Layer* layer) {
    const image_ops::Pixels p = layer != nullptr ? image_ops::shown_layer_pixels(doc, page, *layer) : image_ops::page_pixels(doc, page);
    const image_ops::Held held(std::uint64_t(p.size()) * sizeof(segment::Rgb));
    RgbImage out(p.width(), p.height());
    const auto put = [&out](std::size_t i, const float* rgba) {
        const float a = std::isfinite(rgba[3]) ? std::clamp(rgba[3], 0.0f, 1.0f) : 0.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            const float v = std::isfinite(rgba[c]) ? std::clamp(rgba[c], 0.0f, 1.0f) : 0.0f;
            out.px[i][c] = v * a + 0.5f * (1.0f - a);
        }
    };
    if (p.kind() == image_ops::Pixels::Kind::Inks) {
        const image_ops::Held meaning(std::uint64_t(p.size()) * sizeof(image_ops::Rgba));
        const std::vector<image_ops::Rgba> rgba = image_ops::rgba_of(p);
        for (std::size_t i = 0; i < rgba.size(); ++i) put(i, rgba[i].data());
    } else {
        for (std::size_t i = 0; i < p.size(); ++i) put(i, p.at(i));
    }
    return out;
}

// The colours on the area's grid (`grid` pixels over the page): the source stretched to it bilinear at the pixels'
// centres (the same pixels when it is that size), read when first asked.
class GridSampler : public segment::Sampler {
public:
    GridSampler(std::function<RgbImage()> read, Size grid) : read_(std::move(read)), grid_(grid) {}

    RgbImage rgb(const Rect& r) const override {
        const RgbImage& s = source();
        RgbImage out(static_cast<std::size_t>(std::max(0, r.width())), static_cast<std::size_t>(std::max(0, r.height())));
        const bool same = s.w == static_cast<std::size_t>(grid_.width) && s.h == static_cast<std::size_t>(grid_.height);
        const double kx = static_cast<double>(s.w) / grid_.width, ky = static_cast<double>(s.h) / grid_.height;
        std::size_t i = 0;
        for (int y = r.y0; y < r.y1; ++y) {
            const bool row_in = y >= 0 && y < grid_.height;
            const double fy = std::clamp((y + 0.5) * ky - 0.5, 0.0, static_cast<double>(s.h - 1));
            const auto y0 = static_cast<std::size_t>(fy);
            const std::size_t y1 = std::min(y0 + 1, s.h - 1);
            const auto ty = static_cast<float>(fy - static_cast<double>(y0));
            for (int x = r.x0; x < r.x1; ++x, ++i) {
                if (!row_in || x < 0 || x >= grid_.width) {
                    out.px[i] = segment::Rgb{0.5f, 0.5f, 0.5f};
                    continue;
                }
                if (same) {
                    out.px[i] = s.at(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
                    continue;
                }
                const double fx = std::clamp((x + 0.5) * kx - 0.5, 0.0, static_cast<double>(s.w - 1));
                const auto x0 = static_cast<std::size_t>(fx);
                const std::size_t x1 = std::min(x0 + 1, s.w - 1);
                const auto tx = static_cast<float>(fx - static_cast<double>(x0));
                const segment::Rgb &a = s.at(x0, y0), &b = s.at(x1, y0), &c = s.at(x0, y1), &d = s.at(x1, y1);
                for (std::size_t k = 0; k < 3; ++k) {
                    const float top = a[k] + (b[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
                    out.px[i][k] = top + (bot - top) * ty;
                }
            }
        }
        return out;
    }

private:
    const RgbImage& source() const {
        if (!source_) source_ = std::make_unique<RgbImage>(read_());
        return *source_;
    }

    std::function<RgbImage()> read_;
    Size grid_;
    mutable std::unique_ptr<RgbImage> source_;
};

struct Work {
    GridSampler sampler;
    Rect canvas;
};

Work work_of(const Json& value, const area_kinds::Context& c, const std::string& what) {
    if (c.episode == nullptr) throw OpError(what + " needs the book to read its pixels");
    const core::Layer* layer = layer_of(value, c.page, what);
    const core::Document& doc = *c.episode;
    const core::Page& page = c.page;
    return Work{GridSampler([&doc, &page, layer] { return source_of(doc, page, layer); }, c.size), Rect{0, 0, c.size.width, c.size.height}};
}

// A region found as the kind's mask on the grid (none: nothing).
Image mask_of(const std::optional<segment::Region>& region, Size size) {
    if (!region) return Image::create("L", size, Ink(0));
    return selection_ext::region_image(*region, size);
}

}  // namespace

Image subject(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "subject", {"layer"}, true);
    Work w = work_of(value, c, "subject");
    return mask_of(segment::select_subject(w.sampler, w.canvas), c.size);
}

Image sky(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "sky", {"threshold", "softness", "layer"}, true);
    segment::SkyParams p;
    p.threshold = static_cast<float>(std::clamp(number(value, "threshold", 50.0, "sky"), 0.0, 100.0));
    p.softness = static_cast<float>(std::clamp(number(value, "softness", 50.0, "sky"), 0.0, 100.0));
    Work w = work_of(value, c, "sky");
    return mask_of(segment::select_sky(w.sampler, w.canvas, p), c.size);
}

Image object(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "object", {"rect_mm", "layer"}, false);
    const Json* rect = field(value, "rect_mm");
    const auto bad = [] { return OpError("object.rect_mm must be [x, y, w, h] (mm)"); };
    if (rect == nullptr || !rect->is_array() || rect->size() != 4) throw bad();
    double v[4];
    for (std::size_t i = 0; i < 4; ++i) {
        if (!(*rect)[i].is_number() || (*rect)[i].is_boolean() || !std::isfinite((*rect)[i].get<double>())) throw bad();
        v[i] = px_of((*rect)[i].get<double>(), c.dpi);
    }
    // (a size dragged up or left: the same box)
    const double x0 = std::min(v[0], v[0] + v[2]), x1 = std::max(v[0], v[0] + v[2]);
    const double y0 = std::min(v[1], v[1] + v[3]), y1 = std::max(v[1], v[1] + v[3]);
    for (const double e : {x0, x1, y0, y1}) limits::check_coordinate(e, "object.rect_mm");
    const Rect box{static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(x1)), static_cast<int>(std::ceil(y1))};
    Work w = work_of(value, c, "object");
    return mask_of(segment::object_select(w.sampler, w.canvas, box, segment::kQuickWorkPx), c.size);
}

Image quick(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "quick", {"points_mm", "size_mm", "enhance_edge", "layer"}, false);
    const Json* points = field(value, "points_mm");
    const auto bad = [] { return OpError("quick.points_mm must be [[x, y], …] (mm), at least one"); };
    if (points == nullptr || !points->is_array() || points->empty()) throw bad();
    limits::check_count(static_cast<double>(points->size()), static_cast<double>(limits::kPoints), "quick.points_mm");
    std::vector<std::pair<float, float>> stroke;
    stroke.reserve(points->size());
    for (const Json& p : *points) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() || !std::isfinite(p[0].get<double>()) || !std::isfinite(p[1].get<double>()))
            throw bad();
        const double x = px_of(p[0].get<double>(), c.dpi), y = px_of(p[1].get<double>(), c.dpi);
        limits::check_coordinate(x, "quick.points_mm");
        limits::check_coordinate(y, "quick.points_mm");
        stroke.emplace_back(static_cast<float>(x), static_cast<float>(y));
    }
    const double size_mm = number(value, "size_mm", 4.0, "quick");
    if (size_mm <= 0) throw OpError("quick.size_mm must be more than 0");
    const auto size = static_cast<float>(std::clamp(px_of(size_mm, c.dpi), 1.0, 5000.0));
    const bool enhance = flag(value, "enhance_edge", false, "quick");
    Work w = work_of(value, c, "quick");
    std::optional<segment::Region> found = segment::quick_select(w.sampler, w.canvas, stroke, size, segment::kQuickWorkPx);
    if (found && enhance) {
        // (Auto-Enhance: the edge flowed to the picture's)
        const segment::Region grown = *found;
        const matting::MaskReader read = [&grown](const Rect& r) {
            std::vector<float> v;
            v.reserve(static_cast<std::size_t>(r.width()) * static_cast<std::size_t>(r.height()));
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0; x < r.x1; ++x) v.push_back(grown.at(x, y));
            return v;
        };
        found = matting::refine_mask(w.sampler, read, grown.box, w.canvas, matting::enhance_params());
    }
    return mask_of(found, c.size);
}

Image focus(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "focus", {"range", "noise", "layer"}, true);
    const auto range = static_cast<float>(std::clamp(number(value, "range", 0.5, "focus"), 0.0, 1.0));
    const auto noise = static_cast<float>(std::clamp(number(value, "noise", 0.0, "focus"), 0.0, 1.0));
    Work w = work_of(value, c, "focus");
    return mask_of(segment::focus_area(w.sampler, w.canvas, range, noise), c.size);
}

Image refine(const Json& value, const Json&, const area_kinds::Context& c) {
    only(value, "refine", {"area", "radius_mm", "smart_radius", "smooth", "feather_mm", "contrast", "shift_edge", "layer"}, false);
    const Json* inner = field(value, "area");
    if (inner == nullptr) throw OpError("refine.area (the selection to refine) is required");
    const double radius_mm = number(value, "radius_mm", 0.0, "refine");
    const double feather_mm = number(value, "feather_mm", 0.0, "refine");
    if (radius_mm < 0 || feather_mm < 0) throw OpError("refine: radius_mm and feather_mm must not be negative");
    matting::RefineParams p;
    p.radius = static_cast<float>(std::min(px_of(radius_mm, c.dpi), 250.0));
    p.smart_radius = flag(value, "smart_radius", false, "refine");
    p.smooth = static_cast<float>(std::clamp(number(value, "smooth", 0.0, "refine"), 0.0, 100.0));
    p.feather = static_cast<float>(std::min(px_of(feather_mm, c.dpi), 1000.0));
    p.contrast = static_cast<float>(std::clamp(number(value, "contrast", 0.0, "refine"), 0.0, 100.0));
    p.shift_edge = static_cast<float>(std::clamp(number(value, "shift_edge", 0.0, "refine"), -100.0, 100.0));
    const Image chosen = c.nested(*inner);
    const auto w = static_cast<std::size_t>(c.size.width), h = static_cast<std::size_t>(c.size.height);
    const image_ops::Held held(std::uint64_t(w) * h * sizeof(float));
    const std::vector<float> m = selection_ext::from_l(chosen.size() == c.size ? chosen : chosen.resize(c.size, Resample::Bilinear));
    const auto box = chosen.getbbox();
    if (!box) return Image::create("L", c.size, Ink(0));
    const Rect content{box->x0, box->y0, box->x1, box->y1};
    Work work = work_of(value, c, "refine");
    return mask_of(matting::refine_mask(work.sampler, matting::buffer_reader(m, w, h), content, work.canvas, p), c.size);
}

}  // namespace genko::render::select_smart
