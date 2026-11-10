#pragma once
#include <string_view>
#include "render/png.hpp"

namespace genko::render {
Image read_bmp(std::string_view bytes, const PngLimits& limits = {});
// Image.open(bytes).size of a BMP: its headers and palette read as read_bmp reads them (their errors), its pixels not.
Size bmp_size(std::string_view bytes, const PngLimits& limits = {});
}
