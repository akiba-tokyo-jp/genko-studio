#include "core/ink.hpp"

#include <zlib.h>

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", "the page's colour mode cannot be read: " + why); }
[[noreturn]] void bad_raster(const std::string& why) { throw Error("format", "the ink raster cannot be read: " + why); }

bool is_ref(const std::string& text) {
    if (text.size() != 71 || text.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A number of the JSON that is a number and not a bool (nlohmann keeps them apart), finite.
std::optional<double> number(const Json& v) {
    if (!v.is_number()) return std::nullopt;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

std::uint32_t le32(std::string_view b, std::size_t at) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= std::uint32_t(static_cast<unsigned char>(b[at + i])) << (8 * i);
    return v;
}

void put_le32(std::string& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 255));
}

constexpr std::size_t kHeader = 16;

// PNG's Paeth predictor.
inline unsigned paeth(unsigned a, unsigned b, unsigned c) {
    const int p = int(a) + int(b) - int(c);
    const int pa = std::abs(p - int(a)), pb = std::abs(p - int(b)), pc = std::abs(p - int(c));
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// The row's bytes as filter `type` gives them (PNG §9), from the row and the one above (zero for the first).
void filter_row(int type, const unsigned char* row, const unsigned char* above, std::size_t n, std::size_t bpp, unsigned char* out) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned a = i >= bpp ? row[i - bpp] : 0;
        const unsigned b = above ? above[i] : 0;
        const unsigned c = (i >= bpp && above) ? above[i - bpp] : 0;
        unsigned p = 0;
        switch (type) {
            case 1: p = a; break;
            case 2: p = b; break;
            case 3: p = (a + b) / 2; break;
            case 4: p = paeth(a, b, c); break;
            default: p = 0; break;
        }
        out[i] = static_cast<unsigned char>((row[i] - p) & 255);
    }
}

// The row back from its filtered bytes, in place (`row` holds them; `above` is the row above, decoded).
void unfilter_row(int type, unsigned char* row, const unsigned char* above, std::size_t n, std::size_t bpp) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned a = i >= bpp ? row[i - bpp] : 0;
        const unsigned b = above ? above[i] : 0;
        const unsigned c = (i >= bpp && above) ? above[i - bpp] : 0;
        unsigned p = 0;
        switch (type) {
            case 1: p = a; break;
            case 2: p = b; break;
            case 3: p = (a + b) / 2; break;
            case 4: p = paeth(a, b, c); break;
            default: p = 0; break;
        }
        row[i] = static_cast<unsigned char>((row[i] + p) & 255);
    }
}

// The bytes a sample takes at a depth, and one row of samples (checked against the limits).
std::size_t sample_bytes(int depth) { return depth == 8 ? 1 : depth == 16 ? 2 : 4; }

std::size_t row_bytes(std::uint32_t width, unsigned channels, int depth) { return std::size_t(width) * channels * sample_bytes(depth); }

void check_size(std::uint32_t width, std::uint32_t height, unsigned channels, int depth) {
    if (width < 1 || height < 1 || width > kInkRasterMaxSide || height > kInkRasterMaxSide) bad_raster("its size is out of range");
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (pixels > kInkRasterMaxPixels) bad_raster("it has too many pixels");
    if (pixels * channels * sample_bytes(depth) > kInkRasterMaxBytes) bad_raster("it is too large");
}

constexpr std::array<SpaceTraits, 5> kTraits{{
    {"cmyk", 1, 4, false, false, true},
    {"lab", 2, 3, false, false, true},
    {"gray", 3, 1, true, true, true},
    {"multichannel", 4, 0, false, false, true},
    {"mask", 5, 1, false, false, false},
}};

// A 32-bit sample of the space and channel is one it may hold (finite; 0..1 but a Gray colour plane's).
bool float_ok(InkSpace space, unsigned colours, unsigned c, float v) {
    if (!std::isfinite(v)) return false;
    if (c < colours && traits(space).hdr) return true;
    return v >= 0.0f && v <= 1.0f;
}

}  // namespace

const SpaceTraits& traits(InkSpace space) { return kTraits[static_cast<std::size_t>(space)]; }

std::string_view to_string(InkSpace space) { return traits(space).name; }

std::optional<InkSpace> ink_space_from(std::string_view text) {
    if (text == "cmyk") return InkSpace::Cmyk;
    if (text == "lab") return InkSpace::Lab;
    if (text == "gray" || text == "bitmap" || text == "duotone") return InkSpace::Gray;
    if (text == "multichannel") return InkSpace::Multi;
    return std::nullopt;
}

std::string_view mode_name(const PageColor& color) {
    if (color.space == InkSpace::Gray && color.bitmap) return "bitmap";
    if (color.space == InkSpace::Gray && !color.duotone.empty()) return "duotone";
    return to_string(color.space);
}

unsigned colour_channels(InkSpace space) { return traits(space).planes; }

bool is_intent(std::string_view intent) { return std::find(kIntents.begin(), kIntents.end(), intent) != kIntents.end(); }

double curve_at(const std::vector<CurvePoint>& curve, double x) {
    // (PhotoCraft's curve_eval: before the first point its output, after the last the last's, straight between)
    if (curve.empty()) return x;
    if (curve.size() == 1) return curve[0][1] / 100.0;
    if (x <= curve[0][0] / 100.0) return curve[0][1] / 100.0;
    for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
        const double x0 = curve[i][0] / 100.0, x1 = curve[i + 1][0] / 100.0;
        if (x <= x1) {
            const double span = std::max(x1 - x0, 1e-6);
            return curve[i][1] / 100.0 + (curve[i + 1][1] - curve[i][1]) / 100.0 * (x - x0) / span;
        }
    }
    return curve.back()[1] / 100.0;
}

std::array<double, 3> duotone_print(const std::vector<DuotoneInk>& inks, double grey) {
    const double density = 1.0 - std::clamp(grey, 0.0, 1.0);
    std::array<double, 3> out{1.0, 1.0, 1.0};
    for (const DuotoneInk& ink : inks) {
        const double d = curve_at(ink.curve, density);
        for (unsigned c = 0; c < 3; ++c) out[c] *= 1.0 - d * (1.0 - ink.color[c] / 255.0);
    }
    return out;
}

void print_plates(const std::vector<Plate>& plates, const double* density, double* p) {
    for (std::size_t k = 0; k < plates.size(); ++k) {
        const double v = std::clamp(density[k], 0.0, 1.0);
        if (!(v > 0)) continue;
        const double s = std::clamp(plates[k].solidity, 0.0, 1.0);
        const double keep = 1.0 - s * v;
        for (unsigned j = 0; j < 3; ++j) {
            const double c = plates[k].color[j] / 255.0;
            p[j] = keep * p[j] * (1.0 - v + v * c) + s * v * c;
        }
    }
}

namespace {

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

void check_name(const std::string& name, const char* what) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    if (name.empty() || code_points(name) > kInkNameMax || space(name.front()) || space(name.back())) {
        bad(std::string(what) + "'s name is 1..64 characters without spaces around it");
    }
}

void check_rgb(const std::array<int, 3>& rgb, const char* what) {
    for (const int c : rgb) {
        if (c < 0 || c > 255) bad(std::string(what) + "'s colour is three integers 0..255");
    }
}

std::array<int, 3> rgb_from(const Json& v, const char* what) {
    if (!v.is_array() || v.size() != 3) bad(std::string(what) + "'s colour is [r, g, b]");
    std::array<int, 3> out{};
    for (unsigned c = 0; c < 3; ++c) {
        if (!v[c].is_number_integer() || v[c].get<std::int64_t>() < 0 || v[c].get<std::int64_t>() > 255) {
            bad(std::string(what) + "'s colour is three integers 0..255");
        }
        out[c] = static_cast<int>(v[c].get<std::int64_t>());
    }
    return out;
}

// An object's keys: these, in any order (the journal's snapshots keep their keys sorted); `required` among them.
void check_keys(const Json& value, std::initializer_list<std::string_view> keys, std::initializer_list<std::string_view> required, const char* what) {
    if (!value.is_object()) bad(std::string(what) + " is an object");
    for (const auto& [key, v] : value.items()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) bad(std::string(what) + " has an unknown key " + py_repr_str(key));
    }
    for (const std::string_view key : required) {
        if (!value.contains(std::string(key))) bad(std::string(what) + " has no " + std::string(key));
    }
}

}  // namespace

void check_curve(const std::vector<CurvePoint>& curve) {
    if (curve.size() < 2 || curve.size() > kCurvePointsMax) bad("a curve has 2.." + std::to_string(kCurvePointsMax) + " points");
    for (std::size_t i = 0; i < curve.size(); ++i) {
        const double in = curve[i][0], out = curve[i][1];
        if (!std::isfinite(in) || !std::isfinite(out) || in < 0 || in > 100 || out < 0 || out > 100) bad("a curve's points are 0..100");
        if (i > 0 && !(in > curve[i - 1][0])) bad("a curve's inputs go up, each past the one before");
    }
}

void check_bitmap(const BitmapLook& look) {
    if (std::find(kBitmapMethods.begin(), kBitmapMethods.end(), look.method) == kBitmapMethods.end()) {
        bad("a bitmap's method is threshold, pattern, diffusion or halftone");
    }
    if (!std::isfinite(look.frequency) || look.frequency < 1 || look.frequency > 999) bad("a bitmap's frequency is 1..999");
    if (!std::isfinite(look.angle) || look.angle < -180 || look.angle > 180) bad("a bitmap's angle is -180..180");
    if (std::find(kHalftoneShapes.begin(), kHalftoneShapes.end(), look.shape) == kHalftoneShapes.end()) {
        bad("a bitmap's shape is round, ellipse, line, square, diamond or cross");
    }
}

void check_duotone(const std::vector<DuotoneInk>& inks) {
    if (inks.empty() || inks.size() > kDuotoneInksMax) bad("a duotone page has 1..4 inks");
    for (const DuotoneInk& ink : inks) {
        check_name(ink.name, "a duotone ink");
        check_rgb(ink.color, "a duotone ink");
        check_curve(ink.curve);
    }
}

void check_plates(const std::vector<Plate>& plates) {
    if (plates.empty() || plates.size() > kPlatesMax) bad("a multichannel page has 1..32 plates");
    std::set<std::string> names;
    for (const Plate& plate : plates) {
        check_name(plate.name, "a plate");
        if (!names.insert(plate.name).second) bad("two plates have the name " + py_repr_str(plate.name));
        check_rgb(plate.color, "a plate");
        if (!std::isfinite(plate.solidity) || plate.solidity < 0 || plate.solidity > 1) bad("a plate's solidity is 0..1");
        if (!plate.curve.empty()) check_curve(plate.curve);
    }
}

Json curve_json(const std::vector<CurvePoint>& curve) {
    Json out = Json::array();
    for (const CurvePoint& p : curve) out.push_back(Json::array({p[0], p[1]}));
    return out;
}

std::vector<CurvePoint> curve_from_json(const Json& value) {
    if (!value.is_array()) bad("a curve is [[in, out], …]");
    std::vector<CurvePoint> out;
    for (const Json& p : value) {
        if (!p.is_array() || p.size() != 2) bad("a curve is [[in, out], …]");
        const auto in = number(p[0]), o = number(p[1]);
        if (!in || !o) bad("a curve's points are numbers");
        out.push_back({*in, *o});
    }
    check_curve(out);
    return out;
}

Json duotone_json(const std::vector<DuotoneInk>& inks) {
    Json out = Json::array();
    for (const DuotoneInk& ink : inks) {
        Json item = Json::object();
        item["name"] = ink.name;
        item["color"] = Json::array({ink.color[0], ink.color[1], ink.color[2]});
        item["curve"] = curve_json(ink.curve);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<DuotoneInk> duotone_from_json(const Json& value) {
    if (!value.is_array()) bad("duotone is a list of inks");
    std::vector<DuotoneInk> out;
    for (const Json& item : value) {
        check_keys(item, {"name", "color", "curve"}, {"name", "color", "curve"}, "a duotone ink");
        DuotoneInk ink;
        if (!item["name"].is_string()) bad("a duotone ink's name is text");
        ink.name = item["name"].get<std::string>();
        ink.color = rgb_from(item["color"], "a duotone ink");
        ink.curve = curve_from_json(item["curve"]);
        out.push_back(std::move(ink));
    }
    check_duotone(out);
    return out;
}

namespace {

Json plates_json(const std::vector<Plate>& plates) {
    Json out = Json::array();
    for (const Plate& plate : plates) {
        Json item = Json::object();
        item["name"] = plate.name;
        item["color"] = Json::array({plate.color[0], plate.color[1], plate.color[2]});
        item["solidity"] = plate.solidity;
        if (plate.key) item["key"] = true;
        if (!plate.curve.empty()) item["curve"] = curve_json(plate.curve);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Plate> plates_from_json(const Json& value) {
    if (!value.is_array()) bad("plates is a list");
    std::vector<Plate> out;
    for (const Json& item : value) {
        check_keys(item, {"name", "color", "solidity", "key", "curve"}, {"name", "color", "solidity"}, "a plate");
        Plate plate;
        if (!item["name"].is_string()) bad("a plate's name is text");
        plate.name = item["name"].get<std::string>();
        plate.color = rgb_from(item["color"], "a plate");
        const auto solidity = number(item["solidity"]);
        if (!solidity) bad("a plate's solidity is a number 0..1");
        plate.solidity = *solidity;
        if (item.contains("key")) {
            if (!item["key"].is_boolean() || !item["key"].get<bool>()) bad("a plate's key is written only as true");
            plate.key = true;
        }
        if (item.contains("curve")) plate.curve = curve_from_json(item["curve"]);
        out.push_back(std::move(plate));
    }
    check_plates(out);
    return out;
}

Json bitmap_json(const BitmapLook& look) {
    Json out = Json::object();
    out["method"] = look.method;
    out["frequency"] = look.frequency;
    out["angle"] = look.angle;
    out["shape"] = look.shape;
    return out;
}

BitmapLook bitmap_from_json(const Json& value) {
    check_keys(value, {"method", "frequency", "angle", "shape"}, {"method", "frequency", "angle", "shape"}, "bitmap");
    BitmapLook out;
    if (!value["method"].is_string() || !value["shape"].is_string()) bad("a bitmap's method and shape are text");
    out.method = value["method"].get<std::string>();
    out.shape = value["shape"].get<std::string>();
    const auto frequency = number(value["frequency"]), angle = number(value["angle"]);
    if (!frequency || !angle) bad("a bitmap's frequency and angle are numbers");
    out.frequency = *frequency;
    out.angle = *angle;
    check_bitmap(out);
    return out;
}

constexpr std::array<std::string_view, 5> kFromModes{"rgb", "cmyk", "lab", "gray", "duotone"};

}  // namespace

void check_ink(const PageColor& color, const std::vector<double>& ink) {
    if (color.space != InkSpace::Multi) {
        check_ink(color.space, ink);
        return;
    }
    if (ink.size() != color.plates.size()) bad("an ink of a multichannel page has a value for each of its " + std::to_string(color.plates.size()) + " plates");
    for (const double v : ink) {
        if (!std::isfinite(v) || v < 0 || v > 1) bad("a plate's ink value is 0..1");
    }
}

void check_ink(InkSpace space, const std::vector<double>& ink) {
    if (space == InkSpace::Multi) {
        for (const double v : ink) {
            if (!std::isfinite(v) || v < 0 || v > 1) bad("a plate's ink value is 0..1");
        }
        return;
    }
    if (ink.size() != colour_channels(space)) {
        bad(std::string("an ink of a ") +
            (space == InkSpace::Cmyk ? "CMYK page has 4 values (c, m, y, k)" : space == InkSpace::Lab ? "Lab page has 3 values (L, a, b)" : "grey page has 1 value (k)"));
    }
    for (std::size_t i = 0; i < ink.size(); ++i) {
        const double v = ink[i];
        if (!std::isfinite(v)) bad("an ink value is not a finite number");
        if (space == InkSpace::Cmyk && !(v >= 0 && v <= 1)) bad("a CMYK ink value is 0..1");
        if (space == InkSpace::Gray && !(v >= 0 && v <= 1)) bad("a grey ink value is 0..1");
        if (space == InkSpace::Lab && i == 0 && !(v >= 0 && v <= 100)) bad("a Lab L is 0..100");
        if (space == InkSpace::Lab && i > 0 && !(v >= -128 && v <= 127)) bad("a Lab a or b is -128..127");
    }
}

Json to_json(const PageColor& color) {
    Json out = Json::object();
    out["mode"] = std::string(mode_name(color));
    out["depth"] = color.depth;
    if (color.space != InkSpace::Lab && !(color.space == InkSpace::Multi && color.profile.empty())) out["profile"] = color.profile;
    out["intent"] = color.intent;
    if (!color.bpc) out["bpc"] = false;
    Json inks = Json::array();
    for (const InkEntry& entry : color.inks) {
        Json ink = Json::array();
        for (const double v : entry.ink) ink.push_back(v);
        inks.push_back(Json::array({Json::array({entry.rgb[0], entry.rgb[1], entry.rgb[2]}), std::move(ink)}));
    }
    out["inks"] = std::move(inks);
    if (color.bitmap) out["bitmap"] = bitmap_json(*color.bitmap);
    if (!color.duotone.empty()) out["duotone"] = duotone_json(color.duotone);
    if (color.space == InkSpace::Multi) {
        out["plates"] = plates_json(color.plates);
        out["from"] = color.from;
    }
    return out;
}

PageColor page_color_from_json(const Json& value) {
    if (!value.is_object()) bad("it must be an object");
    // (written in this order; read in any: the journal's snapshots keep their keys sorted)
    static const std::set<std::string> kKeys{"mode", "depth", "profile", "intent", "bpc", "inks", "bitmap", "duotone", "plates", "from"};
    for (const auto& [key, v] : value.items()) {
        if (!kKeys.contains(key)) bad("unknown key " + py_repr_str(key));
    }
    PageColor out;
    const auto mode = value.find("mode");
    if (mode == value.end() || !mode->is_string()) bad("mode is cmyk, lab, gray, bitmap, duotone or multichannel");
    const std::string mode_text = mode->get<std::string>();
    const auto space = ink_space_from(mode_text);
    if (!space) bad("mode is cmyk, lab, gray, bitmap, duotone or multichannel");
    out.space = *space;
    // (each mode's own keys, and only its)
    const bool bitmap = mode_text == "bitmap", duotone = mode_text == "duotone", multi = out.space == InkSpace::Multi;
    if (bitmap != value.contains("bitmap")) bad(bitmap ? "a bitmap page has its bitmap" : "only a bitmap page has a bitmap");
    if (duotone != value.contains("duotone")) bad(duotone ? "a duotone page has its inks (duotone)" : "only a duotone page has duotone");
    if (multi != value.contains("plates") || multi != value.contains("from")) {
        bad(multi ? "a multichannel page has its plates and from" : "only a multichannel page has plates and from");
    }
    const auto depth = value.find("depth");
    if (depth == value.end() || !depth->is_number_integer() ||
        (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16 && depth->get<std::int64_t>() != 32)) {
        bad("depth is 8, 16 or 32");
    }
    out.depth = static_cast<int>(depth->get<std::int64_t>());
    if (bitmap && out.depth != 8) bad("a bitmap page is 8 bits");
    if (multi) {
        const Json& from = value["from"];
        if (!from.is_string() || std::find(kFromModes.begin(), kFromModes.end(), from.get<std::string>()) == kFromModes.end()) {
            bad("from is rgb, cmyk, lab, gray or duotone");
        }
        out.from = from.get<std::string>();
        out.plates = plates_from_json(value["plates"]);
    }
    if (bitmap) out.bitmap = bitmap_from_json(value["bitmap"]);
    if (duotone) out.duotone = duotone_from_json(value["duotone"]);
    const auto profile = value.find("profile");
    // (a multichannel page made from CMYK, grey or duotone keeps that profile: its elements' colours go through it)
    const bool profiled = out.space == InkSpace::Multi ? out.from == "cmyk" || out.from == "gray" || out.from == "duotone" : out.space != InkSpace::Lab;
    if (profiled) {
        if (profile == value.end() || !profile->is_string() || !is_ref(profile->get<std::string>())) {
            bad(std::string("a ") + (out.space == InkSpace::Cmyk ? "CMYK" : out.space == InkSpace::Multi ? "multichannel" : "grey") +
                " page names its ICC profile (sha256:<64 hex>)");
        }
        out.profile = profile->get<std::string>();
    } else if (profile != value.end()) {
        bad(out.space == InkSpace::Lab ? "a Lab page has no profile" : "a multichannel page from RGB or Lab has no profile");
    }
    const auto intent = value.find("intent");
    if (intent == value.end() || !intent->is_string() || !is_intent(intent->get<std::string>())) {
        bad("intent is perceptual, relative, saturation or absolute");
    }
    out.intent = intent->get<std::string>();
    if (const auto bpc = value.find("bpc"); bpc != value.end()) {
        if (!bpc->is_boolean() || bpc->get<bool>()) bad("bpc is written only as false");
        out.bpc = false;
    }
    const auto inks = value.find("inks");
    if (inks == value.end() || !inks->is_array()) bad("inks is a list");
    if (inks->size() > kInksMax) bad("a page has at most " + std::to_string(kInksMax) + " inks");
    std::set<std::array<int, 3>> seen;
    for (const Json& item : *inks) {
        if (!item.is_array() || item.size() != 2 || !item[0].is_array() || item[0].size() != 3 || !item[1].is_array()) {
            bad("each ink is [[r, g, b], [values]]");
        }
        InkEntry entry;
        for (unsigned c = 0; c < 3; ++c) {
            const Json& v = item[0][c];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::int64_t>() > 255) bad("an ink's RGB is three integers 0..255");
            entry.rgb[c] = static_cast<int>(v.get<std::int64_t>());
        }
        for (const Json& v : item[1]) {
            const auto d = number(v);
            if (!d) bad("an ink value is not a finite number");
            entry.ink.push_back(*d);
        }
        check_ink(out, entry.ink);
        if (!seen.insert(entry.rgb).second) bad("an RGB colour is in the table twice");
        out.inks.push_back(std::move(entry));
    }
    return out;
}

const InkEntry* ink_for(const PageColor& color, const std::array<int, 3>& rgb) {
    for (const InkEntry& entry : color.inks) {
        if (entry.rgb == rgb) return &entry;
    }
    return nullptr;
}

std::string bytes_ref(std::string_view bytes) {
    return "sha256:" + QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                                QCryptographicHash::Sha256).toHex().toStdString();
}

bool lab_blend(std::string_view mode) {
    static constexpr std::array<std::string_view, 9> kModes{"",         "normal",      "multiply",   "screen",    "overlay",
                                                            "add",      "linear_burn", "soft_light", "hard_light"};
    return std::find(kModes.begin(), kModes.end(), mode) != kModes.end();
}

bool multi_blend(std::string_view mode) { return mode != "hue" && mode != "saturation" && mode != "color" && mode != "luminosity"; }

void validate_ink_document(const Document& doc) {
    if (doc.color && doc.rgb) throw Error("value", "the book has two colour models (inks and RGB) for its new pages");
    for (const auto& page : doc.pages) {
        if (std::find(doc.deferred.begin(), doc.deferred.end(), page) != doc.deferred.end()) continue;
        if (page->color && page->rgb) throw Error("value", "a page has two colour models (inks and RGB)");
        for (const Layer& layer : page->layers) {
            const int kinds = (layer.raster_png && !layer.raster_png->empty()) + (layer.color_raster != nullptr) + (layer.ink_raster != nullptr);
            if (kinds > 1) throw Error("value", "a layer holds one kind of pixels at most (layer " + layer.id + ")");
            if (page->color && page->color->space == InkSpace::Lab && !lab_blend(layer.blend)) {
                throw Error("value", "a Lab page cannot composite the blend " + py_repr_str(layer.blend) + " (layer " + layer.id + ")");
            }
            if (page->color && page->color->space == InkSpace::Multi && !multi_blend(layer.blend)) {
                throw Error("value", "a multichannel page cannot composite the blend " + py_repr_str(layer.blend) + " (layer " + layer.id + ")");
            }
            if (page->color && ((layer.raster_png && !layer.raster_png->empty()) || layer.color_raster)) {
                // (what drew RGB pixels on it does not know the page's inks yet)
                throw Error("not_yet_ported", "RGB pixels on a CMYK, Lab or grey page are not in this build yet (layer " + layer.id + ")");
            }
            if (!layer.ink_raster) continue;
            if (!page->color) throw Error("value", "an RGB page holds no ink pixels (layer " + layer.id + ")");
            const InkRasterHeader header = ink_raster_header(*layer.ink_raster);
            if (header.space != page->color->space || header.depth != page->color->depth ||
                (header.space == InkSpace::Multi && header.planes != page->color->plates.size())) {
                throw Error("value", "the layer " + layer.id + "'s pixels are not in its page's colour mode");
            }
        }
    }
    // (color-model@1, PC1d: each mode's own parts as they must be)
    const auto check_mode = [](const PageColor& color) {
        if (color.bitmap && (color.space != InkSpace::Gray || color.depth != 8 || !color.duotone.empty())) {
            throw Error("value", "a bitmap page is a grey page of 8 bits");
        }
        if (!color.duotone.empty() && color.space != InkSpace::Gray) throw Error("value", "a duotone page is a grey page");
        if (color.bitmap) check_bitmap(*color.bitmap);
        if (!color.duotone.empty()) check_duotone(color.duotone);
        if ((color.space == InkSpace::Multi) != !color.plates.empty()) throw Error("value", "a multichannel page has its plates");
        if (color.space == InkSpace::Multi) check_plates(color.plates);
    };
    if (doc.color) check_mode(*doc.color);
    for (const auto& page : doc.pages) {
        if (page->color && std::find(doc.deferred.begin(), doc.deferred.end(), page) == doc.deferred.end()) check_mode(*page->color);
    }
}

void InkPixels::set(std::size_t i, unsigned c, double v) {
    const std::size_t at = i * channels() + c;
    if (depth == 32) {
        if (!(c < colours() && traits(space).hdr)) v = std::clamp(v, 0.0, 1.0);
        floats[at] = static_cast<float>(v);
        return;
    }
    samples[at] = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * max()));
}

void InkPixels::copy_pixel(std::size_t i, const InkPixels& from, std::size_t j) {
    const unsigned all = channels();
    if (depth == 32) {
        std::copy_n(from.floats.data() + j * all, all, floats.data() + i * all);
    } else {
        std::copy_n(from.samples.data() + j * all, all, samples.data() + i * all);
    }
}

InkPixels blank_ink_pixels(InkSpace space, int depth, std::uint32_t width, std::uint32_t height, unsigned planes) {
    InkPixels out;
    out.space = space;
    out.depth = depth;
    out.width = width;
    out.height = height;
    out.planes = space == InkSpace::Multi ? planes : 0;
    if (space == InkSpace::Multi && (planes < 1 || planes > 32)) bad_raster("a multichannel raster has 1..32 planes");
    if (depth != 8 && depth != 16 && depth != 32) bad_raster("its depth is not 8, 16 or 32");
    const unsigned all = out.channels();
    check_size(width, height, all, depth);
    if (depth == 32) out.floats.assign(out.pixel_count() * all, 0.0f);
    else out.samples.assign(out.pixel_count() * all, 0);
    if (space == InkSpace::Lab || space == InkSpace::Gray) {  // (white: L 100, a = b = 0; a grey's lightness 1; transparent)
        for (std::size_t i = 0; i < out.pixel_count(); ++i) {
            out.set(i, 0, 1.0);
            if (space == InkSpace::Lab) {
                if (depth == 32) {
                    out.floats[i * all + 1] = out.floats[i * all + 2] = static_cast<float>(128.0 / 255.0);
                } else {
                    const std::uint16_t mid = depth == 8 ? 128 : 32896;
                    out.samples[i * all + 1] = out.samples[i * all + 2] = mid;
                }
            }
        }
    }
    return out;
}

InkRasterHeader ink_raster_header(std::string_view bytes) {
    if (bytes.size() < kHeader || bytes.substr(0, 4) != "GKIR") bad_raster("it is not GKIR");
    const unsigned version = static_cast<unsigned char>(bytes[4]);
    if (version != 1 && version != 2) bad_raster("its version is not 1 or 2");
    InkRasterHeader out;
    out.version = version;
    const unsigned space = static_cast<unsigned char>(bytes[5]);
    const auto known = std::find_if(kTraits.begin(), kTraits.end(), [&](const SpaceTraits& t) { return t.code == space; });
    if (known == kTraits.end()) bad_raster("its colour space is not one this build knows");
    out.space = static_cast<InkSpace>(known - kTraits.begin());
    const unsigned depth = static_cast<unsigned char>(bytes[6]);
    if (depth != 8 && depth != 16 && depth != 32) bad_raster("its depth is not 8, 16 or 32");
    out.depth = static_cast<int>(depth);
    const unsigned planes = static_cast<unsigned char>(bytes[7]);
    const bool v1 = (out.space == InkSpace::Cmyk || out.space == InkSpace::Lab) && depth != 32;
    if (version == 1) {
        if (!v1) bad_raster("version 1 holds CMYK or Lab at 8 or 16 bits only");
        if (planes != 0) bad_raster("its reserved byte is not 0");
        out.planes = colour_channels(out.space);
    } else {
        // (version 2 only for what version 1 cannot hold: the same pixels always give the same bytes)
        if (v1) bad_raster("version 2 does not hold what version 1 does");
        if (out.space == InkSpace::Multi ? (planes < 1 || planes > 32) : planes != colour_channels(out.space)) {
            bad_raster("its count of planes is not its colour space's");
        }
        out.planes = planes;
    }
    out.width = le32(bytes, 8);
    out.height = le32(bytes, 12);
    check_size(out.width, out.height, out.planes + (traits(out.space).alpha ? 1 : 0), out.depth);
    return out;
}

InkPixels decode_ink_raster(std::string_view bytes) {
    const InkRasterHeader header = ink_raster_header(bytes);
    InkPixels out;
    out.space = header.space;
    out.depth = header.depth;
    out.width = header.width;
    out.height = header.height;
    out.planes = header.space == InkSpace::Multi ? header.planes : 0;
    const unsigned channels = out.channels();
    const std::size_t rb = row_bytes(out.width, channels, out.depth);
    const std::size_t bpp = std::size_t(channels) * sample_bytes(out.depth);
    const std::size_t total = (rb + 1) * out.height;
    std::string raw(total, '\0');

    z_stream z{};
    if (inflateInit(&z) != Z_OK) bad_raster("zlib cannot start");
    const std::string_view stream = bytes.substr(kHeader);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(stream.data()));
    z.next_out = reinterpret_cast<Bytef*>(raw.data());
    std::size_t in_left = stream.size(), out_left = total;
    int status = Z_OK;
    while (true) {
        const uInt in_chunk = static_cast<uInt>(std::min<std::size_t>(in_left, 1u << 30));
        const uInt out_chunk = static_cast<uInt>(std::min<std::size_t>(out_left, 1u << 30));
        z.avail_in = in_chunk;
        z.avail_out = out_chunk;
        status = inflate(&z, Z_NO_FLUSH);
        const std::size_t took = in_chunk - z.avail_in, gave = out_chunk - z.avail_out;
        in_left -= took;
        out_left -= gave;
        if (status == Z_STREAM_END) break;
        if (status != Z_OK && status != Z_BUF_ERROR) break;
        if (took == 0 && gave == 0) break;  // (no progress: input or room ran out)
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) bad_raster(out_left == 0 ? "it holds more pixels than its size" : "its pixels are cut short or broken");
    if (out_left != 0) bad_raster("it holds fewer pixels than its size");
    if (in_left != 0) bad_raster("bytes follow its pixels");

    auto* data = reinterpret_cast<unsigned char*>(raw.data());
    if (out.depth == 32) out.floats.resize(out.pixel_count() * channels);
    else out.samples.resize(out.pixel_count() * channels);
    const unsigned colours = out.colours();
    for (std::uint32_t y = 0; y < out.height; ++y) {
        unsigned char* line = data + std::size_t(y) * (rb + 1);
        const int type = line[0];
        if (type > 4) bad_raster("a row has an unknown filter");
        const unsigned char* above = y > 0 ? data + std::size_t(y - 1) * (rb + 1) + 1 : nullptr;
        unfilter_row(type, line + 1, above, rb, bpp);
        if (out.depth == 8) {
            std::uint16_t* samples = out.samples.data() + std::size_t(y) * out.width * channels;
            for (std::size_t i = 0; i < rb; ++i) samples[i] = line[1 + i];
        } else if (out.depth == 16) {
            std::uint16_t* samples = out.samples.data() + std::size_t(y) * out.width * channels;
            for (std::size_t i = 0; i < rb / 2; ++i) samples[i] = static_cast<std::uint16_t>((line[1 + 2 * i] << 8) | line[2 + 2 * i]);
        } else {
            float* floats = out.floats.data() + std::size_t(y) * out.width * channels;
            for (std::size_t i = 0; i < rb / 4; ++i) {
                const unsigned char* b = line + 1 + 4 * i;
                const std::uint32_t bits = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
                const float v = std::bit_cast<float>(bits);
                if (!float_ok(out.space, colours, static_cast<unsigned>(i % channels), v)) bad_raster("a 32-bit sample is not finite or out of its range");
                floats[i] = v;
            }
        }
    }
    return out;
}

std::string encode_ink_raster(const InkPixels& pixels) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if (pixels.depth != 8 && pixels.depth != 16 && pixels.depth != 32) bad_raster("its depth is not 8, 16 or 32");
    if (pixels.space == InkSpace::Multi && (pixels.planes < 1 || pixels.planes > 32)) bad_raster("a multichannel raster has 1..32 planes");
    const unsigned channels = pixels.channels(), colours = pixels.colours();
    check_size(pixels.width, pixels.height, channels, pixels.depth);
    if (pixels.depth == 32 ? pixels.floats.size() != pixels.pixel_count() * channels : pixels.samples.size() != pixels.pixel_count() * channels) {
        bad_raster("its samples do not fill its size");
    }
    const bool v1 = (pixels.space == InkSpace::Cmyk || pixels.space == InkSpace::Lab) && pixels.depth != 32;
    const std::uint32_t most = pixels.max();
    std::string out;
    out.append("GKIR");
    out.push_back(static_cast<char>(v1 ? 1 : 2));
    out.push_back(static_cast<char>(traits(pixels.space).code));
    out.push_back(static_cast<char>(pixels.depth));
    out.push_back(static_cast<char>(v1 ? 0 : colours));
    put_le32(out, pixels.width);
    put_le32(out, pixels.height);

    const std::size_t rb = row_bytes(pixels.width, channels, pixels.depth);
    const std::size_t bpp = std::size_t(channels) * sample_bytes(pixels.depth);
    std::vector<unsigned char> row(rb), above(rb), best(rb + 1), trial(rb);
    z_stream z{};
    if (deflateInit(&z, 6) != Z_OK) bad_raster("zlib cannot start");
    std::vector<unsigned char> buffer(1 << 16);
    const auto pump = [&](const unsigned char* data, std::size_t n, int flush) {
        z.next_in = const_cast<Bytef*>(data);
        z.avail_in = static_cast<uInt>(n);
        do {
            z.next_out = buffer.data();
            z.avail_out = static_cast<uInt>(buffer.size());
            const int status = deflate(&z, flush);
            if (status == Z_STREAM_ERROR) {
                deflateEnd(&z);
                bad_raster("zlib cannot compress it");
            }
            out.append(reinterpret_cast<const char*>(buffer.data()), buffer.size() - z.avail_out);
        } while (z.avail_out == 0 || (flush == Z_FINISH && z.avail_in != 0));
    };
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        if (pixels.depth == 8) {
            const std::uint16_t* samples = pixels.samples.data() + std::size_t(y) * pixels.width * channels;
            for (std::size_t i = 0; i < rb; ++i) row[i] = static_cast<unsigned char>(std::min<std::uint32_t>(samples[i], most));
        } else if (pixels.depth == 16) {
            const std::uint16_t* samples = pixels.samples.data() + std::size_t(y) * pixels.width * channels;
            for (std::size_t i = 0; i < rb / 2; ++i) {
                row[2 * i] = static_cast<unsigned char>(samples[i] >> 8);
                row[2 * i + 1] = static_cast<unsigned char>(samples[i] & 255);
            }
        } else {
            const float* floats = pixels.floats.data() + std::size_t(y) * pixels.width * channels;
            for (std::size_t i = 0; i < rb / 4; ++i) {
                if (!float_ok(pixels.space, colours, static_cast<unsigned>(i % channels), floats[i])) {
                    deflateEnd(&z);
                    bad_raster("a 32-bit sample is not finite or out of its range");
                }
                const std::uint32_t bits = std::bit_cast<std::uint32_t>(floats[i]);
                for (unsigned k = 0; k < 4; ++k) row[4 * i + k] = static_cast<unsigned char>((bits >> (24 - 8 * k)) & 255);
            }
        }
        // (libpng's choice: the filter whose bytes, read as signed, sum to the least)
        std::uint64_t least = UINT64_MAX;
        for (int type = 0; type <= 4; ++type) {
            filter_row(type, row.data(), y > 0 ? above.data() : nullptr, rb, bpp, trial.data());
            std::uint64_t sum = 0;
            for (const unsigned char b : trial) sum += b < 128 ? b : 256 - b;
            if (sum < least) {
                least = sum;
                best[0] = static_cast<unsigned char>(type);
                std::memcpy(best.data() + 1, trial.data(), rb);
            }
        }
        pump(best.data(), rb + 1, Z_NO_FLUSH);
        std::swap(row, above);
    }
    pump(nullptr, 0, Z_FINISH);
    deflateEnd(&z);
    return out;
}

void read_ink_raster_rows(std::string_view bytes, const std::function<void(std::uint32_t, const float*)>& row) {
    const InkRasterHeader header = ink_raster_header(bytes);
    const unsigned colours = header.planes, channels = colours + (traits(header.space).alpha ? 1 : 0);
    const std::size_t rb = row_bytes(header.width, channels, header.depth);
    const std::size_t bpp = std::size_t(channels) * sample_bytes(header.depth);
    const double most = header.depth == 8 ? 255.0 : 65535.0;
    std::vector<unsigned char> line(rb + 1), above(rb);
    std::vector<float> values(std::size_t(header.width) * channels);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) bad_raster("zlib cannot start");
    struct End {
        z_stream* z;
        ~End() { inflateEnd(z); }
    } end{&z};
    const std::string_view stream = bytes.substr(kHeader);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(stream.data()));
    std::size_t in_left = stream.size();
    int status = Z_OK;
    // (as decode_ink_raster: every row whole, then the stream's end and nothing after it)
    const auto fill = [&](unsigned char* out, std::size_t n) {
        std::size_t out_left = n;
        while (out_left > 0) {
            if (status == Z_STREAM_END) return out_left;
            const uInt in_chunk = static_cast<uInt>(std::min<std::size_t>(in_left, 1u << 30));
            z.avail_in = in_chunk;
            z.next_out = out + (n - out_left);
            z.avail_out = static_cast<uInt>(out_left);
            status = inflate(&z, Z_NO_FLUSH);
            const std::size_t took = in_chunk - z.avail_in, gave = out_left - z.avail_out;
            in_left -= took;
            out_left -= gave;
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) bad_raster("its pixels are cut short or broken");
            if (took == 0 && gave == 0 && status != Z_STREAM_END) bad_raster("its pixels are cut short or broken");
        }
        return out_left;
    };
    for (std::uint32_t y = 0; y < header.height; ++y) {
        if (fill(line.data(), rb + 1) != 0) bad_raster("it holds fewer pixels than its size");
        const int type = line[0];
        if (type > 4) bad_raster("a row has an unknown filter");
        unfilter_row(type, line.data() + 1, y > 0 ? above.data() : nullptr, rb, bpp);
        const unsigned char* b = line.data() + 1;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (header.depth == 8) {
                values[i] = static_cast<float>(b[i] / most);
            } else if (header.depth == 16) {
                values[i] = static_cast<float>(((b[2 * i] << 8) | b[2 * i + 1]) / most);
            } else {
                const unsigned char* f = b + 4 * i;
                const float v = std::bit_cast<float>((std::uint32_t(f[0]) << 24) | (std::uint32_t(f[1]) << 16) | (std::uint32_t(f[2]) << 8) | f[3]);
                if (!float_ok(header.space, colours, static_cast<unsigned>(i % channels), v)) bad_raster("a 32-bit sample is not finite or out of its range");
                values[i] = v;
            }
        }
        row(y, values.data());
        std::memcpy(above.data(), line.data() + 1, rb);
    }
    unsigned char extra = 0;
    if (status != Z_STREAM_END && fill(&extra, 1) == 0) bad_raster("it holds more pixels than its size");
    if (status != Z_STREAM_END) bad_raster("its pixels are cut short or broken");
    if (in_left != 0) bad_raster("bytes follow its pixels");
}

}  // namespace genko::core
