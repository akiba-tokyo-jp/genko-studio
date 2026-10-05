#pragma once
#include <string_view>
#include "render/png.hpp"

namespace genko::render {
Image read_bmp(std::string_view bytes, const PngLimits& limits = {});
}
