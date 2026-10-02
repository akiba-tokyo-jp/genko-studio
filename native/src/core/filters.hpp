#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// The colour adjustments of Python's genko/filters.py (a correction layer's "adjust"): how apply_filter reads their
// settings and the tables it makes of them. One reading for the ops, which try an adjustment when it is set (Python's
// _adjust_spec runs apply_filter on a small picture, where only the settings can fail), and for the renderer, which
// lays the tables on the picture (render/composite.cpp).

namespace genko::core {

// filters.ADJUSTMENTS, in Python's order.
inline constexpr std::array<std::string_view, 9> kAdjustments{"levels",       "curve",   "hue",
                                                              "invert",       "posterize", "threshold",
                                                              "gradient_map", "bitonal", "brightness_contrast"};

// What an adjustment does to the picture: tables of 256 values each, as Image.point takes them (it keeps each value in
// 0..255).
struct Adjustment {
    enum class Way {
        Rgb,   // the picture's red, green and blue through tables[0], [1] and [2] (Image.point)
        Hsv,   // the picture in HSV, its hue, saturation and value through tables[0], [1] and [2]
        Grey,  // the picture in grey (ImageOps.grayscale), its red, green and blue made by tables[0], [1] and [2]
    };
    Way way = Way::Rgb;
    std::array<std::vector<int>, 3> tables;
};

// apply_filter(image, kind, params) for one of kAdjustments, without the picture: the tables, in the order Python
// reads the settings and with its exceptions (core/error.hpp): PyValueError (a value of the wrong kind, a tone curve
// or a gradient map with fewer than two points, round() or int() of NaN), PyTypeError, OpKeyError (KeyError),
// PyUncaught (round() or int() of an infinity: OverflowError; a colour with fewer than three values: IndexError;
// ZeroDivisionError). PyValueError "unknown filter <kind>" for another kind.
Adjustment adjustment(std::string_view kind, const Json& params);

// numpy.interp(x, xp, fp) for sorted sample points xp (the gradient map here, the gradient fills of the renderer).
class NumpyInterp {
public:
    // core::Error("value") with numpy's message for no sample points or lists of two lengths.
    NumpyInterp(std::vector<double> xp, std::vector<double> fp);
    double operator()(double x) const;

private:
    std::vector<double> xp_;
    std::vector<double> fp_;
    std::vector<double> slopes_;
};

}  // namespace genko::core
