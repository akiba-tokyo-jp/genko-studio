#pragma once
#include "render/png.hpp"

namespace genko::render {
// Pillow-compatible GIF87a/89a first-frame P/L pixels, palette, transparency, offsets and interlace.
// Later animation frames belong to the separate animation interface. All pixels load before return.
Image read_gif(std::string_view bytes, PngLimits limits = {});
// Image.open(bytes).size of a GIF: read as read_gif reads it up to the first frame's pixels (their errors), those not.
Size gif_size(std::string_view bytes, PngLimits limits = {});
}  // namespace genko::render
