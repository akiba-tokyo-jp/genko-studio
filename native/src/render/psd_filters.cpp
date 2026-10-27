// Photoshop's smart filters of a PSD's placed layers read as Genko's (render/psd_filters.hpp, PC4 D7). The descriptors'
// classes, keys and enumerations are Photoshop's as the public descriptions of the PSD format have them (checked with
// psd-tools, which reads them as descriptors); each is turned into PhotoCraft's command and params — Photoshop's units:
// the document's pixels, percent, degrees — and from there into Genko's kind (core::from_upstream: one table of units).

#include "render/psd_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "core/filter_kinds.hpp"
#include "core/smart.hpp"

namespace genko::render::psd_filters {

using core::Json;
using psd::descriptor::Broken;
using psd::descriptor::Bytes;
using psd::descriptor::Object;
using psd::descriptor::Value;

namespace {

// A four-character id as a filterID's number.
constexpr std::int64_t id4(const char (&c)[5]) {
    return (static_cast<std::int64_t>(static_cast<unsigned char>(c[0])) << 24) | (static_cast<std::int64_t>(static_cast<unsigned char>(c[1])) << 16) |
           (static_cast<std::int64_t>(static_cast<unsigned char>(c[2])) << 8) | static_cast<std::int64_t>(static_cast<unsigned char>(c[3]));
}

// (a filter that cannot be read so: not in the table)
struct NotMapped {};

const Object* object_at(const Object& o, std::string_view key) {
    const Value* v = o.get(key);
    return v != nullptr && v->object && (v->type == "Objc" || v->type == "GlbO") ? v->object.get() : nullptr;
}

// A number of the key: a plain number, or a unit float of one of `units` ("#Pxl", "#Prc", "#Ang"); NotMapped otherwise.
double number(const Object& o, std::string_view key, std::initializer_list<std::string_view> units = {}) {
    const Value* v = o.get(key);
    if (v == nullptr) throw NotMapped{};
    if (v->type == "UntF" && std::find(units.begin(), units.end(), v->unit) == units.end()) throw NotMapped{};
    const auto n = v->numeric();
    if (!n || !std::isfinite(*n) || v->type == "UnFl") throw NotMapped{};
    return *n;
}

double pixels(const Object& o, std::string_view key) { return number(o, key, {"#Pxl"}); }
double percent(const Object& o, std::string_view key) { return number(o, key, {"#Prc"}); }
double degrees(const Object& o, std::string_view key) { return number(o, key, {"#Ang"}); }

bool flag(const Object& o, std::string_view key) {
    const Value* v = o.get(key);
    if (v == nullptr || v->type != "bool") throw NotMapped{};
    return v->boolean;
}

// A seed of Photoshop's (a long: its 32 bits) as a u32.
std::uint32_t seed(const Object& o, std::string_view key) {
    const Value* v = o.get(key);
    if (v == nullptr || v->type != "long") throw NotMapped{};
    return static_cast<std::uint32_t>(v->integer);
}

// An enumeration's value as PhotoCraft's word.
std::string pick(const Object& o, std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> words) {
    const Value* v = o.get(key);
    if (v == nullptr || v->type != "enum") throw NotMapped{};
    for (const auto& [psd, word] : words)
        if (v->enum_value == psd) return std::string(word);
    throw NotMapped{};
}

// The descriptor has these keys and no other (one this build does not know may change what the filter makes).
void only(const Object& o, std::initializer_list<std::string_view> keys) {
    for (const auto& [key, value] : o.items)
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) throw NotMapped{};
}

// A colour descriptor (RGBC: Rd, Grn, Bl 0..255 or redFloat… 0..1; Grsc: Gry, a percent of black; HSBC) as PhotoCraft's
// [r, g, b] 0..1.
Json colour(const Object& item, std::string_view key) {
    const Object* c = object_at(item, key);
    if (c == nullptr) throw NotMapped{};
    std::array<double, 3> rgb{};
    if (c->class_id == "RGBC") {
        if (c->get("redFloat") != nullptr) {
            rgb = {number(*c, "redFloat"), number(*c, "greenFloat"), number(*c, "blueFloat")};
        } else {
            rgb = {number(*c, "Rd  ") / 255.0, number(*c, "Grn ") / 255.0, number(*c, "Bl  ") / 255.0};
        }
    } else if (c->class_id == "Grsc") {
        const double g = 1.0 - number(*c, "Gry ", {"#Prc"}) / 100.0;
        rgb = {g, g, g};
    } else if (c->class_id == "HSBC") {
        const double h = std::fmod(std::fmod(number(*c, "H   ", {"#Ang"}), 360.0) + 360.0, 360.0) / 60.0;
        const double s = number(*c, "Strt") / 100.0, b = number(*c, "Brgh") / 100.0;
        const double f = h - std::floor(h), p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
        switch (static_cast<int>(std::floor(h)) % 6) {
            case 0: rgb = {b, t, p}; break;
            case 1: rgb = {q, b, p}; break;
            case 2: rgb = {p, b, t}; break;
            case 3: rgb = {p, q, b}; break;
            case 4: rgb = {t, p, b}; break;
            default: rgb = {b, p, q}; break;
        }
    } else {
        throw NotMapped{};
    }
    Json out = Json::array();
    for (const double v : rgb) out.push_back(std::clamp(v, 0.0, 1.0));
    return out;
}

// A place of the file's pixels as a share of the page (the base box of a smart filter: the page's grid).
double share_x(const Placement& w, double x) { return (w.at_x + x * w.scale_x) / w.page_w; }
double share_y(const Placement& w, double y) { return (w.at_y + y * w.scale_y) / w.page_h; }

// One filter of the stack as PhotoCraft's command and params (Photoshop's units).
std::pair<std::string, Json> upstream_of(const Object& item, const Placement& where) {
    const Object* f = object_at(item, "Fltr");
    const Value* id = item.get("filterID");
    if (f == nullptr) {
        // (the filters without a dialog: their id alone)
        if (id == nullptr || id->type != "long") throw NotMapped{};
        static const std::array<std::pair<std::int64_t, std::string_view>, 12> kBare{{
            {id4("Avrg"), "filter.blur.average"},
            {id4("Blr "), "filter.blur.blur"},
            {id4("BlrM"), "filter.blur.blurMore"},
            {id4("Dspc"), "filter.noise.despeckle"},
            {id4("Fct "), "filter.pixelate.facet"},
            {id4("Frgm"), "filter.pixelate.fragment"},
            {id4("Shrp"), "filter.sharpen.sharpen"},
            {id4("ShrE"), "filter.sharpen.sharpenEdges"},
            {id4("ShrM"), "filter.sharpen.sharpenMore"},
            {id4("FndE"), "filter.stylize.findEdges"},
            {id4("Slrz"), "filter.stylize.solarize"},
            {id4("NTSC"), "filter.video.ntscColors"},
        }};
        for (const auto& [n, command] : kBare)
            if (id->integer == n) return {std::string(command), Json::object()};
        throw NotMapped{};
    }
    const Object& o = *f;
    const std::string& c = o.class_id;
    if (c == "GsnB" || c == "boxblur") {
        only(o, {"Rds "});
        return {c == "GsnB" ? "filter.blur.gaussianBlur" : "filter.blur.boxBlur", Json{{"radius", pixels(o, "Rds ")}}};
    }
    if (c == "MtnB") {
        only(o, {"Angl", "Dstn"});
        return {"filter.blur.motionBlur", Json{{"angle", degrees(o, "Angl")}, {"distance", pixels(o, "Dstn")}}};
    }
    if (c == "RdlB") {
        // (its centre: the canvas's — the descriptor holds none —, as a share of the page)
        only(o, {"Amnt", "BlrM", "BlrQ"});
        return {"filter.blur.radialBlur", Json{{"amount", number(o, "Amnt")},
                                               {"method", pick(o, "BlrM", {{"Spn ", "spin"}, {"Zm  ", "zoom"}})},
                                               {"centerX", share_x(where, where.file_w / 2)},
                                               {"centerY", share_y(where, where.file_h / 2)}}};
    }
    if (c == "surfaceBlur") {
        only(o, {"Rds ", "Thsh"});
        return {"filter.blur.surfaceBlur", Json{{"radius", pixels(o, "Rds ")}, {"threshold", number(o, "Thsh")}}};
    }
    if (c == "SmrB") {
        only(o, {"Rds ", "Thsh", "SmBQ", "SmBM"});
        return {"filter.blur.smartBlur", Json{{"radius", pixels(o, "Rds ")},
                                              {"threshold", number(o, "Thsh")},
                                              {"quality", pick(o, "SmBQ", {{"SBQL", "low"}, {"SBQM", "medium"}, {"SBQH", "high"}})},
                                              {"mode", pick(o, "SmBM", {{"SBMN", "normal"}, {"SBME", "edgeOnly"}, {"SBMO", "overlayEdge"}})}}};
    }
    if (c == "UnsM") {
        only(o, {"Amnt", "Rds ", "Thsh"});
        return {"filter.sharpen.unsharpMask", Json{{"amount", percent(o, "Amnt")}, {"radius", pixels(o, "Rds ")}, {"threshold", number(o, "Thsh")}}};
    }
    if (c == "AdNs") {
        only(o, {"Dstr", "Nose", "Mnch", "FlRs"});
        return {"filter.noise.addNoise", Json{{"amount", percent(o, "Nose")},
                                              {"distribution", pick(o, "Dstr", {{"Unfr", "uniform"}, {"Gsn ", "gaussian"}})},
                                              {"monochromatic", flag(o, "Mnch")},
                                              {"seed", seed(o, "FlRs")}}};
    }
    if (c == "DstS") {
        only(o, {"Rds ", "Thsh"});
        return {"filter.noise.dustAndScratches", Json{{"radius", pixels(o, "Rds ")}, {"threshold", number(o, "Thsh")}}};
    }
    if (c == "Mdn ") {
        only(o, {"Rds "});
        return {"filter.noise.median", Json{{"radius", pixels(o, "Rds ")}}};
    }
    if (c == "denoise") {
        // (its basic form: one setting for every channel)
        only(o, {"ClNs", "Shrp", "removeJPEGArtifact", "channelDenoise", "preset"});
        const Value* channels = o.get("channelDenoise");
        if (channels == nullptr || channels->type != "VlLs" || channels->list.size() != 1 || !channels->list[0].object) throw NotMapped{};
        const Object& each = *channels->list[0].object;
        only(each, {"Chnl", "Amnt", "EdgF"});
        const Value* chnl = each.get("Chnl");
        if (chnl == nullptr || chnl->type != "VlLs" || chnl->list.size() != 1 || chnl->list[0].type != "enum" || chnl->list[0].enum_value != "Cmps") throw NotMapped{};
        Json p{{"strength", number(each, "Amnt")},
               {"reduceColorNoise", percent(o, "ClNs")},
               {"sharpenDetails", percent(o, "Shrp")},
               {"removeJpegArtifact", flag(o, "removeJPEGArtifact")}};
        if (each.get("EdgF") != nullptr) p["preserveDetails"] = number(each, "EdgF", {"#Prc"});
        return {"filter.noise.reduceNoise", std::move(p)};
    }
    if (c == "HghP" || c == "Mxm " || c == "Mnm ") {
        only(o, {"Rds "});
        return {c == "HghP" ? "filter.other.highPass" : c == "Mxm " ? "filter.other.maximum" : "filter.other.minimum", Json{{"radius", pixels(o, "Rds ")}}};
    }
    if (c == "Ofst") {
        only(o, {"Hrzn", "Vrtc", "Fl  "});
        return {"filter.other.offset", Json{{"horizontal", pixels(o, "Hrzn")},
                                            {"vertical", pixels(o, "Vrtc")},
                                            {"undefinedAreas", pick(o, "Fl  ", {{"Bckg", "transparent"}, {"Rpt ", "repeat"}, {"Wrp ", "wrap"}})}}};
    }
    if (c == "Cstm") {
        only(o, {"Scl ", "Ofst", "Mtrx"});
        const Value* m = o.get("Mtrx");
        if (m == nullptr || m->type != "VlLs" || m->list.size() != 25) throw NotMapped{};
        Json kernel = Json::array();
        for (const Value& v : m->list) {
            const auto n = v.numeric();
            if (!n || !std::isfinite(*n)) throw NotMapped{};
            kernel.push_back(*n);
        }
        return {"filter.other.custom", Json{{"kernel", std::move(kernel)}, {"scale", number(o, "Scl ")}, {"offset", number(o, "Ofst")}}};
    }
    if (c == "HsbP") {
        only(o, {"Inpt", "Otpt"});
        const std::initializer_list<std::pair<std::string_view, std::string_view>> spaces{{"RGBC", "rgb"}, {"HSBl", "hsb"}, {"HSLC", "hsl"}};
        return {"filter.other.hsbHsl", Json{{"inputMode", pick(o, "Inpt", spaces)}, {"rowOrder", pick(o, "Otpt", spaces)}}};
    }
    if (c == "Dntr") {
        only(o, {"IntE", "IntC"});
        return {"filter.video.deInterlace", Json{{"eliminate", pick(o, "IntE", {{"ElmO", "oddFields"}, {"ElmE", "evenFields"}})},
                                                 {"createBy", pick(o, "IntC", {{"CrtD", "duplication"}, {"CrtI", "interpolation"}})}}};
    }
    if (c == "Pnch") {
        only(o, {"Amnt"});
        return {"filter.distort.pinch", Json{{"amount", number(o, "Amnt")}}};
    }
    if (c == "Plr ") {
        only(o, {"Cnvr"});
        return {"filter.distort.polarCoordinates", Json{{"mode", pick(o, "Cnvr", {{"RctP", "rectangularToPolar"}, {"PlrR", "polarToRectangular"}})}}};
    }
    if (c == "Rple") {
        only(o, {"Amnt", "RplS"});
        return {"filter.distort.ripple", Json{{"amount", number(o, "Amnt")}, {"size", pick(o, "RplS", {{"Sml ", "small"}, {"Mdm ", "medium"}, {"Lrg ", "large"}})}}};
    }
    if (c == "Sphr") {
        only(o, {"Amnt", "SphM"});
        return {"filter.distort.spherize",
                Json{{"amount", number(o, "Amnt")}, {"mode", pick(o, "SphM", {{"Nrml", "normal"}, {"HrzO", "horizontalOnly"}, {"VrtO", "verticalOnly"}})}}};
    }
    if (c == "Twrl") {
        only(o, {"Angl"});
        return {"filter.distort.twirl", Json{{"angle", degrees(o, "Angl")}}};
    }
    if (c == "Wave") {
        // (Genko's 波形 has no scale of its own: 100 % alone)
        only(o, {"Wvtp", "NmbG", "WLMn", "WLMx", "AmMn", "AmMx", "SclH", "SclV", "UndA", "RndS"});
        if (number(o, "SclH") != 100 || number(o, "SclV") != 100) throw NotMapped{};
        return {"filter.distort.wave", Json{{"generators", number(o, "NmbG")},
                                            {"wavelengthMin", number(o, "WLMn")},
                                            {"wavelengthMax", number(o, "WLMx")},
                                            {"amplitudeMin", number(o, "AmMn")},
                                            {"amplitudeMax", number(o, "AmMx")},
                                            {"type", pick(o, "Wvtp", {{"WvSn", "sine"}, {"WvTr", "triangle"}, {"WvSq", "square"}})},
                                            {"undefinedAreas", pick(o, "UndA", {{"WrpA", "wrap"}, {"RptE", "repeat"}})},
                                            {"seed", seed(o, "RndS")}}};
    }
    if (c == "ZgZg") {
        only(o, {"Amnt", "NmbR", "ZZTy"});
        return {"filter.distort.zigZag", Json{{"amount", number(o, "Amnt")},
                                              {"ridges", number(o, "NmbR")},
                                              {"style", pick(o, "ZZTy", {{"ArnC", "aroundCenter"}, {"OtFr", "outFromCenter"}, {"PndR", "pondRipples"}})}}};
    }
    if (c == "ClrH") {
        only(o, {"Rds ", "Ang1", "Ang2", "Ang3", "Ang4"});
        return {"filter.pixelate.colorHalftone", Json{{"maxRadius", pixels(o, "Rds ")},
                                                      {"channel1", degrees(o, "Ang1")},
                                                      {"channel2", degrees(o, "Ang2")},
                                                      {"channel3", degrees(o, "Ang3")},
                                                      {"channel4", degrees(o, "Ang4")}}};
    }
    if (c == "Crst" || c == "Pntl") {
        only(o, {"ClSz", "FlRs"});
        Json p{{"cellSize", pixels(o, "ClSz")}, {"seed", seed(o, "FlRs")}};
        if (c == "Pntl") p["background"] = colour(item, "BckC");  // (its canvas: the background colour)
        return {c == "Crst" ? "filter.pixelate.crystallize" : "filter.pixelate.pointillize", std::move(p)};
    }
    if (c == "Mztn") {
        only(o, {"MztT", "FlRs"});
        return {"filter.pixelate.mezzotint",
                Json{{"type", pick(o, "MztT", {{"FnDt", "fineDots"}, {"MdmD", "mediumDots"}, {"GrnD", "grainyDots"}, {"CrsD", "coarseDots"}, {"ShrL", "shortLines"},
                                               {"MdmL", "mediumLines"}, {"LngL", "longLines"}, {"ShSt", "shortStrokes"}, {"MdmS", "mediumStrokes"}, {"LngS", "longStrokes"}})},
                     {"seed", seed(o, "FlRs")}}};
    }
    if (c == "Msc ") {
        only(o, {"ClSz"});
        return {"filter.pixelate.mosaic", Json{{"cellSize", pixels(o, "ClSz")}}};
    }
    if (c == "Dfs ") {
        only(o, {"Md  ", "FlRs"});
        return {"filter.stylize.diffuse", Json{{"mode", pick(o, "Md  ", {{"Nrml", "normal"}, {"DrkO", "darkenOnly"}, {"LghO", "lightenOnly"}, {"anisotropic", "anisotropic"}})},
                                               {"seed", seed(o, "FlRs")}}};
    }
    if (c == "Embs") {
        only(o, {"Angl", "Hght", "Amnt"});
        return {"filter.stylize.emboss", Json{{"angle", degrees(o, "Angl")}, {"height", pixels(o, "Hght")}, {"amount", percent(o, "Amnt")}}};
    }
    if (c == "Extr") {
        only(o, {"ExtS", "ExtD", "ExtF", "ExtM", "ExtT", "ExtR", "FlRs"});
        return {"filter.stylize.extrude", Json{{"type", pick(o, "ExtT", {{"Blks", "blocks"}, {"Pyrm", "pyramids"}})},
                                               {"size", pixels(o, "ExtS")},
                                               {"depth", pixels(o, "ExtD")},
                                               {"depthMode", pick(o, "ExtR", {{"Rndm", "random"}, {"LvlB", "levelBased"}})},
                                               {"solidFrontFaces", flag(o, "ExtF")},
                                               {"maskIncompleteBlocks", flag(o, "ExtM")},
                                               {"seed", seed(o, "FlRs")}}};
    }
    if (c == "Tls ") {
        only(o, {"TlNm", "TlOf", "FlCl", "FlRs"});
        return {"filter.stylize.tiles", Json{{"count", number(o, "TlNm")},
                                             {"maxOffset", percent(o, "TlOf")},
                                             {"fill", pick(o, "FlCl", {{"FlBc", "background"}, {"FlFr", "foreground"}, {"FlIn", "inverse"}, {"FlSm", "unaltered"}})},
                                             {"foreground", colour(item, "FrgC")},
                                             {"background", colour(item, "BckC")},
                                             {"seed", seed(o, "FlRs")}}};
    }
    if (c == "TrcC") {
        only(o, {"Lvl ", "Edg "});
        return {"filter.stylize.traceContour", Json{{"level", number(o, "Lvl ")}, {"edge", pick(o, "Edg ", {{"Lwr ", "lower"}, {"Upr ", "upper"}})}}};
    }
    if (c == "Wnd ") {
        only(o, {"WndM", "Drct"});
        return {"filter.stylize.wind", Json{{"method", pick(o, "WndM", {{"Wnd ", "wind"}, {"Blst", "blast"}, {"Stgr", "stagger"}})},
                                            {"direction", pick(o, "Drct", {{"Left", "fromLeft"}, {"Rght", "fromRight"}})}}};
    }
    if (c == "oilPaint") {
        only(o, {"lightingOn", "stylization", "cleanliness", "brushScale", "microBrush", "LghD", "specularity"});
        return {"filter.stylize.oilPaint", Json{{"lighting", flag(o, "lightingOn")},
                                                {"stylization", number(o, "stylization")},
                                                {"cleanliness", number(o, "cleanliness")},
                                                {"scale", number(o, "brushScale")},
                                                {"bristleDetail", number(o, "microBrush")},
                                                {"angle", degrees(o, "LghD")},
                                                {"shine", number(o, "specularity")}}};
    }
    if (c == "Clds" || c == "DfrC") {
        only(o, {"FlRs"});
        return {c == "Clds" ? "filter.render.clouds" : "filter.render.differenceClouds",
                Json{{"seed", seed(o, "FlRs")}, {"foreground", colour(item, "FrgC")}, {"background", colour(item, "BckC")}}};
    }
    if (c == "Fbrs") {
        only(o, {"Vrnc", "Strg", "RndS"});
        return {"filter.render.fibers", Json{{"variance", number(o, "Vrnc")},
                                             {"strength", number(o, "Strg")},
                                             {"seed", seed(o, "RndS")},
                                             {"foreground", colour(item, "FrgC")},
                                             {"background", colour(item, "BckC")}}};
    }
    if (c == "LnsF") {
        only(o, {"Brgh", "FlrC", "Lns "});
        const Object* at = object_at(o, "FlrC");
        if (at == nullptr) throw NotMapped{};
        only(*at, {"Hrzn", "Vrtc"});
        return {"filter.render.lensFlare", Json{{"brightness", number(o, "Brgh")},
                                                {"centerX", share_x(where, pixels(*at, "Hrzn"))},
                                                {"centerY", share_y(where, pixels(*at, "Vrtc"))},
                                                {"lens", pick(o, "Lns ", {{"Zm  ", "zoom"}, {"Nkn ", "prime35"}, {"Nkn1", "prime105"}, {"PnVs", "moviePrime"}})}}};
    }
    throw NotMapped{};
}

// Photoshop's blend modes (BlnM) as Genko's 20; none for one Genko has not (dissolve, vivid light…).
std::optional<std::string> blend_of(const Object& options) {
    const Value* v = options.get("Md  ");
    if (v == nullptr || v->type != "enum") return std::nullopt;
    static const std::array<std::pair<std::string_view, std::string_view>, 20> kModes{{
        {"Nrml", "normal"},       {"Drkn", "darken"},      {"Mltp", "multiply"},     {"CBrn", "color_burn"},          {"linearBurn", "linear_burn"},
        {"Lghn", "lighten"},      {"Scrn", "screen"},      {"CDdg", "color_dodge"},  {"linearDodge", "add"},          {"Ovrl", "overlay"},
        {"SftL", "soft_light"},   {"HrdL", "hard_light"},  {"Dfrn", "difference"},   {"Xclu", "exclusion"},           {"blendSubtraction", "subtract"},
        {"blendDivide", "divide"}, {"H   ", "hue"},        {"Strt", "saturation"},   {"Clr ", "color"},               {"Lmns", "luminosity"},
    }};
    for (const auto& [psd, genko] : kModes)
        if (v->enum_value == psd) return std::string(genko);
    return std::nullopt;
}

std::string name_of(const Object& item) {
    const Value* name = item.get("Nm  ");
    std::string out = name != nullptr && name->type == "TEXT" ? psd::utf8_of(name->text) : std::string();
    if (out.empty()) {
        const Object* f = object_at(item, "Fltr");
        out = f != nullptr ? f->class_id : std::string("?");
    }
    // (at most 64 characters: the file's words)
    std::size_t chars = 0, at = 0;
    while (at < out.size() && chars < 64) {
        const auto lead = static_cast<unsigned char>(out[at]);
        at += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        ++chars;
    }
    return out.substr(0, std::min(at, out.size()));
}

// A mask's bytes (a rectangle's rows, compression 0 raw or 1 PackBits with the rows' 2-byte counts first) all 255?
// Anything it cannot read so: not.
bool all_white(std::string_view data, std::int64_t width, std::int64_t height, unsigned compression) {
    if (width <= 0 || height <= 0) return true;
    if (compression == 0) {
        if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) != data.size()) return false;
        return std::all_of(data.begin(), data.end(), [](char c) { return static_cast<unsigned char>(c) == 255; });
    }
    if (compression != 1) return false;
    if (static_cast<std::uint64_t>(height) * 2 > data.size()) return false;
    std::size_t at = static_cast<std::size_t>(height) * 2;
    for (std::int64_t row = 0; row < height; ++row) {
        const std::size_t count = (static_cast<std::size_t>(static_cast<unsigned char>(data[row * 2])) << 8) | static_cast<unsigned char>(data[row * 2 + 1]);
        if (count > data.size() - at) return false;
        const std::string_view line = data.substr(at, count);
        at += count;
        std::int64_t made = 0;
        for (std::size_t i = 0; i < line.size();) {
            const int n = static_cast<signed char>(line[i++]);
            if (n >= 0) {
                const std::size_t run = static_cast<std::size_t>(n) + 1;
                if (run > line.size() - i) return false;
                for (std::size_t k = 0; k < run; ++k)
                    if (static_cast<unsigned char>(line[i + k]) != 255) return false;
                i += run;
                made += static_cast<std::int64_t>(run);
            } else if (n != -128) {
                if (i >= line.size() || static_cast<unsigned char>(line[i]) != 255) return false;
                ++i;
                made += 1 - n;
            }
            if (made > width) return false;
        }
        if (made != width) return false;
    }
    return true;
}

}  // namespace

Masks read_masks(const std::vector<psd::File::TaggedBlock>& globals) {
    Masks out;
    std::size_t entries = 0;
    for (const psd::File::TaggedBlock& block : globals) {
        if (block.key != "FEid" && block.key != "FXid") continue;
        try {
            Bytes b(block.data);
            const std::uint32_t version = b.u32();
            if (version < 1 || version > 3) throw Broken("its version");
            while (b.left() >= 8) {
                if (++entries > kMaskEntriesMax) throw Broken("too many");
                const std::uint64_t length = (static_cast<std::uint64_t>(b.u32()) << 32) | b.u32();
                if (length > b.left()) throw Broken("it ends early");
                Bytes e(b.take(static_cast<std::size_t>(length)));
                (void)b.take(std::min<std::size_t>(b.left(), (4 - length % 4) % 4));
                const std::string id(e.take(e.u8()));
                if (e.u32() > 1) throw Broken("its entry's version");
                const std::uint64_t body = (static_cast<std::uint64_t>(e.u32()) << 32) | e.u32();
                if (body > e.left()) throw Broken("it ends early");
                (void)e.take(static_cast<std::size_t>(body));  // (its rectangle, depth and channels: the filtered picture)
                bool hides = false;
                if (e.left() >= 1 && e.u8() != 0) {
                    // (its mask: a rectangle, its length, compression and data)
                    const std::int64_t top = e.i32(), left = e.i32(), bottom = e.i32(), right = e.i32();
                    const std::uint64_t size = (static_cast<std::uint64_t>(e.u32()) << 32) | e.u32();
                    if (size < 2 || size > e.left()) throw Broken("it ends early");
                    const unsigned compression = e.u16();
                    hides = !all_white(e.take(static_cast<std::size_t>(size - 2)), right - left, bottom - top, compression);
                }
                out.hides[id] = out.hides[id] || hides;
            }
        } catch (const Broken&) {
            out.unread = true;
        }
    }
    return out;
}

Read read(std::string_view sold, const psd::Placed& placed, const Placement& where, const Masks& masks, Budget& budget) {
    Read out;
    std::optional<Object> d;
    try {
        if (sold.size() < 8) throw Broken("it ends early");
        Bytes b(sold.substr(8), budget.left);
        try {
            d = psd::descriptor::read_versioned(b);
        } catch (const Broken& broken) {
            // (past the budget: nothing more for the file's other layers either; else what it took)
            budget.left = std::string_view(broken.what()).find("too large") != std::string_view::npos ? 0 : budget.left - std::min(budget.left, b.spent());
            throw;
        }
        budget.left -= std::min(budget.left, b.spent());
    } catch (const Broken&) {
        out.why = "unreadable";
        return out;
    }
    const Object* fx = object_at(*d, "filterFX");
    if (fx == nullptr) {
        if (d->get("filterFX") != nullptr) out.why = "unreadable";
        return out;
    }
    const Value* list = fx->get("filterFXList");
    if (list == nullptr || list->type != "VlLs") {
        out.why = "unreadable";
        return out;
    }
    if (list->list.empty()) return out;  // (nothing on it: Photoshop's picture is its contents')
    if (list->list.size() > core::smart::kFiltersMax) {
        out.why = "too_many";
        return out;
    }
    if (const Value* on = fx->get("enab"); on != nullptr && on->type == "bool") out.on = on->boolean;
    // (a document's pixels as the page's: the page's mm a pixel of the file, as dots an inch)
    const double mm_a_pixel = where.width_mm / where.page_w * where.scale_x;
    const double dpi = mm_a_pixel > 0 ? 25.4 / mm_a_pixel : 0;
    Json filters = Json::array();
    for (const Value& v : list->list) {
        const Object* item = v.object && v.type == "Objc" ? v.object.get() : nullptr;
        std::optional<std::pair<std::string, Json>> mapped;
        std::optional<std::string> blend;
        double opacity = 1;
        bool visible = true;
        if (item != nullptr) {
            try {
                const auto [command, params] = upstream_of(*item, where);
                mapped = core::from_upstream(command, params, dpi, core::UpstreamCanvas{where.width_mm, where.height_mm});
                const Object* options = object_at(*item, "blendOptions");
                if (options != nullptr) {
                    blend = blend_of(*options);
                    if (options->get("Opct") != nullptr) opacity = std::clamp(percent(*options, "Opct") / 100.0, 0.0, 1.0);
                } else {
                    blend = "normal";
                }
                if (const Value* enab = item->get("enab"); enab != nullptr && enab->type == "bool") visible = enab->boolean;
            } catch (const NotMapped&) {
                mapped.reset();
            }
        }
        if (!mapped || !blend) {
            if (out.names.size() < kNamesMax) out.names.push_back(item != nullptr ? name_of(*item) : std::string("?"));
            else ++out.more;
            continue;
        }
        filters.push_back(Json{{"kind", mapped->first},
                               {"params", std::move(mapped->second)},
                               {"blend", *blend},
                               {"opacity", std::round(opacity * 1000.0) / 1000.0},
                               {"visible", visible}});
    }
    if (!out.names.empty()) {
        out.why = "unknown";
        return out;
    }
    // (its filter mask, when on: Genko does not read it — a stack it hides something of stays Photoshop's picture)
    if (const Value* mask = fx->get("filterMaskEnable"); mask != nullptr && mask->type == "bool" && mask->boolean) {
        bool hides = masks.unread;
        bool found = false;
        for (const std::string* key : {&placed.placed, &placed.uuid}) {
            if (const auto at = masks.hides.find(*key); !key->empty() && at != masks.hides.end()) {
                found = true;
                hides = hides || at->second;
            }
        }
        if (!found) hides = hides || std::any_of(masks.hides.begin(), masks.hides.end(), [](const auto& e) { return e.second; });
        if (hides) {
            out.why = "mask";
            return out;
        }
    }
    out.filters = std::move(filters);
    return out;
}

}  // namespace genko::render::psd_filters
