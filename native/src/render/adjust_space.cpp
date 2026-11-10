// PC2a (SPEC.md COLOR-04, design 4.2): the conversions levels' and a tone curve's channel records in another space than
// their page's go through (core/adjust_tone.hpp's ToneBridge), by Little CMS (render/ink): a page's colour — an RGB
// page's encoded value (its profile's, or sRGB), a page of inks' the sRGB colour they mean — into the built-in
// coated-cmyk's inks (relative colorimetric, black point compensation) or D50 Lab, and back the same way.

#include <optional>
#include <vector>

#include "core/adjust_tone.hpp"
#include "core/color_model.hpp"
#include "render/adjust.hpp"
#include "render/ink.hpp"

namespace genko::render::adjust {

namespace {

core::ToneBridge bridge(const core::Page& page, std::string_view space) {
    core::PageColor target;
    if (space == "cmyk") {
        const core::BuiltinProfile* coated = core::builtin_profile("coated-cmyk");
        if (coated == nullptr) return {};
        target.space = core::InkSpace::Cmyk;
        target.profile = coated->ref();
        target.icc = coated->bytes;
    } else if (space == "lab") {
        target.space = core::InkSpace::Lab;
    } else {
        return {};
    }
    target.depth = 16;
    target.intent = "relative";
    target.bpc = true;
    // (the page's values: an RGB page of a profile its profile's; else sRGB — a page of inks: the colours they mean)
    std::optional<core::RgbMode> own;
    if (!page.color && page.rgb && core::profiled_rgb(page)) own = *page.rgb;
    const bool cmyk = space == "cmyk";
    core::ToneBridge out;
    out.into = [target, own, cmyk](const std::array<double, 3>& rgb) {
        std::array<double, 3> srgb = rgb;
        if (own) {
            std::vector<float> v{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2]), 1.0F};
            ink::rgb_to_rgb(v, own->icc, core::RgbMode{});
            srgb = {v[0], v[1], v[2]};
        }
        std::vector<double> tones = ink::ink_of_srgb(target, srgb);
        if (cmyk) {
            for (double& t : tones) t = 1.0 - t;  // (each plate's brightness)
        } else {
            tones = {tones[0] / 100.0, (tones[1] + 128.0) / 255.0, (tones[2] + 128.0) / 255.0};
        }
        return tones;
    };
    out.back = [target, own, cmyk](const std::vector<double>& tones) {
        std::vector<double> inks = tones;
        if (cmyk) {
            for (double& t : inks) t = 1.0 - t;
        } else {
            inks = {tones[0] * 100.0, tones[1] * 255.0 - 128.0, tones[2] * 255.0 - 128.0};
        }
        std::array<double, 3> rgb = ink::srgb_of(target, inks);
        if (own) {
            std::vector<float> v{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2]), 1.0F};
            ink::rgb_to_rgb(v, nullptr, *own);
            rgb = {v[0], v[1], v[2]};
        }
        return rgb;
    };
    return out;
}

}  // namespace

void install_tone_bridge() { core::set_tone_bridge(bridge); }

}  // namespace genko::render::adjust
