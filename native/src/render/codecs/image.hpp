#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The codecs' picture (PC4 F-0, SPEC.md FILE-01; PhotoCraft v0.2.0 crates/codecs/src/image.rs, MIT): one flat raster of
// any depth — its samples interleaved (RGBARGBA…), rows from the top, in this machine's byte order; integer samples
// unsigned and normalized to 0..max, float samples nominally 0..1 but any value (HDR) —, with the ICC profile and the
// metadata (EXIF, XMP, resolution, text) its file carried. What every format is read into and written from
// (render/codecs/codecs.hpp), and what a page's or a layer's picture is taken out as at its depth (formats/page_picture).
// Not Pillow's Image (render/image.hpp): that one stays the 8-bit picture of the drawing.

namespace genko::render::codecs {

// The channels: an alpha, when there is one, is the last and straight (not premultiplied). CMYK samples are ink amounts
// (0: no ink, max: full ink; not Adobe's inverted ones).
enum class Layout { Gray, GrayA, Rgb, Rgba, Cmyk, CmykA };
// A sample: 8 or 16 bits unsigned, a half float (stored as its 16 bits) or a float.
enum class Sample { U8, U16, F16, F32 };

inline constexpr std::array<Layout, 6> kLayouts{Layout::Gray, Layout::GrayA, Layout::Rgb, Layout::Rgba, Layout::Cmyk, Layout::CmykA};
inline constexpr std::array<Sample, 4> kSamples{Sample::U8, Sample::U16, Sample::F16, Sample::F32};

unsigned channels(Layout layout);         // with the alpha
unsigned colour_channels(Layout layout);  // without it
bool has_alpha(Layout layout);
bool is_gray(Layout layout);
bool is_rgb(Layout layout);
bool is_cmyk(Layout layout);
Layout with_alpha(Layout layout);
Layout without_alpha(Layout layout);
// The same colours (grey, RGB or CMYK), with or without an alpha.
bool same_model(Layout a, Layout b);
unsigned sample_bytes(Sample sample);
bool is_float(Sample sample);
std::string_view to_string(Layout layout);  // gray, graya, rgb, rgba, cmyk, cmyka
std::string_view to_string(Sample sample);  // u8, u16, f16, f32
// The bits a sample holds: 8, 16 (U16 and F16), 32.
int depth_of(Sample sample);

// IEEE 754 half floats (round to nearest even; infinities and NaN kept).
std::uint16_t half_from_float(float value);
float float_from_half(std::uint16_t bits);

// What a file says besides its pixels.
struct Metadata {
    std::optional<std::string> exif;  // TIFF structure (II*\0 / MM\0*), without JPEG's "Exif\0\0"
    std::optional<std::string> xmp;   // the packet, UTF-8
    std::optional<std::pair<double, double>> dpi;
    std::vector<std::pair<std::string, std::string>> text;  // PNG's tEXt and iTXt, TIFF's ASCII tags

    bool empty() const { return !exif && !xmp && !dpi && text.empty(); }
    bool operator==(const Metadata&) const = default;
};

class CodecImage {
public:
    CodecImage() = default;

    // A picture of zeros. core::Error("image_too_large") when its bytes cannot be counted in this machine's sizes.
    static CodecImage blank(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample);
    // These bytes (interleaved, this machine's order): core::Error("value") unless they are exactly the picture's.
    static CodecImage from_bytes(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample, std::string data);
    // From normalized floats (0..1 for integer samples: clamped and rounded; NaN as 0), one per sample.
    static CodecImage from_normalized(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample, std::span<const float> values);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    Layout layout() const { return layout_; }
    Sample sample() const { return sample_; }
    bool empty() const { return width_ == 0 || height_ == 0; }
    std::size_t pixel_count() const { return std::size_t(width_) * height_; }
    std::size_t sample_count() const { return pixel_count() * channels(layout_); }
    const std::string& data() const { return data_; }
    std::string& data() { return data_; }

    // Sample i (counted in samples) normalized: integers to 0..1, floats as they are.
    float value(std::size_t i) const;
    // Channel c of pixel (x, y), normalized.
    float at(std::uint32_t x, std::uint32_t y, unsigned c) const;
    // Every sample normalized.
    std::vector<float> normalized() const;
    // Has an alpha and a pixel that is not wholly opaque.
    bool has_translucency() const;
    // Float samples and one of them outside 0..1 (or NaN).
    bool has_out_of_range() const;

    // To another layout or sample (the same colours: integers rescaled, floats clamped to 0..1 for integers and rounded;
    // another colour model by plain formulas — grey ↔ RGB by Rec. 709's luma, CMYK ↔ RGB without colour management: a
    // colour-managed conversion is Little CMS's, the caller's). The ICC profile is kept only for the same colour model,
    // the metadata always.
    CodecImage convert(Layout layout, Sample sample) const;

    // The part `x, y, w, h` (inside the picture) as a picture of its own, with this one's profile and metadata.
    CodecImage crop(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) const;

    std::optional<std::string> icc;
    Metadata meta;

    bool operator==(const CodecImage&) const = default;

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Layout layout_ = Layout::Rgb;
    Sample sample_ = Sample::U8;
    std::string data_;
};

// Normalized samples as stored ones (clamped and rounded for integers).
std::string quantize(std::span<const float> values, Sample sample);
// Normalized interleaved samples from one layout to another (CodecImage::convert's formulas).
std::vector<float> convert_layout(std::span<const float> values, Layout from, Layout to);

}  // namespace genko::render::codecs
