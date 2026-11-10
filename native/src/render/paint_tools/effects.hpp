#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ink.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ink.hpp"
#include "render/paint_tools/tone.hpp"
#include "render/retouch_dabs.hpp"
#include "render/tiled_pixels.hpp"

// PC4 E5 (SPEC.md COLOR-09「画素を変える筆」): what each brush that changes pixels does to a dab of a layer's working copy
// (render/tiled_pixels, retouch::apply_dab_stroke) — PhotoCraft v0.2.0's crates/engine/src/retouch_cmds.rs (lines 434-575:
// color_dab, the toning and focus brushes), crates/paint/src/mixer.rs (the Mixer Brush), bg_erase.rs (the Background
// Eraser) and replace.rs (Color Replacement), MIT (native/third_party/photocraft/NOTICE.md) — in the layer's own values of
// any kind (image_ops::Pixels: 8 bits, precise, inks).
//
// The colour formulas (Dodge, Burn, Sponge, Color Replacement, the Mixer Brush) take an RGB layer's values as they are
// (the page's: sRGB, or its profile's) and an ink layer's as the colours its inks mean by the page's rules, a changed
// colour going back into inks by the rules (a colour left as it was keeps its inks); Blur, Sharpen, Smudge and the
// Background Eraser work the values themselves (an ink layer's plates, as upstream works each channel).

namespace genko::render::paint_tools {

// How the colour formulas see a layer's values.
class Colours {
public:
    explicit Colours(const image_ops::Pixels& like);
    bool inks() const { return ink_.has_value(); }
    unsigned colours() const { return colours_; }
    // Whether its values may lie past 1 (a 32-bit page's): Dodge, Burn and Sponge leave such a pixel as it is.
    bool floating() const { return floating_; }
    // Pixels' colours (`px`: a pixel's values each, colours then the alpha) as straight RGB (3 floats each), and back
    // (each pixel's colours set from `rgb`: an ink layer's by the page's rules from the inks it had — a colour the same as
    // `before`, the RGB to_rgb gave, keeps its inks).
    void to_rgb(const std::vector<const float*>& px, std::vector<float>& rgb) const;
    void from_rgb(const std::vector<float*>& px, const std::vector<float>& rgb, const std::vector<float>& before) const;
    // A straight RGB colour as the layer's colour values (an ink layer's: the inks the rules give it).
    std::vector<float> values_of(const std::array<float, 3>& rgb) const;

private:
    unsigned colours_ = 3;
    bool floating_ = false;
    std::optional<core::PageColor> ink_;
    std::optional<ink::Rules> rules_;
};

// A dab of a colour formula: each pixel of the footprint (not clear) changed by f(colour, k), k = per_dab(its coverage ×
// strength, spacing). `keep_bright`: a pixel with a colour value past 1 is left (Dodge, Burn, Sponge on a 32-bit page).
using ColourFormula = std::function<std::array<float, 3>(const std::array<float, 3>& rgb, float k)>;
void colour_dab(tiled::TiledPixels& work, const retouch::Footprint& fp, const Colours& colours, float strength, float spacing, bool keep_bright,
                const ColourFormula& f);

// A dab of Blur (sharpen false: a Gaussian of sigma) or Sharpen (unsharp masking; protect_detail): the footprint's pixels
// moved toward the result by per_dab(coverage × strength, spacing), every value (the alpha too). It reads around the
// footprint as far as ceil(3 sigma) (sharpen: 3) pixels: the stroke's halo is focus_halo().
void focus_dab(tiled::TiledPixels& work, const retouch::Footprint& fp, bool sharpen, float sigma, float strength, float spacing, bool protect_detail);
// The focus brushes' sigma for a brush's diameter (pixels: upstream's 3 % of it, 1..4) and the halo they read.
float focus_sigma(float diameter);
int focus_halo(float sigma);

// --- the Mixer Brush ---------------------------------------------------------------------------------------------------

// Its paint between strokes (straight RGBA as the colour formulas see the page: an RGB page's values, an ink page's sRGB):
// the paint loaded (none: clean, unloaded) and how much is left of it, the colour picked up from the canvas (none: clean).
struct Reservoir {
    std::optional<std::array<float, 4>> paint;
    float amount = 0.0f;
    std::optional<std::array<float, 4>> pickup;
};
// Its settings (0..1 each): Wet, Load, Mix, Flow.
struct MixerSettings {
    float wet = 0.5f, load = 0.5f, mix = 0.5f, flow = 1.0f;
};
// Fraction of the way the picked-up colour moves toward the canvas's per dab at 100 % Wet.
inline constexpr float kPickupRate = 0.25f;

class Mixer {
public:
    // `sample`: the colours a dab picks up when it samples every layer (straight RGBA over `sample_box` of the grid;
    // none: the layer's own, under the dab). `lock_alpha`: the layer's transparency kept.
    Mixer(const Colours& colours, const MixerSettings& settings, Reservoir reservoir, bool lock_alpha, std::vector<image_ops::Rgba> sample = {},
          Box sample_box = {});
    void dab(tiled::TiledPixels& work, const retouch::Footprint& fp);
    // The paint after the stroke.
    const Reservoir& reservoir() const { return state_; }

private:
    const Colours& colours_;
    MixerSettings m_;
    Reservoir state_;
    bool lock_;
    std::vector<image_ops::Rgba> sample_;
    Box sample_box_;
};

// --- the Background Eraser and Color Replacement -------------------------------------------------------------------------

// Where they sample the colour they match (Photoshop's サンプル): under the brush's centre at every dab, once where the
// stroke starts, or a swatch (背景のスウォッチ).
enum class Sampling { Continuous, Once, Swatch };
// Which matching pixels they take (制限): any under the brush, those joined to the centre, joined and without the soft
// fall of the tolerance's last quarter (輪郭検出).
enum class Limits { Discontiguous, Contiguous, FindEdges };

// The Background Eraser: the pixels under a dab whose colour values (the layer's own: an ink layer's plates) are each
// within `tolerance` (0..1) of the colour sampled lose their alpha by the dab's coverage × how well they match; with
// protect_foreground, a pixel within the tolerance of `foreground` is kept. `foreground` and `swatch`: the layer's colour
// values.
class BackgroundEraser {
public:
    BackgroundEraser(Sampling sampling, Limits limits, float tolerance, bool protect_foreground, std::vector<float> foreground, std::vector<float> swatch);
    void dab(tiled::TiledPixels& work, const retouch::Footprint& fp);

private:
    Sampling sampling_;
    Limits limits_;
    float tolerance_;
    bool protect_;
    std::vector<float> foreground_, swatch_;
    std::optional<std::optional<std::vector<float>>> once_;
};

// Color Replacement: the pixels under a dab whose colour before the stroke (`before`: the layer as it was) is within
// `tolerance` (0..1, largest RGB difference) of the colour sampled take `colour`'s hue, saturation, colour or luminosity
// (`mode`: image_ops::blend_rgb's hue | saturation | color | luminosity) by the dab's coverage × how well they match.
// `colour` and `swatch` are straight RGB as the colour formulas see the page.
class ColorReplacement {
public:
    ColorReplacement(const Colours& colours, const tiled::TiledPixels& before, std::string mode, Sampling sampling, Limits limits, float tolerance,
                     bool antialias, std::array<float, 3> colour, std::array<float, 3> swatch);
    void dab(tiled::TiledPixels& work, const retouch::Footprint& fp);

private:
    const Colours& colours_;
    const tiled::TiledPixels& before_;
    std::string mode_;
    Sampling sampling_;
    Limits limits_;
    float tolerance_;
    bool antialias_;
    std::array<float, 3> colour_, swatch_;
    std::optional<std::array<float, 3>> once_;
};

}  // namespace genko::render::paint_tools
