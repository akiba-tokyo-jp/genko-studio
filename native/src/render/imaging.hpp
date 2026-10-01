#pragma once

// Internal to genko_render: libImaging's C API for render's own .cpp files. (Its header defines macros such as
// UINT8 and CLIP8, so it never appears in render's public headers.)

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "ImagingCxx.h"  // libImaging/Imaging.h as C++ reads it (made by native/third_party/pillow/CMakeLists.txt)
#include "imaging_glue.h"
#include "resample_region.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <string>
#include <string_view>

#include "render/image.hpp"

namespace genko::render::detail {

// Throws the error libImaging reported on this thread (core::Error "memory" or "value").
[[noreturn]] void throw_imaging_error(std::string_view fallback = "image operation failed");

// The image libImaging returned, or its error thrown.
inline Imaging check(Imaging im) {
    if (im == nullptr) throw_imaging_error();
    return im;
}

// A status libImaging returned (negative or zero: failed), or its error thrown.
void check_status(int status, std::string_view fallback = "image operation failed");

ModeID mode_id(std::string_view mode);
RawModeID rawmode_id(std::string_view rawmode);
std::string_view mode_name(ModeID mode);

// Pillow's getink: a colour as libImaging draws with it (four bytes in an int).
INT32 ink_for(const Ink& colour, Imaging im);

// Pillow's ImageMode.getmode(mode): the base mode ("L" for "1", "I;16", "LA"…; "RGB" for RGBA, HSV…; "P" for P),
// the type of each band ("L", "I", "F") and the band names. core::Error("value", "illegal image mode") otherwise.
std::string_view mode_base(std::string_view mode);
std::string_view mode_type(std::string_view mode);
int mode_bands(std::string_view mode);

}  // namespace genko::render::detail
