// 取り込む画像のプロファイル (PC2d, color.profileMismatch): render/import_profile.hpp.

#include "render/import_profile.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "render/ink.hpp"
#include "render/proof.hpp"

namespace genko::render::import_profile {

namespace {

using core::Json;
using core::OpError;
using core::ProfileSpace;

bool grey_mode(std::string_view mode) { return mode == "L" || mode == "LA" || mode == "1" || mode.starts_with("I"); }

// The space a profile for the picture's colours is of (none: a Lab picture, which carries no profile).
std::optional<ProfileSpace> wanted_space(std::string_view mode) {
    if (mode == "LAB") return std::nullopt;
    if (mode == "CMYK") return ProfileSpace::Cmyk;
    return grey_mode(mode) ? ProfileSpace::Gray : ProfileSpace::Rgb;
}

// A profile the book holds (a page's or the book's mode's), by its ref.
core::Bytes held_profile(const core::Document& doc, const std::string& ref) {
    const auto held = [&](const std::optional<core::PageColor>& inks, const std::optional<core::RgbMode>& rgb) -> core::Bytes {
        if (inks && inks->profile == ref && inks->icc) return inks->icc;
        if (rgb && rgb->profile == ref && rgb->icc) return rgb->icc;
        return nullptr;
    };
    if (core::Bytes icc = held(doc.color, doc.rgb)) return icc;
    for (const auto& page : doc.pages) {
        if (core::Bytes icc = held(page->color, page->rgb)) return icc;
    }
    return nullptr;
}

// The op's profile for "assign" ("profile" or "icc"): its bytes (null: sRGB) and its space.
std::pair<core::Bytes, ProfileSpace> assigned(const core::Document& doc, const Json& op) {
    const Json* icc = op.contains("icc") ? &op["icc"] : nullptr;
    const Json* profile = op.contains("profile") ? &op["profile"] : nullptr;
    if (icc != nullptr && profile != nullptr) throw OpError("give the profile as profile or as icc, not both");
    if (icc != nullptr) {
        if (!icc->is_string()) throw OpError("icc is the profile's bytes in base64");
        std::string bytes = core::a2b_base64(icc->get<std::string>());
        const auto space = ink::profile_space(bytes);
        if (!space || *space == ProfileSpace::Lab) throw OpError("the ICC profile cannot be used: Little CMS cannot read it as an RGB, grey or CMYK profile");
        if (const auto why = ink::profile_problem(bytes, *space)) throw OpError("the ICC profile cannot be used: " + *why);
        return {std::make_shared<const std::string>(std::move(bytes)), *space};
    }
    if (profile == nullptr) throw OpError("embedded_profile assign needs the profile (profile or icc)");
    if (!profile->is_string()) throw OpError("profile is a built-in profile's id or sha256:<64 hex>");
    const std::string name = profile->get<std::string>();
    if (name.starts_with("sha256:")) {
        if (const core::BuiltinProfile* builtin = core::builtin_profile_by_ref(name)) return {builtin->bytes, builtin->space};
        core::Bytes bytes = held_profile(doc, name);
        if (!bytes) throw OpError("the book has no ICC profile " + name + " (give it as icc)");
        const auto space = ink::profile_space(*bytes);
        if (!space) throw OpError("the ICC profile cannot be used: Little CMS cannot read it");
        return {bytes, *space};
    }
    const core::BuiltinProfile* builtin = core::builtin_profile(name);
    if (builtin == nullptr) throw OpError("no built-in profile " + core::py_repr_str(name));
    return {builtin->bytes, builtin->space};
}

// Floats 0..1 (straight RGBA) of an "RGBA" picture, and back (rounded).
std::vector<float> floats_of(const Image& rgba) {
    const std::string bytes = rgba.tobytes();
    std::vector<float> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}
Image picture_of(const std::vector<float>& rgba, Size size) {
    std::string bytes(rgba.size(), '\0');
    for (std::size_t i = 0; i < rgba.size(); ++i) bytes[i] = static_cast<char>(std::lround(std::clamp(rgba[i], 0.0f, 1.0f) * 255.0f));
    return Image::frombytes("RGBA", size, bytes);
}

}  // namespace

Taking taking(const core::Document& doc, const core::Page& page, const Json& op, const std::optional<std::string>& embedded,
              std::string_view picture_mode) {
    Taking out;
    const Json* way = op.is_object() && op.contains("embedded_profile") ? &op["embedded_profile"] : nullptr;
    if (way == nullptr || way->is_null()) return out;
    if (!way->is_string() || (*way != "keep" && *way != "convert" && *way != "assign")) throw OpError("embedded_profile is keep, convert or assign");
    out.way = way->get<std::string>();
    if (out.way == "keep") return out;
    // (how: the op's, else the page's own way of bringing colours in)
    if (page.rgb) {
        out.intent = page.rgb->intent;
        out.bpc = page.rgb->bpc;
    } else if (page.color) {
        out.intent = page.color->intent;
        out.bpc = page.color->bpc;
    }
    if (op.contains("intent")) {
        const Json& intent = op["intent"];
        if (!intent.is_string() || std::find(core::kIntents.begin(), core::kIntents.end(), intent.get<std::string>()) == core::kIntents.end())
            throw OpError("intent is perceptual, relative, saturation or absolute");
        out.intent = intent.get<std::string>();
    }
    if (op.contains("bpc")) {
        if (!op["bpc"].is_boolean()) throw OpError("bpc is true or false");
        out.bpc = op["bpc"].get<bool>();
    }
    const std::optional<ProfileSpace> wanted = wanted_space(picture_mode);
    if (out.way == "assign") {
        auto [bytes, space] = assigned(doc, op);
        if (!wanted || space != *wanted) {
            throw OpError(std::string("the profile is not one for the picture's colours (") + std::string(core::to_string(space)) + " for a " +
                          std::string(picture_mode) + " picture)");
        }
        out.converting = true;
        out.from = std::move(bytes);
    } else {
        // (convert: the file's own profile — none for its colours, or none at all: the numbers as they are)
        if (!wanted) return out;
        if (!embedded || embedded->empty() || ink::profile_space(*embedded) != *wanted) return out;
        out.converting = true;
        // (one that shows its numbers as sRGB's: Little CMS's own sRGB, exactly)
        if (*wanted == ProfileSpace::Cmyk || !proof::same_as_srgb(*embedded)) out.from = std::make_shared<const std::string>(*embedded);
    }
    out.description = out.from ? core::icc_description(*out.from) : std::string("sRGB IEC61966-2.1");
    return out;
}

Image taken(const Image& picture, const Taking& way, const core::Page& page) {
    const std::string_view mode = picture.mode();
    if (!way.converting) return picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    core::RgbMode to = page.rgb ? *page.rgb : core::RgbMode{};
    to.intent = way.intent;
    to.bpc = way.bpc;
    const bool rgb_numbers = mode != "CMYK" && !grey_mode(mode);
    if (rgb_numbers) {
        const Image rgba = picture.convert("RGBA");
        if (!way.from && !to.icc) return rgba;  // (sRGB onto sRGB: the same numbers)
        std::vector<float> values = floats_of(rgba);
        ink::rgb_to_rgb(values, way.from, to);
        return picture_of(values, rgba.size());
    }
    // (grey and CMYK numbers through their profile to sRGB, then into the page's profile)
    Image srgb = way.from ? proof::convert_picture(picture, *way.from, way.intent, way.bpc).convert("RGBA") : picture.convert("RGBA");
    if (!to.icc) return srgb;
    std::vector<float> values = floats_of(srgb);
    ink::rgb_to_rgb(values, nullptr, to);
    return picture_of(values, srgb.size());
}

core::InkPixels taken_inks(const Image& picture, const Taking& way, const core::PageColor& color) {
    core::PageColor into = color;
    into.intent = way.intent;
    into.bpc = way.bpc;
    const std::string_view mode = picture.mode();
    if (mode == "CMYK") return ink::from_cmyk_image(picture, nullptr, way.from ? std::string_view(*way.from) : std::string_view(), into);
    if (grey_mode(mode)) {
        const Image srgb = way.from ? proof::convert_picture(picture, *way.from, way.intent, way.bpc) : picture;
        return ink::from_rgb(srgb.convert("RGBA"), into);
    }
    return ink::from_rgb(picture.convert("RGBA"), into, false, way.from);
}

Json report(const Taking& way) {
    Json out{{"way", way.way}};
    if (way.converting) out["profile"] = way.description;
    return out;
}

}  // namespace genko::render::import_profile
