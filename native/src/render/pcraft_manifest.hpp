#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/vector.hpp"

// PhotoCraft's .pcraft manifest (PC4 F-f, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/format/src/manifest.rs, MIT): the parts of
// its serde form that Genko reads and writes — the pixel formats, surfaces of 256 × 256 tiles named by their BLAKE3 hash,
// colours, blend modes, paths and vector masks, the affine maps — each read strictly (a shape or value of another version
// is refused, never guessed) and written in the same form. What a layer or a document holds beyond these is read by
// render/pcraft_read.cpp and written by formats/pcraft_write.cpp with these.
//
// The serde forms (upstream's types are the specification; only format_version 1, pin ad86321):
//   PixelFormat {"mode": "Rgb", "sample": "U8", "alpha": true}; a surface {"format", "default": the untouched pixel as hex
//   of its little-endian samples, "tiles": [{"tx", "ty", "hash"}]} (tile (tx, ty) covers the document's pixels
//   [256·tx, 256·tx + 256) × [256·ty, …), its bytes the samples interleaved, little-endian); a colour {"mode", "c": [4
//   numbers], "alpha"} (normalized: Lab's c is L/100, (a + 128)/255, (b + 128)/255; CMYK's ink, 0 none; grey's lightness);
//   an enum without data as its variant's name ("Multiply"), with data as {"Variant": …}; an affine map {"m": [a, b, c,
//   d, e, f]} (x' = a·x + c·y + e, y' = b·x + d·y + f, document pixels).

namespace genko::render::pcraft {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr int kTileSize = 256;
inline constexpr std::string_view kManifest = "manifest.json";
inline constexpr std::string_view kThumb = "thumb.png";
inline constexpr std::string_view kComposite = "composite/preview.png";
inline constexpr std::string_view kGenerator = "genko-native 0.1.0";

// A part of the manifest this build does not read (another version's shape, a value it does not know): the reader leaves
// that part out and says so (its notes), or refuses the file when it is the document's own.
class Unreadable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Mode { Bitmap, Grayscale, Indexed, Rgb, Cmyk, Lab, Multichannel, Duotone };
enum class Sample { U8, U16, F32 };

std::string_view to_string(Mode mode);
std::string_view to_string(Sample sample);
Mode mode_from(const core::Json& value);      // (Unreadable)
Sample sample_from(const core::Json& value);  // (Unreadable)
// The colour channels a pixel of the mode stores (an indexed and a multichannel document's as RGB, a bitmap and a duotone
// one's as grey: upstream's Document::pixel_format).
unsigned colour_channels(Mode mode);
unsigned sample_bytes(Sample sample);

struct PixelFormat {
    Mode mode = Mode::Rgb;
    Sample sample = Sample::U8;
    bool alpha = true;

    unsigned channels() const { return colour_channels(mode) + (alpha ? 1u : 0u); }
    std::size_t pixel_bytes() const { return channels() * static_cast<std::size_t>(sample_bytes(sample)); }
    std::size_t tile_bytes() const { return static_cast<std::size_t>(kTileSize) * kTileSize * pixel_bytes(); }
    bool operator==(const PixelFormat&) const = default;
};
PixelFormat format_from(const core::Json& value);
core::Json to_json(const PixelFormat& format);

// One sample of the little-endian bytes as a normalized value (U8 / 255, U16 / 65535, F32 as it is) and back (integers
// rounded and clamped to 0..1, as upstream's write_sample).
double read_sample(const unsigned char* bytes, Sample sample);
void write_sample(unsigned char* bytes, Sample sample, double value);

struct TileRef {
    std::int32_t tx = 0;
    std::int32_t ty = 0;
    std::string hash;
};

struct Surface {
    PixelFormat format;
    std::vector<double> background;  // the untouched pixel, normalized (format.channels() values)
    std::vector<TileRef> tiles;
};
// (Unreadable for another shape; a tile named twice or by a name that is not a hash: core::Error("format"))
Surface surface_from(const core::Json& value);
core::Json to_json(const Surface& surface);

struct Color {
    Mode mode = Mode::Rgb;
    std::array<double, 4> c{0, 0, 0, 0};
    double alpha = 1.0;
};
Color color_from(const core::Json& value);
core::Json to_json(const Color& color);
Color rgb_color(double r, double g, double b, double alpha = 1.0);

// Upstream's 28 blend modes ↔ Genko's 20 ("Normal" ↔ "normal", "LinearDodge" ↔ "add" …). None for a mode Genko does
// not composite (Dissolve, DarkerColor, LighterColor, VividLight, LinearLight, PinLight, HardMix; PassThrough is a group's).
std::optional<std::string> genko_blend(std::string_view upstream);
std::string upstream_blend(std::string_view genko);
// One of upstream's names (Unreadable for another).
std::string blend_from(const core::Json& value);

// An affine map of document pixels.
using Affine = std::array<double, 6>;
Affine affine_from(const core::Json& value);
core::Json to_json(const Affine& m);

// A path of document pixels as Genko's (mm: each coordinate × `mm_per_px`), and back (÷).
core::vector::Path path_from(const core::Json& value, double mm_per_px);
core::Json path_json(const core::vector::Path& path, double px_per_mm);
// A vector mask ({"path", "enabled", "linked", "density", "feather": pixels}) the same way.
core::vector::VectorMask vector_mask_from(const core::Json& value, double mm_per_px);
core::Json vector_mask_json(const core::vector::VectorMask& mask, double px_per_mm);

// Strict reading of serde's values (Unreadable naming `what`).
const core::Json& field(const core::Json& object, std::string_view key, std::string_view what);
const core::Json* optional_field(const core::Json& object, std::string_view key);  // (null: absent or null)
double number(const core::Json& value, std::string_view what);                   // (finite)
bool boolean(const core::Json& value, std::string_view what);
std::string text(const core::Json& value, std::string_view what);
std::int64_t whole(const core::Json& value, std::string_view what);
// An externally tagged enum: its variant's name and data (a unit variant: its name and null).
std::pair<std::string, const core::Json*> variant(const core::Json& value, std::string_view what);

// A 4-byte key as upstream writes it (8 hex digits) and back; none for one that is not.
std::optional<std::string> key_from_hex(std::string_view hex);
std::string hex_of(std::string_view bytes);

}  // namespace genko::render::pcraft
