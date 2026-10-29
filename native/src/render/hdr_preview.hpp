#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/ink.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// 32 bit プレビューオプション (view.thirtyTwoBitPreviewOptions, PC5 H-a, SPEC VIEW-01; PhotoCraft's HdrPreview, MIT): how a
// 32-bit page is shown on the screen — display only, never the book's, never an export. 露光量とガンマ: each colour in linear
// light × 2^exposure, then ^(1/gamma); ハイライト圧縮: the light (Rec. 709's weights) L as L / (1 + L), the colours kept
// in proportion (Reinhard; exposure and gamma not used, as Photoshop's window greys them). Applied to the page's values
// before they are cut to 0..1 — what lies above 1 comes back with a negative exposure (PhotoCraft applies it after the
// display's cut, so it cannot).

namespace genko::render {

class RgbSpace;

struct HdrPreview {
    bool highlight_compression = false;
    double exposure = 0.0;  // stops, −20…20
    double gamma = 1.0;     // 0.1…9.99

    static constexpr double kMinExposure = -20.0;
    static constexpr double kMaxExposure = 20.0;
    static constexpr double kMinGamma = 0.1;
    static constexpr double kMaxGamma = 9.99;

    // Shown as the page is (露光量 0, ガンマ 1): nothing is done.
    bool identity() const { return !highlight_compression && exposure == 0.0 && gamma == 1.0; }
    // Within its ranges.
    HdrPreview clamped() const;
    bool operator==(const HdrPreview&) const = default;
};

// One colour in linear light (r, g, b) through the preview, in place (not cut to 0..1).
void hdr_linear(const HdrPreview& preview, double* rgb);
// One grey in linear light through the preview (not cut).
double hdr_grey(const HdrPreview& preview, double linear);

// Does the page take the preview (a 32-bit page: RGB, grey, CMYK, Lab or multichannel)?
bool hdr_page(const core::Page& page);

// A tile of a 32-bit RGB page as shown with the preview: `drawn` ("RGB", as render_page gave it) and `precise` (its GKCR
// f32 values, the page's profile's: RenderOptions::precise). Each pixel the values show as `drawn` shows it (within one
// step: the values are the page under what is drawn over it) is shown through the preview; any other (what is drawn over
// the page and not in its values) as drawn.
Image hdr_shown(const Image& drawn, const std::string& precise, const RgbSpace& space, const HdrPreview& preview,
                const std::vector<unsigned char>* drawn_over = nullptr);
// The same for a 32-bit CMYK, Lab, grey or multichannel page from its inks (RenderOptions::inks): a grey page's lightness
// (any value) through its tone curve, the other spaces' colours as the screen shows them (their inks never pass 0..1).
Image hdr_shown(const Image& drawn, const core::InkPixels& inks, const core::PageColor& color, const HdrPreview& preview,
                const std::vector<unsigned char>* drawn_over = nullptr);

}  // namespace genko::render
