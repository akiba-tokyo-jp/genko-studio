#include "render/psd_shapes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/json.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/patterns.hpp"
#include "core/pynum.hpp"
#include "render/pat.hpp"
#include "render/patterns.hpp"
#include "render/psd_descriptor.hpp"
#include "render/psd_paths.hpp"
#include "render/shape_parts.hpp"

// PhotoCraft v0.2.0 crates/io/src/vector_map.rs (130-420: vscg, vstk, vogk, shape blocks) and crates/io/src/blocks.rs
// (98-300: colours, gradients, fill descriptors), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::psd_shapes {

using core::Json;
namespace vec = core::vector;
namespace d = psd::descriptor;

namespace {

constexpr double kPi = std::numbers::pi;

void put_u32(std::string& out, std::uint32_t v) {
    for (const int shift : {24, 16, 8, 0}) out += static_cast<char>((v >> shift) & 0xff);
}

std::string tagged(std::string_view key, std::string payload) {
    payload.append((4 - payload.size() % 4) % 4, '\0');
    std::string out = "8BIM";
    out += key;
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    return out + payload;
}

std::vector<std::int64_t> colour_of(const Json& rgb) {
    if (rgb.is_array() && rgb.size() >= 3) {
        std::vector<std::int64_t> out;
        for (std::size_t i = 0; i < 3; ++i) out.push_back(rgb[i].is_number() ? rgb[i].get<std::int64_t>() : 0);
        return out;
    }
    return shape_parts::kInkColour;  // (null: the layer's ink)
}

d::Writer rgbc(const std::vector<std::int64_t>& rgb) {
    d::Writer w;
    w.number("Rd  ", static_cast<double>(rgb[0]));
    w.number("Grn ", static_cast<double>(rgb[1]));
    w.number("Bl  ", static_cast<double>(rgb[2]));
    return w;
}

// --- gradients: Genko's from → to over the page and Photoshop's angle, scale and offset over the path's box ------------

struct Stop {
    double pos;
    std::vector<std::int64_t> rgb;
    double opacity;
};

std::vector<Stop> stops_of(const Json& g) {
    std::vector<Stop> out;
    if (const auto s = g.find("stops"); s != g.end() && s->is_array() && !s->empty()) {
        for (const Json& stop : *s) {
            if (!stop.is_array() || stop.size() < 2) continue;
            out.push_back(Stop{stop[0].is_number() ? stop[0].get<double>() : 0.0, colour_of(stop[1]),
                               stop.size() > 2 && stop[2].is_number() ? stop[2].get<double>() : 1.0});
        }
        return out;
    }
    const auto num = [&](const char* key, double fallback) {
        const auto it = g.find(key);
        return it != g.end() && it->is_number() ? it->get<double>() : fallback;
    };
    const auto rgb = [&](const char* key, std::vector<std::int64_t> fallback) {
        const auto it = g.find(key);
        return it != g.end() && it->is_array() ? colour_of(*it) : fallback;
    };
    out.push_back(Stop{0.0, rgb("rgb_from", {20, 20, 20}), num("opacity_from", 1.0)});
    out.push_back(Stop{1.0, rgb("rgb_to", {255, 255, 255}), num("opacity_to", 1.0)});
    return out;
}

std::array<double, 2> point_of(const Json& g, const char* key, std::array<double, 2> fallback) {
    const auto it = g.find(key);
    if (it == g.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return fallback;
    return {(*it)[0].get<double>(), (*it)[1].get<double>()};
}

std::string shape_of(const Json& g) {
    const auto it = g.find("shape");
    return it != g.end() && it->is_string() ? it->get<std::string>() : std::string("linear");
}

// The box of the path's anchors and handles, mm (a part with no knot: a unit box at the origin).
std::array<double, 4> box_of(const vec::Path& path) {
    const auto b = path.control_bounds();
    return b ? *b : std::array<double, 4>{0, 0, 1, 1};
}

// Photoshop's length of a gradient over a box of w × h at `angle` (degrees): the box's extent along it.
double extent(double w, double h, double angle) {
    const double a = angle * kPi / 180.0;
    return std::max(1e-9, std::hypot(std::cos(a) * w, std::sin(a) * h));
}

d::Writer gradient_descriptor(const Json& g, const vec::Path& path, const Canvas& canvas, bool shape_class) {
    (void)shape_class;
    const auto f = point_of(g, "from", {0, 0});
    const auto t = point_of(g, "to", {0, 100});
    const std::string shape = shape_of(g);
    const bool linear = shape == "linear";
    const double kx = canvas.width_px / std::max(canvas.width_mm, 1e-9), ky = canvas.height_px / std::max(canvas.height_mm, 1e-9);
    const double dx = (t[0] - f[0]) * kx, dy = (t[1] - f[1]) * ky;
    const double span = std::hypot(dx, dy);
    const double angle = span > 0 ? std::atan2(-dy, dx) * 180.0 / kPi : 90.0;
    const std::array<double, 2> c = linear ? std::array<double, 2>{(f[0] + t[0]) / 2 * kx, (f[1] + t[1]) / 2 * ky}
                                           : std::array<double, 2>{f[0] * kx, f[1] * ky};
    const double length = linear ? span : 2 * span;
    const auto b = box_of(path);
    const double bw = std::max(1.0, (b[2] - b[0]) * kx), bh = std::max(1.0, (b[3] - b[1]) * ky);
    const double bcx = (b[0] + b[2]) / 2 * kx, bcy = (b[1] + b[3]) / 2 * ky;
    d::Writer w;
    w.unit("Angl", "#Ang", core::py_round(angle, 6));
    const std::string type = shape == "radial" || shape == "ellipse" ? "Rdl " : shape == "angle" ? "Angl" : shape == "reflected" ? "Rflc"
                             : shape == "diamond"                     ? "Dmnd"
                                                                      : "Lnr ";
    w.enumerated("Type", "GrdT", type);
    if (const auto r = g.find("reverse"); r != g.end() && r->is_boolean() && r->get<bool>()) w.boolean("Rvrs", true);
    w.boolean("Dthr", false);
    w.boolean("Algn", true);
    w.unit("Scl ", "#Prc", core::py_round(length / extent(bw, bh, angle) * 100.0, 6));
    d::Writer offset;
    offset.unit("Hrzn", "#Prc", core::py_round((c[0] - bcx) / bw * 100.0, 6));
    offset.unit("Vrtc", "#Prc", core::py_round((c[1] - bcy) / bh * 100.0, 6));
    w.object("Ofst", offset, "Pnt ");
    d::Writer grad;
    grad.text("Nm  ", u"Custom");
    grad.enumerated("GrdF", "GrdF", "CstS");
    grad.number("Intr", 4096.0);
    std::vector<std::pair<std::string, d::Writer>> colours, opacities;
    const std::vector<Stop> stops = stops_of(g);
    const auto opacity_stop = [&opacities](double pos, double opacity) {
        d::Writer op;
        op.unit("Opct", "#Prc", std::clamp(opacity, 0.0, 1.0) * 100.0);
        op.integer("Lctn", static_cast<std::int32_t>(std::lround(std::clamp(pos, 0.0, 1.0) * 4096.0)));
        op.integer("Mdpn", 50);
        opacities.emplace_back("TrnS", std::move(op));
    };
    for (const Stop& s : stops) {
        d::Writer stop;
        stop.object("Clr ", rgbc(s.rgb), "RGBC");
        stop.enumerated("Type", "Clry", "UsrS");
        stop.integer("Lctn", static_cast<std::int32_t>(std::lround(std::clamp(s.pos, 0.0, 1.0) * 4096.0)));
        stop.integer("Mdpn", 50);
        colours.emplace_back("Clrt", std::move(stop));
    }
    // (PC4 E4: a gradient's transparency stops times its colour stops' opacity, at the places of both; none: as before)
    std::vector<std::pair<double, double>> transparency;
    if (const auto given = g.find("transparency"); given != g.end() && given->is_array()) {
        for (const Json& each : *given)
            if (each.is_array() && each.size() == 2 && each[0].is_number() && each[1].is_number()) transparency.emplace_back(each[0].get<double>(), each[1].get<double>());
    }
    if (transparency.empty()) {
        for (const Stop& s : stops) opacity_stop(s.pos, s.opacity);
    } else {
        const auto along = [](const std::vector<std::pair<double, double>>& points, double x) {
            if (points.empty()) return 1.0;
            if (x <= points.front().first) return points.front().second;
            for (std::size_t i = 1; i < points.size(); ++i) {
                if (x <= points[i].first) {
                    const double gap = points[i].first - points[i - 1].first;
                    return gap > 0 ? points[i - 1].second + (points[i].second - points[i - 1].second) * (x - points[i - 1].first) / gap : points[i].second;
                }
            }
            return points.back().second;
        };
        std::vector<std::pair<double, double>> own;
        for (const Stop& s : stops) own.emplace_back(s.pos, s.opacity);
        std::stable_sort(own.begin(), own.end());
        std::stable_sort(transparency.begin(), transparency.end());
        std::vector<double> places;
        for (const auto& [pos, value] : own) places.push_back(pos);
        for (const auto& [pos, value] : transparency) places.push_back(pos);
        std::sort(places.begin(), places.end());
        places.erase(std::unique(places.begin(), places.end(), [](double one, double other) { return std::fabs(one - other) < 1e-4; }), places.end());
        for (const double place : places) opacity_stop(place, along(own, place) * along(transparency, place));
    }
    grad.objects("Clrs", colours);
    grad.objects("Trns", opacities);
    w.object("Grad", grad, "Grdn");
    return w;
}

// patterns@1 (PC4 E3): a pattern fill's PtFl descriptor (PhotoCraft v0.2.0 crates/io/src/blocks.rs fill_to_desc,
// pattern_ref_desc, with_pattern_placement).
d::Writer pattern_descriptor(const Json& pattern, const Canvas& canvas) {
    const std::string id = pattern.is_object() && pattern.contains("id") && pattern["id"].is_string() ? pattern["id"].get<std::string>() : std::string();
    const core::Pattern* kept = canvas.book != nullptr ? core::find_pattern(*canvas.book, id) : nullptr;
    const patterns::Spot spot = patterns::spot_of(pattern);
    const double mm_px = canvas.width_px > 0 && canvas.width_mm > 0 ? canvas.width_mm / canvas.width_px : 25.4 / std::max(canvas.dpi, 1.0);
    const double pattern_dpi = kept != nullptr ? kept->dpi : 25.4 / mm_px;
    d::Writer w;
    w.unit("Scl ", "#Prc", core::py_round(std::clamp(spot.scale * 100.0 * (25.4 / pattern_dpi) / mm_px, 1.0, 1000.0), 6));
    d::Writer ref;
    ref.text("Nm  ", psd::utf16(kept != nullptr ? kept->name : id));
    ref.text("Idnt", psd::utf16(kept != nullptr ? pat::file_id(*kept) : id));
    w.object("Ptrn", ref, "Ptrn");
    if (spot.angle != 0.0) w.unit("Angl", "#Ang", spot.angle);
    w.boolean("Algn", spot.link);
    d::Writer phase;
    phase.number("Hrzn", core::py_round(spot.phase_x_mm / mm_px, 6));
    phase.number("Vrtc", core::py_round(spot.phase_y_mm / mm_px, 6));
    w.object("phase", phase, "Pnt ");
    return w;
}

// A PtFl descriptor read back as a pattern fill (PhotoCraft's pattern_ref, pattern_placement).
std::optional<Json> pattern_read(const d::Object& o, const Placement& at) {
    if (!at.pattern) return std::nullopt;
    std::string name, id;
    if (const d::Value* ref = o.get("Ptrn"); ref != nullptr && ref->object) {
        if (const d::Value* n = ref->object->get("Nm  ")) name = psd::utf8_of(n->text);
        if (const d::Value* i = ref->object->get("Idnt")) id = psd::utf8_of(i->text);
    }
    const auto found = at.pattern(id, name);
    if (!found) return std::nullopt;
    const auto number = [&](const d::Object& from, std::string_view key, double fallback) {
        const d::Value* v = from.get(key);
        const std::optional<double> n = v != nullptr ? v->numeric() : std::nullopt;
        return n && std::isfinite(*n) ? *n : fallback;
    };
    // (Photoshop's one pattern pixel is Scl % of a canvas pixel: mm_x of the page; the book's is 1/dpi inch × its scale)
    const double scale = number(o, "Scl ", 100.0) * at.mm_x * found->second / 25.4;
    double px = 0.0, py = 0.0;
    if (const d::Value* phase = o.get("phase"); phase != nullptr && phase->object) {
        px = number(*phase->object, "Hrzn", 0.0);
        py = number(*phase->object, "Vrtc", 0.0);
    }
    const d::Value* link = o.get("Algn");
    Json spec = Json::object();
    spec["id"] = found->first;
    spec["scale"] = core::py_round(std::clamp(scale, core::kPatternScaleMin, core::kPatternScaleMax), 4);
    spec["angle"] = core::py_round(std::clamp(number(o, "Angl", 0.0), -360.0, 360.0), 4);
    spec["phase_mm"] = Json::array({core::py_round(std::clamp(at.x0_mm + px * at.mm_x, -core::kPatternPhaseMax, core::kPatternPhaseMax), 4),
                                    core::py_round(std::clamp(at.y0_mm + py * at.mm_y, -core::kPatternPhaseMax, core::kPatternPhaseMax), 4)});
    spec["link"] = link == nullptr || link->type != "bool" || link->boolean;
    try {
        return core::pattern_fill_spec(spec);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A fill as its block's key and descriptor (a fill of none: black, its vstk saying the fill is off).
std::pair<std::string, d::Writer> fill_descriptor(const Json& fill, const vec::Path& path, const Canvas& canvas) {
    if (fill.is_object()) {
        if (const auto p = fill.find("pattern"); p != fill.end() && p->is_object()) return {"PtFl", pattern_descriptor(*p, canvas)};  // (patterns@1)
        if (const auto g = fill.find("gradient"); g != fill.end() && g->is_object()) return {"GdFl", gradient_descriptor(*g, path, canvas, false)};
        if (const auto rgb = fill.find("rgb"); rgb != fill.end()) {
            d::Writer w;
            w.object("Clr ", rgbc(colour_of(*rgb)), "RGBC");
            return {"SoCo", std::move(w)};
        }
    }
    d::Writer w;
    w.object("Clr ", rgbc({0, 0, 0}), "RGBC");
    return {"SoCo", std::move(w)};
}

std::string vstk(const vec::ShapePart& part, const Canvas& canvas) {
    const vec::ShapeStroke s = part.stroke.value_or(vec::ShapeStroke{});
    const double px_per_mm = canvas.width_px / std::max(canvas.width_mm, 1e-9);
    d::Writer w;
    w.integer("strokeStyleVersion", 2);
    w.boolean("strokeEnabled", part.stroke.has_value() && s.width_mm > 0);
    w.boolean("fillEnabled", shape_parts::fills(part.fill));
    w.unit("strokeStyleLineWidth", "#Pxl", core::py_round(s.width_mm * px_per_mm, 6));
    w.unit("strokeStyleLineDashOffset", "#Pnt", s.dash_offset);
    w.number("strokeStyleMiterLimit", s.miter_limit);
    w.enumerated("strokeStyleLineCapType", "strokeStyleLineCapType",
                 s.cap == vec::LineCap::Round    ? "strokeStyleRoundCap"
                 : s.cap == vec::LineCap::Square ? "strokeStyleSquareCap"
                                                 : "strokeStyleButtCap");
    w.enumerated("strokeStyleLineJoinType", "strokeStyleLineJoinType",
                 s.join == vec::LineJoin::Round   ? "strokeStyleRoundJoin"
                 : s.join == vec::LineJoin::Bevel ? "strokeStyleBevelJoin"
                                                  : "strokeStyleMiterJoin");
    w.enumerated("strokeStyleLineAlignment", "strokeStyleLineAlignment",
                 s.align == vec::StrokeAlign::Inside    ? "strokeStyleAlignInside"
                 : s.align == vec::StrokeAlign::Outside ? "strokeStyleAlignOutside"
                                                        : "strokeStyleAlignCenter");
    w.boolean("strokeStyleScaleLock", false);
    w.boolean("strokeStyleStrokeAdjust", false);
    w.units("strokeStyleLineDashSet", "#Nne", s.dashes);
    w.enumerated("strokeStyleBlendMode", "BlnM", "Nrml");
    w.unit("strokeStyleOpacity", "#Prc", std::clamp(s.opacity, 0.0, 1.0) * 100.0);
    const Json& paint = s.fill.is_null() ? Json{{"rgb", nullptr}} : s.fill;
    auto [key, content] = fill_descriptor(paint, part.path, canvas);
    w.object("strokeStyleContent", content, key == "GdFl" ? "gradientLayer" : key == "PtFl" ? "patternLayer" : "solidColorLayer");
    w.number("strokeStyleResolution", canvas.dpi);
    return w.versioned("strokeStyle");
}

std::optional<std::string> vogk(const vec::LiveShape& live, const Canvas& canvas) {
    const double kx = canvas.width_px / std::max(canvas.width_mm, 1e-9), ky = canvas.height_px / std::max(canvas.height_mm, 1e-9);
    const auto bbox = [&](const std::array<double, 4>& r) {
        d::Writer b;
        b.integer("unitValueQuadVersion", 1);
        b.unit("Top ", "#Pxl", r[1] * ky);
        b.unit("Left", "#Pxl", r[0] * kx);
        b.unit("Btom", "#Pxl", (r[1] + r[3]) * ky);
        b.unit("Rght", "#Pxl", (r[0] + r[2]) * kx);
        return b;
    };
    d::Writer o;
    using Kind = vec::LiveShape::Kind;
    const bool round = live.kind == Kind::Rect && std::any_of(live.radii.begin(), live.radii.end(), [](double r) { return r > 0; });
    switch (live.kind) {
        case Kind::Rect:
            o.integer("keyOriginType", round ? 2 : 1);
            o.number("keyOriginResolution", canvas.dpi);
            if (round) {
                d::Writer radii;
                radii.integer("unitValueQuadVersion", 1);
                radii.unit("topRight", "#Pxl", live.radii[1] * kx);
                radii.unit("topLeft", "#Pxl", live.radii[0] * kx);
                radii.unit("bottomLeft", "#Pxl", live.radii[3] * kx);
                radii.unit("bottomRight", "#Pxl", live.radii[2] * kx);
                o.object("keyOriginRRectRadii", radii, "radii");
            }
            o.object("keyOriginShapeBBox", bbox(live.rect), "unitRect");
            break;
        case Kind::Ellipse:
            o.integer("keyOriginType", 5);
            o.number("keyOriginResolution", canvas.dpi);
            o.object("keyOriginShapeBBox", bbox(live.rect), "unitRect");
            break;
        case Kind::Line: {
            o.integer("keyOriginType", 4);
            o.number("keyOriginResolution", canvas.dpi);
            const std::array<double, 4> r{std::min(live.from.x, live.to.x), std::min(live.from.y, live.to.y), std::fabs(live.from.x - live.to.x),
                                          std::fabs(live.from.y - live.to.y)};
            o.object("keyOriginShapeBBox", bbox(r), "unitRect");
            const auto pnt = [&](const vec::XY& p) {
                d::Writer w;
                w.number("Hrzn", p.x * kx);
                w.number("Vrtc", p.y * ky);
                return w;
            };
            o.object("keyOriginLineEnd", pnt(live.to), "Pnt ");
            o.object("keyOriginLineStart", pnt(live.from), "Pnt ");
            o.number("keyOriginLineWeight", live.weight_mm * kx);
            o.boolean("keyOriginLineArrowSt", false);
            o.boolean("keyOriginLineArrowEnd", false);
            o.number("keyOriginLineArrWdth", 0.0);
            o.number("keyOriginLineArrLngth", 0.0);
            o.integer("keyOriginLineArrConc", 0);
            break;
        }
        case Kind::Polygon:
            return std::nullopt;  // (no origination type Photoshop reads for one here)
    }
    o.integer("keyOriginIndex", 0);
    d::Writer list;
    list.objects("keyDescriptorList", {{"null", o}});
    std::string out;
    put_u32(out, 1);
    return out + list.versioned("null");
}

// --- reading ----------------------------------------------------------------------------------------------------------

std::optional<double> number_of(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr ? v->numeric() : std::nullopt;
}

std::optional<std::vector<std::int64_t>> colour_read(const d::Object& c) {
    const auto g = [&](std::string_view k) { return number_of(c, k); };
    const auto byte = [](double v) { return static_cast<std::int64_t>(std::clamp<long>(std::lround(v), 0, 255)); };
    if (c.get("redFloat") != nullptr) {
        const auto r = g("redFloat"), gr = g("greenFloat"), b = g("blueFloat");
        if (!r || !gr || !b) return std::nullopt;
        return std::vector<std::int64_t>{byte(*r * 255), byte(*gr * 255), byte(*b * 255)};
    }
    if (c.class_id == "Grsc") {
        const auto grey = g("Gry ");
        if (!grey) return std::nullopt;
        const auto v = byte((1.0 - *grey / 100.0) * 255);
        return std::vector<std::int64_t>{v, v, v};
    }
    if (c.class_id == "CMYC") {
        const auto cy = g("Cyn "), m = g("Mgnt"), y = g("Ylw "), k = g("Blck");
        if (!cy || !m || !y || !k) return std::nullopt;
        const double kk = 1.0 - *k / 100.0;
        return std::vector<std::int64_t>{byte((1.0 - *cy / 100.0) * kk * 255), byte((1.0 - *m / 100.0) * kk * 255), byte((1.0 - *y / 100.0) * kk * 255)};
    }
    const auto r = g("Rd  "), gr = g("Grn "), b = g("Bl  ");
    if (!r || !gr || !b) return std::nullopt;
    return std::vector<std::int64_t>{byte(*r), byte(*gr), byte(*b)};
}

std::optional<std::vector<std::int64_t>> colour_in(const d::Object& o) {
    const d::Value* c = o.get("Clr ");
    if (c == nullptr || !c->object) return std::nullopt;
    return colour_read(*c->object);
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

// The colour and opacity of a gradient's stops at `t` (linear between them, as written).
std::pair<std::vector<double>, double> sample(const std::vector<std::pair<double, std::vector<std::int64_t>>>& colours,
                                              const std::vector<std::pair<double, double>>& opacities, double t) {
    std::vector<double> rgb{0, 0, 0};
    if (!colours.empty()) {
        std::size_t i = 0;
        while (i + 1 < colours.size() && colours[i + 1].first < t) ++i;
        const auto& a = colours[i];
        const auto& b = i + 1 < colours.size() ? colours[i + 1] : colours[i];
        const double k = t <= a.first ? 0.0 : b.first > a.first ? std::clamp((t - a.first) / (b.first - a.first), 0.0, 1.0) : 0.0;
        for (std::size_t c = 0; c < 3; ++c) rgb[c] = lerp(static_cast<double>(a.second[c]), static_cast<double>(b.second[c]), k);
    }
    double opacity = 1.0;
    if (!opacities.empty()) {
        std::size_t i = 0;
        while (i + 1 < opacities.size() && opacities[i + 1].first < t) ++i;
        const auto& a = opacities[i];
        const auto& b = i + 1 < opacities.size() ? opacities[i + 1] : opacities[i];
        const double k = t <= a.first ? 0.0 : b.first > a.first ? std::clamp((t - a.first) / (b.first - a.first), 0.0, 1.0) : 0.0;
        opacity = lerp(a.second, b.second, k);
    }
    return {rgb, opacity};
}

// A GdFl descriptor as Genko's gradient over the page (its from and to through the path's box as the canvas lies on the
// page).
std::optional<Json> gradient_read(const d::Object& o, const vec::Path& path, const Placement& at) {
    const d::Value* grad = o.get("Grad");
    if (grad == nullptr || !grad->object) return std::nullopt;
    std::vector<std::pair<double, std::vector<std::int64_t>>> colours;
    std::vector<std::pair<double, double>> opacities;
    if (const d::Value* list = grad->object->get("Clrs")) {
        for (const d::Value& item : list->list) {
            if (!item.object) continue;
            const auto c = colour_in(*item.object);
            if (!c) continue;
            colours.emplace_back(number_of(*item.object, "Lctn").value_or(0.0) / 4096.0, *c);
        }
    }
    if (const d::Value* list = grad->object->get("Trns")) {
        for (const d::Value& item : list->list) {
            if (!item.object) continue;
            opacities.emplace_back(number_of(*item.object, "Lctn").value_or(0.0) / 4096.0, number_of(*item.object, "Opct").value_or(100.0) / 100.0);
        }
    }
    std::stable_sort(colours.begin(), colours.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::stable_sort(opacities.begin(), opacities.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (colours.empty()) colours = {{0.0, {0, 0, 0}}, {1.0, {255, 255, 255}}};
    std::vector<double> places;
    for (const auto& c : colours) places.push_back(std::clamp(c.first, 0.0, 1.0));
    for (const auto& c : opacities) places.push_back(std::clamp(c.first, 0.0, 1.0));
    std::sort(places.begin(), places.end());
    places.erase(std::unique(places.begin(), places.end(), [](double a, double b) { return std::fabs(a - b) < 1e-4; }), places.end());
    while (places.size() > 16) places.erase(places.begin() + static_cast<std::ptrdiff_t>(places.size() / 2));  // (16 at most)
    if (places.size() < 2) places = {0.0, 1.0};
    Json stops = Json::array();
    for (const double t : places) {
        const auto [rgb, opacity] = sample(colours, opacities, t);
        stops.push_back(Json::array({core::py_round(t, 4),
                                     Json::array({std::lround(std::clamp(rgb[0], 0.0, 255.0)), std::lround(std::clamp(rgb[1], 0.0, 255.0)),
                                                  std::lround(std::clamp(rgb[2], 0.0, 255.0))}),
                                     core::py_round(std::clamp(opacity, 0.0, 1.0), 3)}));
    }
    std::string type = "Lnr ";
    if (const d::Value* t = o.get("Type")) type = t->enum_value;
    const std::string shape = type == "Rdl " ? "radial" : type == "Angl" ? "angle" : type == "Rflc" ? "reflected" : type == "Dmnd" ? "diamond" : "linear";
    const double angle = number_of(o, "Angl").value_or(90.0);
    const double scale = number_of(o, "Scl ").value_or(100.0) / 100.0;
    double ox = 0, oy = 0;
    if (const d::Value* off = o.get("Ofst"); off != nullptr && off->object) {
        ox = number_of(*off->object, "Hrzn").value_or(0.0) / 100.0;
        oy = number_of(*off->object, "Vrtc").value_or(0.0) / 100.0;
    }
    // (the box in canvas pixels; the centre and length there, then onto the page)
    const auto b = box_of(path);
    const double bx0 = (b[0] - at.x0_mm) / at.mm_x, by0 = (b[1] - at.y0_mm) / at.mm_y;
    const double bw = std::max(1.0, (b[2] - b[0]) / at.mm_x), bh = std::max(1.0, (b[3] - b[1]) / at.mm_y);
    const double cx = bx0 + bw / 2 + ox * bw, cy = by0 + bh / 2 + oy * bh;
    const double length = extent(bw, bh, angle) * scale;
    const double a = angle * kPi / 180.0;
    const double ux = std::cos(a), uy = -std::sin(a);
    const auto mm = [&](double x, double y) { return Json::array({core::py_round(at.x0_mm + x * at.mm_x, 4), core::py_round(at.y0_mm + y * at.mm_y, 4)}); };
    Json g = Json::object();
    if (shape == "linear") {
        g["from"] = mm(cx - ux * length / 2, cy - uy * length / 2);
        g["to"] = mm(cx + ux * length / 2, cy + uy * length / 2);
    } else {
        g["from"] = mm(cx, cy);
        g["to"] = mm(cx + ux * length / 2, cy + uy * length / 2);
    }
    g["shape"] = shape;
    g["stops"] = std::move(stops);
    if (const d::Value* r = o.get("Rvrs"); r != nullptr && r->boolean) g["reverse"] = true;
    try {
        return core::fill_spec(Json{{"gradient", g}});
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A fill block's descriptor as a fill (SoCo: a colour; GdFl: a gradient; PtFl: a pattern of the file, patterns@1); none for
// another.
std::optional<Json> fill_read(std::string_view key, const d::Object& o, const vec::Path& path, const Placement& at) {
    if (key == "PtFl" || key == "patternLayer") {
        if (auto spec = pattern_read(o, at)) return Json{{"pattern", std::move(*spec)}};
        return std::nullopt;
    }
    if (key == "SoCo" || key == "solidColorLayer") {
        const auto c = colour_in(o);
        if (!c) return std::nullopt;
        return Json{{"rgb", core::ints_json(*c)}};
    }
    if (key == "GdFl" || key == "gradientLayer") return gradient_read(o, path, at);
    return std::nullopt;
}

std::optional<d::Object> versioned_at(std::string_view payload, std::size_t at) {
    try {
        return d::read_versioned(payload, at);
    } catch (const d::Broken&) {
        return std::nullopt;
    }
}

std::string enum_value(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr ? v->enum_value : std::string();
}

// A size of a vstk (a unit float: pixels, points or mm) in mm.
double size_mm(const d::Value* v, const Placement& at, double fallback) {
    if (v == nullptr) return fallback;
    const std::optional<double> n = v->numeric();
    if (!n) return fallback;
    if (v->unit == "#Pnt") return *n / 72.0 * 25.4;
    if (v->unit == "#Mlm") return *n;
    return *n * at.mm_x;  // (#Pxl, or a plain number: the canvas's pixels)
}

}  // namespace

bool writable(const vec::ShapePart& part) {
    const auto ok = [](const Json& fill) {
        if (fill.is_null()) return true;
        if (!fill.is_object()) return false;
        return fill.contains("rgb") || (fill.contains("gradient") && fill["gradient"].is_object()) ||
               (fill.contains("pattern") && fill["pattern"].is_object());  // (patterns@1: PtFl)
    };
    return ok(part.fill) && (!part.stroke || ok(part.stroke->fill));
}

Plan plan(const core::Layer& layer) {
    if (layer.clip || (layer.screen && core::py_truthy(*layer.screen)) || (layer.color && !layer.color->empty()) ||
        (layer.effect && core::py_truthy(*layer.effect))) {
        return Plan::Pixels;
    }
    for (const vec::ShapePart& part : layer.shapes->items)
        if (!writable(part)) return Plan::Pixels;
    if (layer.shapes->items.size() == 1 && layer.stroke_count() == 0 && layer.patches.empty() && !layer.vector_mask &&
        (!layer.raster_png || layer.raster_png->empty())) {
        return Plan::Shape;
    }
    return Plan::Group;
}

std::string blocks(const vec::ShapePart& given, const Canvas& canvas) {
    vec::ShapePart part = given;
    if (part.path.fill_rule == vec::FillRule::EvenOdd) {  // (Photoshop fills each subpath non-zero: the area baked)
        part.path = shape_parts::baked(part.path);
        part.live.reset();
    }
    auto [key, fill] = fill_descriptor(part.fill, part.path, canvas);
    std::string out = tagged(key, fill.versioned("null"));
    vec::VectorMask outline;
    outline.path = part.path;
    out += tagged("vsms", psd_paths::vmsk(outline, canvas.width_mm, canvas.height_mm));
    out += tagged("vscg", key + fill.versioned("null"));
    if (part.stroke || !shape_parts::fills(part.fill)) out += tagged("vstk", vstk(part, canvas));
    if (part.live) {
        if (auto origin = vogk(*part.live, canvas)) out += tagged("vogk", std::move(*origin));
    }
    if (part.opacity < 1.0) {
        std::string opacity(1, static_cast<char>(std::clamp<long>(std::lround(part.opacity * 255.0), 0, 255)));
        out += tagged("iOpa", opacity);
    }
    return out;
}

std::string pattern_block(const Json& pattern, const Canvas& canvas) { return pattern_descriptor(pattern, canvas).versioned("null"); }

std::optional<Json> pattern_fill_of(std::string_view payload, const Placement& at) {
    const std::optional<d::Object> o = versioned_at(payload, 0);
    return o ? pattern_read(*o, at) : std::nullopt;
}

std::optional<vec::ShapePart> part_of(const Blocks& blocks, const Placement& at) {
    std::optional<vec::VectorMask> mask = psd_paths::vector_mask_of(blocks.path, 1.0, 1.0);
    if (!mask || mask->path.empty()) return std::nullopt;
    vec::ShapePart part;
    // (the path: fractions of the canvas, onto the page)
    const double w_mm = at.width_px * at.mm_x, h_mm = at.height_px * at.mm_y;
    part.path = mask->path.transformed({w_mm, 0.0, 0.0, h_mm, at.x0_mm, at.y0_mm});
    // (its fill: the fill layer's block, else vscg)
    std::optional<Json> fill;
    if (!blocks.fill.empty() && !blocks.fill_key.empty()) {
        if (auto o = versioned_at(blocks.fill, 0)) fill = fill_read(blocks.fill_key, *o, part.path, at);
    }
    if (!fill && blocks.vscg.size() > 4) {
        if (auto o = versioned_at(blocks.vscg, 4)) fill = fill_read(blocks.vscg.substr(0, 4), *o, part.path, at);
    }
    if (!fill) return std::nullopt;
    part.fill = *fill;
    if (!blocks.vstk.empty()) {
        if (auto o = versioned_at(blocks.vstk, 0)) {
            const d::Value* enabled = o->get("strokeEnabled");
            const d::Value* fill_on = o->get("fillEnabled");
            if (fill_on != nullptr && fill_on->type == "bool" && !fill_on->boolean) part.fill = nullptr;
            if (enabled != nullptr && enabled->boolean) {
                vec::ShapeStroke s;
                s.width_mm = std::clamp(size_mm(o->get("strokeStyleLineWidth"), at, 0.35), 0.0, vec::kStrokeWidthMax);
                s.opacity = std::clamp(number_of(*o, "strokeStyleOpacity").value_or(100.0) / 100.0, 0.0, 1.0);
                const std::string cap = enum_value(*o, "strokeStyleLineCapType");
                s.cap = cap == "strokeStyleRoundCap" ? vec::LineCap::Round : cap == "strokeStyleSquareCap" ? vec::LineCap::Square : vec::LineCap::Butt;
                const std::string join = enum_value(*o, "strokeStyleLineJoinType");
                s.join = join == "strokeStyleRoundJoin" ? vec::LineJoin::Round : join == "strokeStyleBevelJoin" ? vec::LineJoin::Bevel : vec::LineJoin::Miter;
                const std::string align = enum_value(*o, "strokeStyleLineAlignment");
                s.align = align == "strokeStyleAlignInside"    ? vec::StrokeAlign::Inside
                          : align == "strokeStyleAlignOutside" ? vec::StrokeAlign::Outside
                                                               : vec::StrokeAlign::Center;
                s.miter_limit = std::clamp(number_of(*o, "strokeStyleMiterLimit").value_or(100.0), 1.0, 1000.0);
                if (const d::Value* dashes = o->get("strokeStyleLineDashSet")) {
                    for (const d::Value& v : dashes->list) {
                        if (const auto n = v.numeric(); n && s.dashes.size() < 64) s.dashes.push_back(std::clamp(*n, 0.0, 1000.0));
                    }
                }
                s.dash_offset = std::clamp(number_of(*o, "strokeStyleLineDashOffset").value_or(0.0), -1000.0, 1000.0);
                s.fill = Json{{"rgb", Json::array({0, 0, 0})}};
                if (const d::Value* content = o->get("strokeStyleContent"); content != nullptr && content->object) {
                    if (auto f = fill_read(content->object->class_id, *content->object, part.path, at)) s.fill = *f;
                }
                part.stroke = s;
            }
        }
    }
    if (blocks.vogk.size() > 4) {  // (the live shape: one origination, not transformed, of a kind there is)
        if (auto o = versioned_at(blocks.vogk, 4)) {
            const d::Value* list = o->get("keyDescriptorList");
            if (list != nullptr && list->list.size() == 1 && list->list.front().object) {
                const d::Object& one = *list->list.front().object;
                const d::Value* invalid = one.get("keyShapeInvalidated");
                bool plain = !(invalid != nullptr && invalid->boolean);
                if (const d::Value* t = one.get("Trnf"); t != nullptr && t->object) {
                    for (const auto& [k, ident] : {std::pair<const char*, double>{"xx", 1.0}, {"xy", 0.0}, {"yx", 0.0}, {"yy", 1.0}, {"tx", 0.0}, {"ty", 0.0}}) {
                        if (std::fabs(number_of(*t->object, k).value_or(ident) - ident) > 1e-9) plain = false;
                    }
                }
                const auto rect = [&]() -> std::optional<std::array<double, 4>> {
                    const d::Value* b = one.get("keyOriginShapeBBox");
                    if (b == nullptr || !b->object) return std::nullopt;
                    const auto top = number_of(*b->object, "Top "), left = number_of(*b->object, "Left"), bottom = number_of(*b->object, "Btom"),
                               right = number_of(*b->object, "Rght");
                    if (!top || !left || !bottom || !right) return std::nullopt;
                    return std::array<double, 4>{at.x0_mm + *left * at.mm_x, at.y0_mm + *top * at.mm_y, (*right - *left) * at.mm_x, (*bottom - *top) * at.mm_y};
                };
                const std::optional<double> type = number_of(one, "keyOriginType");
                std::optional<vec::LiveShape> live;
                if (plain && type) {
                    vec::LiveShape l;
                    if ((*type == 1 || *type == 2) && rect()) {
                        l.kind = vec::LiveShape::Kind::Rect;
                        l.rect = *rect();
                        if (const d::Value* r = one.get("keyOriginRRectRadii"); *type == 2 && r != nullptr && r->object) {
                            const auto g = [&](const char* k) { return number_of(*r->object, k).value_or(0.0) * at.mm_x; };
                            l.radii = {g("topLeft"), g("topRight"), g("bottomRight"), g("bottomLeft")};
                        }
                        live = l;
                    } else if (*type == 5 && rect()) {
                        l.kind = vec::LiveShape::Kind::Ellipse;
                        l.rect = *rect();
                        live = l;
                    } else if (*type == 4) {
                        const d::Value* s = one.get("keyOriginLineStart");
                        const d::Value* e = one.get("keyOriginLineEnd");
                        if (s != nullptr && e != nullptr && s->object && e->object) {
                            l.kind = vec::LiveShape::Kind::Line;
                            l.from = vec::XY{at.x0_mm + number_of(*s->object, "Hrzn").value_or(0.0) * at.mm_x, at.y0_mm + number_of(*s->object, "Vrtc").value_or(0.0) * at.mm_y};
                            l.to = vec::XY{at.x0_mm + number_of(*e->object, "Hrzn").value_or(0.0) * at.mm_x, at.y0_mm + number_of(*e->object, "Vrtc").value_or(0.0) * at.mm_y};
                            l.weight_mm = std::clamp(number_of(one, "keyOriginLineWeight").value_or(1.0) * at.mm_x, 0.0, vec::kStrokeWidthMax);
                            live = l;
                        }
                    }
                }
                if (live) part.live = live;  // (a shape Photoshop changed since says so: keyShapeInvalidated, or a transform)
            }
        }
    }
    return part;
}

std::vector<std::string> notes(const core::Document& book) {
    bool pixels = false, ellipse = false, repeat = false, evenodd = false;
    const auto look = [&](const Json& fill) {
        if (!fill.is_object() || !fill.contains("gradient") || !fill["gradient"].is_object()) return;
        const Json& g = fill["gradient"];
        if (g.value("shape", std::string()) == "ellipse") ellipse = true;
        if (g.contains("repeat") && g["repeat"].is_string() && g["repeat"] != "none") repeat = true;
    };
    for (std::size_t i = 0; i < book.pages.size(); ++i) {
        if (book.is_deferred(i)) continue;
        for (const core::Layer& layer : book.pages[i]->layers) {
            if (!shape_parts::any(layer) || !layer.visible || !layer.exportable) continue;
            if (plan(layer) == Plan::Pixels) {
                pixels = true;
                continue;
            }
            for (const vec::ShapePart& part : layer.shapes->items) {
                look(part.fill);
                if (part.stroke) look(part.stroke->fill);
                if (part.path.fill_rule == vec::FillRule::EvenOdd) evenodd = true;
            }
        }
    }
    std::vector<std::string> out;
    if (pixels) out.push_back("シェイプを持つペンのレイヤーのうち、クリッピング・トーン化・レイヤーの色・境界効果のあるものは、画素のレイヤーとして書き出しました");
    if (ellipse) out.push_back("シェイプの楕円のグラデーションは、Photoshop に同じものが無いため円形のグラデーションとして書き出しました");
    if (repeat) out.push_back("シェイプのグラデーションの繰り返しは、Photoshop に同じものが無いため繰り返さずに書き出しました");
    if (evenodd) out.push_back("塗りの規則が「奇偶」のシェイプは、見えている形どおりの輪郭にして書き出しました");
    return out;
}

}  // namespace genko::render::psd_shapes
