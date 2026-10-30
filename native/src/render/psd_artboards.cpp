// PC5 I-c (render/psd_artboards.hpp): a PSD's artboard block. After PhotoCraft v0.2.0's crates/io/src/comps_map.rs (67-110:
// parse_artboard, write_artboard; MIT, native/third_party/photocraft/NOTICE.md).

#include "render/psd_artboards.hpp"

#include <algorithm>
#include <cmath>

#include "render/psd_descriptor.hpp"

namespace genko::render::psd_artboards {

namespace d = psd::descriptor;

namespace {

std::optional<double> number(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    if (v == nullptr) return std::nullopt;
    const auto n = v->numeric();
    if (!n || !std::isfinite(*n)) return std::nullopt;
    return *n;
}

const d::Object* object(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->object && (v->type == "Objc" || v->type == "GlbO") ? v->object.get() : nullptr;
}

int channel(double v) { return static_cast<int>(std::clamp<long long>(std::llround(v), 0, 255)); }

// A colour descriptor (RGBC: Rd, Grn, Bl 0..255, or newer Photoshop's redFloat… 0..1) as RGB; none for another model.
std::optional<std::array<int, 3>> rgb_of(const d::Object& c) {
    if (const auto r = number(c, "redFloat")) {
        const auto g = number(c, "greenFloat"), b = number(c, "blueFloat");
        if (!g || !b) return std::nullopt;
        return std::array<int, 3>{channel(*r * 255.0), channel(*g * 255.0), channel(*b * 255.0)};
    }
    const auto r = number(c, "Rd  "), g = number(c, "Grn "), b = number(c, "Bl  ");
    if (!r || !g || !b) return std::nullopt;
    return std::array<int, 3>{channel(*r), channel(*g), channel(*b)};
}

}  // namespace

bool artboard_key(std::string_view key) { return key == "artb" || key == "artd" || key == "abdd"; }

std::optional<Board> read(std::string_view block) {
    d::Object top;
    try {
        top = d::read_versioned(block);
    } catch (const d::Broken&) {
        return std::nullopt;
    }
    const d::Object* r = object(top, "artboardRect");
    if (r == nullptr) return std::nullopt;
    const auto left = number(*r, "Left"), t = number(*r, "Top "), right = number(*r, "Rght"), b = number(*r, "Btom");
    if (!left || !t || !right || !b) return std::nullopt;
    Board out;
    out.rect = {std::llround(*left), std::llround(*t), std::llround(*right), std::llround(*b)};
    for (const std::int64_t v : out.rect)
        if (std::llabs(v) > core::kArtboardOriginMax * 2) return std::nullopt;
    if (out.rect[2] <= out.rect[0] || out.rect[3] <= out.rect[1]) return std::nullopt;
    if (const d::Value* preset = top.get("artboardPresetName"); preset != nullptr && preset->type == "TEXT") {
        out.preset = psd::utf8_of(preset->text);
    }
    const auto kind = number(top, "artboardBackgroundType");
    switch (kind ? static_cast<int>(std::llround(*kind)) : 1) {
    case 2: out.background = "black"; break;
    case 3: out.background = "transparent"; break;
    case 4: {
        const d::Object* colour = object(top, "Clr ");
        const auto rgb = colour != nullptr ? rgb_of(*colour) : std::nullopt;
        if (rgb) {
            out.background = "custom";
            out.rgb = rgb;
        }
        break;
    }
    default: break;
    }
    return out;
}

std::string write(const core::Artboard& artboard, const std::array<std::int64_t, 4>& rect) {
    d::Writer bounds;
    bounds.number("Top ", static_cast<double>(rect[1]));
    bounds.number("Left", static_cast<double>(rect[0]));
    bounds.number("Btom", static_cast<double>(rect[3]));
    bounds.number("Rght", static_cast<double>(rect[2]));
    const std::array<int, 3> rgb = artboard.background == "black" ? std::array<int, 3>{0, 0, 0}
                                   : artboard.rgb                 ? *artboard.rgb
                                                                  : std::array<int, 3>{255, 255, 255};
    d::Writer colour;
    colour.number("Rd  ", rgb[0]);
    colour.number("Grn ", rgb[1]);
    colour.number("Bl  ", rgb[2]);
    const int type = artboard.background == "black" ? 2 : artboard.background == "transparent" ? 3 : artboard.background == "custom" ? 4 : 1;
    d::Writer board;
    board.object("artboardRect", bounds, "classFloatRect");
    board.integers("guideIndeces", {});
    board.text("artboardPresetName", psd::utf16(artboard.preset.value_or(std::string())));
    board.object("Clr ", colour, "RGBC");
    board.integer("artboardBackgroundType", type);
    std::string out = board.versioned("artboard");
    if (out.size() % 2 != 0) out.push_back('\0');
    return out;
}

}  // namespace genko::render::psd_artboards
