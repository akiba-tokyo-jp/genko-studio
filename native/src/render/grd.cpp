#include "render/grd.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#include "core/error.hpp"
#include "render/image_ops.hpp"
#include "render/psd_descriptor.hpp"

// PhotoCraft v0.2.0 crates/psd/src/grd.rs (lines 1-207: the layout, the colours, the stops, the writer), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::grd {

namespace d = psd::descriptor;
using core::Json;

namespace {

[[noreturn]] void broken(const std::string& why) { throw core::Error("format", "the gradient file cannot be read: " + why); }

double number(const d::Object& o, std::string_view key, double fallback = 0.0) {
    const d::Value* v = o.get(key);
    if (v == nullptr) return fallback;
    const auto n = v->numeric();
    return n && std::isfinite(*n) ? *n : fallback;
}

const d::Object* object(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    if (v == nullptr || (v->type != "Objc" && v->type != "GlbO") || !v->object) return nullptr;
    return v->object.get();
}

const std::vector<d::Value>* list(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->type == "VlLs" ? &v->list : nullptr;
}

bool enum_is(const d::Object& o, std::string_view key, std::string_view value) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->type == "enum" && v->enum_value == value;
}

std::optional<Colour> colour(const d::Object& c) {
    Colour out;
    const std::string& id = c.class_id;
    if (id == "RGBC") {
        out.kind = Colour::Kind::Rgb;
        out.values = {number(c, "Rd  ") / 255.0, number(c, "Grn ") / 255.0, number(c, "Bl  ") / 255.0, 0};
    } else if (id == "HSBC") {
        out.kind = Colour::Kind::Hsb;
        out.values = {number(c, "H   "), number(c, "Strt") / 100.0, number(c, "Brgh") / 100.0, 0};
    } else if (id == "CMYC") {
        out.kind = Colour::Kind::Cmyk;
        out.values = {number(c, "Cyn ") / 100.0, number(c, "Mgnt") / 100.0, number(c, "Ylw ") / 100.0, number(c, "Blck") / 100.0};
    } else if (id == "LbCl") {
        out.kind = Colour::Kind::Lab;
        out.values = {number(c, "Lmnc"), number(c, "A   "), number(c, "B   "), 0};
    } else if (id == "Grsc") {
        out.kind = Colour::Kind::Gray;
        out.values = {number(c, "Gry ") / 100.0, 0, 0, 0};
    } else {
        return std::nullopt;
    }
    return out;
}

double location(const d::Object& o) { return std::clamp(number(o, "Lctn") / 4096.0, 0.0, 1.0); }
double midpoint(const d::Object& o) { return std::clamp(number(o, "Mdpn", 50.0) / 100.0, 0.0, 1.0); }

Gradient gradient(const d::Object& g) {
    Gradient out;
    if (const d::Value* name = g.get("Nm  "); name != nullptr && name->type == "TEXT") out.name = psd::utf8_of(name->text);
    out.noise = enum_is(g, "GrdF", "ClNs");
    if (const auto* stops = list(g, "Clrs")) {
        for (std::size_t i = 0; i < stops->size() && i < kMaxStops; ++i) {
            const d::Value& v = (*stops)[i];
            if (v.type != "Objc" || !v.object) continue;
            const d::Object& s = *v.object;
            std::optional<Colour> c;
            if (enum_is(s, "Type", "FrgC")) c = Colour{Colour::Kind::Foreground, {}};
            else if (enum_is(s, "Type", "BckC")) c = Colour{Colour::Kind::Background, {}};
            else if (const d::Object* inner = object(s, "Clr ")) c = colour(*inner);
            if (c) out.stops.push_back(Stop{location(s), midpoint(s), *c});
        }
    }
    if (const auto* stops = list(g, "Trns")) {
        for (std::size_t i = 0; i < stops->size() && i < kMaxStops; ++i) {
            const d::Value& v = (*stops)[i];
            if (v.type != "Objc" || !v.object) continue;
            const d::Object& s = *v.object;
            out.opacity.push_back(Opacity{location(s), std::clamp(number(s, "Opct", 100.0) / 100.0, 0.0, 1.0), midpoint(s)});
        }
    }
    return out;
}

// sRGB 0..255 of a colour of another model
std::array<int, 3> srgb(const Colour& c) {
    std::array<double, 3> rgb{};
    switch (c.kind) {
        case Colour::Kind::Rgb: rgb = {c.values[0], c.values[1], c.values[2]}; break;
        case Colour::Kind::Hsb: {
            const double h = std::fmod(std::fmod(c.values[0], 360.0) + 360.0, 360.0) / 60.0, s = std::clamp(c.values[1], 0.0, 1.0),
                         v = std::clamp(c.values[2], 0.0, 1.0);
            const int i = static_cast<int>(std::floor(h)) % 6;
            const double f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            const std::array<std::array<double, 3>, 6> sextants{{{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}}};
            rgb = sextants[static_cast<std::size_t>(i)];
            break;
        }
        case Colour::Kind::Cmyk:
            for (std::size_t k = 0; k < 3; ++k) rgb[k] = (1 - std::clamp(c.values[k], 0.0, 1.0)) * (1 - std::clamp(c.values[3], 0.0, 1.0));
            break;
        case Colour::Kind::Lab: {
            const auto s = image_ops::lab_to_srgb({static_cast<float>(c.values[0]), static_cast<float>(c.values[1]), static_cast<float>(c.values[2])});
            rgb = {s[0], s[1], s[2]};
            break;
        }
        case Colour::Kind::Gray: rgb.fill(1 - std::clamp(c.values[0], 0.0, 1.0)); break;
        default: break;
    }
    std::array<int, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) out[k] = static_cast<int>(std::lround(std::clamp(rgb[k], 0.0, 1.0) * 255.0));
    return out;
}

double rounded(double v) { return std::round(v * 10000.0) / 10000.0; }

}  // namespace

std::string kept_name(std::string_view name, std::string_view fallback, bool* cut) {
    const auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::size_t start = 0, end = name.size();
    while (start < end && space(static_cast<unsigned char>(name[start]))) ++start;
    while (end > start && space(static_cast<unsigned char>(name[end - 1]))) --end;
    std::string_view kept = name.substr(start, end - start);
    // (at most kNameChars characters: cut before the lead byte of the one past them)
    std::size_t chars = 0, at = 0;
    for (; at < kept.size(); ++at) {
        if ((static_cast<unsigned char>(kept[at]) & 0xC0) == 0x80) continue;
        if (chars == kNameChars) break;
        ++chars;
    }
    if (cut != nullptr) *cut = at < kept.size();
    kept = kept.substr(0, at);
    while (!kept.empty() && space(static_cast<unsigned char>(kept.back()))) kept.remove_suffix(1);
    return kept.empty() ? std::string(fallback) : std::string(kept);
}

std::vector<Gradient> read(std::string_view data) {
    if (data.size() > kMaxBytes) broken("it is larger than " + std::to_string(kMaxBytes >> 20) + " MB");
    if (data.size() < 6) broken("it ends early");
    if (data.substr(0, 4) != "8BGR") broken("it is not a gradient file (8BGR)");
    const unsigned version = (static_cast<unsigned char>(data[4]) << 8) | static_cast<unsigned char>(data[5]);
    if (version != 5) broken("its version is " + std::to_string(version) + " (only version 5, Photoshop 6 and later)");
    std::vector<Gradient> out;
    try {
        const d::Object top = d::read_versioned(data, 6);
        if (const auto* items = list(top, "GrdL")) {
            for (const d::Value& v : *items) {
                if (out.size() >= kMaxGradients) break;
                if (v.type != "Objc" || !v.object) continue;
                if (const d::Object* g = object(*v.object, "Grad")) out.push_back(gradient(*g));
            }
        }
    } catch (const d::Broken& error) {
        broken(error.what());
    }
    if (out.empty()) broken("it holds no gradients");
    return out;
}

std::string write(const std::vector<Gradient>& gradients) {
    std::vector<std::pair<std::string, d::Writer>> items;
    for (const Gradient& g : gradients) {
        std::vector<std::pair<std::string, d::Writer>> stops, trns;
        for (const Stop& s : g.stops) {
            d::Writer w;
            const char* type = s.colour.kind == Colour::Kind::Foreground ? "FrgC" : s.colour.kind == Colour::Kind::Background ? "BckC" : "UsrS";
            if (s.colour.kind != Colour::Kind::Foreground && s.colour.kind != Colour::Kind::Background) {
                d::Writer rgb;
                const bool own = s.colour.kind == Colour::Kind::Rgb;
                rgb.number("Rd  ", own ? s.colour.values[0] * 255.0 : 0.0);
                rgb.number("Grn ", own ? s.colour.values[1] * 255.0 : 0.0);
                rgb.number("Bl  ", own ? s.colour.values[2] * 255.0 : 0.0);
                w.object("Clr ", rgb, "RGBC");
            }
            w.enumerated("Type", "Clry", type);
            w.integer("Lctn", static_cast<std::int32_t>(std::lround(s.location * 4096.0)));
            w.integer("Mdpn", static_cast<std::int32_t>(std::lround(s.midpoint * 100.0)));
            stops.emplace_back("Clrt", std::move(w));
        }
        for (const Opacity& o : g.opacity) {
            d::Writer w;
            w.unit("Opct", "#Prc", o.opacity * 100.0);
            w.integer("Lctn", static_cast<std::int32_t>(std::lround(o.location * 4096.0)));
            w.integer("Mdpn", static_cast<std::int32_t>(std::lround(o.midpoint * 100.0)));
            trns.emplace_back("TrnS", std::move(w));
        }
        d::Writer grad;
        grad.text("Nm  ", psd::utf16(g.name));
        grad.enumerated("GrdF", "GrdF", g.noise ? "ClNs" : "CstS");
        grad.number("Intr", 4096.0);
        grad.objects("Clrs", stops);
        grad.objects("Trns", trns);
        d::Writer item;
        item.object("Grad", grad, "Grdn");
        items.emplace_back("Grdn", std::move(item));
    }
    d::Writer top;
    top.objects("GrdL", items);
    std::string out = "8BGR";
    d::put_u16(out, 5);
    out += top.versioned("null");
    return out;
}

Json to_presets(const std::vector<Gradient>& gradients, std::vector<std::string>& warnings) {
    Json out = Json::array();
    std::set<std::string> names;
    bool midpoints = false, models = false;
    for (const Gradient& g : gradients) {
        bool cut = false;
        const std::string label = kept_name(g.name, "グラデーション", &cut);
        if (cut) warnings.push_back("「" + label + "…」の名前は長すぎるので " + std::to_string(kNameChars) + " 字に切りました");
        if (g.noise) {
            warnings.push_back("「" + label + "」はノイズのグラデーションなので読み込みませんでした");
            continue;
        }
        std::vector<Stop> stops = g.stops;
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.location < b.location; });
        if (stops.size() > 16) {
            warnings.push_back("「" + label + "」の色の分岐点は " + std::to_string(stops.size()) + " 個あり、はじめの 16 個を読み込みました");
            stops.resize(16);
        }
        if (stops.empty()) {
            warnings.push_back("「" + label + "」には色の分岐点がないので読み込みませんでした");
            continue;
        }
        if (stops.size() == 1) stops.push_back(Stop{1.0, 0.5, stops.front().colour});  // (one colour: from end to end)
        Json colours = Json::array();
        for (std::size_t i = 0; i < stops.size(); ++i) {
            const Stop& s = stops[i];
            if (i + 1 < stops.size() && std::fabs(s.midpoint - 0.5) > 1e-6) midpoints = true;
            Json c;
            if (s.colour.kind == Colour::Kind::Foreground) {
                c = "foreground";
            } else if (s.colour.kind == Colour::Kind::Background) {
                c = "background";
            } else {
                if (s.colour.kind != Colour::Kind::Rgb) models = true;
                const auto rgb = srgb(s.colour);
                c = Json::array({rgb[0], rgb[1], rgb[2]});
            }
            colours.push_back(Json::array({rounded(s.location), c}));
        }
        std::string name = label;
        for (int n = 2; names.contains(name); ++n) name = label + " " + std::to_string(n);
        names.insert(name);
        Json item{{"name", name}, {"stops", colours}};
        std::vector<Opacity> opacity = g.opacity;
        std::stable_sort(opacity.begin(), opacity.end(), [](const Opacity& a, const Opacity& b) { return a.location < b.location; });
        if (opacity.size() > 16) {
            warnings.push_back("「" + label + "」の不透明度の分岐点は " + std::to_string(opacity.size()) + " 個あり、はじめの 16 個を読み込みました");
            opacity.resize(16);
        }
        if (opacity.size() == 1) opacity.push_back(Opacity{1.0, opacity.front().opacity, 0.5});
        const bool opaque = std::all_of(opacity.begin(), opacity.end(), [](const Opacity& o) { return o.opacity >= 1.0 - 1e-6; });
        if (!opacity.empty() && !opaque) {
            Json trns = Json::array();
            for (std::size_t i = 0; i < opacity.size(); ++i) {
                if (i + 1 < opacity.size() && std::fabs(opacity[i].midpoint - 0.5) > 1e-6) midpoints = true;
                trns.push_back(Json::array({rounded(opacity[i].location), rounded(opacity[i].opacity)}));
            }
            item["transparency"] = trns;
        }
        out.push_back(std::move(item));
    }
    if (midpoints) warnings.push_back("中間点が 50% でない分岐点は、50% で描きます");
    if (models) warnings.push_back("RGB でない色（HSB・CMYK・Lab・グレー）は sRGB にして読み込みました");
    return out;
}

}  // namespace genko::render::grd
