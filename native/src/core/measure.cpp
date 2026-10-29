// measure@1 (PC5 J-0): a page's measuring, its JSON and checks (core/measure.hpp). The shapes after PhotoCraft v0.2.0's
// doc/src/analysis.rs (MIT, native/third_party/photocraft/NOTICE.md); Genko keeps them on the page, the places in mm.

#include "core/measure.hpp"

#include <limits>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
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
    for (const CountGroup& g : measure.count_groups) {
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

}  // namespace genko::core
