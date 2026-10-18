// PC2b: the codecs of the colour kinds' PSD adjustment layers for the table of render/psd_adjust.hpp — hue2
// (hue_saturation), vibA (vibrance), blnc (color_balance), blwh (black_white), phfl (photo_filter), mixr
// (channel_mixer), selc (selective_color). PhotoCraft v0.2.0 io/src/adjust_map.rs (177-197, 315-329, 374-485,
// 508-695; the Adobe spec's "Adjustment layer" section); attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md. vibA and blwh are ActionDescriptors (a version 16, then the descriptor),
// read and written by the PSD's one descriptor reader and writer (render/psd_descriptor). Python's hue is written as the
// nearest hue2 (design 4.4; read back, a hue2 is hue_saturation).
//
// A block read is the correction's normal form (core::normalize_adjust: the table's ranges and defaults, the kind's own
// normal form — the hue ranges' sliders in their canonical form); one written holds what the PSD can: whole numbers
// (Photoshop's fields), the photo filter's colour at 16 bits.

#include "render/psd_adjust.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "core/adjust.hpp"
#include "core/adjust_color.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/psd_descriptor.hpp"
#include "render/quantize.hpp"

namespace genko::render::psd {

namespace {

using core::Json;
namespace color = core::color_adjust;

// --- bytes ------------------------------------------------------------------------------------------------------------

std::optional<std::uint32_t> be16(std::string_view d, std::size_t at) {
    if (at + 2 > d.size()) return std::nullopt;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]);
}
std::optional<std::int32_t> bei16(std::string_view d, std::size_t at) {
    const auto v = be16(d, at);
    if (!v) return std::nullopt;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(*v));
}
std::optional<std::uint32_t> be32(std::string_view d, std::size_t at) {
    const auto hi = be16(d, at), lo = be16(d, at + 2);
    if (!hi || !lo) return std::nullopt;
    return (*hi << 16) | *lo;
}

using descriptor::put_u16;
using descriptor::put_u32;
using descriptor::put_u8;
// A whole number as Photoshop's i16 field (rounded, held to its range).
void put_i16(std::string& out, double v) {
    const double r = std::clamp(std::round(v), -32768.0, 32767.0);
    put_u16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(r)));
}
// A whole number as a descriptor's long (rounded, held to its range).
std::int32_t whole(double v) { return static_cast<std::int32_t>(std::clamp(std::round(v), -2147483648.0, 2147483647.0)); }

// --- ActionDescriptor (vibA, blwh: render/psd_descriptor, the one reader and writer) ---------------------------------

// A descriptor's number of any number type, or none.
std::optional<double> number_in(const descriptor::Object& d, std::string_view key) {
    const descriptor::Value* v = d.get(key);
    return v != nullptr ? v->numeric() : std::nullopt;
}

// --- the codecs -------------------------------------------------------------------------------------------------------

std::optional<Json> unread(std::string_view key, std::vector<std::string>& notes, std::string_view why) {
    note_unread(notes, key, why);
    return std::nullopt;
}

// The settings read as an op reads them: the correction's normal form.
std::optional<Json> normal(std::string_view key, const Json& adjust, std::vector<std::string>& notes) {
    try {
        return core::normalize_adjust(adjust, nullptr);
    } catch (const core::OpError&) {
        return unread(key, notes, "値が使えない");
    }
}

// The stored correction as the codec writes it (its defaults filled: a book of another build may leave some out);
// none when its settings cannot be read (then it is in the merged picture only).
std::optional<Json> settings_of(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    try {
        return core::normalize_adjust(adjust, &page);
    } catch (const std::exception&) {
        note_unwritten(notes, adjust.value("kind", std::string()), "設定が読めない");
        return std::nullopt;
    }
}

double num(const Json& s, std::string_view key) { return s.at(std::string(key)).get<double>(); }

// A descriptor's number as a setting (a whole one as an integer: the stored form's 40, not 40.0).
Json number_json(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

Json ints(std::initializer_list<std::int32_t> values) {
    Json out = Json::array();
    for (const std::int32_t v : values) out.push_back(v);
    return out;
}

// hue2: version 2, colorize (1 byte), a pad, the colorize hue/saturation/lightness and the master's (i16 each), then the
// six ranges: their four sliders (degrees) and hue, saturation, lightness.
std::optional<Json> read_hue2(std::string_view d, std::vector<std::string>& notes) {
    if (d.size() < 16 || be16(d, 0) != 2u) return unread("hue2", notes, "読めない形の");
    const bool colorize = d[2] != 0;
    const std::size_t base = colorize ? 4 : 10;
    Json adjust{{"kind", "hue_saturation"}, {"hue", *bei16(d, base)}, {"saturation", *bei16(d, base + 2)}, {"lightness", *bei16(d, base + 4)},
                {"colorize", colorize}};
    for (std::size_t i = 0; i < 6; ++i) {
        const std::size_t at = 16 + i * 14;
        std::array<std::optional<std::int32_t>, 7> v;
        for (std::size_t k = 0; k < 7; ++k) v[k] = bei16(d, at + 2 * k);
        if (std::any_of(v.begin(), v.end(), [](const auto& x) { return !x; })) continue;  // (a short block: the range's defaults)
        adjust[std::string(color::kHueRanges[i])] = Json{{"hue", *v[4]}, {"saturation", *v[5]}, {"lightness", *v[6]}, {"range", ints({*v[0], *v[1], *v[2], *v[3]})}};
    }
    return normal("hue2", adjust, notes);
}

std::optional<std::string> write_hue2(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    std::string v;
    put_u16(v, 2);
    put_u8(v, s->at("colorize").get<bool>() ? 1 : 0);
    put_u8(v, 0);
    for (int twice = 0; twice < 2; ++twice) {
        for (const char* key : {"hue", "saturation", "lightness"}) put_i16(v, num(*s, key));
    }
    for (const std::string_view key : color::kHueRanges) {
        const Json& r = s->at(std::string(key));
        for (const Json& x : r.at("range")) put_u16(v, static_cast<std::uint32_t>(std::lround(std::fmod(std::fmod(x.get<double>(), 360) + 360, 360))) % 360);
        for (const char* k : {"hue", "saturation", "lightness"}) put_i16(v, num(r, k));
    }
    return v;
}

// Python's hue (filters.py: HSV — its hue turned by int(float(shift)) % 360°, its saturation and value multiplied, each
// at least 0) as the nearest Hue/Saturation: the turn its hue (−180..180), the saturation's factor its saturation and
// the value's its lightness, (f − 1)·100 each (−100..100: the same colours for a saturation up to twice and a value up to
// once — lighter, Photoshop's lightness goes towards white), said in the notes. Not written when Python cannot read it
// (the layer does nothing there), said.
std::optional<std::string> write_python_hue(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    double turn = 30, saturation = 1, value = 1;
    try {
        const auto read = [&adjust](const char* key, double fallback) {
            const Json* v = core::get(adjust, key);
            return v == nullptr ? fallback : core::to_float(*v);
        };
        turn = read("shift", 30);
        saturation = std::max(0.0, read("saturation", 1));
        value = std::max(0.0, read("value", 1));
    } catch (const core::Error&) {
        note_unwritten(notes, "hue", "値が読めない");
        return std::nullopt;
    }
    if (!std::isfinite(turn) || !std::isfinite(saturation) || !std::isfinite(value)) {
        note_unwritten(notes, "hue", "値が数でない");
        return std::nullopt;
    }
    const double whole_turn = std::fmod(std::fmod(std::trunc(turn), 360.0) + 360.0, 360.0);
    const Json nearest{{"kind", "hue_saturation"},
                       {"hue", number_json(whole_turn > 180 ? whole_turn - 360 : whole_turn)},
                       {"saturation", number_json(std::round(std::clamp((saturation - 1) * 100, -100.0, 100.0)))},
                       {"lightness", number_json(std::round(std::clamp((value - 1) * 100, -100.0, 100.0)))}};
    notes.push_back("Genko（Python）の色相・彩度・明度の補正レイヤーは、Photoshop の色相・彩度の近い値として書きました（色の変わり方が少し違うことがあります）");
    return write_hue2(nearest, page, notes);
}

// vibA: a versioned descriptor of vibrance and Strt (saturation), whole numbers; Photoshop leaves out a 0.
std::optional<Json> read_viba(std::string_view d, std::vector<std::string>& notes) {
    try {
        const descriptor::Object desc = descriptor::read_versioned(d);
        return normal("vibA",
                      Json{{"kind", "vibrance"}, {"vibrance", number_json(number_in(desc, "vibrance").value_or(0))},
                           {"saturation", number_json(number_in(desc, "Strt").value_or(0))}},
                      notes);
    } catch (const descriptor::Broken&) {
        return unread("vibA", notes, "読めない形の");
    }
}

std::optional<std::string> write_viba(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    descriptor::Writer d;
    d.integer("vibrance", whole(num(*s, "vibrance")));
    d.integer("Strt", whole(num(*s, "saturation")));
    return d.versioned("null");
}

// blnc: the shadows, midtones and highlights (cyan−red, magenta−green, yellow−blue: i16 each), preserve luminosity.
std::optional<Json> read_blnc(std::string_view d, std::vector<std::string>& notes) {
    if (d.size() < 19) return unread("blnc", notes, "読めない形の");
    Json adjust{{"kind", "color_balance"}};
    const char* keys[] = {"shadows", "midtones", "highlights"};
    for (std::size_t k = 0; k < 3; ++k) adjust[keys[k]] = ints({*bei16(d, (k * 3) * 2), *bei16(d, (k * 3 + 1) * 2), *bei16(d, (k * 3 + 2) * 2)});
    adjust["preserve_luminosity"] = d[18] != 0;
    return normal("blnc", adjust, notes);
}

std::optional<std::string> write_blnc(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    std::string v;
    for (const char* key : {"shadows", "midtones", "highlights"}) {
        for (const Json& x : s->at(key)) put_i16(v, x.get<double>());
    }
    put_u8(v, s->at("preserve_luminosity").get<bool>() ? 1 : 0);
    put_u8(v, 0);
    return v;
}

// blwh: a versioned descriptor of the six weights (Rd  , Yllw, Grn , Cyn , Bl  , Mgnt), useTint and tintColor.
constexpr std::array<std::string_view, 6> kWeightKeys{"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};

// A descriptor's colour as 0..255 RGB (RGBC: 0..255 numbers, or the newer 0..1 floats; Grsc: grey ink %; CMYC: inks %,
// naively), or none.
std::optional<std::array<double, 3>> colour_of(const descriptor::Object& c) {
    const auto g = [&c](std::string_view k) { return number_in(c, k); };
    if (const auto r = g("redFloat"), gr = g("greenFloat"), b = g("blueFloat"); r && gr && b) return std::array<double, 3>{*r * 255, *gr * 255, *b * 255};
    if (c.class_id == "Grsc") {
        if (const auto k = g("Gry ")) {
            const double v = (1 - *k / 100) * 255;
            return std::array<double, 3>{v, v, v};
        }
        return std::nullopt;
    }
    if (c.class_id == "CMYC") {
        const auto cy = g("Cyn "), m = g("Mgnt"), y = g("Ylw "), k = g("Blck");
        if (!cy || !m || !y || !k) return std::nullopt;
        const double kk = 1 - *k / 100;
        return std::array<double, 3>{(1 - *cy / 100) * kk * 255, (1 - *m / 100) * kk * 255, (1 - *y / 100) * kk * 255};
    }
    const auto r = g("Rd  "), gr = g("Grn "), b = g("Bl  ");
    if (!r || !gr || !b) return std::nullopt;
    return std::array<double, 3>{*r, *gr, *b};
}

Json rgb_json(const std::array<double, 3>& c) {
    return ints({static_cast<std::int32_t>(std::lround(std::clamp(c[0], 0.0, 255.0))), static_cast<std::int32_t>(std::lround(std::clamp(c[1], 0.0, 255.0))),
                 static_cast<std::int32_t>(std::lround(std::clamp(c[2], 0.0, 255.0)))});
}

std::optional<Json> read_blwh(std::string_view d, std::vector<std::string>& notes) {
    try {
        const descriptor::Object desc = descriptor::read_versioned(d);
        Json adjust{{"kind", "black_white"}};
        for (std::size_t i = 0; i < 6; ++i) {
            if (const auto w = number_in(desc, kWeightKeys[i])) adjust[std::string(color::kHueRanges[i])] = number_json(*w);
        }
        const descriptor::Value* tint = desc.get("useTint");
        adjust["tint"] = tint != nullptr && tint->type == "bool" && tint->boolean;
        if (const descriptor::Value* colour = desc.get("tintColor"); colour != nullptr && colour->object) {
            if (const auto rgb = colour_of(*colour->object)) adjust["tint_color"] = rgb_json(*rgb);
        }
        return normal("blwh", adjust, notes);
    } catch (const descriptor::Broken&) {
        return unread("blwh", notes, "読めない形の");
    }
}

std::optional<std::string> write_blwh(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    descriptor::Writer d;
    for (std::size_t i = 0; i < 6; ++i) d.integer(kWeightKeys[i], whole(num(*s, color::kHueRanges[i])));
    d.boolean("useTint", s->at("tint").get<bool>());
    descriptor::Writer colour;
    const Json& rgb = s->at("tint_color");
    colour.number("Rd  ", rgb[0].get<double>());
    colour.number("Grn ", rgb[1].get<double>());
    colour.number("Bl  ", rgb[2].get<double>());
    d.object("tintColor", colour, "RGBC");
    d.integer("bwPresetKind", 1);
    d.text("blackAndWhitePresetFileName", u"");
    return d.versioned("null");
}

// phfl: version 2 (a colour structure: its space — RGB 0, CMYK 2, Lab 7, grey 8 — and four u16) or 3 (Lab × 100 in
// three i32), then the density (u32 percent) and preserve luminosity (1 byte).
std::optional<Json> read_phfl(std::string_view d, std::vector<std::string>& notes) {
    const auto version = be16(d, 0);
    std::array<double, 3> rgb{};  // (0..1)
    std::size_t at = 0;
    if (version == 2u) {
        const auto space = be16(d, 2);
        const auto c0 = be16(d, 4), c1 = be16(d, 6), c2 = be16(d, 8), c3 = be16(d, 10);
        if (!space || !c0 || !c1 || !c2 || !c3) return unread("phfl", notes, "読めない形の");
        const auto n = [](std::uint32_t v) { return v / 65535.0; };
        if (*space == 0) {
            rgb = {n(*c0), n(*c1), n(*c2)};
        } else if (*space == 2) {  // (inverted: 65535 no ink; naively, (1 − c)(1 − k))
            const double k = n(*c3);
            rgb = {n(*c0) * k, n(*c1) * k, n(*c2) * k};
        } else if (*space == 7) {
            const auto lab = quantize::lab_to_srgb({static_cast<float>(std::min<std::uint32_t>(*c0, 10000)) / 100.0F,
                                                    static_cast<float>(static_cast<std::int16_t>(*c1)) / 100.0F,
                                                    static_cast<float>(static_cast<std::int16_t>(*c2)) / 100.0F});
            rgb = {lab[0], lab[1], lab[2]};
        } else if (*space == 8) {
            const double v = 1 - std::min<std::uint32_t>(*c0, 10000) / 10000.0;
            rgb = {v, v, v};
        } else {
            return unread("phfl", notes, "読めない色空間の");
        }
        at = 12;
    } else if (version == 3u) {
        const auto l = be32(d, 2), a = be32(d, 6), b = be32(d, 10);
        if (!l || !a || !b) return unread("phfl", notes, "読めない形の");
        const std::array<float, 3> lab{static_cast<float>(static_cast<std::int32_t>(*l)) / 100.0F, static_cast<float>(static_cast<std::int32_t>(*a)) / 100.0F,
                                       static_cast<float>(static_cast<std::int32_t>(*b)) / 100.0F};
        if (!(lab[0] >= 0 && lab[0] <= 100) || std::fabs(lab[1]) > 128 || std::fabs(lab[2]) > 128) return unread("phfl", notes, "読めない色の");
        const auto s = quantize::lab_to_srgb(lab);
        rgb = {s[0], s[1], s[2]};
        at = 14;
    } else {
        return unread("phfl", notes, "読めない版の");
    }
    const auto density = be32(d, at);
    if (!density || at + 4 >= d.size()) return unread("phfl", notes, "読めない形の");
    return normal("phfl",
                  Json{{"kind", "photo_filter"}, {"color", rgb_json({rgb[0] * 255, rgb[1] * 255, rgb[2] * 255})},
                       {"density", std::min<std::uint32_t>(*density, 100)}, {"preserve_luminosity", d[at + 4] != 0}},
                  notes);
}

std::optional<std::string> write_phfl(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    std::string v;
    put_u16(v, 2);  // (an RGB colour structure: the colour at 16 bits)
    put_u16(v, 0);
    for (const Json& x : s->at("color")) put_u16(v, static_cast<std::uint32_t>(std::lround(std::clamp(x.get<double>(), 0.0, 255.0) / 255 * 65535)));
    put_u16(v, 0);
    put_u32(v, static_cast<std::uint32_t>(std::lround(std::clamp(num(*s, "density"), 0.0, 100.0))));
    put_u8(v, s->at("preserve_luminosity").get<bool>() ? 1 : 0);
    put_u8(v, 0);
    return v;
}

// mixr: version 1, monochrome (u16), then a 10-byte record per output channel — red, green, blue and the fourth
// source percentages (the fourth only a CMYK document's) and the constant (i16 each). With monochrome on, the first
// record is the grey mix. A record with a fourth source is a CMYK mixer: no RGB meaning, not read.
std::optional<Json> read_mixr(std::string_view d, std::vector<std::string>& notes) {
    if (be16(d, 0) != 1u || !be16(d, 2)) return unread("mixr", notes, "読めない形の");
    for (std::size_t r = 0;; ++r) {
        const auto fourth = bei16(d, 4 + r * 10 + 6);
        if (!fourth) break;
        if (*fourth != 0) return unread("mixr", notes, " CMYK のチャンネルミキサーの");
    }
    const bool monochrome = *be16(d, 2) != 0;
    const auto record = [&d](std::size_t r) -> std::optional<Json> {
        const std::size_t at = 4 + r * 10;
        const auto red = bei16(d, at), green = bei16(d, at + 2), blue = bei16(d, at + 4), constant = bei16(d, at + 8);
        if (!red || !green || !blue || !constant) return std::nullopt;
        return ints({*red, *green, *blue, *constant});
    };
    const auto first = record(0), second = record(1), third = record(2);
    if (!first || (!monochrome && (!second || !third))) return unread("mixr", notes, "読めない形の");
    Json adjust{{"kind", "channel_mixer"}};
    adjust[monochrome ? "gray" : "red"] = *first;  // (monochrome: the red output's own row is not in the PSD)
    if (second) adjust["green"] = *second;
    if (third) adjust["blue"] = *third;
    adjust["monochrome"] = monochrome;
    return normal("mixr", adjust, notes);
}

std::optional<std::string> write_mixr(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    const bool monochrome = s->at("monochrome").get<bool>();
    const Json& first = s->at(monochrome ? "gray" : "red");
    std::string v;
    put_u16(v, 1);
    put_u16(v, monochrome ? 1 : 0);
    // red (monochrome: the grey mix), green, blue, then the first again (PhotoCraft's: the record monochrome uses)
    for (const Json* row : {&first, &s->at("green"), &s->at("blue"), &first}) {
        for (std::size_t i = 0; i < 3; ++i) put_i16(v, std::clamp((*row)[i].get<double>(), -200.0, 200.0));
        put_u16(v, 0);
        put_i16(v, std::clamp((*row)[3].get<double>(), -200.0, 200.0));
    }
    return v;
}

// selc: version 1, method (0 relative, 1 absolute), then ten [c, m, y, k] records of i16 percent: the first reserved,
// then reds … blacks.
std::optional<Json> read_selc(std::string_view d, std::vector<std::string>& notes) {
    if (be16(d, 0) != 1u || d.size() < 84) return unread("selc", notes, "読めない形の");
    Json adjust{{"kind", "selective_color"}, {"method", *be16(d, 2) == 0 ? "relative" : "absolute"}};
    for (std::size_t r = 0; r < 9; ++r) {
        const std::size_t at = 4 + (r + 1) * 8;
        adjust[std::string(color::kSelectiveRanges[r])] = ints({*bei16(d, at), *bei16(d, at + 2), *bei16(d, at + 4), *bei16(d, at + 6)});
    }
    return normal("selc", adjust, notes);
}

std::optional<std::string> write_selc(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto s = settings_of(adjust, page, notes);
    if (!s) return std::nullopt;
    std::string v;
    put_u16(v, 1);
    put_u16(v, s->at("method") == Json("relative") ? 0 : 1);
    v.append(8, '\0');
    for (const std::string_view key : color::kSelectiveRanges) {
        for (const Json& x : s->at(std::string(key))) put_i16(v, std::clamp(x.get<double>(), -100.0, 100.0));
    }
    return v;
}

// A codec's read and write (`kind`: only that kind's row of its block; none: every row of the block).
AdjustCodec codec(decltype(AdjustCodec::read) read, decltype(AdjustCodec::write) write, std::string_view kind = {}) {
    AdjustCodec out;
    out.kind = kind;
    out.read = std::move(read);
    out.write = std::move(write);
    return out;
}

}  // namespace

CodecHooks color_codecs() {
    return {{"hue2", codec(read_hue2, write_hue2, "hue_saturation")}, {"hue2", codec(nullptr, write_python_hue, "hue")},
            {"vibA", codec(read_viba, write_viba)},                   {"blnc", codec(read_blnc, write_blnc)},
            {"blwh", codec(read_blwh, write_blwh)},                   {"phfl", codec(read_phfl, write_phfl)},
            {"mixr", codec(read_mixr, write_mixr)},                   {"selc", codec(read_selc, write_selc)}};
}

}  // namespace genko::render::psd
