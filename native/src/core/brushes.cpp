#include "core/brushes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <tuple>
#include <utility>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

Brush make(std::string key, std::string label, double width_mm) {
    Brush b;
    b.key = std::move(key);
    b.label = std::move(label);
    b.width_mm = width_mm;
    return b;
}

std::vector<Brush> make_builtins() {
    std::vector<Brush> out;
    {
        Brush b = make("gpen", "G ペン", 0.5);
        b.min_pressure = 0.1, b.gamma = 1.4, b.stabilize = 3, b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("maru", "丸ペン", 0.25);
        b.min_pressure = 0.2, b.gamma = 1.2, b.stabilize = 3, b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("kabura", "かぶらペン", 0.6);
        b.min_pressure = 0.35, b.gamma = 1.0, b.stabilize = 2, b.taper = true;
        out.push_back(b);
    }
    {
        Brush b = make("mili", "ミリペン", 0.3);
        b.fixed_width = true, b.stabilize = 4, b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("pencil", "鉛筆", 0.5);
        b.min_pressure = 0.4, b.gamma = 1.0, b.opacity = 0.85, b.stabilize = 1, b.taper = false, b.texture = "grain";
        out.push_back(b);
    }
    {
        Brush b = make("fude", "筆", 1.6);
        b.min_pressure = 0.05, b.gamma = 1.8, b.stabilize = 2, b.taper = true, b.texture = "dry";
        out.push_back(b);
    }
    {
        Brush b = make("marker", "マーカー", 1.5);
        b.fixed_width = true, b.opacity = 0.6, b.stabilize = 2, b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("airbrush", "エアブラシ", 8.0);
        b.min_pressure = 0.5, b.opacity = 0.5, b.stabilize = 1, b.taper = false, b.texture = "soft";
        out.push_back(b);
    }
    {
        Brush b = make("fill_pen", "ベタ塗りペン", 3.0);
        b.fixed_width = true, b.stabilize = 1, b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("white", "ホワイト（修正）", 1.0);
        b.min_pressure = 0.3, b.stabilize = 2, b.taper = false, b.rgb = std::vector<std::int64_t>{255, 255, 255};
        out.push_back(b);
    }
    {
        Brush b = make("fx", "効果線ペン", 0.5);
        b.min_pressure = 0.0, b.gamma = 1.0, b.stabilize = 0, b.taper = false;
        out.push_back(b);
    }
    {
        Brush b = make("calligraphy", "カリグラフィ（平たいペン先）", 1.6);
        b.min_pressure = 0.5, b.stabilize = 3, b.taper = false, b.tip = "flat", b.tip_angle = 35, b.tip_rotation = true;
        b.tip_ratio = 0.22;
        out.push_back(b);
    }
    {
        Brush b = make("water", "水彩", 4.0);
        b.min_pressure = 0.4, b.opacity = 0.55, b.stabilize = 2, b.taper = false, b.texture = "water";
        out.push_back(b);
    }
    {
        Brush b = make("spray", "スプレー", 8.0);
        b.min_pressure = 0.5, b.stabilize = 1, b.taper = false, b.pattern = "dots", b.spacing = 0.08, b.scatter = 0.5;
        b.size_jitter = 0.6, b.count = 6, b.stamp_size = 0.06;
        out.push_back(b);
    }
    {
        Brush b = make("stipple", "点描", 3.0);
        b.min_pressure = 0.5, b.stabilize = 1, b.taper = false, b.pattern = "dots", b.spacing = 0.35, b.scatter = 0.45;
        b.size_jitter = 0.5, b.count = 2, b.stamp_size = 0.2;
        out.push_back(b);
    }
    {
        Brush b = make("dotline", "点線", 0.8);
        b.fixed_width = true, b.stabilize = 4, b.taper = false, b.pattern = "dots", b.spacing = 2.2, b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("dashline", "破線", 0.5);
        b.fixed_width = true, b.stabilize = 4, b.taper = false, b.pattern = "dash", b.spacing = 5.0, b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("lace", "レース", 3.0);
        b.fixed_width = true, b.stabilize = 4, b.taper = false, b.pattern = "lace", b.spacing = 1.0, b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("grass", "草むら", 6.0);
        b.min_pressure = 0.5, b.stabilize = 2, b.taper = false, b.pattern = "grass", b.spacing = 0.22, b.size_jitter = 0.5;
        b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("leaves", "木の葉", 5.0);
        b.min_pressure = 0.5, b.stabilize = 2, b.taper = false, b.pattern = "leaves", b.spacing = 0.45, b.scatter = 0.6;
        b.size_jitter = 0.5, b.turn_jitter = true, b.count = 2, b.stamp_size = 0.6;
        out.push_back(b);
    }
    {
        Brush b = make("hearts", "ハート", 3.0);
        b.fixed_width = true, b.stabilize = 3, b.taper = false, b.pattern = "hearts", b.spacing = 1.4, b.stamp_size = 1.0;
        out.push_back(b);
    }
    {
        Brush b = make("stars", "星", 3.0);
        b.fixed_width = true, b.stabilize = 3, b.taper = false, b.pattern = "stars", b.spacing = 1.5, b.turn_jitter = true;
        b.size_jitter = 0.3, b.stamp_size = 1.0;
        out.push_back(b);
    }
    return out;
}

// LEGACY.get(key, key)
std::string legacy(std::string key) { return key == "oil" ? std::string("marker") : key; }

const char* const kTextures[] = {"", "grain", "soft", "dry", "water"};
const char* const kTips[] = {"round", "flat", "image"};
const char* const kPatterns[] = {"", "dots", "dash", "lace", "grass", "hearts", "stars", "leaves"};
const char* const kAas[] = {"none", "weak", "normal", "strong"};

// value in CHOICES (a tuple of str: only a str equal to one of them is in it)
template <std::size_t N>
bool one_of(const Json& value, const char* const (&choices)[N]) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    for (const char* c : choices) {
        if (text == c) return true;
    }
    return false;
}

const char* const kJ3Keys[] = {"tip", "tip_angle", "tip_ratio", "tip_follow", "tip_rotation", "tip_png", "spacing",
                               "scatter", "size_jitter", "turn_jitter", "count", "pattern", "speed", "post_smooth",
                               "aa", "stamp_size", "mix", "stretch"};

// getattr(b, key) for a J3 key, as Python holds it
Json j3_value(const Brush& b, std::string_view key) {
    if (key == "tip") return b.tip;
    if (key == "tip_angle") {
        // (the built-in calligraphy pen says tip_angle=35: an int in Python)
        if (b.key == "calligraphy" && find_builtin("calligraphy") != nullptr && b == *find_builtin("calligraphy")) {
            return static_cast<std::int64_t>(b.tip_angle);
        }
        return b.tip_angle;
    }
    if (key == "tip_ratio") return b.tip_ratio;
    if (key == "tip_follow") return b.tip_follow;
    if (key == "tip_rotation") return b.tip_rotation;
    if (key == "tip_png") return b.tip_png;
    if (key == "spacing") return b.spacing;
    if (key == "scatter") return b.scatter;
    if (key == "size_jitter") return b.size_jitter;
    if (key == "turn_jitter") return b.turn_jitter;
    if (key == "count") return b.count;
    if (key == "pattern") return b.pattern;
    if (key == "speed") return b.speed;
    if (key == "post_smooth") return b.post_smooth;
    if (key == "aa") return b.aa;
    if (key == "stamp_size") return b.stamp_size;
    if (key == "mix") return b.mix;
    return b.stretch;
}

// Python's str.isspace() for one code point.
bool py_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

// The code points of UTF-8 text with where each starts.
std::vector<std::pair<char32_t, std::size_t>> code_points(const std::string& s) {
    std::vector<std::pair<char32_t, std::size_t>> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > s.size()) n = s.size() - i;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.emplace_back(cp, i);
        i += n;
    }
    return out;
}

// BRUSH-01: "sha256:" and 64 lowercase hex digits (storage::AssetStore::is_ref, which core cannot see).
bool paper_ref(const std::string& ref) {
    if (ref.size() != 71 || ref.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(ref.begin() + 7, ref.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A paper's number: finite, an int or a float (not a bool), in its range; its default when it is not given.
double paper_number(const Json& data, const char* name, double fallback, double lo, double hi) {
    const Json* value = get(data, name);
    if (value == nullptr) return fallback;
    const double v = value->is_number() ? value->get<double>() : std::nan("");
    if (!(std::isfinite(v) && lo <= v && v <= hi)) {
        throw PyValueError(std::string("paper ") + name + " must be between " + py_format_g(lo) + " and " + py_format_g(hi));
    }
    return v;
}

bool paper_flag(const Json& data, const char* name, bool fallback) {
    const Json* value = get(data, name);
    if (value == nullptr) return fallback;
    if (!value->is_boolean()) throw PyValueError(std::string("paper ") + name + " must be true or false");
    return value->get<bool>();
}

std::string paper_choice(const Json& data, const char* name, const std::string& fallback, const char* a, const char* b) {
    const Json* value = get(data, name);
    if (value == nullptr) return fallback;
    if (!(value->is_string() && (*value == a || *value == b))) {
        throw PyValueError(std::string("paper ") + name + " must be " + a + " or " + b);
    }
    return value->get<std::string>();
}

// str(label).strip()[:40]
std::string clean_label(const std::string& text) {
    const auto cps = code_points(text);
    std::size_t first = 0;
    std::size_t last = cps.size();
    while (first < last && py_space(cps[first].first)) ++first;
    while (last > first && py_space(cps[last - 1].first)) --last;
    if (last - first > 40) last = first + 40;
    if (first == last) return {};
    const std::size_t begin = cps[first].second;
    const std::size_t end = last < cps.size() ? cps[last].second : text.size();
    return text.substr(begin, end - begin);
}

}  // namespace

Json paper_to_json(const Paper& paper) {
    Json out = Json::object();
    out["asset"] = paper.asset;
    out["density"] = paper.density;
    out["scale"] = paper.scale;
    out["rotation"] = paper.rotation;
    out["flip_x"] = paper.flip_x;
    out["flip_y"] = paper.flip_y;
    out["invert"] = paper.invert;
    out["blend"] = paper.blend;
    out["coords"] = paper.coords;
    out["seam"] = paper.seam;
    out["seed"] = paper.seed;
    return out;
}

Paper paper_from_json(const Json& data) {
    if (!data.is_object()) throw PyValueError("paper must be an object or null");
    static const char* const kKeys[] = {"asset", "density", "scale", "rotation", "flip_x", "flip_y", "invert", "blend", "coords", "seam", "seed"};
    for (const auto& [key, value] : data.items()) {
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) {
            throw PyValueError("paper has an unknown setting: " + key);
        }
    }
    Paper out;
    const Json* asset = get(data, "asset");
    if (asset == nullptr || !asset->is_string() || !paper_ref(asset->get_ref<const std::string&>())) {
        throw PyValueError("paper asset must be an asset ref (sha256:<64 hex>)");
    }
    out.asset = asset->get<std::string>();
    out.density = paper_number(data, "density", out.density, 0.0, 1.0);
    out.scale = paper_number(data, "scale", out.scale, 0.1, 10.0);
    out.rotation = paper_number(data, "rotation", out.rotation, -360.0, 360.0);
    out.flip_x = paper_flag(data, "flip_x", out.flip_x);
    out.flip_y = paper_flag(data, "flip_y", out.flip_y);
    out.invert = paper_flag(data, "invert", out.invert);
    out.blend = paper_choice(data, "blend", out.blend, "multiply", "subtract");
    out.coords = paper_choice(data, "coords", out.coords, "paper", "stroke");
    out.seam = paper_choice(data, "seam", out.seam, "repeat", "mirror");
    if (const Json* seed = get(data, "seed")) {
        const bool whole = seed->is_number_integer() &&
                           (seed->is_number_unsigned() ? seed->get<std::uint64_t>() <= static_cast<std::uint64_t>(kPaperMaxSeed)
                                                       : seed->get<std::int64_t>() >= 0 && seed->get<std::int64_t>() <= kPaperMaxSeed);
        if (!whole) throw PyValueError("paper seed must be a whole number between 0 and " + std::to_string(kPaperMaxSeed));
        out.seed = seed->get<std::int64_t>();
    }
    return out;
}

std::vector<std::string> paper_refs(const Json& brush_custom) {
    std::vector<std::string> out;
    if (!brush_custom.is_object()) return out;
    for (const auto& [key, brush] : brush_custom.items()) {
        const Json* paper = get(brush, "paper");
        if (paper == nullptr || paper->is_null()) continue;
        try {
            const std::string ref = paper_from_json(*paper).asset;
            if (std::find(out.begin(), out.end(), ref) == out.end()) out.push_back(ref);
        } catch (const PyValueError&) {
            // (the reader reports it)
        }
    }
    return out;
}

std::span<const Brush> builtin_brushes() {
    static const std::vector<Brush> brushes = make_builtins();
    return brushes;
}

const Brush* find_builtin(std::string_view key) {
    for (const Brush& b : builtin_brushes()) {
        if (b.key == key) return &b;
    }
    return nullptr;
}

Brush find_brush(const Json& key, std::span<const Brush> custom) {
    // LEGACY.get(key or "", key or DEFAULT), then BRUSHES.get(key) or CUSTOM.get(key) or BRUSHES[DEFAULT]
    require_hashable(key);
    const Brush& fallback = *find_builtin(kDefaultBrush);
    if (!py_truthy(key) || !key.is_string()) return fallback;  // (no brush has a key that is not a str)
    const std::string name = legacy(key.get<std::string>());
    if (const Brush* found = find_builtin(name)) return *found;
    for (const Brush& b : custom) {
        if (b.key == name) return b;
    }
    return fallback;
}

Json brush_to_dict(const Brush& b) {
    Json out = Json::object();
    out["label"] = b.label;
    out["width_mm"] = b.width_mm;
    out["min_pressure"] = b.min_pressure;
    out["gamma"] = b.gamma;
    out["opacity"] = b.opacity;
    out["stabilize"] = b.stabilize;
    out["taper"] = b.taper;
    out["texture"] = b.texture;
    if (b.rgb && !b.rgb->empty()) {
        out["rgb"] = *b.rgb;
    } else {
        out["rgb"] = nullptr;
    }
    out["fixed_width"] = b.fixed_width;
    const Brush plain = make("", "", 1.0);
    for (const char* key : kJ3Keys) {  // (only what differs from a plain round pen: older books stay as they were)
        const Json mine = j3_value(b, key);
        const Json theirs = j3_value(plain, key);
        if (mine != theirs) out[key] = mine;
    }
    if (b.paper) out["paper"] = paper_to_json(*b.paper);  // (BRUSH-01: only a brush with one)
    return out;
}

Brush brush_from_dict(std::string_view key, const Json& data, const std::optional<std::string>& base,
                      std::span<const Brush> custom) {
    // brush(base or data.get("base") or DEFAULT)
    Json start_key = Json(std::string(kDefaultBrush));
    if (base && !base->empty()) {
        start_key = *base;
    } else if (const Json* given = get(data, "base"); given != nullptr && py_truthy(*given)) {
        start_key = *given;
    }
    const Brush start_brush = find_brush(start_key, custom);
    const Brush plain = make("", "", 1.0);
    Json merged = Json::object();
    for (const char* k : kJ3Keys) merged[k] = j3_value(plain, k);
    const Json start = brush_to_dict(start_brush);
    for (const auto& [k, v] : start.items()) merged[k] = v;
    if (!merged.contains("paper")) merged["paper"] = nullptr;  // (BRUSH-01: the base's paper, or none)
    if (data.is_object()) {
        for (const auto& [k, v] : data.items()) {
            if (merged.contains(k)) merged[k] = v;
        }
    }

    static const std::array<std::tuple<const char*, double, double>, 16> kLimits{{
        {"width_mm", 0.05, 50.0}, {"min_pressure", 0.0, 1.0}, {"gamma", 0.2, 5.0}, {"opacity", 0.05, 1.0},
        {"stabilize", 0, 15}, {"tip_angle", -360.0, 360.0}, {"tip_ratio", 0.02, 1.0}, {"spacing", 0.0, 5.0},
        {"scatter", 0.0, 5.0}, {"size_jitter", 0.0, 1.0}, {"count", 1, 12}, {"speed", 0.0, 1.0},
        {"post_smooth", 0, 10}, {"stamp_size", 0.02, 3.0}, {"mix", 0.0, 1.0}, {"stretch", 0.0, 1.0}}};
    for (const auto& [name, lo, hi] : kLimits) {
        const double value = to_float(merged[name]);
        if (!(lo <= value && value <= hi)) {
            throw PyValueError(std::string("brush ") + name + " must be between " + py_format_g(lo) + " and " +
                               py_format_g(hi));
        }
    }
    if (!one_of(merged["texture"], kTextures)) throw PyValueError("brush texture must be none, grain, soft, dry or water");
    if (!one_of(merged["tip"], kTips)) throw PyValueError("brush tip must be round, flat or image");
    if (!one_of(merged["pattern"], kPatterns)) {
        throw PyValueError("brush pattern must be dots, dash, lace, grass, hearts, stars or leaves");
    }
    if (!one_of(merged["aa"], kAas)) throw PyValueError("brush aa must be none, weak, normal or strong");
    if (merged["tip"] == Json("image") && !py_truthy(merged["tip_png"])) {
        throw PyValueError("an image tip needs its picture (tip_png)");
    }
    const Json& raw_label = merged["label"];
    const std::string label = clean_label(py_truthy(raw_label) ? py_str(raw_label) : std::string());
    if (label.empty()) throw PyValueError("a brush needs a name");

    // (in the order Python's Brush(…) takes them: the first that fails decides the error)
    const auto text = [&](const char* name) { return py_truthy(merged[name]) ? py_str(merged[name]) : std::string(); };
    Brush out;
    out.key = std::string(key);
    out.label = label;
    out.width_mm = to_float(merged["width_mm"]);
    out.min_pressure = to_float(merged["min_pressure"]);
    out.gamma = to_float(merged["gamma"]);
    out.opacity = to_float(merged["opacity"]);
    out.stabilize = to_int(merged["stabilize"]);
    out.taper = py_truthy(merged["taper"]);
    out.texture = text("texture");
    if (py_truthy(merged["rgb"])) {
        std::vector<std::int64_t> rgb = int_tuple(merged["rgb"]);
        if (rgb.size() > 3) rgb.resize(3);
        out.rgb = std::move(rgb);
    }
    out.fixed_width = py_truthy(merged["fixed_width"]);
    out.tip = py_str(merged["tip"]);
    out.tip_angle = to_float(merged["tip_angle"]);
    out.tip_ratio = to_float(merged["tip_ratio"]);
    out.tip_follow = py_truthy(merged["tip_follow"]);
    out.tip_rotation = py_truthy(merged["tip_rotation"]);
    out.tip_png = text("tip_png");
    out.spacing = to_float(merged["spacing"]);
    out.scatter = to_float(merged["scatter"]);
    out.size_jitter = to_float(merged["size_jitter"]);
    out.turn_jitter = py_truthy(merged["turn_jitter"]);
    out.count = to_int(merged["count"]);
    out.pattern = text("pattern");
    out.speed = to_float(merged["speed"]);
    out.post_smooth = to_int(merged["post_smooth"]);
    out.aa = py_str(merged["aa"]);
    out.stamp_size = to_float(merged["stamp_size"]);
    out.mix = to_float(merged["mix"]);
    out.stretch = to_float(merged["stretch"]);
    if (!merged["paper"].is_null()) out.paper = paper_from_json(merged["paper"]);
    return out;
}

void register_brushes(const Json& definitions, std::vector<Brush>& custom) {
    if (!definitions.is_object()) return;
    for (const auto& [key, data] : definitions.items()) {
        if (find_builtin(key) != nullptr) continue;
        Brush made;
        try {
            made = brush_from_dict(key, py_dict(data), std::nullopt, custom);
        } catch (const PyUncaught&) {
            throw;  // (OverflowError: register does not catch it)
        } catch (const Error&) {
            continue;  // (ValueError, TypeError: a brush that does not make sense is skipped)
        } catch (const OpKeyError&) {
            continue;  // (KeyError)
        }
        bool replaced = false;
        for (Brush& existing : custom) {
            if (existing.key == key) {
                existing = made;
                replaced = true;
                break;
            }
        }
        if (!replaced) custom.push_back(std::move(made));
    }
}

std::string brush_kind(const Json& kind, const Document& doc) {
    const std::string name = legacy(py_str(kind));
    if (find_builtin(name) == nullptr && !doc.brush_custom.contains(name)) {
        std::string names;
        for (const Brush& b : builtin_brushes()) names += (names.empty() ? "" : ", ") + b.key;
        throw OpError("kind must be one of " + names + " (or a brush defined in the book with define_brush)");
    }
    return name;
}

Brush brush_for(const Json& key, const Document& doc) {
    std::vector<Brush> custom;
    register_brushes(doc.brush_custom, custom);
    return find_brush(key, custom);
}

}  // namespace genko::core
