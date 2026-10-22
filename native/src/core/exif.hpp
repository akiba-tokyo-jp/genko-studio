#pragma once

#include <optional>
#include <string>
#include <string_view>

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」: レンズ補正・広角補正): what a camera's EXIF says of a shot that the lens
// filters use — PhotoCraft v0.2.0's algo/src/exif.rs (MIT, native/third_party/photocraft/NOTICE.md), a minimal reader of
// the TIFF-structured EXIF block (II*\0 / MM\0*, a JPEG's "Exif\0\0" before it skipped) as a file holds it
// (render/codecs read_metadata gives it; a page opened from a file keeps it: file-info@1's native_source). Bytes that are
// cut short or broken give what was read before them, never an error. Tag numbers: EXIF 2.32 (CIPA DC-008).

namespace genko::core::exif {

struct Camera {
    std::optional<std::string> make;
    std::optional<std::string> model;
    std::optional<std::string> lens;
    std::optional<double> exposure_time;      // seconds
    std::optional<double> f_number;
    std::optional<double> iso;
    std::optional<double> focal_length;       // mm (0x920A)
    std::optional<double> focal_length_35mm;  // the 35 mm-equivalent focal length, mm (0xA405; 0 is none)

    bool operator==(const Camera&) const = default;
};

// What the EXIF block says (IFD0 and the Exif IFD it points to).
Camera read(std::string_view exif);
// A little-endian EXIF block saying `camera` (the tests' and synthetic files').
std::string build(const Camera& camera);

}  // namespace genko::core::exif
