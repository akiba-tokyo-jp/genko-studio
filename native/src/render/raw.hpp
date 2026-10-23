#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/raw_develop.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/limits.hpp"

// Camera RAW (PC4 F-e, SPEC.md FILE-01「カメラ RAW」; design 4.2): camera RAW files read and developed with LibRaw (0.22,
// LGPL-2.1 or CDDL-1.0, linked as a shared library: docs/cpp-migration/THIRD_PARTY.md, native/package/LIBRAW-LGPL-ja.md).
//
// The RAW stage is LibRaw's: open_buffer → unpack → dcraw_process (the demosaic, the white balance, the black and white
// levels, the highlights, the camera's matrix) into linear ProPhoto RGB of 16 bits (output_color 4, gamma 1, no
// automatic brightness), the file's orientation applied by LibRaw (or none). Genko then gives it the exposure (the
// development's EV after the file's baseline exposure, and back what LibRaw's highlight modes other than clip leave
// below white), Camera Raw's adjustments (節 D's camera_raw kernel, in the built-in prophoto-compat: the window's preview
// works the same values) and takes it with Little CMS into the output space (one of the built-in profiles — srgb,
// adobe-rgb-compat, prophoto-compat, display-p3 —: the page's profile is then the picture's own, its pixels the same) at
// 8 or 16 bits, with the output sharpening last. The white balance is the camera's as shot, LibRaw's automatic one, or a
// colour temperature and tint (custom: the white's multipliers through the camera's matrix — the colour science of
// PhotoCraft v0.2.0's raw/src/color.rs, MIT, with the Planckian locus of Kim et al. 2002 and DNG's tint scale of 1/3000
// of a uv unit).
//
// LibRaw is given what Genko's limits allow before it allocates: the file at most 256 MiB, LibRaw's own memory limit
// (max_raw_memory_mb) from the budget, and the sensor's size and the picture's checked against codecs::Limits — what the
// whole development will hold at once — after the header is read and before the sensor data is unpacked (a decompression
// bomb or a size that lies is refused there). The work stops (render::Cancelled) between LibRaw's stages and its rows
// when `stopped` says so. LibRaw runs without OpenMP: one thread a development.
//
// Errors are core::Error: "unidentified_image" (not a RAW file LibRaw reads: a camera or a kind of compression it does not
// know — 開く… then opens its embedded preview, if it has one), "format" (a RAW file cut short or broken), "image_too_large"
// (past the limits), "too_large" (a file larger than 256 MiB).

namespace genko::render::raw {

inline constexpr std::uint64_t kFileMaxBytes = 256ull * 1024 * 1024;

// LibRaw's version this build runs with ("0.22.2"): `genko --version`, and a development's camera.libraw.
std::string libraw_version();

// Is it a RAW's suffix (core::smart::kLaterFileFormats' raw: .dng .cr2 .cr3 .nef …)?
bool raw_suffix(std::string_view suffix);
// Do the bytes say they are a camera RAW file — a mark only RAW files carry (CR3's, RAF's, ORF's, RW2's, MRW's, X3F's,
// CRW's), or a TIFF-shaped file that is a DNG (its DNGVersion) or a CR2 (its "CR")? A TIFF-shaped RAW without such a mark
// (NEF, ARW, PEF…) is known by its suffix (raw_suffix). Nothing is decoded.
bool looks_raw(std::string_view bytes);
// A camera RAW file: its suffix is a RAW's or its bytes look so, and LibRaw opens it.
bool is_raw(std::string_view bytes, std::string_view suffix);

using Mat3 = std::array<std::array<double, 3>, 3>;

// What the camera wrote: its maker and model (LibRaw's names), the kind of RAW file ("DNG", "CR2", …), ISO, shutter (s),
// aperture (f), focal length (mm) and when it was taken (seconds since 1970; 0: not said).
struct Camera {
    std::string make, model, format;
    std::optional<double> iso, shutter, aperture, focal;
    std::int64_t taken = 0;
};

// What LibRaw read of the file's header (open_buffer: nothing decoded).
struct Info {
    Camera camera;
    std::int64_t raw_width = 0, raw_height = 0;  // the sensor data's
    std::int64_t width = 0, height = 0;          // the picture's, before its orientation
    int flip = 0;                                 // LibRaw's orientation (0 none, 3 180°, 5 90° left, 6 90° right)
    int colors = 3;                               // 1 for a monochrome sensor
    bool mosaic = true;                           // a colour filter array (false: LinearRaw)
    std::array<double, 3> as_shot{0, 0, 0};       // the camera's white balance (R, G, B multipliers, G 1); 0: none
    std::array<double, 3> daylight{1, 1, 1};      // the daylight white's multipliers (its matrix's)
    Mat3 xyz_to_camera{};                         // XYZ → camera (the matrix the white balance is worked through)
    bool matrix = false;                          // xyz_to_camera is the file's or LibRaw's table's (false: sRGB's)
    double black = 0, white = 0;                  // the levels
    double baseline_exposure = 0;                 // a DNG's BaselineExposure (EV)
    bool preview = false;                         // it carries an embedded preview
};
Info identify(std::string_view bytes, const codecs::Limits& limits = {});
// identify, and the sensor data decoded once (LibRaw's unpack: a file cut short or broken refused here, core::Error("format"))
// — what placing a RAW file as a smart object checks before it becomes contents.
Info unpacked(std::string_view bytes, const codecs::Limits& limits = {});
// The suffix a RAW file of no name is kept as (its kind's: .dng, .cr2, .cr3, .raf, .orf, .rw2, .mrw, .x3f; a TIFF-shaped
// RAW's by its maker — .nef, .arw, .pef… —, else .dng).
std::string suffix_of(const Info& info);
// An output space's ICC profile (the built-in's bytes; none for srgb: Little CMS's own sRGB, a page's of no profile).
std::optional<std::string> space_bytes(std::string_view space);

// The developed picture's size with this development (its orientation applied or not).
std::pair<std::int64_t, std::int64_t> developed_size(const Info& info, const core::raw::Develop& develop);

// The white (CIE xy) of a colour temperature (kelvin, 2000..50000, on the Planckian locus) and a tint (−150..150: 1/3000
// of a CIE 1960 uv unit off the locus, positive toward green — a white of positive tint is balanced to magenta, as Camera
// Raw's 色かぶり補正), and back.
std::array<double, 2> white_xy(double temperature_k, double tint);
std::pair<double, double> temperature_tint(const std::array<double, 2>& xy);
// The camera's R, G, B multipliers (G 1) that balance this white, and the temperature and tint of the white multipliers
// balance (through the camera's matrix: Info::xyz_to_camera).
std::array<double, 3> multipliers_for(const Info& info, double temperature_k, double tint);
std::pair<double, double> temperature_of(const Info& info, const std::array<double, 3>& multipliers);

struct Options {
    // The window's preview: LibRaw's half size, and the picture made at most this many pixels a side (0: whole).
    bool half_size = false;
    int max_side = 0;
    codecs::Limits limits;
    // Asked between LibRaw's stages and its rows: true stops the work (render::Cancelled).
    std::function<bool()> stopped;
};

// A development: the picture in its output space (RGB of its depth, its ICC profile the output space's — none for
// srgb —; for the window, floats in prophoto-compat: preview()), what LibRaw read, the multipliers it was balanced with
// (R, G, B; G 1) and what it says of it (Japanese: approximations, a missing white balance).
struct Developed {
    codecs::CodecImage picture;
    Info info;
    std::array<double, 3> multipliers{1, 1, 1};
    std::vector<std::string> notes;
};
// The file developed with `raw` (a contents' raw, set_camera_raw's: {"develop", "adjust"?, "camera"?}; read as
// core::raw::normal_raw reads it — keys left out at their defaults).
Developed develop(std::string_view bytes, const core::Json& raw, const Options& options = {});

// Camera Raw's window: the RAW stage of `develop` (no adjustments, no sharpening) at the window's size, as straight RGBA
// floats in prophoto-compat (its values past 1 kept), and how they show on the screen (prophoto-compat → sRGB's display
// values, in place: thread-safe).
struct Preview {
    int width = 0, height = 0;
    std::vector<float> rgba;
    Info info;
    std::array<double, 3> multipliers{1, 1, 1};
    std::vector<std::string> notes;
};
Preview preview(std::string_view bytes, const core::raw::Develop& develop, int max_side, const Options& options = {});
void working_to_screen(float* rgba, std::size_t pixels);
// 自動 (環境設定 → Camera Raw → 自動補正, and the window's button): the exposure (EV) to add to the preview's development
// so that its mean luminance — the geometric mean of ProPhoto's Y in linear light, the clear and black pixels left out — is
// the middle grey (0.18), within −3..3 and to 0.05 EV.
double auto_exposure(const Preview& preview);

// The embedded preview the camera wrote (its largest JPEG, or LibRaw's bitmap), as an 8-bit RGB picture — what 開く…
// opens of a RAW file LibRaw cannot develop (a camera it does not know). None when it carries none that reads.
std::optional<codecs::CodecImage> embedded_preview(std::string_view bytes, const codecs::Limits& limits = {});

// A RAW contents' development settings as placing it makes them (camera-raw@1's defaults, and what LibRaw read of the
// camera): {"develop": its defaults, "camera": {"make", "model", "format", "libraw"}}.
core::Json default_raw(std::string_view bytes);

// The developed picture as a file the drawing reads (an uncompressed TIFF of its depth with its output space's ICC
// profile): what a RAW smart object draws — render/smart.cpp keeps it by the contents' file and development.
std::string developed_file(std::string_view bytes, const core::Json& raw, const Options& options = {});

}  // namespace genko::render::raw
