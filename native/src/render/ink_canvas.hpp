#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "render/image.hpp"

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

    // The page as the screen shows it ("RGB", sRGB).
    Image display() const;
    // The page's inks at a depth (opaque: alpha full).
    core::InkPixels ink_pixels(int depth) const;

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

// A part of ink pixels, and ink pixels laid into others at a corner (the same space and depth).
core::InkPixels crop_inks(const core::InkPixels& pixels, Box box);
void paste_inks(core::InkPixels& into, const core::InkPixels& part, Point at);

// A canvas value of one channel from the stored ink (CMYK 0..1 as is; Lab L/100, (a+128)/255, (b+128)/255) and back.
double to_canvas(core::InkSpace space, unsigned channel, double ink);
double from_canvas(core::InkSpace space, unsigned channel, double value);

}  // namespace genko::render
