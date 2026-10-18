// The table of correction kinds (core/adjust_kinds.hpp). Defaults and ranges: PhotoCraft v0.2.0 engine/adjust_params.rs
// (19-65, 67-286), engine/adjust_cmds.rs (24-64, 207-222), algo/tone.rs (34-65), doc/adjust.rs (HueRange::neutral);
// attribution and the upstream MIT license: native/third_party/photocraft/NOTICE.md.

#include "core/adjust_kinds.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

using Type = AdjustParam::Type;

AdjustParam number(std::string_view key, double fallback, double lo, double hi) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Number;
    p.fallback = fallback;
    p.lo = lo;
    p.hi = hi;
    return p;
}
AdjustParam whole(std::string_view key, double fallback, double lo, double hi) {
    AdjustParam p = number(key, fallback, lo, hi);
    p.type = Type::Whole;
    return p;
}
AdjustParam boolean(std::string_view key, bool fallback) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Bool;
    p.fallback = fallback ? 1 : 0;
    return p;
}
AdjustParam choice(std::string_view key, std::vector<std::string_view> choices, std::string_view fallback = {}) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Choice;
    p.choices = std::move(choices);
    p.text = fallback.empty() ? p.choices.front() : fallback;
    return p;
}
AdjustParam rgb(std::string_view key, std::array<int, 3> fallback) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Rgb;
    p.numbers = {static_cast<double>(fallback[0]), static_cast<double>(fallback[1]), static_cast<double>(fallback[2])};
    p.lo = 0;
    p.hi = 255;
    return p;
}
AdjustParam numbers(std::string_view key, std::vector<double> fallback, double lo, double hi) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Numbers;
    p.numbers = std::move(fallback);
    p.lo = lo;
    p.hi = hi;
    return p;
}
AdjustParam group(std::string_view key, std::vector<AdjustParam> settings) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Group;
    p.group = std::move(settings);
    return p;
}
AdjustParam text(std::string_view key) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Text;
    p.optional = true;
    return p;
}
AdjustParam custom(std::string_view key) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Custom;
    p.optional = true;
    return p;
}
AdjustParam optional(AdjustParam p) {
    p.optional = true;
    return p;
}

constexpr std::array<std::string_view, 20> kBlends{"normal",     "multiply",   "screen",     "add",        "overlay",
                                                   "darken",     "lighten",    "color_burn", "color_dodge", "linear_burn",
                                                   "soft_light", "hard_light", "difference", "exclusion",  "subtract",
                                                   "divide",     "hue",        "saturation", "color",      "luminosity"};

// Every native's keys start with its blend (design 2.5).
std::vector<AdjustParam> native_keys(std::vector<AdjustParam> more = {}) {
    std::vector<AdjustParam> out{optional(choice("blend", {kBlends.begin(), kBlends.end()}))};
    for (AdjustParam& p : more) out.push_back(optional(std::move(p)));
    return out;
}

// levels and a tone curve: their formula (the user's decision 4) and channel records in a space (design 2.2)
std::vector<AdjustParam> tone_native() {
    return native_keys({choice("method", {"genko", "upstream"}), choice("space", {"rgb", "cmyk", "lab", "gray"}), custom("channels")});
}

// Hue/Saturation's six ranges (PhotoCraft's HueRange::neutral: fully on 30° round its centre, gone 30° further)
AdjustParam hue_range(std::string_view key, int i) {
    const double c = 60.0 * i;
    return group(key, {number("hue", 0, -180, 180), number("saturation", 0, -100, 100), number("lightness", 0, -100, 100),
                       numbers("range", {c - 45, c - 15, c + 15, c + 45}, -720, 720)});
}

// Selective Color's nine ranges: [c, m, y, k] in percent
AdjustParam cmyk_change(std::string_view key) { return numbers(key, {0, 0, 0, 0}, -100, 100); }

AdjustKind python(std::string_view name, std::vector<AdjustParam> more_native = {}) {
    AdjustKind k;
    k.name = name;
    k.group = "Python";
    if (!more_native.empty()) k.native_group = "PC2a";  // (the upstream formulas, the channel records, reverse and dither)
    k.layer = true;
    k.filter = true;
    k.smart = true;
    k.native = native_keys(std::move(more_native));
    return k;
}

AdjustKind own(std::string_view name, std::string_view group_name, bool layer, std::vector<AdjustParam> params) {
    AdjustKind k;
    k.name = name;
    k.group = group_name;
    k.layer = layer;
    k.filter = true;
    k.smart = true;
    k.params = std::move(params);
    k.native = native_keys();
    return k;
}

std::vector<AdjustKind> make_table() {
    std::vector<AdjustKind> all;
    // Python's (filters.ADJUSTMENTS and exposure, in its order): their own settings are Python's (core/filters.hpp); the
    // table holds what their native may say. Their formulas with a native that changes them: PC2a's (tone_hooks).
    all.push_back(python("levels", tone_native()));
    all.push_back(python("curve", tone_native()));
    all.push_back(python("hue"));
    all.push_back(python("invert"));
    all.push_back(python("posterize", {choice("method", {"floor"})}));
    all.push_back(python("threshold"));
    all.push_back(python("gradient_map", {boolean("reverse", false), boolean("dither", false)}));
    all.push_back(python("bitonal"));
    all.push_back(python("brightness_contrast", {choice("method", {"modern", "legacy"})}));
    all.push_back(python("exposure"));

    // PC2b: the colour corrections (design 2.3; layers and filter_raster)
    all.push_back(own("hue_saturation", "PC2b", true,
                      {number("hue", 0, -180, 360), number("saturation", 0, -100, 100), number("lightness", 0, -100, 100),
                       boolean("colorize", false), hue_range("reds", 0), hue_range("yellows", 1), hue_range("greens", 2),
                       hue_range("cyans", 3), hue_range("blues", 4), hue_range("magentas", 5)}));
    all.push_back(own("vibrance", "PC2b", true, {number("vibrance", 0, -100, 100), number("saturation", 0, -100, 100)}));
    all.push_back(own("color_balance", "PC2b", true,
                      {numbers("shadows", {0, 0, 0}, -100, 100), numbers("midtones", {0, 0, 0}, -100, 100),
                       numbers("highlights", {0, 0, 0}, -100, 100), boolean("preserve_luminosity", true)}));
    all.push_back(own("black_white", "PC2b", true,
                      {number("reds", 40, -200, 300), number("yellows", 60, -200, 300), number("greens", 40, -200, 300),
                       number("cyans", 60, -200, 300), number("blues", 20, -200, 300), number("magentas", 80, -200, 300),
                       boolean("tint", false), rgb("tint_color", {225, 211, 179})}));
    all.push_back(own("photo_filter", "PC2b", true,
                      {optional(choice("filter", {"warming85", "warmingLBA", "warming81", "cooling80", "coolingLBB", "cooling82", "red",
                                                  "orange", "yellow", "green", "cyan", "blue", "violet", "magenta", "sepia", "deepRed",
                                                  "deepBlue", "deepEmerald", "deepYellow", "underwater"})),
                       rgb("color", {236, 138, 0}), number("density", 25, 0, 100), boolean("preserve_luminosity", true)}));
    all.push_back(own("channel_mixer", "PC2b", true,
                      {numbers("red", {100, 0, 0, 0}, -200, 200), numbers("green", {0, 100, 0, 0}, -200, 200),
                       numbers("blue", {0, 0, 100, 0}, -200, 200), numbers("gray", {40, 40, 20, 0}, -200, 200),
                       boolean("monochrome", false)}));
    all.push_back(own("selective_color", "PC2b", true,
                      {choice("method", {"relative", "absolute"}), cmyk_change("reds"), cmyk_change("yellows"), cmyk_change("greens"),
                       cmyk_change("cyans"), cmyk_change("blues"), cmyk_change("magentas"), cmyk_change("whites"),
                       cmyk_change("neutrals"), cmyk_change("blacks")}));
    all.push_back(own("desaturate", "PC2b", false, {}));

    // PC2a: the 3D lookup table (design 2.3, 2.4: `data` and `file_name` are an op's, made into the asset `lut`) and
    // equalize (a filter that looks at the whole layer)
    all.push_back(own("color_lookup", "PC2a", true,
                      {optional(choice("look", {"warm", "cool", "tealOrange", "bleachBypass", "fadedFilm", "dayForNight", "monoContrast",
                                                "crossProcess"})),
                       custom("lut"), optional(whole("size", 33, 2, 129)), text("name"), choice("interpolation", {"trilinear", "tetrahedral"}),
                       boolean("dither", false), custom("data"), text("file_name")}));
    all.push_back(own("equalize", "PC2a", false, {}));
    all.back().local = true;

    // PC2c: the local corrections (design 3.4; filter_raster only: they look beyond each pixel)
    all.push_back(own("shadows_highlights", "PC2c", false,
                      {number("shadow_amount", 35, 0, 100), number("shadow_tone", 50, 0, 100), number("shadow_radius", 30, 0, 2500),
                       number("highlight_amount", 0, 0, 100), number("highlight_tone", 50, 0, 100), number("highlight_radius", 30, 0, 2500),
                       number("color", 20, -100, 100), number("midtone", 0, -100, 100), number("black_clip", 0.01, 0, 50),
                       number("white_clip", 0.01, 0, 50)}));
    {
        AdjustParam colour = rgb("color", {0, 0, 0});
        colour.numbers.clear();  // (required: the GUI gives the main colour)
        all.push_back(own("replace_color", "PC2c", false,
                          {colour, number("fuzziness", 40, 0, 200), number("hue", 0, -180, 180), number("saturation", 0, -100, 100),
                           number("lightness", 0, -100, 100)}));
    }
    all.push_back(own("match_color", "PC2c", false,
                      {custom("source"), custom("source_stats"), number("luminance", 100, 1, 200), number("intensity", 100, 1, 200),
                       number("fade", 0, 0, 100), boolean("neutralize", false), boolean("stats_in_area", false)}));
    all.back().smart = false;  // (it reads another layer's colours, or the selection's: never a smart filter)
    for (AdjustKind& kind : all) {
        if (kind.group == "PC2c") kind.local = true;
    }

    for (const HookTable& hooks : {tone_hooks(), color_hooks()}) {
        for (const auto& [name, given] : hooks) {
            for (AdjustKind& kind : all) {
                if (kind.name != name) continue;
                if (given.normalize) kind.hooks.normalize = given.normalize;
                if (given.function) kind.hooks.function = given.function;
            }
        }
    }
    return all;
}

Json number_json(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

std::string at(std::string_view where, std::string_view key) { return std::string(where) + "." + std::string(key); }

double finite_number(const Json& v, const std::string& where) {
    if (!v.is_number()) throw OpError(where + " must be a number");
    const double x = v.get<double>();
    if (!std::isfinite(x)) throw OpError(where + " must be finite");
    return x;
}

// One number of a setting: clamped (Op) or refused out of range (Stored); as given when in range.
Json ranged(const Json& v, double lo, double hi, bool round, ReadMode mode, const std::string& where) {
    double x = finite_number(v, where);
    if (round) {
        if (mode == ReadMode::Stored && x != std::floor(x)) throw OpError(where + " must be a whole number");
        x = std::floor(x + 0.5);
    }
    if (x < lo || x > hi) {
        if (mode == ReadMode::Stored) throw OpError(where + " must be from " + py_repr(number_json(lo)) + " to " + py_repr(number_json(hi)));
        return number_json(std::clamp(x, lo, hi));
    }
    return round ? number_json(x) : v;
}

Json default_of(const AdjustParam& p) {
    switch (p.type) {
        case Type::Number:
        case Type::Whole: return number_json(p.fallback);
        case Type::Bool: return Json(p.fallback != 0);
        case Type::Choice: return Json(std::string(p.text));
        case Type::Rgb:
        case Type::Numbers: {
            Json out = Json::array();
            for (const double v : p.numbers) out.push_back(number_json(v));
            return out;
        }
        case Type::Group: return read_params(p.group, Json::object(), ReadMode::Op, p.key);
        case Type::Text:
        case Type::Custom: break;
    }
    return Json();
}

Json read_one(const AdjustParam& p, const Json& v, ReadMode mode, const std::string& where) {
    switch (p.type) {
        case Type::Number: return ranged(v, p.lo, p.hi, false, mode, where);
        case Type::Whole: return ranged(v, p.lo, p.hi, true, mode, where);
        case Type::Bool:
            if (!v.is_boolean()) throw OpError(where + " must be true or false");
            return v;
        case Type::Choice: {
            std::string names;
            for (const std::string_view c : p.choices) names += (names.empty() ? "" : ", ") + std::string(c);
            if (!v.is_string() || std::find(p.choices.begin(), p.choices.end(), v.get<std::string>()) == p.choices.end())
                throw OpError(where + " must be one of " + names);
            return v;
        }
        case Type::Rgb:
        case Type::Numbers: {
            const std::size_t n = p.type == Type::Rgb ? 3 : p.numbers.size();
            if (!v.is_array() || v.size() != n) {
                throw OpError(where + (p.type == Type::Rgb ? std::string(" must be [r, g, b] (0..255)")
                                                          : " must be a list of " + std::to_string(n) + " numbers"));
            }
            Json out = Json::array();
            for (std::size_t i = 0; i < n; ++i) out.push_back(ranged(v[i], p.lo, p.hi, p.type == Type::Rgb, mode, where));
            return out;
        }
        case Type::Group: return read_params(p.group, v, mode, where);
        case Type::Text:
            if (!v.is_string() || v.get_ref<const std::string&>().empty() || v.get_ref<const std::string&>().size() > 1024)
                throw OpError(where + " must be a name (1 to 1024 bytes)");
            return v;
        case Type::Custom: return v;
    }
    return v;
}

}  // namespace

const std::vector<AdjustKind>& adjust_kinds() {
    static const std::vector<AdjustKind> table = make_table();
    return table;
}

const AdjustKind* adjust_kind(std::string_view name) {
    for (const AdjustKind& kind : adjust_kinds()) {
        if (kind.name == name) return &kind;
    }
    return nullptr;
}

bool python_adjust_kind(std::string_view kind) {
    const AdjustKind* found = adjust_kind(kind);
    return found != nullptr && found->group == "Python";
}

Json read_params(const std::vector<AdjustParam>& params, const Json& given, ReadMode mode, std::string_view where) {
    if (!given.is_object()) throw OpError(std::string(where) + " must be an object of settings");
    for (const auto& [key, value] : given.items()) {
        if (std::none_of(params.begin(), params.end(), [&key](const AdjustParam& p) { return p.key == key; }))
            throw OpError(std::string(where) + " has no setting " + py_repr_str(key));
    }
    Json out = Json::object();
    for (const AdjustParam& p : params) {
        const auto it = given.find(p.key);
        if (it == given.end() || it->is_null()) {
            if (mode == ReadMode::Stored || p.optional) continue;
            if (p.type == Type::Rgb && p.numbers.empty()) throw OpError(at(where, p.key) + " is required");
            out[std::string(p.key)] = default_of(p);
            continue;
        }
        out[std::string(p.key)] = read_one(p, *it, mode, at(where, p.key));
    }
    return out;
}

std::vector<std::string_view> tone_channels(std::string_view space) {
    if (space == "cmyk") return {"cyan", "magenta", "yellow", "black"};
    if (space == "lab") return {"lightness", "a", "b"};
    if (space == "gray") return {"gray"};
    return {"red", "green", "blue"};
}

std::string_view default_tone_space(const Page* page) {
    if (page == nullptr || !page->color) return "rgb";
    switch (page->color->space) {
        case InkSpace::Cmyk: return "cmyk";
        case InkSpace::Lab: return "lab";
        case InkSpace::Gray: return "gray";
        default: return "rgb";
    }
}

Json read_tone_channels(std::string_view kind, const Json& channels, std::string_view space, ReadMode mode, std::string_view where) {
    static const std::vector<AdjustParam> levels_record{number("black", 0, 0, 253), number("white", 255, 2, 255), number("gamma", 1, 0.01, 9.99),
                                                        number("out_black", 0, 0, 255), number("out_white", 255, 0, 255)};
    if (!channels.is_object()) throw OpError(std::string(where) + " must be an object of channels");
    const std::vector<std::string_view> names = tone_channels(space);
    std::string listed;
    for (const std::string_view n : names) listed += (listed.empty() ? "" : ", ") + std::string(n);
    for (const auto& [key, value] : channels.items()) {
        if (std::find(names.begin(), names.end(), key) == names.end())
            throw OpError(std::string(where) + " has no channel " + py_repr_str(key) + " in the space " + std::string(space) + " (" + listed + ")");
    }
    Json out = Json::object();
    for (const std::string_view name : names) {
        const auto it = channels.find(name);
        if (it == channels.end() || it->is_null()) continue;
        const std::string here = at(where, name);
        if (kind == "levels") {
            Json record = read_params(levels_record, *it, mode, here);
            if (record.contains("black") && record.contains("white")) {
                const double black = record["black"].get<double>(), white = record["white"].get<double>();
                if (white < black + 2) {
                    if (mode == ReadMode::Stored) throw OpError(here + ".white must be at least black + 2");
                    record["white"] = number_json(black + 2);
                }
            }
            out[std::string(name)] = std::move(record);
            continue;
        }
        // a tone curve's points
        const std::string message = here + " must be a list of 2 to 19 points [in, out] in 0..255";
        if (!it->is_array() || it->size() < 2 || it->size() > 64) throw OpError(message);
        std::vector<std::pair<Json, Json>> points;
        for (const Json& point : *it) {
            if (!point.is_array() || point.size() != 2) throw OpError(message);
            points.emplace_back(ranged(point[0], 0, 255, false, mode, here), ranged(point[1], 0, 255, false, mode, here));
        }
        if (mode == ReadMode::Op) {
            std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first.template get<double>() < b.first.template get<double>(); });
            std::vector<std::pair<Json, Json>> kept;
            for (auto& p : points) {
                if (!kept.empty() && std::fabs(p.first.get<double>() - kept.back().first.get<double>()) < 0.5) continue;
                kept.push_back(std::move(p));
            }
            points = std::move(kept);
        } else {
            for (std::size_t i = 1; i < points.size(); ++i) {
                if (points[i].first.get<double>() - points[i - 1].first.get<double>() < 0.5) throw OpError(here + " must be sorted by in, half a level apart");
            }
        }
        if (points.size() < 2 || points.size() > 19) throw OpError(message);
        Json list = Json::array();
        for (auto& [in, outv] : points) list.push_back(Json::array({in, outv}));
        out[std::string(name)] = std::move(list);
    }
    return out;
}

}  // namespace genko::core
