#pragma once

#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

// The monochrome finish of Python's genko/screentone.py (finish_gray and what it uses: the line mask, the flat tone
// steps, AM dots and FM noise): a greyscale picture turned into print (black and white with dots) or a proof (the
// quantized greys). The AM threshold tile is tones::threshold_tile's. The same pixels as the Python baseline.

namespace genko::render::screentone {

// screentone.Finish.from_dict({**studio.style.finish, **layer.finish}): the finish's settings. Python keeps each value as
// it is given (no check); a value is looked at where finish_gray uses it, and one that is not a number there is refused
// then (core::Error "value": "the finish's <name> is not a number" — Python fails with a TypeError of its own there).
// The steps are float(s) of each, those in (0, 1), sorted (Python's errors for one float() cannot read).
class Finish {
public:
    static Finish from_dict(const core::Json& data);

    double number(const char* name) const;  // black, white, line_threshold, line_contrast, lpi, angle, face_light, smooth
    bool fm() const;                        // screen == "fm"
    const std::vector<double>& steps() const { return steps_; }

private:
    core::Json values_ = core::Json::object();
    std::vector<double> steps_{0.1, 0.2, 0.3};
};

// screentone.line_mask: 255 where a pixel is part of a drawn line (dark, and darker than its neighbourhood).
Image line_mask(const Image& grey, const Finish& finish, int dpi);

// screentone.dots(coverage, size, dpi, lpi, angle, origin): 0 where ink goes, 255 elsewhere ("L").
Image dots(double coverage, Size size, int dpi, double lpi, double angle, Point origin);
// screentone.noise(coverage, size): the FM (Floyd–Steinberg) tone ("L").
Image noise(double coverage, Size size);

// screentone.finish_gray: greyscale art → print (screen: only 0 and 255) or proof (quantized greys). `faces` (white
// where a face is) lightens their mid greys; `dark` / `light` print the clothes there solid black / white.
Image finish_gray(const Image& grey, const Finish& finish, int dpi, bool screen, Point origin, const Image* faces,
                  const Image* dark, const Image* light);

}  // namespace genko::render::screentone
