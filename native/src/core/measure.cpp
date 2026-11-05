// measure@1 (PC5 J-0): a page's measuring, its JSON and checks (core/measure.hpp). The shapes after PhotoCraft v0.2.0's
// doc/src/analysis.rs (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, the places in mm.

#include "core/measure.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>

#include "core/error.hpp"
#include "core/lens.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace pj = production;

namespace {

constexpr double kLengthMax = 1e9;

}  // namespace

Json to_json(const Measure& measure) {
    Json out = Json::object();
    out["scale"] = measure.scale ? Json{{"pixel_length", measure.scale->pixel_length},
                                        {"logical_length", measure.scale->logical_length},
                                        {"units", measure.scale->units}}
                                 : Json(nullptr);
    Json groups = Json::array();
    for (const CountGroup& g : measure.count_groups) {
        Json points = Json::array();
        for (const auto& p : g.points) points.push_back(pj::to_json(p));
        groups.push_back(Json{{"name", g.name}, {"color", pj::to_json(g.color)}, {"marker_size", g.marker_size}, {"label_size", g.label_size},
                              {"visible", g.visible}, {"points", std::move(points)}});
    }
    out["count_groups"] = std::move(groups);
    out["active_group"] = measure.active_group;
    out["ruler"] = measure.ruler ? Json{{"start", pj::to_json(measure.ruler->start)},
                                        {"end", pj::to_json(measure.ruler->end)},
                                        {"protractor", measure.ruler->protractor ? pj::to_json(*measure.ruler->protractor) : Json(nullptr)}}
                                 : Json(nullptr);
    return out;
}

Measure measure_from_json(const Json& value) {
    const std::string where = "measure";
    pj::only_keys(value, where, {"scale", "count_groups", "active_group", "ruler"});
    Measure out;
    if (const Json& scale = pj::field(value, "scale", where); !scale.is_null()) {
        const std::string at = where + ".scale";
        pj::only_keys(scale, at, {"pixel_length", "logical_length", "units"});
        MeasurementScale s;
        s.pixel_length = pj::number(pj::field(scale, "pixel_length", at), at + ".pixel_length", std::numeric_limits<double>::min(), kLengthMax);
        s.logical_length = pj::number(pj::field(scale, "logical_length", at), at + ".logical_length", std::numeric_limits<double>::min(), kLengthMax);
        s.units = pj::text(pj::field(scale, "units", at), at + ".units", 256);
        out.scale = std::move(s);
    }
    const Json& groups = pj::field(value, "count_groups", where);
    if (!groups.is_array()) pj::wrong(where + ".count_groups", "must be a list");
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::string at = where + ".count_groups[" + std::to_string(i) + "]";
        const Json& item = groups[i];
        pj::only_keys(item, at, {"name", "color", "marker_size", "label_size", "visible", "points"});
        CountGroup g;
        g.name = pj::text(pj::field(item, "name", at), at + ".name", 1024);
        g.color = pj::rgb(pj::field(item, "color", at), at + ".color");
        g.marker_size = static_cast<int>(pj::integer(pj::field(item, "marker_size", at), at + ".marker_size", 1, 10));
        g.label_size = static_cast<int>(pj::integer(pj::field(item, "label_size", at), at + ".label_size", 8, 72));
        g.visible = pj::boolean(pj::field(item, "visible", at), at + ".visible");
        const Json& points = pj::field(item, "points", at);
        if (!points.is_array()) pj::wrong(at + ".points", "must be a list");
        if (points.size() > kCountPointsMax) pj::wrong(at + ".points", "holds more than " + std::to_string(kCountPointsMax) + " marks");
        for (std::size_t k = 0; k < points.size(); ++k) g.points.push_back(pj::point_mm(points[k], at + ".points[" + std::to_string(k) + "]"));
        out.count_groups.push_back(std::move(g));
    }
    out.active_group = static_cast<std::size_t>(pj::integer(pj::field(value, "active_group", where), where + ".active_group", 0, static_cast<std::int64_t>(kCountGroupsMax)));
    if (const Json& ruler = pj::field(value, "ruler", where); !ruler.is_null()) {
        const std::string at = where + ".ruler";
        pj::only_keys(ruler, at, {"start", "end", "protractor"});
        RulerLine r;
        r.start = pj::point_mm(pj::field(ruler, "start", at), at + ".start");
        r.end = pj::point_mm(pj::field(ruler, "end", at), at + ".end");
        if (const Json& p = pj::field(ruler, "protractor", at); !p.is_null()) r.protractor = pj::point_mm(p, at + ".protractor");
        out.ruler = r;
    }
    check_measure(out);
    return out;
}

void check_measure(const Measure& measure) {
    const std::string where = "measure";
    if (measure.count_groups.size() > kCountGroupsMax) pj::wrong(where, "holds more than " + std::to_string(kCountGroupsMax) + " count groups");
    if (measure.count_groups.empty() ? measure.active_group != 0 : measure.active_group >= measure.count_groups.size()) {
        pj::wrong(where + ".active_group", "names no count group of the page");
    }
    // (the review's P2-2: the places the reader takes — pj::point_mm's —, so a book written is read)
    const auto far = [](const std::array<double, 2>& p) {
        return !std::isfinite(p[0]) || !std::isfinite(p[1]) || std::fabs(p[0]) > pj::kCoordinateMaxMm || std::fabs(p[1]) > pj::kCoordinateMaxMm;
    };
    if (measure.ruler && (far(measure.ruler->start) || far(measure.ruler->end) || (measure.ruler->protractor && far(*measure.ruler->protractor)))) {
        pj::wrong(where + ".ruler", "lies past 1000 m from the page");
    }
    for (const CountGroup& g : measure.count_groups) {
        if (std::any_of(g.points.begin(), g.points.end(), far)) pj::wrong(where, "has a count mark past 1000 m from the page");
        if (g.points.size() > kCountPointsMax) pj::wrong(where, "has a count group of more than " + std::to_string(kCountPointsMax) + " marks");
        if (g.marker_size < 1 || g.marker_size > 10 || g.label_size < 8 || g.label_size > 72) pj::wrong(where, "has a count group whose sizes are out of range");
    }
    if (measure.scale && (!(measure.scale->pixel_length > 0) || !(measure.scale->logical_length > 0))) {
        pj::wrong(where + ".scale", "must have lengths greater than 0");
    }
}

bool uses_measure(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->measure && !written.page->measure->empty()) return true;
    }
    return false;
}

void validate_measure(const Document& doc) {
    const auto check = [](const Page& page) {
        if (page.measure) check_measure(*page.measure);
    };
    for (const PagePtr& page : doc.pages)
        if (page) check(*page);
    for (const smart::WrittenPage& written : smart::written_pages(doc))
        if (!written.contents.empty()) check(*written.page);
}

// --- PC5 J-d1: the measuring's values ----------------------------------------------------------------------------------

namespace {

double r4(double v) { return py_round(v, 4); }

// (a number as the scale's words have it: a whole one without its point)
std::string trimmed(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    std::ostringstream out;
    out << std::setprecision(15) << v;
    return out.str();
}

// Photoshop's colours of new count groups, in turn (PhotoCraft's GROUP_COLORS).
constexpr std::array<std::array<int, 3>, 6> kGroupColours{{{255, 0, 0}, {0, 153, 255}, {0, 204, 0}, {255, 153, 0}, {204, 0, 204}, {255, 255, 0}}};

Json point_json(const std::array<double, 2>& p) { return Json::array({p[0], p[1]}); }

}  // namespace

double page_px_per_mm(const Page& page) {
    const double dpi = page.spec.dpi.truthy() && page.spec.dpi.value() > 0 ? page.spec.dpi.value() : 350.0;
    return dpi / 25.4;
}

bool is_default(const MeasurementScale& scale) { return scale == MeasurementScale{}; }

double scale_factor(const std::optional<MeasurementScale>& scale) {
    if (!scale || !(scale->pixel_length > 0)) return 1.0;
    return scale->logical_length / scale->pixel_length;
}

std::string describe(const MeasurementScale& scale) {
    std::ostringstream logical;
    logical << std::fixed << std::setprecision(4) << scale.logical_length;
    return trimmed(scale.pixel_length) + " pixels = " + logical.str() + " " + scale.units;
}

Json scale_json(const std::optional<MeasurementScale>& scale) {
    const MeasurementScale s = scale.value_or(MeasurementScale{});
    return Json{{"pixel_length", s.pixel_length}, {"logical_length", s.logical_length}, {"units", s.units},
                {"factor", scale_factor(s)},      {"text", describe(s)},                {"default", is_default(s)}};
}

CountGroup new_count_group(std::size_t n, const std::optional<std::string>& name, const std::optional<std::array<int, 3>>& color) {
    CountGroup g;
    g.name = name.value_or("カウントグループ " + std::to_string(n + 1));
    g.color = color.value_or(kGroupColours[n % kGroupColours.size()]);
    return g;
}

std::size_t count_total(const Measure& measure) {
    std::size_t n = 0;
    for (const CountGroup& g : measure.count_groups) n += g.points.size();
    return n;
}

Json count_json(const Measure& measure) {
    Json groups = Json::array();
    for (const CountGroup& g : measure.count_groups) {
        Json points = Json::array();
        for (const auto& p : g.points) points.push_back(point_json(p));
        groups.push_back(Json{{"name", g.name},
                              {"color", Json::array({g.color[0], g.color[1], g.color[2]})},
                              {"marker_size", g.marker_size},
                              {"label_size", g.label_size},
                              {"visible", g.visible},
                              {"count", g.points.size()},
                              {"points", std::move(points)}});
    }
    return Json{{"total", count_total(measure)}, {"active_group", measure.active_group}, {"groups", std::move(groups)}};
}

RulerReadout ruler_readout(const RulerLine& ruler, const std::optional<MeasurementScale>& scale, double px_per_mm) {
    constexpr double kDeg = 180.0 / std::numbers::pi;
    RulerReadout out;
    const double dx = ruler.end[0] - ruler.start[0], dy = ruler.end[1] - ruler.start[1];
    const double k = px_per_mm > 0 ? px_per_mm : 1.0;
    out.x_mm = r4(ruler.start[0]);
    out.y_mm = r4(ruler.start[1]);
    out.w_mm = r4(dx);
    out.h_mm = r4(dy);
    out.l1_mm = r4(std::hypot(dx, dy));
    out.x = r4(ruler.start[0] * k);
    out.y = r4(ruler.start[1] * k);
    out.w = r4(dx * k);
    out.h = r4(dy * k);
    const double l1 = std::hypot(dx, dy) * k;
    out.l1 = r4(l1);
    const double a1 = std::atan2(-dy, dx);  // (counter-clockwise, y up: Photoshop's)
    if (ruler.protractor) {
        const double ex = (*ruler.protractor)[0] - ruler.start[0], ey = (*ruler.protractor)[1] - ruler.start[1];
        out.protractor = true;
        out.l2_mm = r4(std::hypot(ex, ey));
        out.l2 = r4(std::hypot(ex, ey) * k);
        double a = std::fabs((a1 - std::atan2(-ey, ex)) * kDeg);
        if (a > 180.0) a = 360.0 - a;
        out.angle = r4(a);
    } else {
        out.angle = r4(a1 * kDeg);
    }
    out.length = r4(l1 * scale_factor(scale));
    out.units = scale ? scale->units : MeasurementScale{}.units;
    return out;
}

Json to_json(const RulerReadout& r) {
    return Json{{"x", r.x},
                {"y", r.y},
                {"w", r.w},
                {"h", r.h},
                {"angle", r.angle},
                {"l1", r.l1},
                {"l2", r.l2 ? Json(*r.l2) : Json(nullptr)},
                {"x_mm", r.x_mm},
                {"y_mm", r.y_mm},
                {"w_mm", r.w_mm},
                {"h_mm", r.h_mm},
                {"l1_mm", r.l1_mm},
                {"l2_mm", r.l2_mm ? Json(*r.l2_mm) : Json(nullptr)},
                {"length", r.length},
                {"units", r.units},
                {"protractor", r.protractor}};
}

double straighten_turn(const RulerLine& ruler) {
    return lens::straighten_angle({ruler.start[0], ruler.start[1]}, {ruler.end[0], ruler.end[1]});
}

std::array<double, 2> inscribed(double w, double h, double radians) {
    const double sa = std::fabs(std::sin(radians)), ca = std::fabs(std::cos(radians));
    const bool long_w = w >= h;
    const double long_side = long_w ? w : h, short_side = long_w ? h : w;
    if (short_side <= 2.0 * sa * ca * long_side || std::fabs(sa - ca) < 1e-10) {
        const double x = 0.5 * short_side;
        return long_w ? std::array<double, 2>{x / sa, x / ca} : std::array<double, 2>{x / ca, x / sa};
    }
    const double c2 = ca * ca - sa * sa;
    return {(w * ca - h * sa) / c2, (h * ca - w * sa) / c2};
}

}  // namespace genko::core
