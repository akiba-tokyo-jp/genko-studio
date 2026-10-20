#include "render/area_kinds.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/path_areas.hpp"
#include "render/png.hpp"
#include "render/select_color.hpp"
#include "render/select_smart.hpp"
#include "render/selection.hpp"
#include "render/selection_ext.hpp"

namespace genko::render::area_kinds {

using core::Json;

namespace {

double mm2px(double v, int dpi) { return v / 25.4 * dpi; }

double finite(const Json& value, std::string_view what) {
    if (!value.is_number() || value.is_boolean() || !std::isfinite(value.get<double>())) {
        throw core::OpError(std::string(what) + " must be a finite number");
    }
    return value.get<double>();
}

const core::Layer& layer_named(const core::Page& page, const Json& wanted, std::string_view kind) {
    if (wanted.is_string()) {
        for (const core::Layer& layer : page.layers)
            if (layer.id == wanted.get_ref<const std::string&>()) return layer;
    }
    throw core::OpError(std::string(kind) + ": no layer " + core::py_str(wanted));
}

// {"transparency": id}: the layer's alpha, grey as it is (ピクセルを選択; the kind "layer" is its alpha past 8, two-valued).
// Its own pixels' alpha: its layer mask and vector mask not applied, as Photoshop's transparency of a layer.
Image transparency(const Json& value, const Json&, const Context& c) {
    core::Layer bare = layer_named(c.page, value, "transparency");
    bare.mask.reset();
    bare.vector_mask.reset();
    return layer_image(c.page, bare, c.dpi, c.episode).getchannel(3).resize(c.size, Resample::Bilinear);
}

// {"layer_mask": id}: the layer's mask, its density applied (1 − density·(1 − m))
Image layer_mask(const Json& value, const Json&, const Context& c) {
    const core::Layer& layer = layer_named(c.page, value, "layer_mask");
    if (!layer.mask || !layer.mask->png || layer.mask->png->empty()) throw core::OpError("layer_mask: the layer " + layer.id + " has no mask");
    Image mask = open_image(*layer.mask->png, kPillowOpenLimits).convert("L").resize(c.size, Resample::Bilinear);
    const double density = std::clamp(layer.mask->density, 0.0, 1.0);
    if (density < 1.0) {
        mask = mask.point([density](int v) { return static_cast<int>(std::lround(255.0 - density * (255.0 - v))); });
    }
    return mask;
}

// (the kinds, in the order an area's keys are looked at; a row's solve is its group's, null until then)
const std::vector<Kind>& table() {
    static const std::vector<Kind> all{
        {"transparency", "B-0", &transparency},
        {"layer_mask", "B-0", &layer_mask},
        // B1 (選択の基本): Color Range, Grow, Similar (render/select_color.cpp)
        {"range", "B1", &select_color::range},
        {"grow", "B1", &select_color::grow},
        {"similar", "B1", &select_color::similar},
        // B3 (賢い選択・境界の調整): render/select_smart over render/segment and render/matting
        {"subject", "B3", &select_smart::subject},
        {"sky", "B3", &select_smart::sky},
        {"object", "B3", &select_smart::object},
        {"quick", "B3", &select_smart::quick},
        {"focus", "B3", &select_smart::focus},
        {"refine", "B3", &select_smart::refine},
        // C-a (パス): a path's coverage (render/path_areas)
        {"path", "C-a", &path_areas::path},
        {"path_name", "C-a", &path_areas::path_name},
        {"shape_part", "C-a", &path_areas::shape_part},
        {"vector_mask", "C-a", &path_areas::vector_mask},
    };
    return all;
}

// The modifiers past grow_mm, in their order, each a distance in mm worked by the distance transform (or the box).
constexpr std::string_view kAfterGrow[] = {"expand_mm", "contract_mm", "border_mm", "smooth_mm"};

std::vector<float> as_floats(const Image& mask) { return selection_ext::from_l(mask); }

}  // namespace

const std::vector<Kind>& kinds() { return table(); }

const Kind* kind_of(const Json& area) {
    if (!area.is_object()) return nullptr;
    for (const Kind& kind : table()) {
        if (area.contains(std::string(kind.key))) return &kind;
    }
    return nullptr;
}

Image solve(const Kind& kind, const Json& area, const Context& context) {
    if (kind.solve == nullptr) {
        core::not_yet_ported("the area kind " + std::string(kind.key) + " is not in the C++ build yet (PC3 " + std::string(kind.group) + ")");
    }
    return kind.solve(area.at(std::string(kind.key)), area, context);
}

Image before_grow(Image mask, const Json& area, const Context& c) {
    const auto it = area.find("matrix");
    if (it == area.end() || it->is_null()) return mask;
    // (the selection's outline moved by [a, b, c, d, e, f] in mm, bilinear: Select › Transform Selection)
    if (!it->is_array() || it->size() != 6) throw core::OpError("matrix must be [a, b, c, d, e, f]");
    selection::Matrix m{};
    for (std::size_t i = 0; i < 6; ++i) m[i] = finite((*it)[i], "matrix");
    selection::Matrix inv{};
    try {
        inv = selection::invert(m);
    } catch (const core::Error&) {
        throw core::OpError("matrix squashes the selection flat");
    }
    const double k = c.dpi / 25.4;
    const double data[6] = {inv[0], inv[2], inv[4] * k, inv[1], inv[3], inv[5] * k};
    return mask.transform(c.size, TransformMethod::Affine, data, Resample::Bilinear);
}

Image after_grow(Image mask, const Json& area, const Context& c) {
    bool any = false;
    for (const std::string_view key : kAfterGrow) any = any || (area.contains(std::string(key)) && !area.at(std::string(key)).is_null());
    if (!any) return mask;
    const std::size_t w = static_cast<std::size_t>(mask.width()), h = static_cast<std::size_t>(mask.height());
    std::vector<float> m = as_floats(mask);
    for (const std::string_view key : kAfterGrow) {
        const auto it = area.find(std::string(key));
        if (it == area.end() || it->is_null()) continue;
        const double mm = finite(*it, key);
        if (mm < 0) throw core::OpError(std::string(key) + " must not be negative");
        const double px = mm2px(mm, c.dpi);
        limits::check_count(px, limits::kReach, key);
        const float r = static_cast<float>(px);
        if (key == "expand_mm") m = selection_ext::expand(m, w, h, r);
        else if (key == "contract_mm") m = selection_ext::contract(m, w, h, r);
        else if (key == "border_mm") m = selection_ext::border(m, w, h, r);
        else m = selection_ext::smooth(m, w, h, r);
    }
    return selection_ext::to_l(m, mask.size());
}

bool antialiased(const Json& area) {
    const auto it = area.find("antialias");
    return it != area.end() && it->is_boolean() && it->get<bool>();
}

Image antialiased_polygon(const std::vector<std::pair<double, double>>& corners, Size size) {
    std::vector<std::pair<float, float>> points;
    points.reserve(corners.size());
    for (const auto& [x, y] : corners) points.emplace_back(static_cast<float>(x), static_cast<float>(y));
    // (worked over the rows the polygon spans and the columns up to its right — the very sums the whole page gives, the
    // rest of the page not held in floats: a precise or ink page's selection grid is up to 600 dpi)
    double low = INFINITY, high = -INFINITY, right = -INFINITY;
    for (const auto& [x, y] : points) {
        low = std::min(low, static_cast<double>(y));
        high = std::max(high, static_cast<double>(y));
        right = std::max(right, static_cast<double>(x));
    }
    Image out = Image::create("L", size, Ink(0));
    if (points.empty()) return out;
    const int y0 = static_cast<int>(std::clamp(std::floor(low) - 1.0, 0.0, static_cast<double>(size.height)));
    const int y1 = static_cast<int>(std::clamp(std::ceil(high) + 1.0, static_cast<double>(y0), static_cast<double>(size.height)));
    const int x1 = static_cast<int>(std::clamp(std::ceil(right) + 1.0, 0.0, static_cast<double>(size.width)));
    if (y1 <= y0 || x1 <= 0) return out;
    const selection_ext::Rect band{0, y0, x1, y1};
    out.paste(selection_ext::to_l(selection_ext::polygon(points, band, true), Size{x1, y1 - y0}), Point{0, y0});
    return out;
}

void use_keys() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const Kind& kind : table()) core::add_area_key(std::string(kind.key));
        core::add_area_key("matrix");
        core::add_area_key("antialias");
        for (const std::string_view key : kAfterGrow) core::add_area_key(std::string(key));
    });
}

}  // namespace genko::render::area_kinds
