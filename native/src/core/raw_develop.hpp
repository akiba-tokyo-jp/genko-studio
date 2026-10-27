#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"

// camera-raw@1 (PC4 F-e, SPEC.md FILE-01「カメラ RAW」, schema-v4.md「ファイル・形式」): a camera RAW file's development
// settings as a book keeps them — a RAW smart object's contents' `raw` ({"develop", "adjust"?, "camera"}), set_camera_raw's
// settings and what 開く… develops a RAW file with (Camera Raw's window, `genko new --from x.dng --raw '<JSON>'`).
//
// - `develop`: the RAW stage (render/raw: LibRaw) and the output stage — the white balance (撮影時の設定 as_shot, 自動 auto,
//   or custom: a colour temperature in kelvin 2000..50000 and a tint −150..150, the white the picture is balanced to), the
//   exposure in EV (−5..5, after the file's own baseline exposure), the demosaic (LibRaw's: AHD unless chosen), what is done
//   with the highlights a channel clips (clip, unclip, blend, rebuild), the output space (one of the built-in profiles:
//   prophoto-compat, adobe-rgb-compat, srgb, display-p3), its depth (8 or 16 bits), the orientation (the file's, or none)
//   and the output sharpening (none, screen, glossy, matte). Its defaults are PhotoCraft's development's (ProPhoto 16 bits,
//   as shot, AHD; design 0.1), not 環境設定's Camera Raw (the window's: an op reads no settings of the machine).
// - `adjust`: Camera Raw's adjustments (節 D's camera_raw — Camera Raw フィルター —, its normal form), worked on the
//   developed picture before its output space and depth (none: none).
// - `camera`: what the file said of its camera and how it was read ({"make", "model", "format", "libraw"}: LibRaw's version).
//
// A book written by this build holds the normal form; read, a `develop` may leave keys out (their defaults), but a key or a
// value this build does not know is refused (core::Error("value")): the reader opens such a book read-only.

namespace genko::core::raw {

inline constexpr std::array<std::string_view, 3> kWhiteBalances{"as_shot", "auto", "custom"};
inline constexpr std::array<std::string_view, 7> kDemosaics{"ahd", "aahd", "dht", "dcb", "ppg", "vng", "linear"};
inline constexpr std::array<std::string_view, 4> kHighlights{"clip", "unclip", "blend", "rebuild"};
inline constexpr std::array<std::string_view, 4> kSpaces{"prophoto-compat", "adobe-rgb-compat", "srgb", "display-p3"};
inline constexpr std::array<std::string_view, 2> kOrientations{"file", "none"};
inline constexpr std::array<std::string_view, 4> kSharpens{"none", "screen", "glossy", "matte"};
inline constexpr std::array<std::string_view, 4> kCameraKeys{"make", "model", "format", "libraw"};

inline constexpr double kTemperatureMin = 2000;
inline constexpr double kTemperatureMax = 50000;
inline constexpr double kTintMax = 150;
inline constexpr double kExposureMax = 5;
inline constexpr std::size_t kCameraTextMax = 255;  // (bytes of a camera's text)

struct Develop {
    std::string white_balance = "as_shot";
    // (custom only) the white the picture is balanced to
    double temperature_k = 5500;
    double tint = 0;
    double exposure_ev = 0;
    std::string demosaic = "ahd";
    std::string highlights = "clip";
    std::string space = "prophoto-compat";
    int depth = 16;
    std::string orientation = "file";
    std::string sharpen = "none";

    bool operator==(const Develop&) const = default;
};

// The normal form: every key, in this order (temperature_k and tint only for custom; numbers rounded to 1e-6).
Json to_json(const Develop& develop);
// A develop object read (none: the defaults): keys left out at their defaults, numbers out of range clamped (as Camera
// Raw's fields are), custom without a temperature 5500 K. core::OpError for a key it does not know, a value of the wrong
// type or one it does not know.
Develop develop_from(const Json* develop);

// A RAW contents' `raw` (or set_camera_raw's settings) in its normal form: {"develop": normal, "adjust"?: camera_raw's
// normal form (left out when it changes nothing), "camera": as given}. core::OpError for what it refuses.
Json normal_raw(const Json& raw);
// The `raw` a book holds well formed (the reader's and the writer's check): an object of develop (well formed, keys
// left out allowed), adjust (camera_raw's settings it reads) and camera (text of its keys only). core::Error("value").
void check_raw(const Json& raw);

// The develop of a contents' `raw` (none, or none there: the defaults).
Develop develop_of(const std::optional<Json>& raw);
// The adjust of a contents' `raw`: camera_raw's normal form, or none when it changes nothing.
std::optional<Json> adjust_of(const std::optional<Json>& raw);

}  // namespace genko::core::raw
