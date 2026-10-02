#include "core/brushes.hpp"

#include <array>
#include <cstdint>
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

// The settings of a brush as from_dict starts from them (to_dict, with every J3 key).
Json settings(const Brush& b) {
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
        Json rgb = Json::array();
        for (const auto v : *b.rgb) rgb.push_back(v);
        out["rgb"] = std::move(rgb);
    } else {
        out["rgb"] = nullptr;
    }
    out["fixed_width"] = b.fixed_width;
    out["tip"] = b.tip;
    out["tip_angle"] = b.tip_angle;
    out["tip_ratio"] = b.tip_ratio;
    out["tip_follow"] = b.tip_follow;
    out["tip_rotation"] = b.tip_rotation;
    out["tip_png"] = b.tip_png;
    out["spacing"] = b.spacing;
    out["scatter"] = b.scatter;
    out["size_jitter"] = b.size_jitter;
    out["turn_jitter"] = b.turn_jitter;
    out["count"] = b.count;
    out["pattern"] = b.pattern;
    out["speed"] = b.speed;
    out["post_smooth"] = b.post_smooth;
    out["aa"] = b.aa;
    out["stamp_size"] = b.stamp_size;
    out["mix"] = b.mix;
    out["stretch"] = b.stretch;
    return out;
}

// brushes.brush(key) among the presets and `registered` (Python's CUSTOM). PyTypeError for a key that is no hashable.
const Brush& lookup(const Json& key, std::span<const Brush> registered) {
    require_hashable(key);
    const std::span<const Brush> presets = builtin_brushes();
    if (!py_truthy(key)) return presets.front();
    if (!key.is_string()) return presets.front();
    const std::string name = legacy(key.get<std::string>());
    if (const Brush* found = find_builtin(name)) return *found;
    for (const Brush& b : registered) {
        if (b.key == name) return b;
    }
    return presets.front();
}

// Py_UNICODE_ISSPACE for the code point.
bool unicode_space(std::uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// str.strip() is not empty
bool has_text(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        std::uint32_t cp = c;
        if (c >= 0xF0) {
            length = 4;
            cp = c & 0x07u;
        } else if (c >= 0xE0) {
            length = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xC0) {
            length = 2;
            cp = c & 0x1Fu;
        }
        for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        if (!unicode_space(cp)) return true;
        i += length;
    }
    return false;
}

bool one_of(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_string()) return false;
    for (const char* name : names) {
        if (value.get_ref<const std::string&>() == name) return true;
    }
    return false;
}

}  // namespace

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

std::string brush_kind(const Json& kind, const Document& doc) {
    const std::string name = legacy(py_str(kind));
    if (find_builtin(name) == nullptr && !doc.brush_custom.contains(name)) {
        std::string names;
        for (const Brush& b : builtin_brushes()) names += (names.empty() ? "" : ", ") + b.key;
        throw OpError("kind must be one of " + names + " (or a brush defined in the book with define_brush)");
    }
    return name;
}

std::optional<Brush> brush_from_dict(const std::string& key, const Json& data, std::span<const Brush> registered) {
    try {
        const Json* base = get(data, "base");
        const Brush start_brush = lookup(base != nullptr && py_truthy(*base) ? *base : Json("gpen"), registered);
        Json merged = settings(start_brush);
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
            if (!(lo <= value && value <= hi)) return std::nullopt;
        }
        if (!one_of(merged["texture"], {"", "grain", "soft", "dry", "water"})) return std::nullopt;
        if (!one_of(merged["tip"], {"round", "flat", "image"})) return std::nullopt;
        if (!one_of(merged["pattern"], {"", "dots", "dash", "lace", "grass", "hearts", "stars", "leaves"})) return std::nullopt;
        if (!one_of(merged["aa"], {"none", "weak", "normal", "strong"})) return std::nullopt;
        if (merged["tip"] == Json("image") && !py_truthy(merged["tip_png"])) return std::nullopt;
        const std::string label = py_truthy(merged["label"]) ? py_str(merged["label"]) : std::string();
        if (!has_text(label)) return std::nullopt;
        Brush b;
        b.key = key;
        b.label = label;
        b.width_mm = to_float(merged["width_mm"]);
        b.min_pressure = to_float(merged["min_pressure"]);
        b.gamma = to_float(merged["gamma"]);
        b.opacity = to_float(merged["opacity"]);
        b.stabilize = to_int(merged["stabilize"]);
        b.taper = py_truthy(merged["taper"]);
        b.texture = py_truthy(merged["texture"]) ? py_str(merged["texture"]) : "";
        if (py_truthy(merged["rgb"])) {
            std::vector<std::int64_t> rgb;
            for (const Json& v : iterate(merged["rgb"])) rgb.push_back(to_int(v));
            if (rgb.size() > 3) rgb.resize(3);
            b.rgb = std::move(rgb);
        }
        b.fixed_width = py_truthy(merged["fixed_width"]);
        b.tip = py_str(merged["tip"]);
        b.tip_angle = to_float(merged["tip_angle"]);
        b.tip_ratio = to_float(merged["tip_ratio"]);
        b.tip_follow = py_truthy(merged["tip_follow"]);
        b.tip_rotation = py_truthy(merged["tip_rotation"]);
        b.tip_png = py_truthy(merged["tip_png"]) ? py_str(merged["tip_png"]) : "";
        b.spacing = to_float(merged["spacing"]);
        b.scatter = to_float(merged["scatter"]);
        b.size_jitter = to_float(merged["size_jitter"]);
        b.turn_jitter = py_truthy(merged["turn_jitter"]);
        b.count = to_int(merged["count"]);
        b.pattern = py_truthy(merged["pattern"]) ? py_str(merged["pattern"]) : "";
        b.speed = to_float(merged["speed"]);
        b.post_smooth = to_int(merged["post_smooth"]);
        b.aa = py_str(merged["aa"]);
        b.stamp_size = to_float(merged["stamp_size"]);
        b.mix = to_float(merged["mix"]);
        b.stretch = to_float(merged["stretch"]);
        return b;
    } catch (const PyValueError&) {
        return std::nullopt;
    } catch (const PyTypeError&) {
        return std::nullopt;
    } catch (const OpKeyError&) {
        return std::nullopt;
    }
}

Brush brush_for(const Json& key, const Document& doc) {
    // brushes.register(episode.brush_custom), as Python does when it reads the book
    std::vector<Brush> registered;
    if (doc.brush_custom.is_object()) {
        for (const auto& [name, data] : doc.brush_custom.items()) {
            if (find_builtin(name) != nullptr) continue;
            if (!data.is_object()) continue;  // (dict(data) fails: skipped)
            if (auto made = brush_from_dict(name, data, registered)) {
                bool replaced = false;
                for (Brush& b : registered) {
                    if (b.key == name) {
                        b = *made;
                        replaced = true;
                    }
                }
                if (!replaced) registered.push_back(std::move(*made));
            }
        }
    }
    return lookup(key, registered);
}

}  // namespace genko::core
