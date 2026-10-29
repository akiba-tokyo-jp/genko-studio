#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "core/adjust.hpp"
#include "core/ink.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"

// The compositor of a CMYK or Lab page (color-mode@1, SPEC.md COLOR-02): the page's inks composited in its own space,
// never through RGB. Its values are held as doubles: CMYK inks 0..1, Lab as its normalized encoding (L/100,
// (a+128)/255, (b+128)/255), each with its alpha and the alpha of the layer before (what a clipped layer clips to).
// Blend modes: CMYK on the inverted plates (1 − ink; hue, saturation, color, luminosity on the inverted C, M, Y as RGB,
// K from below or, for luminosity, from above); Lab on its normalized values (the modes core::lab_blend allows).

namespace genko::render {

// The pixels of a layer in the page's space over a part of the page: per pixel the colour channels (as the canvas
// holds them) then the straight alpha.
struct InkPicture {
    Size size;
    unsigned channels = 4;  // colour channels
    std::vector<double> values;
};

class InkCanvas {
public:
    // The page's paper (`paper`: the 8-bit picture of the paper and its fills, opaque, the canvas's size) in inks.
    InkCanvas(const core::PageColor& color, const Image& paper);
    // A clear canvas (no ink, transparent): layers composited alone, as a merge composites them (render/ink_edit).
    InkCanvas(const core::PageColor& color, Size size);
    ~InkCanvas();
    InkCanvas(const InkCanvas&) = delete;
    InkCanvas& operator=(const InkCanvas&) = delete;

    const core::PageColor& color() const;
    Size size() const;
    unsigned channels() const;

    // Is the mode one this page composites?
    bool supports_blend(std::string_view mode) const;
    // A layer's picture, its opacity, clipped to the layer before or not, in its blend mode; `mask` ("L"): its mask.
    void blend(const InkPicture& picture, double opacity, bool clip, std::string_view mode, const Image* mask = nullptr);
    // An 8-bit straight RGBA picture of a layer, its colours as the page's inks (core rules: the table, the blacks, the
    // greys, the profile).
    void blend_rgba(const Image& rgba, double opacity, bool clip, std::string_view mode);
    // What `draw` paints over the picture so far in 8 bits (balloons, panel borders, tones, …), laid over it as
    // painted: drawn on black and on white, each pixel's cover and colour, the colour as the page's ink. Not a layer: the
    // alpha a clipped layer clips to stays that of the layer before.
    void draw_8bit(const std::function<void(Image&)>& draw);
    // A layer that sets nothing for the next to clip to (a tone layer: the 8-bit page forgets the alpha before).
    void forget_clip();
    // A correction layer (SPEC.md COLOR-02, 群C): the colours the page's inks mean through `change` and back by its
    // rules (ink::correct: what it leaves as it was keeps its inks), mixed in by the opacity × the mask ("L"), × the
    // alpha of the layer before when clipped. The alpha stays; nothing is set for the next layer to clip to. `mode`
    // (PC2: its native.blend): the changed inks laid over those below by the page's blend formula (as a layer's, here
    // on its whole colour), then mixed in. `at`: the canvas's top left on the page (where a pixel is, for a dither).
    void recolour(const core::AdjustFunction& change, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal",
                  Point at = Point{0, 0});
    // PC2 (design 4.2): a correction's records laid on the plates directly (core::InkRecords: each plate's tone — CMYK
    // its brightness, Lab its normalized value, grey its lightness — through its function), mixed in as recolour.
    void adjust_planes(const core::InkRecords& records, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal");
    // PC2d: a correction that moves shapes, worked on the page's inks so far (ink_pixels with their alpha, through
    // render/ink_filters): `changed` (the canvas's size, its values) laid over them as the 8-bit page composites its
    // result — every value, the alpha too, mixed by the opacity × the mask, × the alpha of the layer before when clipped;
    // `mode` (its native.blend) laid on the inks first by the page's formula.
    void correct(const InkPicture& changed, double opacity, bool clip, const Image* mask = nullptr, std::string_view mode = "normal");

    // PC4 E6 (layer-styles@1, render/layer_styles): a layer drawn with its style over `part` of the canvas — `run` gets its
    // values there (channels() and the alpha a pixel) and what the layer's opacity is multiplied by at each (the alpha of
    // the layer before when `clip`, else 1), and leaves the result in them; `own` (the part's size): the layer's own alpha,
    // what a layer clipped to it clips to. styled_done() after the last part.
    void styled(const Box& part, bool clip, const std::vector<float>& own,
                const std::function<void(std::vector<float>& values, const std::vector<float>& weight)>& run);
    void styled_done();

    // PC5 I-b (groups@1): the canvas's values as a layer's picture (an isolated group drawn on a clear canvas, laid as one
    // layer), and a pass-through group's opacity × mask ("L"; none: 1) between `before` (picture() before what the group
    // holds was drawn) and now — premultiplied by the alpha; what the next layer clips to is left —, and what the next layer
    // clips to set (`alpha`, "L": a group's shape).
    InkPicture picture() const;
    void mix_back(const InkPicture& before, double opacity, const Image* mask = nullptr);
    void clip_to(const Image& alpha);

    // The page as the screen shows it ("RGB", sRGB).
    Image display() const;
    // The page's inks at a depth (opaque: alpha full).
    core::InkPixels ink_pixels(int depth, bool alpha = false) const;  // (alpha: as composited; else opaque)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The 8-bit RGBA picture's colours as inks in the canvas's values (straight alpha kept).
InkPicture ink_picture(const Image& rgba, const core::PageColor& color);

// The decoded pixels of GKIR bytes (each decoded once while its bytes live).
std::shared_ptr<const core::InkPixels> decoded_ink(const core::Bytes& bytes);

// An ink raster stretched over the page (`full`, pixels) as a paint layer is (Pillow's BICUBIC over premultiplied
// values), the part `area` only, in the canvas's values.
InkPicture sample_ink_raster(const core::InkPixels& raster, Size full, Box area);

// `above` laid over `below` (the same size), as one layer's parts are (normal, straight alpha).
void ink_over(InkPicture& below, const InkPicture& above);
// The picture as the screen shows it ("RGBA", sRGB; its alpha kept).
Image ink_picture_rgba(const InkPicture& picture, const core::PageColor& color);
// The picture of an ink raster's alpha alone ("RGBA", black): what a locked layer's lines are kept inside.
Image ink_alpha_rgba(const InkPicture& picture);

// A part of ink pixels, and ink pixels laid into others at a corner (the same space and depth; what falls outside is
// left out), replacing what was there.
core::InkPixels crop_inks(const core::InkPixels& pixels, Box box);
void paste_inks(core::InkPixels& into, const core::InkPixels& part, Point at);
// Ink pixels at another size, as Pillow resizes (premultiplied by their alpha): Bicubic or Lanczos.
core::InkPixels resize_inks(const core::InkPixels& pixels, Size size, Resample resample);

// PC4 E6: the page's formula of a blend mode on its canvas values (B(below, above), `n` channels): what InkCanvas lays a
// layer by (CMYK on the inverted plates, Lab on its values, grey on its lightness).
void ink_blend(core::InkSpace space, unsigned n, std::string_view mode, const double* below, const double* above, double* out);

// A canvas value of one channel from the stored ink (CMYK 0..1 as is; Lab L/100, (a+128)/255, (b+128)/255) and back.
double to_canvas(core::InkSpace space, unsigned channel, double ink);
double from_canvas(core::InkSpace space, unsigned channel, double value);

}  // namespace genko::render
