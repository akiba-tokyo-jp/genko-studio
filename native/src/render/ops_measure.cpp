// PC5 J-d1 (measure@1, render/ops_measure.hpp): a page's measurement scale, count marks, ruler line and レイヤーの角度補正 —
// PhotoCraft v0.2.0's crates/engine/src/analysis_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md), in mm.

#include "render/ops_measure.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/areas.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/measure.hpp"
#include "core/production_json.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/measure_features.hpp"
#include "render/ops_annotations.hpp"
#include "render/ops_registry.hpp"

namespace genko::render::measure {

using core::CountGroup;
using core::Json;
using core::Measure;
using core::OpError;
using core::RulerLine;

namespace {

double r4(double v) { return core::py_round(v, 4); }

const Json* given(const Json& op, std::string_view key) {
    const Json* v = core::get(op, key);
    return v != nullptr && !v->is_null() ? v : nullptr;
}

std::array<double, 2> point_given(const Json& value, const std::string& key) {
    const auto bad = [&] { throw OpError(key + " is a place [x, y] (mm) on the page"); };
    if (!value.is_array() || value.size() != 2) bad();
    std::array<double, 2> out{};
    for (std::size_t i = 0; i < 2; ++i) {
        const Json& v = value[i];
        if (!v.is_number() || !std::isfinite(v.get<double>()) || std::fabs(v.get<double>()) > core::production::kCoordinateMaxMm) bad();
        out[i] = r4(v.get<double>());
    }
    return out;
}

double positive(const Json& value, const char* key) {
    if (!value.is_number() || value.is_boolean() || !std::isfinite(value.get<double>()) || !(value.get<double>() > 0) || value.get<double>() > 1e9)
        throw OpError(std::string(key) + " must be a number greater than 0");
    return value.get<double>();
}

std::size_t index_given(const Json& value, const char* key) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) throw OpError(std::string(key) + " is an index (0 the first)");
    return static_cast<std::size_t>(value.get<std::int64_t>());
}

int whole(const Json& value, const char* key, int lo, int hi) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < lo || value.get<std::int64_t>() > hi)
        throw OpError(std::string(key) + " is a whole number " + std::to_string(lo) + ".." + std::to_string(hi));
    return static_cast<int>(value.get<std::int64_t>());
}

bool flag(const Json& value, const char* key) {
    if (!value.is_boolean()) throw OpError(std::string(key) + " is true or false");
    return value.get<bool>();
}

std::array<int, 3> colour_given(const Json& value) {
    try {
        return annotations::color_given(value);
    } catch (const OpError&) {
        throw OpError("color is the count group's colour: [r, g, b] (0..255) or #rrggbb");
    }
}

std::string name_given(const Json& value) {
    if (!value.is_string()) throw OpError("name is the count group's name");
    std::string name = value.get<std::string>();
    if (name.empty() || name.size() > 1024) throw OpError("a count group's name is 1 to 1024 bytes");
    return name;
}

// The page's measuring to change (made when it has none).
Measure& measure_of(core::Document& doc, std::size_t at) {
    core::Page& page = doc.edit_page(at);
    if (!page.measure) page.measure = Measure{};
    return *page.measure;
}

// The page's measuring checked, none kept when it is all the defaults (nothing written: no measure@1).
void settle(core::Document& doc, std::size_t at) {
    core::Page& page = doc.edit_page(at);
    if (!page.measure) return;
    core::check_measure(*page.measure);
    if (page.measure->empty()) page.measure.reset();
}

const Measure& seen(const core::Document& doc, std::size_t at) {
    static const Measure none;
    const core::Page& page = doc.page(at);
    return page.measure ? *page.measure : none;
}

// The group an op names (`group`, else the active one); OpError when the page has none of it.
std::size_t group_of(const Measure& m, const Json& op) {
    const std::size_t g = given(op, "group") ? index_given(*given(op, "group"), "group") : m.active_group;
    if (g >= m.count_groups.size()) {
        throw OpError(m.count_groups.empty() ? std::string("the page has no count group") : "no count group " + std::to_string(g) + " on the page");
    }
    return g;
}

// The mark an op names: {index, group?} or the nearest visible one within radius_mm of at_mm (of `group` when given).
std::pair<std::size_t, std::size_t> mark_of(const core::Page& page, const Measure& m, const Json& op) {
    if (const Json* index = given(op, "index")) {
        const std::size_t g = group_of(m, op);
        const std::size_t i = index_given(*index, "index");
        if (i >= m.count_groups[g].points.size()) throw OpError("no count mark " + std::to_string(i) + " in count group " + std::to_string(g));
        return {g, i};
    }
    const Json* where = given(op, "at_mm");
    if (where == nullptr) throw OpError("give index (and group) or at_mm");
    const std::array<double, 2> at = point_given(*where, "at_mm");
    double radius = 6.0 / core::page_px_per_mm(page);  // (PhotoCraft's 6 pixels)
    if (const Json* r = given(op, "radius_mm")) radius = positive(*r, "radius_mm");
    std::optional<std::size_t> only;
    if (const Json* g = given(op, "group")) only = index_given(*g, "group");
    std::optional<std::pair<std::size_t, std::size_t>> best;
    double nearest = std::numeric_limits<double>::infinity();
    for (std::size_t g = 0; g < m.count_groups.size(); ++g) {
        if ((only && *only != g) || !m.count_groups[g].visible) continue;
        for (std::size_t i = 0; i < m.count_groups[g].points.size(); ++i) {
            const auto& p = m.count_groups[g].points[i];
            const double d = std::hypot(p[0] - at[0], p[1] - at[1]);
            if (d <= radius && d < nearest) {
                nearest = d;
                best = std::make_pair(g, i);
            }
        }
    }
    if (!best) throw OpError("no count mark there");
    return *best;
}

void set_measurement_scale(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    const core::MeasurementScale now = page.measure && page.measure->scale ? *page.measure->scale : core::MeasurementScale{};
    const bool lengths = given(c.op, "pixel_length") || given(c.op, "logical_length") || given(c.op, "units");
    std::string preset = "custom";
    if (const Json* p = given(c.op, "preset")) {
        if (!p->is_string()) throw OpError("preset is custom, default or paper_mm");
        preset = p->get<std::string>();
    } else if (!lengths) {
        throw OpError("give pixel_length, logical_length and units, or a preset (default, paper_mm)");
    }
    std::optional<core::MeasurementScale> next;
    if (preset == "default") {
        if (lengths) throw OpError("the default preset takes no lengths");
    } else if (preset == "paper_mm") {
        if (lengths) throw OpError("the paper_mm preset takes no lengths");
        next = core::MeasurementScale{core::py_round(core::page_px_per_mm(page), 6), 1.0, "mm"};
    } else if (preset == "custom") {
        core::MeasurementScale s = now;
        if (const Json* v = given(c.op, "pixel_length")) s.pixel_length = positive(*v, "pixel_length");
        if (const Json* v = given(c.op, "logical_length")) s.logical_length = positive(*v, "logical_length");
        if (const Json* v = given(c.op, "units")) {
            if (!v->is_string()) throw OpError("units is the logical units' name (mm, cm, μm…)");
            s.units = v->get<std::string>();
        }
        const auto blank = std::all_of(s.units.begin(), s.units.end(), [](char ch) { return ch == ' ' || ch == '\t'; });
        if (blank) throw OpError("units must not be empty");
        if (s.units.size() > 256) throw OpError("units is at most 256 bytes");
        next = s;
    } else {
        throw OpError("preset is custom, default or paper_mm");
    }
    if (next && core::is_default(*next)) next.reset();
    measure_of(c.doc, at).scale = next;
    settle(c.doc, at);
    c.report = Json{{"scale", core::scale_json(next)}};
}

void add_count(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* where = given(c.op, "at_mm");
    if (where == nullptr) throw OpError("at_mm is a place [x, y] (mm) on the page");
    const std::array<double, 2> p = point_given(*where, "at_mm");
    std::optional<std::size_t> asked;
    if (const Json* g = given(c.op, "group")) asked = index_given(*g, "group");
    const Measure& before = seen(c.doc, at);
    if (asked && *asked >= before.count_groups.size()) throw OpError("no count group " + std::to_string(*asked) + " on the page");
    const std::size_t g = asked ? *asked : before.count_groups.empty() ? 0 : before.active_group;
    if (!before.count_groups.empty() && before.count_groups[g].points.size() >= core::kCountPointsMax)
        throw OpError("a count group holds at most " + std::to_string(core::kCountPointsMax) + " marks");
    Measure& m = measure_of(c.doc, at);
    if (m.count_groups.empty()) {  // (the first mark of a page: カウントグループ 1, active — PhotoCraft's)
        m.count_groups.push_back(core::new_count_group(0));
        m.active_group = 0;
    }
    m.count_groups[g].points.push_back(p);
    const std::size_t number = m.count_groups[g].points.size();
    settle(c.doc, at);
    c.report = Json{{"number", number}, {"group", g}, {"count", core::count_json(seen(c.doc, at))}};
}

void remove_count(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const auto [g, i] = mark_of(c.doc.page(at), seen(c.doc, at), c.op);
    Measure& m = measure_of(c.doc, at);
    m.count_groups[g].points.erase(m.count_groups[g].points.begin() + static_cast<std::ptrdiff_t>(i));
    settle(c.doc, at);
    c.report = Json{{"group", g}, {"index", i}, {"count", core::count_json(seen(c.doc, at))}};
}

void move_count(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* to = given(c.op, "to");
    if (to == nullptr) throw OpError("to is a place [x, y] (mm) on the page");
    const std::array<double, 2> p = point_given(*to, "to");
    const auto [g, i] = mark_of(c.doc.page(at), seen(c.doc, at), c.op);
    measure_of(c.doc, at).count_groups[g].points[i] = p;
    settle(c.doc, at);
    c.report = Json{{"group", g}, {"index", i}, {"count", core::count_json(seen(c.doc, at))}};
}

void clear_count(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* g = given(c.op, "group");
    const bool all = g != nullptr && g->is_string() && g->get<std::string>() == "all";
    if (g != nullptr && g->is_string() && !all) throw OpError("group is an index (0 the first) or \"all\"");
    const std::optional<std::size_t> one = all ? std::nullopt : std::optional<std::size_t>(group_of(seen(c.doc, at), c.op));
    Measure& m = measure_of(c.doc, at);
    for (std::size_t k = 0; k < m.count_groups.size(); ++k)
        if (!one || *one == k) m.count_groups[k].points.clear();
    settle(c.doc, at);
    c.report = Json{{"count", core::count_json(seen(c.doc, at))}};
}

void new_count_group(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    std::optional<std::string> name;
    std::optional<std::array<int, 3>> colour;
    if (const Json* v = given(c.op, "name")) name = name_given(*v);
    if (const Json* v = given(c.op, "color")) colour = colour_given(*v);
    if (seen(c.doc, at).count_groups.size() >= core::kCountGroupsMax)
        throw OpError("a page holds at most " + std::to_string(core::kCountGroupsMax) + " count groups");
    Measure& m = measure_of(c.doc, at);
    m.count_groups.push_back(core::new_count_group(m.count_groups.size(), name, colour));
    m.active_group = m.count_groups.size() - 1;
    const std::size_t g = m.active_group;
    settle(c.doc, at);
    c.report = Json{{"group", g}, {"count", core::count_json(seen(c.doc, at))}};
}

void delete_count_group(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const std::size_t g = group_of(seen(c.doc, at), c.op);
    Measure& m = measure_of(c.doc, at);
    m.count_groups.erase(m.count_groups.begin() + static_cast<std::ptrdiff_t>(g));
    m.active_group = m.count_groups.empty() ? 0 : std::min(m.active_group, m.count_groups.size() - 1);
    settle(c.doc, at);
    c.report = Json{{"count", core::count_json(seen(c.doc, at))}};
}

void set_count_group(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const std::size_t g = group_of(seen(c.doc, at), c.op);
    CountGroup group = seen(c.doc, at).count_groups[g];
    bool any = false;
    if (const Json* v = given(c.op, "name")) {
        group.name = name_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "color")) {
        group.color = colour_given(*v);
        any = true;
    }
    if (const Json* v = given(c.op, "marker_size")) {
        group.marker_size = whole(*v, "marker_size", 1, 10);
        any = true;
    }
    if (const Json* v = given(c.op, "label_size")) {
        group.label_size = whole(*v, "label_size", 8, 72);
        any = true;
    }
    if (const Json* v = given(c.op, "visible")) {
        group.visible = flag(*v, "visible");
        any = true;
    }
    bool activate = !any && given(c.op, "group") != nullptr;  // (a group named alone: made the active one)
    if (const Json* v = given(c.op, "active")) activate = flag(*v, "active");
    if (!any && !activate) throw OpError("give group, name, color, marker_size, label_size, visible or active");
    Measure& m = measure_of(c.doc, at);
    m.count_groups[g] = std::move(group);
    if (activate) m.active_group = g;
    settle(c.doc, at);
    c.report = Json{{"count", core::count_json(seen(c.doc, at))}};
}

Json ruler_json(const core::Page& page) {
    if (!page.measure || !page.measure->ruler) return nullptr;
    const RulerLine& r = *page.measure->ruler;
    return Json{{"start", Json::array({r.start[0], r.start[1]})},
                {"end", Json::array({r.end[0], r.end[1]})},
                {"protractor", r.protractor ? Json::array({(*r.protractor)[0], (*r.protractor)[1]}) : Json(nullptr)},
                {"info", core::to_json(core::ruler_readout(r, page.measure->scale, core::page_px_per_mm(page)))}};
}

void set_ruler_line(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Measure& before = seen(c.doc, at);
    const Json* clear = given(c.op, "clear");
    const Json* start = given(c.op, "start");
    const Json* end = given(c.op, "end");
    const Json* protractor = core::get(c.op, "protractor");
    std::optional<RulerLine> next = before.ruler;
    if (clear != nullptr && flag(*clear, "clear")) {
        if (start != nullptr || end != nullptr || protractor != nullptr) throw OpError("clear takes nothing else");
        next.reset();
    } else if (start != nullptr || end != nullptr) {
        if (start == nullptr || end == nullptr) throw OpError("give both start and end: places [x, y] (mm) on the page");
        RulerLine r;
        r.start = point_given(*start, "start");
        r.end = point_given(*end, "end");
        if (protractor != nullptr && !protractor->is_null()) r.protractor = point_given(*protractor, "protractor");
        next = r;
    } else if (protractor != nullptr) {
        if (!next) throw OpError("the page has no ruler line (give start and end)");
        next->protractor = protractor->is_null() ? std::nullopt : std::optional<std::array<double, 2>>(point_given(*protractor, "protractor"));
    } else {
        throw OpError("give start and end ([x, y] mm), protractor, or clear: true");
    }
    measure_of(c.doc, at).ruler = next;
    settle(c.doc, at);
    const core::Page& page = c.doc.page(at);
    const Json line = ruler_json(page);
    c.report = Json{{"ruler", line}, {"info", line.is_null() ? Json(nullptr) : line["info"]}};
}

void straighten_layer(core::OpContext& c) {
    constexpr double kPi = std::numbers::pi;
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    if (!page.measure || !page.measure->ruler) throw OpError("the page has no ruler line (draw one with the ruler tool: set_ruler_line)");
    const Json* ids = given(c.op, "ids");
    const Json* crop_flag = given(c.op, "crop");
    // (the review's P3-2: no default that turns and cuts the page — ids or crop: true, said)
    const bool crop = crop_flag != nullptr && flag(*crop_flag, "crop");
    if (ids != nullptr && crop) throw OpError("give ids (the layers to turn) or crop: true (the page turned and cut), not both");
    if (ids == nullptr && !crop) throw OpError("give ids (the layers to turn) or crop: true (the page turned and cut)");
    if (ids != nullptr && (!ids->is_array() || ids->empty() || !std::all_of(ids->begin(), ids->end(), [](const Json& v) { return v.is_string(); })))
        throw OpError("ids is [layer id, …]");
    if (!crop) {
        for (const char* key : {"trim", "bleed_mm", "margins"})
            if (given(c.op, key)) throw OpError(std::string(key) + " is only for crop: true");
    }
    const double turn = core::straighten_turn(*page.measure->ruler);
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    if (std::fabs(turn) < 1e-9) {  // (level or upright already: nothing to turn, the line kept — PhotoCraft's)
        c.report = Json{{"angle", 0.0}, {"cropped", false}};
        return;
    }
    const Json index = page.index.json();
    if (crop) {
        // (the page turned clockwise by `turn` about its middle and cut to the largest upright rectangle inside it: crop_page's
        // box, its middle the page's, drawn turned the other way)
        const auto [cw, ch] = core::inscribed(w, h, turn * kPi / 180.0);
        const double bw = std::min(cw, w), bh = std::min(ch, h);
        Json sub{{"op", "crop_page"},
                 {"page", index},
                 {"rect", Json::array({core::py_round((w - bw) / 2, 4), core::py_round((h - bh) / 2, 4), core::py_round(bw, 4), core::py_round(bh, 4)})},
                 {"angle", core::py_round(-turn, 6)}};
        for (const char* key : {"interp", "trim", "bleed_mm", "margins"})
            if (const Json* v = given(c.op, key)) sub[key] = *v;
        core::OpContext inner{c.doc, sub, c.actor};
        (*ops_registry().find("crop_page"))(inner);
        c.notes.insert(c.notes.end(), inner.notes.begin(), inner.notes.end());
    } else {
        const double a = turn * kPi / 180.0, s = std::sin(a), co = std::cos(a), cx = w / 2, cy = h / 2;
        // (clockwise on the page, y down, about its middle — rotate_pages' way)
        Json sub{{"op", "transform_layers"},
                 {"page", index},
                 {"ids", *ids},
                 {"matrix", Json::array({co, s, -s, co, cx - (co * cx - s * cy), cy - (s * cx + co * cy)})},
                 {"box", Json::array({0.0, 0.0, w, h})}};
        if (const Json* v = given(c.op, "interp")) sub["interp"] = *v;
        core::OpContext inner{c.doc, sub, c.actor};
        (*ops_registry().find("transform_layers"))(inner);
        c.notes.insert(c.notes.end(), inner.notes.begin(), inner.notes.end());
    }
    // (the line cleared after, as Photoshop's: it was the picture's as it lay)
    if (c.doc.page(at).measure) {
        measure_of(c.doc, at).ruler.reset();
        settle(c.doc, at);
    }
    const core::Page& after = c.doc.page(at);
    c.report = Json{{"angle", r4(turn)}, {"cropped", crop}, {"paper_mm", Json::array({after.spec.width_mm.value(), after.spec.height_mm.value()})}};
}

void measure_info(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    c.report = listing(c.doc.page(at));
}

// (PC5 J-d2) 計測値を記録's values for the page, asked: the log is the app's (app/measurement_log), so the question only
// answers the rows — every data point of the source, the summary's label 計測 1.
void measure_values(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    std::string source = "auto";
    if (const Json* v = given(c.op, "source")) {
        if (!v->is_string()) throw OpError("source is auto, selection, ruler or count");
        source = v->get<std::string>();
        if (source != "auto" && source != "selection" && source != "ruler" && source != "count") throw OpError("source is auto, selection, ruler or count");
    }
    std::optional<Json> area;
    if (given(c.op, "area") != nullptr) area = core::op_area(c.op);
    if (area && source != "auto" && source != "selection") throw OpError("area is the selection: only for source selection (or auto)");
    const Recorded made = record(c.doc, c.doc.page(at), source, area, 1, now_text());
    Json columns = Json::array();
    for (const std::string& key : log_columns(made.rows)) columns.push_back(Json{{"key", key}, {"name", column_name(key)}});
    c.report = Json{{"source", made.source}, {"columns", std::move(columns)}, {"rows", made.rows}};
    if (made.grid_dpi > 0) (*c.report)["grid_dpi"] = made.grid_dpi;
    c.notes.insert(c.notes.end(), made.notes.begin(), made.notes.end());
}

// The length as the marker's label writes it: a whole number as one, else as short as it reads back.
std::string length_text(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    std::ostringstream out;
    out << std::setprecision(15) << v;
    return out.str();
}

// (PC5 J-d2) 目盛りマーカーを配置: a group 目盛りマーカー of a bar (a pen layer's rectangle, pen-shapes@1) and its label (a type
// layer, type@1) at the trim's bottom left — PhotoCraft's place_scale_marker (its bar a raster layer there) in mm, one step.
void place_scale_marker(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const core::Page& page = c.doc.page(at);
    const std::optional<core::MeasurementScale> scale = page.measure ? page.measure->scale : std::nullopt;
    const core::MeasurementScale s = scale.value_or(core::MeasurementScale{});
    const double factor = core::scale_factor(scale), ppm = core::page_px_per_mm(page);
    const core::Rect trim = page.trim_rect_mm();
    const double tx = trim.x.value(), ty = trim.y.value(), tw = trim.width.value(), th = trim.height.value();
    const double width_px = tw * ppm;
    // (none given: a "nice" length — 1, 2 or 5 × 10ⁿ — near a fifth of the trim's width, in the scale's units)
    const double length = given(c.op, "length") != nullptr ? positive(*given(c.op, "length"), "length") : nice_marker_length(page);
    const double px = length / factor;
    if (!(px >= 1.0) || px > width_px) {
        std::ostringstream said;
        said << std::fixed << std::setprecision(1) << "a " << length_text(length) << " " << s.units << " bar is " << px
             << " pixels; it must fit the trim's " << width_px << " pixels";
        throw OpError(said.str());
    }
    double size_pt = 12;
    if (const Json* v = given(c.op, "size_pt")) {
        size_pt = positive(*v, "size_pt");
        if (size_pt < 1 || size_pt > 1000) throw OpError("size_pt is 1 to 1000");
    }
    std::optional<std::string> font;
    if (const Json* v = given(c.op, "font")) {
        if (!v->is_string() || v->get<std::string>().empty()) throw OpError("font is a font's name");
        font = v->get<std::string>();
    }
    bool show_text = true;
    if (const Json* v = given(c.op, "text")) show_text = flag(*v, "text");
    bool top = false;
    if (const Json* v = given(c.op, "position")) {
        if (*v != Json("top") && *v != Json("bottom")) throw OpError("position is top or bottom");
        top = *v == Json("top");
    }
    int shade = 0;
    if (const Json* v = given(c.op, "color")) {
        if (*v != Json("black") && *v != Json("white")) throw OpError("color is black or white");
        shade = *v == Json("white") ? 255 : 0;
    }
    // (PhotoCraft's measures in the page's pixels, here in mm: a margin of 3 % of the trim's shorter side, the bar a quarter
    // of the letters high, the letters under it — or over it)
    const double size_mm = size_pt * 25.4 / 72.0, pixel = 1.0 / ppm;
    const double margin = std::max(std::min(tw, th) * 0.03, 2 * pixel);
    const double bar_h = std::clamp(size_mm / 4.0, 2 * pixel, std::max(2 * pixel, th / 4.0));
    const double text_h = show_text ? size_mm * 1.25 : 0.0;
    const double bar_y = top ? ty + th - margin - bar_h : ty + th - margin - bar_h - text_h;
    const Json rect = Json::array({r4(tx + margin), r4(bar_y), r4(px / ppm), r4(bar_h)});
    const std::string label = length_text(length) + " " + s.units;
    const Json index = page.index.json();
    const auto run = [&](const Json& sub) {
        core::OpContext inner{c.doc, sub, c.actor};
        (*ops_registry().find(sub["op"].get<std::string>()))(inner);
        c.notes.insert(c.notes.end(), inner.notes.begin(), inner.notes.end());
        return inner.report.value_or(Json::object());
    };
    const Json bar = run(Json{{"op", "add_shape_part"},
                              {"page", index},
                              {"new_layer", Json{{"name", "目盛り"}}},
                              {"shape", Json{{"kind", "rect"}, {"rect", rect}}},
                              {"fill", Json{{"rgb", Json::array({shade, shade, shade})}}}});
    const std::string bar_id = bar.at("layer").get<std::string>();
    Json ids = Json::array({bar_id});
    Json text = nullptr;
    if (show_text) {
        const double baseline = top ? bar_y - size_mm * 0.3 : bar_y + bar_h + size_mm;
        Json chars{{"rgb", Json::array({shade, shade, shade})}};
        if (font) chars["font"] = *font;
        const Json made = run(Json{{"op", "add_type_layer"},
                                   {"page", index},
                                   {"text", label},
                                   {"point", Json::array({r4(tx + margin), r4(baseline)})},
                                   {"wrap", "horizontal"},
                                   {"size_pt", size_pt},
                                   {"char", std::move(chars)},
                                   {"title", label}});
        text = made.at("layer_id");
        ids.push_back(text);
    }
    const std::string group = core::new_id();
    run(Json{{"op", "group_layers"}, {"page", index}, {"ids", ids}, {"id", group}, {"group", true}, {"name", "目盛りマーカー"}});
    c.report = Json{{"group", group}, {"bar", bar_id}, {"text", text}, {"bar_mm", rect}, {"pixels", r4(px)}, {"label", label}};
}

}  // namespace

Json listing(const core::Page& page) {
    static const Measure none;
    const Measure& m = page.measure ? *page.measure : none;
    return Json{{"page", page.index.json()},
                {"dpi", r4(core::page_px_per_mm(page) * 25.4)},
                {"scale", core::scale_json(m.scale)},
                {"ruler", ruler_json(page)},
                {"count", core::count_json(m)},
                {"notes", page.annotations.size()}};
}

}  // namespace genko::render::measure

namespace genko::render {

void register_measure_ops(core::OpRegistry& registry) {
    registry.add("set_measurement_scale", measure::set_measurement_scale);
    registry.add("add_count", measure::add_count);
    registry.add("remove_count", measure::remove_count);
    registry.add("move_count", measure::move_count);
    registry.add("clear_count", measure::clear_count);
    registry.add("new_count_group", measure::new_count_group);
    registry.add("delete_count_group", measure::delete_count_group);
    registry.add("set_count_group", measure::set_count_group);
    registry.add("set_ruler_line", measure::set_ruler_line);
    registry.add("straighten_layer", measure::straighten_layer);
    registry.add_query("measure_info", measure::measure_info);
    // (PC5 J-d2) 計測値を記録's values, asked; 目盛りマーカーを配置
    registry.add_query("measure_values", measure::measure_values);
    registry.add("place_scale_marker", measure::place_scale_marker);
}

}  // namespace genko::render
