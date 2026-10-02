#include "core/strokes.hpp"

#include <bit>
#include <cstdint>
#include <cstring>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// raw.get(key) or new_id()
std::string stroke_id(const Json& raw) {
    const Json* id = raw.is_object() ? get(raw, "id") : nullptr;
    if (id == nullptr || !py_truthy(*id)) return new_id();
    if (!id->is_string()) throw Error("format", "a stroke id must be a string, not " + py_repr(*id));
    return id->get<std::string>();
}

std::vector<std::int64_t> int_list(const Json& value) {
    std::vector<std::int64_t> out;
    for (const auto& v : py_list(value)) out.push_back(py_int(v));
    return out;
}

// Python's `pt[i]` for a point read from JSON.
const Json& item(const Json& pt, std::size_t i) {
    if (!pt.is_array()) throw Error("format", "a stroke point must be a list, not " + py_repr(pt));
    if (i >= pt.size()) throw Error("format", "list index out of range");
    return pt[i];
}

}  // namespace

std::string pack_doubles(std::span<const double> values) {
    std::string bytes(values.size() * 8, '\0');
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto bits = std::bit_cast<std::uint64_t>(values[i]);
        for (int b = 0; b < 8; ++b) {  // little-endian on every system
            bytes[i * 8 + static_cast<std::size_t>(b)] = static_cast<char>(bits & 0xFF);
            bits >>= 8;
        }
    }
    return b64encode(bytes);
}

std::vector<double> unpack_doubles(std::string_view base64) {
    const std::string bytes = a2b_base64(base64);
    if (bytes.size() % 8 != 0) throw Error("format", "bytes length not a multiple of item size");
    std::vector<double> out(bytes.size() / 8);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint64_t bits = 0;
        for (int b = 7; b >= 0; --b) {
            bits = (bits << 8) | static_cast<unsigned char>(bytes[i * 8 + static_cast<std::size_t>(b)]);
        }
        out[i] = std::bit_cast<double>(bits);
    }
    return out;
}

Json stroke_to_packed(const Stroke& stroke) {
    Json out = Json::object();
    out["id"] = stroke.id;
    out["kind"] = stroke.kind;
    out["width_mm"] = stroke.width_mm;
    std::vector<double> xy;
    xy.reserve(stroke.points.size() * 2);
    for (const PointF& p : stroke.points) {
        xy.push_back(p.x);
        xy.push_back(p.y);
    }
    out["xy"] = pack_doubles(xy);
    if (!stroke.pressure.empty()) out["p"] = pack_doubles(stroke.pressure);
    if (stroke.rgb) {
        Json rgb = Json::array();
        for (const auto v : *stroke.rgb) rgb.push_back(v);
        out["rgb"] = std::move(rgb);
    }
    if (stroke.opacity != 1.0) out["opacity"] = stroke.opacity;
    if (!stroke.rotation.empty()) out["r"] = pack_doubles(stroke.rotation);
    if (stroke.pressure_opacity != 0.0) out["po"] = stroke.pressure_opacity;
    return out;
}

std::string strokes_blob(const StrokeList& strokes) {
    Json list = Json::array();
    for (const StrokePtr& stroke : strokes.items) list.push_back(stroke_to_packed(*stroke));
    return dump_canonical(list);
}

Stroke coerce_stroke(const Json& raw) {
    Stroke stroke;
    if (raw.is_object() && raw.contains("xy")) {
        const Json& xy_text = raw["xy"];
        if (!xy_text.is_string()) {
            throw Error("format", "argument should be bytes, buffer or ASCII string, not '" + py_type_name(xy_text) + "'");
        }
        const std::vector<double> xy = unpack_doubles(xy_text.get_ref<const std::string&>());
        stroke.id = stroke_id(raw);
        for (std::size_t i = 0; i + 1 < xy.size(); i += 2) stroke.points.push_back(PointF{xy[i], xy[i + 1]});
        const auto unpack_key = [&](std::string_view key) -> std::vector<double> {
            const Json* v = get(raw, key);
            if (v == nullptr || !py_truthy(*v)) return {};
            if (!v->is_string()) {
                throw Error("format", "argument should be bytes, buffer or ASCII string, not '" + py_type_name(*v) + "'");
            }
            return unpack_doubles(v->get_ref<const std::string&>());
        };
        stroke.pressure = unpack_key("p");
        const Json* width = get(raw, "width_mm");
        stroke.width_mm = width ? py_float(*width) : 0.35;
        const Json* kind = get(raw, "kind");
        stroke.kind = kind && py_truthy(*kind) ? py_str(*kind) : "gpen";
        if (const Json* rgb = get(raw, "rgb"); rgb && py_truthy(*rgb)) stroke.rgb = int_list(*rgb);
        const Json* opacity = get(raw, "opacity");
        stroke.opacity = opacity ? py_float(*opacity) : 1.0;
        stroke.rotation = unpack_key("r");
        const Json* po = get(raw, "po");
        stroke.pressure_opacity = po && py_truthy(*po) ? py_float(*po) : 0.0;
        return stroke;
    }
    if (raw.is_object()) {
        const Json* points_value = get(raw, "points");
        const Json points = points_value && py_truthy(*points_value) ? py_list(*points_value) : Json::array();
        std::vector<std::pair<Json, Json>> pairs;  // tuple(pt[:2])
        for (const Json& pt : points) {
            if (!pt.is_array()) throw Error("format", "a stroke point must be a list, not " + py_repr(pt));
            if (pt.size() < 2) {
                throw Error("format", "not enough values to unpack (expected 2, got " + std::to_string(pt.size()) + ")");
            }
            pairs.emplace_back(pt[0], pt[1]);
        }
        const Json* pressure_value = get(raw, "pressure");
        if (pressure_value && py_truthy(*pressure_value)) {
            for (const Json& p : py_list(*pressure_value)) stroke.pressure.push_back(py_float(p));
        }
        if (stroke.pressure.empty()) {
            for (const Json& pt : points) {
                if (pt.size() > 2) stroke.pressure.push_back(py_float(pt[2]));
            }
        }
        stroke.id = stroke_id(raw);
        for (const auto& [x, y] : pairs) stroke.points.push_back(PointF{py_float(x), py_float(y)});
        const Json* width = get(raw, "width_mm");
        stroke.width_mm = width ? py_float(*width) : 0.35;
        const Json* kind = get(raw, "kind");
        stroke.kind = kind && py_truthy(*kind) ? py_str(*kind) : "gpen";
        if (const Json* rgb = get(raw, "rgb"); rgb && py_truthy(*rgb)) stroke.rgb = int_list(*rgb);
        const Json* opacity = get(raw, "opacity");
        stroke.opacity = opacity ? py_float(*opacity) : 1.0;
        if (const Json* rotation = get(raw, "rotation"); rotation && py_truthy(*rotation)) {
            for (const Json& v : py_list(*rotation)) stroke.rotation.push_back(py_float(v));
        }
        const Json* po = get(raw, "po");
        if (po == nullptr) po = get(raw, "pressure_opacity");
        stroke.pressure_opacity = po && py_truthy(*po) ? py_float(*po) : 0.0;
        return stroke;
    }
    if (!raw.is_array()) throw Error("format", "'" + py_type_name(raw) + "' object is not iterable");
    PenPoints points;
    for (const Json& pt : raw) {
        PenPoint p{py_float(item(pt, 0)), py_float(item(pt, 1)), std::nullopt};
        if (pt.size() > 2) p.p = py_float(pt[2]);
        points.push_back(p);
    }
    return coerce_stroke(points);
}

Stroke coerce_stroke(const PenPoints& points) {
    Stroke stroke;
    for (const PenPoint& p : points) {
        stroke.points.push_back(PointF{p.x, p.y});
        if (p.p) stroke.pressure.push_back(*p.p);
    }
    if (stroke.pressure.size() != stroke.points.size()) stroke.pressure.clear();
    stroke.id = new_id();
    return stroke;
}

PenPoints stroke_points(const Stroke& stroke) {
    PenPoints out;
    out.reserve(stroke.points.size());
    const bool with_pressure = !stroke.pressure.empty() && stroke.pressure.size() == stroke.points.size();
    for (std::size_t i = 0; i < stroke.points.size(); ++i) {
        PenPoint p{stroke.points[i].x, stroke.points[i].y, std::nullopt};
        if (with_pressure) p.p = stroke.pressure[i];
        out.push_back(p);
    }
    return out;
}

Json stroke_points_json(const Stroke& stroke) {
    Json out = Json::array();
    for (const PenPoint& p : stroke_points(stroke)) {
        Json pt = Json::array({p.x, p.y});
        if (p.p) pt.push_back(*p.p);
        out.push_back(std::move(pt));
    }
    return out;
}

}  // namespace genko::core
