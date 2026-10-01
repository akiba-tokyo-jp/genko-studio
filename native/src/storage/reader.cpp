#include "storage/reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/covers.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/strokes.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;
using core::Num;
using core::NumList;
using core::Point;

namespace {

constexpr std::array<std::string_view, 22> kTopKeys{
    "version", "revision", "title", "episode", "binding", "start_side", "strict_gates", "autosave",
    "font_path", "page_locks", "brush", "nombre", "spec", "bible", "tickets", "studio", "pages", "story",
    // v4
    "min_reader", "writer", "book_id", "features"};

constexpr std::array<std::string_view, 21> kPageKeys{
    "id", "art_ok", "plan", "index", "note", "name_ok", "stage", "spread_with", "numero", "onion_from", "lt_threshold",
    "effects", "ruler", "rulers", "prims", "frames", "layers", "texts", "fills", "name_strokes", "ink_strokes"};

// The keys Python reads (or recomputes, like stroke_count) at the other levels; any other key there is lost on
// saving, in Python as here.
constexpr std::array<std::string_view, 10> kSpecKeys{"width_mm", "height_mm", "dpi", "bleed_mm", "inner_margin_mm",
                                                     "trim_w_mm", "trim_h_mm", "margins_mm", "expression", "preset"};
constexpr std::array<std::string_view, 6> kBrushKeys{"rgb", "width_mm", "stabilize", "taper", "curve", "custom"};
constexpr std::array<std::string_view, 3> kBibleKeys{"plot", "characters", "constraints"};
constexpr std::array<std::string_view, 4> kRectKeys{"x", "y", "width", "height"};
constexpr std::array<std::string_view, 14> kFrameKeys{"id", "rect", "split_axis", "clip", "bleed", "border_mm",
                                                      "children", "panel", "poly", "split", "custom", "curves",
                                                      "line", "corner_mm"};
constexpr std::array<std::string_view, 19> kLineKeys{"id", "page_index", "text", "speaker", "frame_id", "ruby",
                                                     "x_mm", "y_mm", "w_mm", "h_mm", "balloon", "tail", "wrap",
                                                     "ruby_runs", "path", "emphasis_runs", "style_runs", "style",
                                                     "tails"};
constexpr std::array<std::string_view, 41> kLayerKeys{
    "id", "role", "kind", "visible", "exportable", "strokes", "raster_relpath", "fill_rgb", "lpi", "density",
    "region", "opacity", "material_id", "angle", "title", "blend", "clip", "lock_alpha", "locked", "panel_clip",
    "panel_each", "patches", "tone", "parent_id", "asset", "mask", "color", "reference", "fill", "adjust", "effect",
    "color_prints", "screen", "source", "strokes_blob", "stroke_count",
    // placed layers only
    "frame_id", "placement_mm", "fit", "clip_to", "finish"};
constexpr std::array<std::string_view, 5> kPlacedOnlyKeys{"frame_id", "placement_mm", "fit", "clip_to", "finish"};
constexpr std::array<std::string_view, 2> kMaskKeys{"enabled", "asset"};
constexpr std::array<std::string_view, 9> kPackedStrokeKeys{"id", "kind", "width_mm", "xy", "p", "rgb", "opacity", "r", "po"};
constexpr std::array<std::string_view, 10> kDictStrokeKeys{"id", "points", "pressure", "width_mm", "kind", "rgb",
                                                           "opacity", "rotation", "po", "pressure_opacity"};

template <std::size_t N>
bool in(const std::array<std::string_view, N>& keys, std::string_view key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

std::string at_key(const std::string& at, std::string_view key) { return core::json_pointer_append(at, key); }
std::string at_index(const std::string& at, std::size_t i) { return core::json_pointer_append(at, i); }

[[noreturn]] void format_error(const std::string& at, const std::string& message) {
    throw core::Error("format", message + (at.empty() ? std::string() : " (at " + at + ")"), at);
}

class Reader {
public:
    Reader(const fs::path& dir, const LoadOptions& options, LoadReport& report, const core::ParseRepairs& repairs)
        : store_(dir), dir_(dir), options_(options), report_(report) {
        for (const auto& pointer : repairs.nonfinite) {
            nonfinite_.insert(pointer);
            repair(pointer, "NaN or Infinity (Genko never writes these) read as null");
        }
        for (const auto& pointer : repairs.inexact) {
            repair(pointer, "an integer beyond 64 bits read as the nearest double");
        }
    }

    core::Document migrate(const Json& payload);

private:
    AssetStore store_;
    fs::path dir_;
    LoadOptions options_;
    LoadReport& report_;
    std::unordered_set<std::string> nonfinite_;
    std::unordered_map<std::string, core::StrokeListPtr> blobs_;  // strokes read in this load (Python's blobcache)

    void issue(std::string kind, std::string pointer, std::string ref, std::string message) {
        report_.issues.push_back(LoadIssue{std::move(kind), std::move(pointer), std::move(ref), std::move(message)});
    }
    void repair(const std::string& pointer, std::string message) { issue("repair", pointer, {}, std::move(message)); }

    bool was_nonfinite(const Json& value, const std::string& here) const {
        return value.is_null() && nonfinite_.contains(here);
    }

    template <std::size_t N>
    void check_keys(const Json& object, const std::array<std::string_view, N>& known, const std::string& at,
                    std::string_view what) {
        for (const auto& [key, value] : object.items()) {
            if (!in(known, key)) {
                issue("unknown_key", at_key(at, key), {},
                      "the " + std::string(what) + " key " + core::py_repr_str(key) +
                          " is not read by this build (nor by Python's) and would not be written back");
            }
        }
    }

    // --- values Python keeps as they were read ------------------------------------------------------------------

    bool asis_bool(const Json& object, std::string_view key, bool fallback, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr) return fallback;
        if (v->is_boolean()) return v->get<bool>();
        const std::string here = at_key(at, key);
        if (!was_nonfinite(*v, here)) {
            repair(here, "expected true or false, found " + core::py_repr(*v) + "; read as its truth value");
        }
        return core::py_truthy(*v);
    }

    std::string asis_str(const Json& object, std::string_view key, std::string fallback, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr) return fallback;
        if (v->is_string()) return v->get<std::string>();
        const std::string here = at_key(at, key);
        if (!was_nonfinite(*v, here)) repair(here, "expected a string, found " + core::py_repr(*v));
        return v->is_null() ? fallback : core::py_str(*v);
    }

    std::string required_str(const Json& object, std::string_view key, const std::string& at) {
        if (find(object, key) == nullptr) format_error(at, "missing key " + core::py_repr_str(key));
        return asis_str(object, key, {}, at);
    }

    std::optional<std::string> asis_opt_str(const Json& object, std::string_view key, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr || v->is_null()) return std::nullopt;
        if (v->is_string()) return v->get<std::string>();
        repair(at_key(at, key), "expected a string or null, found " + core::py_repr(*v));
        return core::py_str(*v);
    }

    std::optional<Num> num_of(const Json& v, const std::string& here) {
        if (auto n = Num::from_json(v)) return n;
        if (!was_nonfinite(v, here)) repair(here, "expected a number, found " + core::py_repr(v));
        return std::nullopt;
    }

    Num asis_num(const Json& object, std::string_view key, const Num& fallback, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr) return fallback;
        return num_of(*v, at_key(at, key)).value_or(fallback);
    }

    Num required_num(const Json& object, std::string_view key, const std::string& at) {
        if (find(object, key) == nullptr) format_error(at, "missing key " + core::py_repr_str(key));
        return asis_num(object, key, Num(0), at);
    }

    std::optional<Num> asis_opt_num(const Json& object, std::string_view key, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr || v->is_null()) return std::nullopt;
        return num_of(*v, at_key(at, key));
    }

    // obj.get(key) for a value Python keeps whatever it is (None when it is missing or null).
    static std::optional<Json> asis_json(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        if (v == nullptr || v->is_null()) return std::nullopt;
        return *v;
    }

    // --- values Python converts -----------------------------------------------------------------------------------

    double float_of(const Json& v, const std::string& here, double fallback) {
        if (was_nonfinite(v, here)) return fallback;
        const double d = core::py_float(v);
        if (std::isfinite(d)) return d;
        repair(here, "not a finite number; read as " + core::py_float_repr(fallback));
        return fallback;
    }

    // float(obj.get(key, fallback))
    double to_float(const Json& object, std::string_view key, double fallback, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr) return fallback;
        return float_of(*v, at_key(at, key), fallback);
    }

    // float(obj.get(key) or fallback)
    double to_float_or(const Json& object, std::string_view key, double fallback, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr || !core::py_truthy(*v)) return fallback;
        return float_of(*v, at_key(at, key), fallback);
    }

    static bool truthy_at(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        return v != nullptr && core::py_truthy(*v);
    }

    // bool(obj.get(key, fallback))
    static bool to_bool(const Json& object, std::string_view key, bool fallback) {
        const Json* v = find(object, key);
        return v == nullptr ? fallback : core::py_truthy(*v);
    }

    // str(obj.get(key) or fallback)
    static std::string to_str_or(const Json& object, std::string_view key, std::string fallback) {
        const Json* v = find(object, key);
        if (v == nullptr || !core::py_truthy(*v)) return fallback;
        return core::py_str(*v);
    }

    // What `for item in value` walks over (a list; a str's characters; a dict's keys).
    static Json iterable(const Json& value, const std::string& here) {
        if (value.is_array()) return value;
        if (value.is_string() || value.is_object()) return core::py_list(value);
        format_error(here, "'" + core::py_type_name(value) + "' object is not iterable");
    }

    // for item in obj.get(key, [])
    static Json items_at(const Json& object, std::string_view key, const std::string& at) {
        const Json* v = find(object, key);
        if (v == nullptr) return Json::array();
        return iterable(*v, at_key(at, key));
    }

    // list(obj.get(key) or [])
    static Json list_or_empty(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        if (v == nullptr || !core::py_truthy(*v)) return Json::array();
        return core::py_list(*v);
    }

    // dict(obj.get(key) or {})
    static Json dict_or_empty(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        if (v == nullptr || !core::py_truthy(*v)) return Json::object();
        return core::py_dict(*v);
    }

    // dict(obj[key]) if isinstance(obj.get(key), dict) else None
    static std::optional<Json> dict_if_dict(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        if (v == nullptr || !v->is_object()) return std::nullopt;
        return *v;
    }

    // dict(obj[key]) if obj.get(key) else None
    static std::optional<Json> dict_if_truthy(const Json& object, std::string_view key) {
        const Json* v = find(object, key);
        if (v == nullptr || !core::py_truthy(*v)) return std::nullopt;
        return core::py_dict(*v);
    }

    std::optional<Point> point_of(const Json& value, const std::string& here) {
        const Json items = iterable(value, here);
        if (items.size() == 2) {
            const auto x = Num::from_json(items[0]);
            const auto y = Num::from_json(items[1]);
            if (x && y) return Point{*x, *y};
        }
        repair(here, "expected a point [x, y], found " + core::py_repr(value));
        return std::nullopt;
    }

    // [tuple(pt) for pt in value]
    std::vector<Point> points_of(const Json& value, const std::string& here) {
        std::vector<Point> out;
        const Json items = iterable(value, here);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (auto p = point_of(items[i], at_index(here, i))) out.push_back(*p);
        }
        return out;
    }

    // tuple(value), every item a number
    NumList numbers_of(const Json& value, const std::string& here) {
        NumList out;
        const Json items = iterable(value, here);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (auto n = num_of(items[i], at_index(here, i))) out.push_back(*n);
        }
        return out;
    }

    std::vector<std::int64_t> ints_of(const Json& value, const std::string& here) {
        std::vector<std::int64_t> out;
        const Json items = iterable(value, here);
        for (const Json& item : items) out.push_back(core::py_int(item));
        return out;
    }

    // --- parts of a book -----------------------------------------------------------------------------------------

    core::PageSpec read_spec(const Json& raw);
    core::Rect read_rect(const Json& data, const std::string& at);
    core::Frame read_frame(const Json& data, const std::string& at);
    core::StoryLine read_line(const Json& data, const std::string& at);
    core::Layer read_layer(const Json& data, const std::string& at);
    core::Stroke read_stroke(const Json& raw, const std::string& at);
    core::StrokeListPtr read_strokes_blob(const Json& ref_value, const std::string& at);
    core::Bytes read_png(const Json& ref_value, const std::string& at, core::BlobMemo* memo);
    core::Bytes read_legacy_raster(const std::string& relpath, const std::string& at);
};

core::PageSpec Reader::read_spec(const Json& raw) {
    const std::string at = "/spec";
    if (!raw.is_object()) format_error(at, "the spec must be an object, not " + core::py_repr(raw));
    core::PageSpec spec;
    spec.width_mm = required_num(raw, "width_mm", at);
    spec.height_mm = required_num(raw, "height_mm", at);
    spec.dpi = required_num(raw, "dpi", at);
    spec.bleed_mm = required_num(raw, "bleed_mm", at);
    spec.inner_margin_mm = required_num(raw, "inner_margin_mm", at);
    spec.trim_w_mm = asis_opt_num(raw, "trim_w_mm", at);
    spec.trim_h_mm = asis_opt_num(raw, "trim_h_mm", at);
    if (const Json* margins = find(raw, "margins_mm"); margins && core::py_truthy(*margins)) {
        const std::string here = at_key(at, "margins_mm");
        NumList list;
        const Json items = iterable(*margins, here);
        for (std::size_t i = 0; i < items.size(); ++i) list.push_back(Num(float_of(items[i], at_index(here, i), 0.0)));
        spec.margins_mm = std::move(list);
    }
    spec.expression = asis_str(raw, "expression", "mono", at);
    spec.preset = asis_opt_str(raw, "preset", at);
    check_keys(raw, kSpecKeys, at, "spec");
    return spec;
}

core::Rect Reader::read_rect(const Json& data, const std::string& at) {
    if (!data.is_object()) format_error(at, "a rect must be an object, not " + core::py_repr(data));
    core::Rect rect;
    rect.x = required_num(data, "x", at);
    rect.y = required_num(data, "y", at);
    rect.width = required_num(data, "width", at);
    rect.height = required_num(data, "height", at);
    check_keys(data, kRectKeys, at, "rect");
    return rect;
}

core::Frame Reader::read_frame(const Json& data, const std::string& at) {
    if (!data.is_object()) format_error(at, "a frame must be an object, not " + core::py_repr(data));
    core::Frame frame;
    frame.id = required_str(data, "id", at);
    const Json* rect = find(data, "rect");
    if (rect == nullptr) format_error(at, "missing key 'rect'");
    frame.rect = read_rect(*rect, at_key(at, "rect"));
    frame.split_axis = asis_opt_str(data, "split_axis", at);
    const Json children = items_at(data, "children", at);
    for (std::size_t i = 0; i < children.size(); ++i) {
        frame.children.push_back(read_frame(children[i], at_index(at_key(at, "children"), i)));
    }
    frame.clip = asis_bool(data, "clip", true, at);
    frame.bleed = asis_bool(data, "bleed", false, at);
    frame.border_mm = to_float(data, "border_mm", 0.8, at);
    frame.panel = asis_json(data, "panel");
    if (const Json* poly = find(data, "poly"); poly && core::py_truthy(*poly)) {
        frame.poly = points_of(*poly, at_key(at, "poly"));
    }
    frame.split = dict_if_truthy(data, "split");
    frame.custom = to_bool(data, "custom", false);
    if (const Json* curves = find(data, "curves"); curves && curves->is_array()) {
        std::vector<double> list;
        const std::string here = at_key(at, "curves");
        for (std::size_t i = 0; i < curves->size(); ++i) list.push_back(float_of((*curves)[i], at_index(here, i), 0.0));
        frame.curves = std::move(list);
    }
    frame.line = dict_if_dict(data, "line");
    frame.corner_mm = to_float_or(data, "corner_mm", 0.0, at);
    check_keys(data, kFrameKeys, at, "frame");
    return frame;
}

core::StoryLine Reader::read_line(const Json& data, const std::string& at) {
    if (!data.is_object()) format_error(at, "a line must be an object, not " + core::py_repr(data));
    core::StoryLine line;
    line.id = required_str(data, "id", at);
    line.page_index = required_num(data, "page_index", at);
    line.text = required_str(data, "text", at);
    line.speaker = asis_str(data, "speaker", "", at);
    line.frame_id = asis_opt_str(data, "frame_id", at);
    line.ruby = asis_str(data, "ruby", "", at);
    line.x_mm = Num(to_float(data, "x_mm", 0.0, at));
    line.y_mm = Num(to_float(data, "y_mm", 0.0, at));
    line.w_mm = Num(to_float(data, "w_mm", 40.0, at));
    line.h_mm = Num(to_float(data, "h_mm", 20.0, at));
    line.balloon = asis_str(data, "balloon", "speech", at);
    if (const Json* tail = find(data, "tail"); tail && core::py_truthy(*tail)) {
        line.tail = point_of(*tail, at_key(at, "tail"));
    }
    line.wrap = asis_str(data, "wrap", "horizontal", at);
    for (const Json& item : list_or_empty(data, "ruby_runs")) {
        line.ruby_runs.push_back(iterable(item, at_key(at, "ruby_runs")));
    }
    if (const Json* path = find(data, "path"); path && core::py_truthy(*path)) {
        line.path = points_of(*path, at_key(at, "path"));
    }
    for (const Json& item : list_or_empty(data, "emphasis_runs")) line.emphasis_runs.push_back(core::py_str(item));
    for (const Json& run : list_or_empty(data, "style_runs")) {
        if (!core::py_truthy(run)) continue;
        const Json items = iterable(run, at_key(at, "style_runs"));
        if (items.size() <= 1) continue;
        if (run.is_object()) format_error(at_key(at, "style_runs"), "a style run must be [words, {style}]");
        line.style_runs.emplace_back(core::py_str(items[0]), core::py_dict(items[1]));
    }
    line.style = dict_or_empty(data, "style");
    for (const Json& tail : list_or_empty(data, "tails")) line.tails.push_back(core::py_dict(tail));
    check_keys(data, kLineKeys, at, "line");
    return line;
}

core::Stroke Reader::read_stroke(const Json& raw, const std::string& at) {
    if (raw.is_object()) {
        if (raw.contains("xy")) {
            check_keys(raw, kPackedStrokeKeys, at, "stroke");
        } else {
            check_keys(raw, kDictStrokeKeys, at, "stroke");
        }
    }
    return core::coerce_stroke(raw);
}

core::Bytes Reader::read_png(const Json& ref_value, const std::string& at, core::BlobMemo* memo) {
    if (!ref_value.is_string() || !AssetStore::is_ref(ref_value.get_ref<const std::string&>())) {
        issue("broken_ref", at, core::py_str(ref_value), "not an asset ref: " + core::py_repr(ref_value));
        return nullptr;
    }
    const std::string& ref = ref_value.get_ref<const std::string&>();
    if (options_.cache) {
        if (const auto it = options_.cache->pictures.find(ref); it != options_.cache->pictures.end()) {
            if (memo != nullptr && options_.verify_hashes) memo->remember(it->second, ref);
            return it->second;
        }
    }
    auto bytes = store_.get_bytes(ref, ".png");
    if (!bytes) {
        issue("missing_asset", at, ref, "the picture " + AssetStore::relpath(ref, ".png") + " is missing");
        return nullptr;
    }
    auto shared = std::make_shared<const std::string>(std::move(*bytes));
    if (options_.verify_hashes) {
        std::string actual = AssetStore::ref(*shared);
        if (actual != ref) {
            issue("hash_mismatch", at, ref,
                  "the picture " + AssetStore::relpath(ref, ".png") + " does not hold the bytes its name says (" +
                      actual + ")");
        } else if (options_.cache) {
            options_.cache->pictures.emplace(ref, shared);
        }
        if (memo != nullptr) memo->remember(shared, std::move(actual));
    }
    return shared;
}

core::Bytes Reader::read_legacy_raster(const std::string& relpath, const std::string& at) {
    // io._attach_legacy_rasters: a v2 layer's picture under pages/NNN/. Python reads any path it is given; here it
    // must stay inside the book.
    const fs::path rel = path_from_utf8(relpath);
    bool safe = !relpath.empty() && rel.is_relative() && !rel.has_root_name() && !rel.has_root_directory() &&
                relpath.find('\\') == std::string::npos;
    for (const auto& part : rel) {
        if (part == "..") safe = false;
    }
    if (!safe) {
        issue("broken_ref", at, relpath, "the picture path " + core::py_repr_str(relpath) + " leaves the book; not read");
        return nullptr;
    }
    const fs::path full = dir_ / rel;
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) {
        issue("missing_asset", at, relpath, "the picture " + relpath + " is missing");
        return nullptr;
    }
    return std::make_shared<const std::string>(read_file(full));
}

core::StrokeListPtr Reader::read_strokes_blob(const Json& ref_value, const std::string& at) {
    if (!ref_value.is_string() || !AssetStore::is_ref(ref_value.get_ref<const std::string&>())) {
        issue("broken_ref", at, core::py_str(ref_value), "not an asset ref: " + core::py_repr(ref_value));
        return nullptr;
    }
    const std::string& ref = ref_value.get_ref<const std::string&>();
    if (auto it = blobs_.find(ref); it != blobs_.end()) return it->second;
    if (options_.cache) {
        if (const auto it = options_.cache->strokes.find(ref); it != options_.cache->strokes.end()) {
            blobs_.emplace(ref, it->second);
            return it->second;
        }
    }
    const auto bytes = store_.get_bytes(ref, ".strokes.json");
    if (!bytes) {
        issue("missing_asset", at, ref, "the strokes " + AssetStore::relpath(ref, ".strokes.json") + " are missing");
        return nullptr;
    }
    bool verified = false;
    if (options_.verify_hashes) {
        const std::string actual = AssetStore::ref(*bytes);
        if (actual != ref) {
            issue("hash_mismatch", at, ref,
                  "the strokes " + AssetStore::relpath(ref, ".strokes.json") + " do not hold the bytes their name says (" +
                      actual + ")");
        } else {
            verified = true;
        }
    }
    std::vector<core::StrokePtr> items;
    const std::size_t issues_before = report_.issues.size();
    try {
        core::ParseRepairs repairs;
        core::ParseOptions parse;
        parse.bytes = true;
        const Json list = core::parse_python_json(*bytes, &repairs, parse);
        if (!repairs.nonfinite.empty() || !repairs.inexact.empty()) {
            issue("broken_asset", at, ref, "the strokes hold NaN, Infinity or an integer beyond 64 bits");
        }
        const Json strokes = iterable(list, at);
        for (std::size_t i = 0; i < strokes.size(); ++i) {
            items.push_back(std::make_shared<const core::Stroke>(read_stroke(strokes[i], at)));
        }
    } catch (const core::Error& error) {
        report_.issues.resize(issues_before);
        issue("broken_asset", at, ref,
              "the strokes " + AssetStore::relpath(ref, ".strokes.json") + " cannot be read: " + error.what());
        return nullptr;
    }
    auto list = core::make_strokes(std::move(items), ref);
    blobs_.emplace(ref, list);
    if (options_.cache && verified && report_.issues.size() == issues_before) options_.cache->strokes.emplace(ref, list);
    return list;
}

core::Layer Reader::read_layer(const Json& data, const std::string& at) {
    if (!data.is_object()) format_error(at, "a layer must be an object, not " + core::py_repr(data));
    const Json* role_value = find(data, "role");
    if (role_value == nullptr) format_error(at, "missing key 'role'");
    const auto role = role_value->is_string() ? core::layer_role_from(role_value->get<std::string>()) : std::nullopt;
    if (!role) format_error(at_key(at, "role"), core::py_repr(*role_value) + " is not a valid LayerRole");

    // _layer_fields
    core::Layer layer;
    if (const Json* id = find(data, "id"); id && core::py_truthy(*id)) {
        layer.id = asis_str(data, "id", {}, at);
    } else {
        layer.id = core::new_id();
    }
    layer.role = *role;
    const Json kind_value = find(data, "kind") ? *find(data, "kind") : Json("strokes");
    const auto kind = kind_value.is_string() ? core::layer_kind_from(kind_value.get<std::string>()) : std::nullopt;
    if (!kind) format_error(at_key(at, "kind"), core::py_repr(kind_value) + " is not a valid LayerKind");
    layer.kind = *kind;
    layer.visible = asis_bool(data, "visible", true, at);
    layer.exportable = asis_bool(data, "exportable", *role != core::LayerRole::Name && *role != core::LayerRole::Draft, at);
    {
        const Json inline_strokes = items_at(data, "strokes", at);
        std::vector<core::StrokePtr> items;
        for (std::size_t i = 0; i < inline_strokes.size(); ++i) {
            items.push_back(std::make_shared<const core::Stroke>(
                read_stroke(inline_strokes[i], at_index(at_key(at, "strokes"), i))));
        }
        layer.strokes = core::make_strokes(std::move(items));
    }
    layer.raster_relpath = asis_opt_str(data, "raster_relpath", at);
    if (const Json* rgb = find(data, "fill_rgb"); rgb && core::py_truthy(*rgb)) {
        layer.fill_rgb = numbers_of(*rgb, at_key(at, "fill_rgb"));
    }
    layer.lpi = asis_opt_num(data, "lpi", at);
    layer.density = asis_opt_num(data, "density", at);
    if (const Json* region = find(data, "region"); region && core::py_truthy(*region)) {
        layer.region = points_of(*region, at_key(at, "region"));
    }
    layer.opacity = to_float(data, "opacity", 1.0, at);
    layer.material_id = asis_opt_str(data, "material_id", at);
    layer.angle = to_float(data, "angle", 45.0, at);
    layer.title = to_str_or(data, "title", "");
    layer.blend = to_str_or(data, "blend", "normal");
    layer.clip = to_bool(data, "clip", false);
    layer.lock_alpha = to_bool(data, "lock_alpha", false);
    layer.locked = to_bool(data, "locked", false);
    layer.panel_clip = to_bool(data, "panel_clip", true);
    layer.panel_each = to_bool(data, "panel_each", false);
    layer.tone = dict_if_truthy(data, "tone");
    layer.parent_id = asis_opt_str(data, "parent_id", at);
    if (const Json* color = find(data, "color"); color && core::py_truthy(*color)) {
        auto values = ints_of(*color, at_key(at, "color"));
        if (values.size() > 3) values.resize(3);
        layer.color = std::move(values);
    }
    layer.reference = to_bool(data, "reference", false);
    layer.fill = dict_if_dict(data, "fill");
    layer.adjust = dict_if_dict(data, "adjust");
    layer.effect = dict_if_dict(data, "effect");
    layer.color_prints = to_bool(data, "color_prints", false);
    layer.screen = dict_if_dict(data, "screen");
    layer.source = dict_if_dict(data, "source");

    const bool placed = layer.kind == core::LayerKind::Placed;
    if (placed) {
        layer.asset = asis_opt_str(data, "asset", at);
        layer.frame_id = asis_opt_str(data, "frame_id", at);
        if (const Json* placement = find(data, "placement_mm"); placement && core::py_truthy(*placement)) {
            layer.placement_mm = read_rect(*placement, at_key(at, "placement_mm"));
        }
        layer.fit = truthy_at(data, "fit") ? asis_str(data, "fit", "cover", at) : "cover";
        layer.clip_to = truthy_at(data, "clip_to") ? asis_str(data, "clip_to", "frame", at) : "frame";
        layer.source = asis_json(data, "source");
        layer.finish = asis_json(data, "finish");
    }
    if (const Json* mask = find(data, "mask"); mask && core::py_truthy(*mask)) {
        const std::string here = at_key(at, "mask");
        if (!mask->is_object()) format_error(here, "a mask must be an object, not " + core::py_repr(*mask));
        if (const Json* asset = find(*mask, "asset"); asset && core::py_truthy(*asset)) {
            core::Mask m;
            auto png = read_png(*asset, at_key(here, "asset"), &m.memo);
            if (png && !png->empty()) {
                m.png = std::move(png);
                m.enabled = to_bool(*mask, "enabled", true);
                layer.mask = std::move(m);
            }
        }
        check_keys(*mask, kMaskKeys, here, "mask");
    }
    check_keys(data, kLayerKeys, at, "layer");
    if (!placed) {
        for (const auto& key : kPlacedOnlyKeys) {
            if (find(data, key) != nullptr) {
                issue("unknown_key", at_key(at, key), {},
                      "the key " + core::py_repr_str(key) + " is read only for placed layers; this " +
                          std::string(core::to_string(layer.kind)) + " layer's would not be written back");
            }
        }
    }
    if (placed) {
        // A placed layer keeps only its picture's ref and mask: Python drops its strokes, patches and raster on
        // saving (and reads its strokes all the same).
        for (const auto* key : {"strokes", "patches", "strokes_blob", "raster_relpath"}) {
            if (truthy_at(data, key)) {
                issue("unknown_key", at_key(at, key), {},
                      "a placed layer's " + std::string(key) + " is not written back (Python drops it too)");
            }
        }
        return layer;
    }

    bool from_asset = false;
    if (const Json* asset = find(data, "asset"); asset && core::py_truthy(*asset)) {
        from_asset = true;
        if (asset->is_string() && AssetStore::is_ref(asset->get_ref<const std::string&>())) {
            layer.raster_relpath = AssetStore::relpath(asset->get_ref<const std::string&>(), ".png");
        }
        layer.raster_png = read_png(*asset, at_key(at, "asset"), &layer.raster_memo);
    }
    if (!from_asset && !layer.raster_png && layer.raster_relpath) {
        layer.raster_png = read_legacy_raster(*layer.raster_relpath, at_key(at, "raster_relpath"));
    }
    if (const Json* patches = find(data, "patches"); patches && core::py_truthy(*patches)) {
        const std::string here = at_key(at, "patches");
        const Json items = iterable(*patches, here);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Json& patch = items[i];
            const std::string patch_at = at_index(here, i);
            if (!patch.is_object()) format_error(patch_at, "a patch must be an object, not " + core::py_repr(patch));
            core::Patch item;
            for (const auto& [key, value] : patch.items()) {
                if (key == "asset") continue;
                if (key == "png") {
                    repair(at_key(patch_at, "png"), "a patch's picture belongs in assets/; the inline \"png\" is dropped");
                    continue;
                }
                item.attrs[key] = value;
            }
            if (const Json* asset = find(patch, "asset"); asset && core::py_truthy(*asset)) {
                item.png = read_png(*asset, at_key(patch_at, "asset"), &item.memo);
                if (asset->is_string()) item.asset = asset->get<std::string>();
            }
            layer.patches.push_back(std::move(item));
        }
    }
    if (const Json* blob = find(data, "strokes_blob"); blob && core::py_truthy(*blob)) {
        if (auto list = read_strokes_blob(*blob, at_key(at, "strokes_blob"))) layer.strokes = std::move(list);
    }
    return layer;
}

core::Document Reader::migrate(const Json& payload) {
    if (!payload.is_object()) format_error("", "project.json must hold an object, not " + core::py_repr(payload));

    // The version: an int (Python: a bool is one too) no newer than this build.
    const std::int64_t version = project_version(payload);
    report_.source_version = static_cast<int>(version);

    const Json* spec_raw = find(payload, "spec");
    if (spec_raw == nullptr) format_error("", "missing key 'spec'");
    const core::PageSpec spec = read_spec(*spec_raw);
    const Json binding_value = find(payload, "binding") ? *find(payload, "binding") : Json("right");
    const auto binding = binding_value.is_string() ? core::binding_from(binding_value.get<std::string>()) : std::nullopt;
    if (!binding) format_error("/binding", core::py_repr(binding_value) + " is not a valid Binding");

    std::vector<core::StoryLine> story;
    {
        const Json lines = items_at(payload, "story", "");
        for (std::size_t i = 0; i < lines.size(); ++i) story.push_back(read_line(lines[i], at_index("/story", i)));
    }
    const bool had_story = !story.empty();
    std::vector<core::StoryLine> page_texts;

    const Json* pages_raw = find(payload, "pages");
    if (pages_raw == nullptr) format_error("", "missing key 'pages'");
    const Json pages = iterable(*pages_raw, "/pages");
    std::vector<core::PagePtr> page_list;
    for (std::size_t p = 0; p < pages.size(); ++p) {
        const Json& raw = pages[p];
        const std::string at = at_index("/pages", p);
        if (!raw.is_object()) format_error(at, "a page must be an object, not " + core::py_repr(raw));
        core::Page page;
        // Page(id=raw.get("id") or "pg_" + new_id(), art_ok=…, plan=…, index=raw["index"], frames=[…], …)
        if (truthy_at(raw, "id")) {
            page.id = asis_str(raw, "id", {}, at);
        } else {
            page.id = core::new_page_id();
        }
        page.art_ok = to_bool(raw, "art_ok", false);
        page.plan = asis_json(raw, "plan");
        page.index = required_num(raw, "index", at);
        page.spec = spec;
        if (find(raw, "frames") == nullptr) format_error(at, "missing key 'frames'");
        {
            const Json frames = iterable(*find(raw, "frames"), at_key(at, "frames"));
            for (std::size_t i = 0; i < frames.size(); ++i) {
                page.frames.push_back(read_frame(frames[i], at_index(at_key(at, "frames"), i)));
            }
        }
        page.binding = *binding;
        page.note = asis_str(raw, "note", "", at);
        page.name_ok = asis_bool(raw, "name_ok", false, at);
        page.stage = asis_str(raw, "stage", "name", at);
        page.spread_with = asis_opt_num(raw, "spread_with", at);
        page.numero = asis_bool(raw, "numero", true, at);
        page.effects = list_or_empty(raw, "effects");
        page.ruler = asis_json(raw, "ruler");
        page.rulers = list_or_empty(raw, "rulers");
        page.prims = list_or_empty(raw, "prims");
        page.onion_from = asis_opt_num(raw, "onion_from", at);
        page.lt_threshold = asis_opt_num(raw, "lt_threshold", at);
        page.layers = core::default_layers();  // (Page.__post_init__: a page starts with the default layers)

        const Json* fills_raw = find(raw, "fills");
        const Json fills = fills_raw ? *fills_raw : Json::object();
        if (!fills.is_object()) format_error(at_key(at, "fills"), "fills must be an object, not " + core::py_repr(fills));
        for (const auto& [role_name, rgb] : fills.items()) {
            const auto role = core::layer_role_from(role_name);
            if (!role) format_error(at_key(at, "fills"), core::py_repr_str(role_name) + " is not a valid LayerRole");
            page.fills.emplace_back(*role, numbers_of(rgb, at_key(at_key(at, "fills"), role_name)));
        }
        if (truthy_at(raw, "layers")) {
            const Json layers = iterable(*find(raw, "layers"), at_key(at, "layers"));
            page.layers.clear();
            for (std::size_t i = 0; i < layers.size(); ++i) {
                page.layers.push_back(read_layer(layers[i], at_index(at_key(at, "layers"), i)));
            }
        } else {
            // v1: no layers; the name and ink lines are lists of points on the page
            page.layers = core::default_layers();
            for (const auto* key : {"name_strokes", "ink_strokes"}) {
                const std::string here = at_key(at, key);
                const Json strokes = items_at(raw, key, at);
                std::vector<core::StrokePtr> items;
                for (std::size_t i = 0; i < strokes.size(); ++i) {
                    Json points = Json::array();
                    for (const Json& pt : iterable(strokes[i], at_index(here, i))) points.push_back(iterable(pt, here));
                    items.push_back(std::make_shared<const core::Stroke>(core::coerce_stroke(points)));
                }
                const auto role = std::string_view(key) == "name_strokes" ? core::LayerRole::Name : core::LayerRole::Ink;
                page.layer_for(role).strokes = core::make_strokes(std::move(items));
            }
            const auto fills_copy = page.fills;
            for (const auto& [role, rgb] : fills_copy) page.paint(role, rgb);
        }
        if (!had_story) {
            const Json texts = items_at(raw, "texts", at);
            for (std::size_t i = 0; i < texts.size(); ++i) {
                page_texts.push_back(read_line(texts[i], at_index(at_key(at, "texts"), i)));
            }
        }
        for (const auto& [key, value] : raw.items()) {
            if (!in(kPageKeys, key)) page.extra[key] = value;
        }
        if (const Json* cover = find(page.extra, "cover"); cover && cover->is_object()) {
            page.spec = core::spec_for(spec, *cover);
        }
        page_list.push_back(std::make_shared<core::Page>(std::move(page)));
    }
    if (!had_story) story = std::move(page_texts);

    core::Document doc;
    const Json* title = find(payload, "title");
    if (title == nullptr) format_error("", "missing key 'title'");
    doc.title = asis_str(payload, "title", {}, "");
    doc.episode = required_num(payload, "episode", "");
    doc.spec = spec;
    doc.binding = *binding;
    doc.pages = std::move(page_list);
    doc.story = std::move(story);
    doc.tickets = list_or_empty(payload, "tickets");
    doc.autosave = to_bool(payload, "autosave", false);
    doc.font_path = to_str_or(payload, "font_path", "");
    doc.page_locks = dict_or_empty(payload, "page_locks");

    if (const Json* brush = find(payload, "brush"); brush && core::py_truthy(*brush)) {
        if (!brush->is_object()) format_error("/brush", "the brush must be an object, not " + core::py_repr(*brush));
        if (truthy_at(*brush, "rgb")) doc.brush_rgb = ints_of(*find(*brush, "rgb"), "/brush/rgb");
        doc.brush_width_mm = to_float(*brush, "width_mm", doc.brush_width_mm, "/brush");
        doc.brush_stabilize = truthy_at(*brush, "stabilize") ? core::py_int(*find(*brush, "stabilize")) : 0;
        doc.brush_taper = to_bool(*brush, "taper", false);
        doc.brush_curve = to_str_or(*brush, "curve", "linear");
        const Json custom = dict_or_empty(*brush, "custom");
        for (const auto& [name, value] : custom.items()) doc.brush_custom[name] = core::py_dict(value);
        check_keys(*brush, kBrushKeys, "/brush", "brush");
    }
    doc.nombre = dict_or_empty(payload, "nombre");
    if (const Json* bible = find(payload, "bible"); bible && core::py_truthy(*bible)) {
        if (!bible->is_object()) format_error("/bible", "the bible must be an object, not " + core::py_repr(*bible));
        doc.bible.plot = asis_str(*bible, "plot", "", "/bible");
        doc.bible.characters = list_or_empty(*bible, "characters");
        doc.bible.constraints = list_or_empty(*bible, "constraints");
        check_keys(*bible, kBibleKeys, "/bible", "bible");
    }
    for (const auto& [key, value] : payload.items()) {
        if (!in(kTopKeys, key)) doc.extra[key] = value;
    }
    doc.revision = truthy_at(payload, "revision") ? core::py_int(*find(payload, "revision")) : 0;
    doc.start_side = asis_opt_str(payload, "start_side", "");
    doc.strict_gates = to_bool(payload, "strict_gates", false);
    doc.studio = dict_or_empty(payload, "studio");

    // v2 keyed locks by page number; v3 by page id.
    {
        std::unordered_map<std::string, std::string> by_index;
        for (const auto& page : doc.pages) by_index[page->index.repr()] = page->id;
        Json locks = Json::object();
        for (const auto& [key, owner] : doc.page_locks.items()) {
            const auto it = by_index.find(key);
            locks[it != by_index.end() ? it->second : key] = owner;
        }
        doc.page_locks = std::move(locks);
    }

    // v4
    if (const Json* id = find(payload, "book_id"); id && id->is_string() && core::is_book_id(id->get<std::string>())) {
        doc.book_id = id->get<std::string>();
    } else {
        if (version >= 4) repair("/book_id", "no valid book_id (32 hex digits): a new one was made");
        doc.book_id = core::new_book_id();
    }
    if (const Json* features = find(payload, "features"); features && !features->is_null()) {
        if (!features->is_array()) {
            repair("/features", "features must be a list of strings, found " + core::py_repr(*features));
        } else {
            for (std::size_t i = 0; i < features->size(); ++i) {
                const Json& f = (*features)[i];
                if (!f.is_string()) {
                    repair(at_index("/features", i), "a feature must be a string, found " + core::py_repr(f));
                    continue;
                }
                doc.features.push_back(f.get<std::string>());
            }
            std::sort(doc.features.begin(), doc.features.end());
            doc.features.erase(std::unique(doc.features.begin(), doc.features.end()), doc.features.end());
        }
    }
    for (const auto& feature : doc.features) {
        if (!is_known_feature(feature)) {
            issue("unknown_feature", "/features", {}, "this build does not know the feature " + core::py_repr_str(feature));
        }
    }
    return doc;
}

std::string read_only_reason(const LoadReport& report) {
    std::vector<std::string> unknown_features;
    std::size_t problems = 0;
    const LoadIssue* first = nullptr;
    for (const auto& issue : report.issues) {
        if (issue.kind == "unknown_feature") {
            unknown_features.push_back(issue.message);
            continue;
        }
        ++problems;
        if (first == nullptr) first = &issue;
    }
    std::string reason;
    if (!unknown_features.empty()) {
        reason = "this book uses features this build does not support";
        for (std::size_t i = 0; i < unknown_features.size(); ++i) reason += (i == 0 ? ": " : "; ") + unknown_features[i];
    }
    if (problems > 0) {
        if (!reason.empty()) reason += ". ";
        reason += "this book needs repairs before it can be saved (" + std::to_string(problems) + " problem" +
                  (problems == 1 ? "" : "s") + "; first: " + first->kind + " at " +
                  (first->pointer.empty() ? "/" : first->pointer) + ": " + first->message + ")";
    }
    return reason;
}

}  // namespace

UnsupportedProjectVersion::UnsupportedProjectVersion(const std::string& message)
    : core::Error("unsupported_version", message) {}

Json LoadIssue::to_json() const {
    Json out = Json::object();
    out["kind"] = kind;
    out["pointer"] = pointer;
    if (!ref.empty()) out["ref"] = ref;
    out["message"] = message;
    return out;
}

Json LoadReport::to_json() const {
    Json out = Json::object();
    out["source_version"] = source_version;
    Json list = Json::array();
    for (const auto& issue : issues) list.push_back(issue.to_json());
    out["issues"] = std::move(list);
    return out;
}

bool is_known_top_key(std::string_view key) { return in(kTopKeys, key); }
bool is_known_page_key(std::string_view key) { return in(kPageKeys, key); }
bool is_known_feature(std::string_view) { return false; }

int project_version(const Json& payload) {
    std::int64_t version = 1;
    if (const Json* v = find(payload, "version")) {
        if (v->is_boolean()) {
            version = v->get<bool>() ? 1 : 0;
        } else if (v->is_number_integer() && !v->is_number_unsigned()) {
            version = v->get<std::int64_t>();
        } else {
            version = kReadableVersion + 1;  // not an int, or an int too big for this build
        }
        if (version > kReadableVersion) {
            throw UnsupportedProjectVersion("project.json version " + core::py_str(*v) +
                                            " is newer than this build supports (" + std::to_string(kReadableVersion) +
                                            "); update Genko");
        }
    }
    if (const Json* m = find(payload, "min_reader"); m && !m->is_null()) {
        const bool readable = m->is_boolean() || (m->is_number_integer() && !m->is_number_unsigned() &&
                                                  m->get<std::int64_t>() <= kReadableVersion);
        if (!readable) {
            throw UnsupportedProjectVersion("project.json needs Genko that reads version " + core::py_str(*m) +
                                            " (this build reads up to " + std::to_string(kReadableVersion) +
                                            "); update Genko");
        }
    }
    return static_cast<int>(std::clamp<std::int64_t>(version, -1000000, kReadableVersion));
}

namespace {

LoadResult load_parsed(const Json& payload, const core::ParseRepairs& repairs, const fs::path& dir,
                       const LoadOptions& options) {
    LoadResult result;
    Reader reader(dir, options, result.report, repairs);
    result.document = reader.migrate(payload);
    result.document.read_only_reason = read_only_reason(result.report);
    return result;
}

}  // namespace

LoadResult load_document_text(std::string_view text, const fs::path& dir, const LoadOptions& options) {
    core::ParseRepairs repairs;
    core::ParseOptions parse;
    parse.universal_newlines = true;  // (Python reads project.json with read_text)
    const Json payload = core::parse_python_json(text, &repairs, parse);
    return load_parsed(payload, repairs, dir, options);
}

LoadResult load_document_payload(const Json& payload, const fs::path& dir, const LoadOptions& options) {
    return load_parsed(payload, core::ParseRepairs{}, dir, options);
}

LoadResult load_document(const fs::path& dir, const LoadOptions& options) {
    return load_document_text(read_file(dir / "project.json"), dir, options);
}

}  // namespace genko::storage
