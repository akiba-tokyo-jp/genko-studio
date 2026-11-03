// artboards@1 (PC5 I-c): a page's artboard mark, its JSON and checks (core/artboards.hpp). The presets after PhotoCraft
// v0.2.0's engine/src/artboard_cmds.rs (20-31, MIT, native/third_party/photocraft/NOTICE.md).

#include "core/artboards.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace pj = production;

namespace {

constexpr ArtboardPreset kPresets[] = {
    // (PhotoCraft's, px)
    {"iPhone 14", 390, 844, false},
    {"iPhone 14 Pro Max", 430, 932, false},
    {"Android 1080p", 360, 640, false},
    {"iPad Pro 12.9", 1024, 1366, false},
    {"Web 1280", 1280, 800, false},
    {"Web 1366", 1366, 768, false},
    {"Web 1920", 1920, 1080, false},
    {"Instagram Post", 1080, 1080, false},
    {"Letter", 612, 792, false},
    {"A4", 595, 842, false},
    // (Genko's finished sizes, mm: their pixels at the book's resolution)
    {"A4 仕上がり", 210, 297, true},
    {"B5 仕上がり", 182, 257, true},
    {"B4 仕上がり", 257, 364, true},
};

bool background_known(std::string_view b) { return b == "white" || b == "black" || b == "transparent" || b == "custom"; }

}  // namespace

Json to_json(const Artboard& artboard) {
    Json out = Json::object();
    out["name"] = artboard.name;
    if (artboard.preset) out["preset"] = *artboard.preset;
    out["background"] = artboard.background;
    if (artboard.rgb) out["rgb"] = pj::to_json(*artboard.rgb);
    if (artboard.origin_px) out["origin_px"] = Json::array({(*artboard.origin_px)[0], (*artboard.origin_px)[1]});
    return out;
}

Artboard artboard_from_json(const Json& value) {
    const std::string where = "artboard";
    pj::only_keys(value, where, {"name", "preset", "background", "rgb", "origin_px"});
    Artboard out;
    out.name = pj::text(pj::field(value, "name", where), where + ".name", kArtboardNameMax);
    if (const Json* preset = pj::optional(value, "preset")) out.preset = pj::text(*preset, where + ".preset", 256);
    out.background = pj::text(pj::field(value, "background", where), where + ".background", 16);
    if (const Json* rgb = pj::optional(value, "rgb")) out.rgb = pj::rgb(*rgb, where + ".rgb");
    if (const Json* origin = pj::optional(value, "origin_px")) {
        if (!origin->is_array() || origin->size() != 2) pj::wrong(where + ".origin_px", "must be [x, y]");
        out.origin_px = std::array<std::int64_t, 2>{pj::integer((*origin)[0], where + ".origin_px", -kArtboardOriginMax, kArtboardOriginMax),
                                                    pj::integer((*origin)[1], where + ".origin_px", -kArtboardOriginMax, kArtboardOriginMax)};
    }
    check_artboard(out);
    return out;
}

void check_artboard(const Artboard& artboard) {
    const std::string where = "artboard";
    if (artboard.name.empty()) pj::wrong(where + ".name", "must not be empty");
    if (artboard.name.size() > kArtboardNameMax) pj::wrong(where + ".name", "is longer than " + std::to_string(kArtboardNameMax) + " bytes");
    if (artboard.preset && artboard.preset->size() > 256) pj::wrong(where + ".preset", "is longer than 256 bytes");
    if (!background_known(artboard.background)) pj::wrong(where + ".background", "is white, black, transparent or custom, not " + artboard.background);
    if ((artboard.background == "custom") != artboard.rgb.has_value()) pj::wrong(where + ".rgb", "is a custom background's colour (and only its)");
    if (artboard.rgb) {
        for (const int v : *artboard.rgb)
            if (v < 0 || v > 255) pj::wrong(where + ".rgb", "must be 0..255");
    }
    if (artboard.origin_px) {
        for (const std::int64_t v : *artboard.origin_px)
            if (v < -kArtboardOriginMax || v > kArtboardOriginMax) pj::wrong(where + ".origin_px", "is out of range");
    }
}

bool uses_artboards(const Document& doc) {
    return std::any_of(doc.pages.begin(), doc.pages.end(), [](const PagePtr& page) { return page && page->artboard.has_value(); });
}

std::optional<std::string> artboard_refusal(const Page& page) {
    if (is_cover(page)) return "page " + page.index.repr() + " is a cover: a cover cannot be an artboard";
    if (page.spread_with && page.spread_with->truthy()) return "page " + page.index.repr() + " is in a spread: a page of a spread cannot be an artboard";
    return std::nullopt;
}

void validate_artboards(const Document& doc) {
    for (const PagePtr& page : doc.pages) {
        if (!page || !page->artboard) continue;
        try {
            check_artboard(*page->artboard);
        } catch (const Error& error) {
            throw Error("value", "page " + page->index.repr() + ": " + error.what());
        }
        if (const auto why = artboard_refusal(*page)) throw Error("value", *why);
    }
}

std::optional<std::array<int, 3>> artboard_underlay(const Page& page) {
    if (!page.artboard) return std::nullopt;
    if (page.artboard->background == "black") return std::array<int, 3>{0, 0, 0};
    if (page.artboard->background == "custom" && page.artboard->rgb) return *page.artboard->rgb;
    return std::nullopt;
}

bool transparent_artboard(const Page& page) { return page.artboard && page.artboard->background == "transparent"; }

std::span<const ArtboardPreset> artboard_presets() { return kPresets; }

const ArtboardPreset* artboard_preset(std::string_view name) {
    for (const ArtboardPreset& p : kPresets) {
        if (p.name == name) return &p;
    }
    // (PhotoCraft's: its names in any case)
    for (const ArtboardPreset& p : kPresets) {
        if (p.name.size() != name.size()) continue;
        if (std::equal(p.name.begin(), p.name.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            return &p;
    }
    return nullptr;
}

std::array<std::int64_t, 2> preset_pixels(const ArtboardPreset& preset, double dpi) {
    if (!preset.mm) return {static_cast<std::int64_t>(preset.width), static_cast<std::int64_t>(preset.height)};
    return {std::max<std::int64_t>(1, std::llround(preset.width / 25.4 * dpi)), std::max<std::int64_t>(1, std::llround(preset.height / 25.4 * dpi))};
}

std::string next_artboard_name(const Document& doc) {
    std::set<std::string> names;
    for (const PagePtr& page : doc.pages) {
        if (page && page->artboard) names.insert(page->artboard->name);
    }
    for (std::size_t n = 1;; ++n) {
        std::string name = "アートボード " + std::to_string(n);
        if (!names.contains(name)) return name;
    }
}

}  // namespace genko::core
