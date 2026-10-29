// PC5 J-d1 (render/pcraft_measure.hpp): a .pcraft's measurement as the page's, and back — PhotoCraft v0.2.0's
// doc/src/analysis.rs and format/src/manifest.rs (MIT, native/third_party/photocraft/NOTICE.md).

#include "render/pcraft_measure.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/error.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "render/pcraft_annotations.hpp"
#include "render/pcraft_manifest.hpp"

namespace genko::render::pcraft_measure {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& what) { throw core::Error("value", "measurement: " + what); }

double number(const Json& v, const char* what) {
    if (!v.is_number() || v.is_boolean() || !std::isfinite(v.get<double>())) bad(std::string(what) + " is not a number");
    return v.get<double>();
}

const Json* field(const Json& object, const char* key) {
    if (!object.is_object()) bad("not an object");
    return pcraft::optional_field(object, key);
}

std::array<double, 2> point(const Json& v, double mm, const char* what) {
    if (!v.is_array() || v.size() != 2) bad(std::string(what) + " is not [x, y]");
    const std::array<double, 2> p{core::py_round(number(v[0], what) * mm, 4), core::py_round(number(v[1], what) * mm, 4)};
    if (std::fabs(p[0]) > core::production::kCoordinateMaxMm || std::fabs(p[1]) > core::production::kCoordinateMaxMm) bad(std::string(what) + " is too far");
    return p;
}

std::int64_t whole(const Json& v, const char* what) {
    const double n = number(v, what);
    if (n != std::floor(n)) bad(std::string(what) + " is not a whole number");
    return static_cast<std::int64_t>(std::clamp(n, -1e15, 1e15));
}

}  // namespace

std::optional<core::Measure> measure_of(const Json& value, double mm, double page_px_per_mm) {
    core::Measure out;
    if (value.is_null()) return std::nullopt;
    if (const Json* scale = field(value, "scale")) {
        core::MeasurementScale s;
        if (const Json* v = field(*scale, "pixel_length")) s.pixel_length = number(*v, "pixel_length");
        if (const Json* v = field(*scale, "logical_length")) s.logical_length = number(*v, "logical_length");
        if (const Json* v = field(*scale, "units")) {
            if (!v->is_string()) bad("units is not text");
            s.units = v->get<std::string>();
        }
        if (!(s.pixel_length > 0) || !(s.logical_length > 0) || s.logical_length > 1e9) bad("the scale's lengths are not greater than 0");
        if (s.units.empty() || s.units.size() > 256) bad("the scale's units are not 1 to 256 bytes");
        if (!core::is_default(s)) {
            s.pixel_length = core::py_round(s.pixel_length * mm * page_px_per_mm, 6);  // (the document's pixels → the page's)
            if (!(s.pixel_length > 0) || s.pixel_length > 1e9) bad("the scale's pixel_length is out of range");
            out.scale = s;
        }
    }
    if (const Json* groups = field(value, "count_groups")) {
        if (!groups->is_array()) bad("count_groups is not a list");
        if (groups->size() > core::kCountGroupsMax) bad("more than 256 count groups");
        for (const Json& g : *groups) {
            core::CountGroup group = core::new_count_group(out.count_groups.size());
            group.color = {255, 0, 0};  // (PhotoCraft's default: red)
            if (const Json* v = field(g, "name")) {
                if (!v->is_string()) bad("a count group's name is not text");
                if (!v->get<std::string>().empty()) group.name = v->get<std::string>().substr(0, 1024);
            }
            if (const Json* v = field(g, "color")) {
                try {
                    group.color = pcraft_annotations::rgb_of_color(pcraft::color_from(*v));
                } catch (const pcraft::Unreadable&) {
                    bad("a count group's colour is not one");
                }
            }
            if (const Json* v = field(g, "marker_size")) group.marker_size = static_cast<int>(std::clamp<std::int64_t>(whole(*v, "marker_size"), 1, 10));
            if (const Json* v = field(g, "label_size")) group.label_size = static_cast<int>(std::clamp<std::int64_t>(whole(*v, "label_size"), 8, 72));
            if (const Json* v = field(g, "visible")) {
                if (!v->is_boolean()) bad("visible is not true or false");
                group.visible = v->get<bool>();
            }
            if (const Json* v = field(g, "points")) {
                if (!v->is_array()) bad("points is not a list");
                if (v->size() > core::kCountPointsMax) bad("a count group of more than 10000 marks");
                for (const Json& p : *v) group.points.push_back(point(p, mm, "a count mark"));
            }
            out.count_groups.push_back(std::move(group));
        }
    }
    if (const Json* v = field(value, "active_count_group")) {
        const std::int64_t n = whole(*v, "active_count_group");
        out.active_group = out.count_groups.empty() ? 0 : static_cast<std::size_t>(std::clamp<std::int64_t>(n, 0, static_cast<std::int64_t>(out.count_groups.size()) - 1));
    }
    if (const Json* r = field(value, "ruler")) {
        const Json* start = field(*r, "start");
        const Json* end = field(*r, "end");
        if (start == nullptr || end == nullptr) bad("the ruler has no start or end");
        core::RulerLine line;
        line.start = point(*start, mm, "the ruler's start");
        line.end = point(*end, mm, "the ruler's end");
        if (const Json* p = field(*r, "protractor")) line.protractor = point(*p, mm, "the ruler's protractor");
        out.ruler = line;
    }
    core::check_measure(out);
    if (out.empty()) return std::nullopt;
    return out;
}

void read(const Json& value, pcraft_production::ReadContext& context) {
    if (context.page == nullptr) return;
    std::optional<core::Measure> measure;
    try {
        measure = measure_of(value, context.mm, core::page_px_per_mm(*context.page));
    } catch (const core::Error&) {
        if (context.say) context.say("計測（カウント・ものさし）は読めない形のため外しました");
        return;
    }
    if (measure) context.page->measure = std::move(*measure);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (!page.measure || page.measure->empty()) return std::nullopt;
    const core::Measure& m = *page.measure;
    const auto px = [&](double mm) { return core::py_round(mm * context.px, 3); };
    const auto point_json = [&](const std::array<double, 2>& p) { return Json::array({px(p[0]), px(p[1])}); };
    const core::MeasurementScale s = m.scale.value_or(core::MeasurementScale{});
    // (the page's pixels → the document's)
    const double pixel_length = m.scale ? core::py_round(s.pixel_length * context.px / core::page_px_per_mm(page), 6) : s.pixel_length;
    Json groups = Json::array();
    for (const core::CountGroup& g : m.count_groups) {
        Json points = Json::array();
        for (const auto& p : g.points) points.push_back(point_json(p));
        groups.push_back(Json{{"name", g.name},
                              {"color", pcraft::to_json(pcraft::rgb_color(g.color[0] / 255.0, g.color[1] / 255.0, g.color[2] / 255.0))},
                              {"marker_size", g.marker_size},
                              {"label_size", g.label_size},
                              {"visible", g.visible},
                              {"points", std::move(points)}});
    }
    Json ruler = nullptr;
    if (m.ruler) {
        ruler = Json{{"start", point_json(m.ruler->start)},
                     {"end", point_json(m.ruler->end)},
                     {"protractor", m.ruler->protractor ? point_json(*m.ruler->protractor) : Json(nullptr)}};
    }
    return Json{{"scale", Json{{"pixel_length", pixel_length}, {"logical_length", s.logical_length}, {"units", s.units}}},
                {"count_groups", std::move(groups)},
                {"active_count_group", m.active_group},
                {"ruler", std::move(ruler)}};
}

}  // namespace genko::render::pcraft_measure
