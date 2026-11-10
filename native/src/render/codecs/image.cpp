// The codecs' picture (render/codecs/image.hpp; PhotoCraft's crates/codecs/src/image.rs).

#include "render/codecs/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/error.hpp"

namespace genko::render::codecs {

namespace {

// Rec. 709's luma weights on the encoded values (PhotoCraft's LUMA).
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};

float clamp01(float v) { return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f); }

std::optional<std::size_t> byte_length(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample) {
    const std::uint64_t pixels = std::uint64_t(width) * height;  // (below 2^64: two 32-bit numbers)
    const std::uint64_t per = std::uint64_t(channels(layout)) * sample_bytes(sample);
    if (pixels > (std::numeric_limits<std::size_t>::max() / 2) / per) return std::nullopt;
    return static_cast<std::size_t>(pixels * per);
}

std::array<float, 3> cmyk_to_rgb(float c, float m, float y, float k) { return {(1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k)}; }

std::array<float, 4> rgb_to_cmyk(float r, float g, float b) {
    const float k = 1.0f - std::max({r, g, b});
    if (k >= 1.0f) return {0, 0, 0, 1};
    const float d = 1.0f - k;
    return {(1 - r - k) / d, (1 - g - k) / d, (1 - b - k) / d, k};
}

}  // namespace

unsigned channels(Layout layout) {
    switch (layout) {
    case Layout::Gray: return 1;
    case Layout::GrayA: return 2;
    case Layout::Rgb: return 3;
    case Layout::Rgba:
    case Layout::Cmyk: return 4;
    case Layout::CmykA: return 5;
    }
    return 1;
}
unsigned colour_channels(Layout layout) { return channels(layout) - (has_alpha(layout) ? 1 : 0); }
bool has_alpha(Layout layout) { return layout == Layout::GrayA || layout == Layout::Rgba || layout == Layout::CmykA; }
bool is_gray(Layout layout) { return layout == Layout::Gray || layout == Layout::GrayA; }
bool is_rgb(Layout layout) { return layout == Layout::Rgb || layout == Layout::Rgba; }
bool is_cmyk(Layout layout) { return layout == Layout::Cmyk || layout == Layout::CmykA; }
Layout with_alpha(Layout layout) { return is_gray(layout) ? Layout::GrayA : is_rgb(layout) ? Layout::Rgba : Layout::CmykA; }
Layout without_alpha(Layout layout) { return is_gray(layout) ? Layout::Gray : is_rgb(layout) ? Layout::Rgb : Layout::Cmyk; }
bool same_model(Layout a, Layout b) { return (is_gray(a) && is_gray(b)) || (is_rgb(a) && is_rgb(b)) || (is_cmyk(a) && is_cmyk(b)); }
unsigned sample_bytes(Sample sample) { return sample == Sample::U8 ? 1 : sample == Sample::F32 ? 4 : 2; }
bool is_float(Sample sample) { return sample == Sample::F16 || sample == Sample::F32; }
int depth_of(Sample sample) { return sample == Sample::U8 ? 8 : sample == Sample::F32 ? 32 : 16; }

std::string_view to_string(Layout layout) {
    switch (layout) {
    case Layout::Gray: return "gray";
    case Layout::GrayA: return "graya";
    case Layout::Rgb: return "rgb";
    case Layout::Rgba: return "rgba";
    case Layout::Cmyk: return "cmyk";
    case Layout::CmykA: return "cmyka";
    }
    return "rgb";
}

std::string_view to_string(Sample sample) {
    switch (sample) {
    case Sample::U8: return "u8";
    case Sample::U16: return "u16";
    case Sample::F16: return "f16";
    case Sample::F32: return "f32";
    }
    return "u8";
}

std::uint16_t half_from_float(float value) {
    std::uint32_t f = 0;
    std::memcpy(&f, &value, 4);
    const std::uint32_t sign = (f >> 16) & 0x8000u;
    const std::uint32_t exponent = (f >> 23) & 0xffu;
    std::uint32_t mantissa = f & 0x7fffffu;
    if (exponent == 0xff) return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u | (mantissa >> 13) : 0u));
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 0x1f) return static_cast<std::uint16_t>(sign | 0x7c00u);  // (too large: infinity)
    if (e <= 0) {  // (a subnormal half, or zero)
        if (e < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1);
        const std::uint32_t middle = 1u << (shift - 1);
        if (rest > middle || (rest == middle && (half & 1u) != 0)) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) ++half;  // (a carry into the exponent is right)
    return static_cast<std::uint16_t>(half);
}

float float_from_half(std::uint16_t bits) {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    const std::uint32_t exponent = (bits >> 10) & 0x1fu;
    std::uint32_t mantissa = bits & 0x3ffu;
    std::uint32_t f = sign;
    if (exponent == 0) {
        if (mantissa != 0) {  // (subnormal: normalized)
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);
            f = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    } else if (exponent == 0x1f) {
        f = sign | 0x7f800000u | (mantissa << 13);
    } else {
        f = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float out = 0;
    std::memcpy(&out, &f, 4);
    return out;
}

std::string quantize(std::span<const float> values, Sample sample) {
    std::string out(values.size() * sample_bytes(sample), '\0');
    char* to = out.data();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float v = values[i];
        switch (sample) {
        case Sample::U8: to[i] = static_cast<char>(static_cast<unsigned char>(std::lround(clamp01(v) * 255.0f))); break;
        case Sample::U16: {
            const auto u = static_cast<std::uint16_t>(std::lround(clamp01(v) * 65535.0f));
            std::memcpy(to + i * 2, &u, 2);
            break;
        }
        case Sample::F16: {
            const std::uint16_t h = half_from_float(v);
            std::memcpy(to + i * 2, &h, 2);
            break;
        }
        case Sample::F32: std::memcpy(to + i * 4, &v, 4); break;
        }
    }
    return out;
}

std::vector<float> convert_layout(std::span<const float> values, Layout from, Layout to) {
    if (from == to) return {values.begin(), values.end()};
    const unsigned fc = channels(from), tc = channels(to);
    const std::size_t n = values.size() / fc;
    std::vector<float> out;
    out.reserve(n * tc);
    for (std::size_t p = 0; p < n; ++p) {
        const float* px = values.data() + p * fc;
        const float alpha = has_alpha(from) ? px[fc - 1] : 1.0f;
        if (same_model(from, to)) {
            out.insert(out.end(), px, px + colour_channels(from));
        } else {
            std::array<float, 3> rgb{};
            if (is_gray(from)) {
                rgb = {px[0], px[0], px[0]};
            } else if (is_rgb(from)) {
                rgb = {px[0], px[1], px[2]};
            } else {
                rgb = cmyk_to_rgb(px[0], px[1], px[2], px[3]);
            }
            if (is_gray(to)) {
                out.push_back(rgb[0] * kLuma[0] + rgb[1] * kLuma[1] + rgb[2] * kLuma[2]);
            } else if (is_rgb(to)) {
                out.insert(out.end(), rgb.begin(), rgb.end());
            } else {
                const auto cmyk = rgb_to_cmyk(rgb[0], rgb[1], rgb[2]);
                out.insert(out.end(), cmyk.begin(), cmyk.end());
            }
        }
        if (has_alpha(to)) out.push_back(alpha);
    }
    return out;
}

CodecImage CodecImage::blank(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample) {
    const auto length = byte_length(width, height, layout, sample);
    if (!length) throw core::Error("image_too_large", "the picture's size does not fit in this computer's memory");
    CodecImage out;
    out.width_ = width;
    out.height_ = height;
    out.layout_ = layout;
    out.sample_ = sample;
    out.data_.assign(*length, '\0');
    return out;
}

CodecImage CodecImage::from_bytes(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample, std::string data) {
    const auto length = byte_length(width, height, layout, sample);
    if (!length) throw core::Error("image_too_large", "the picture's size does not fit in this computer's memory");
    if (data.size() != *length) {
        throw core::Error("value", "the picture's bytes are " + std::to_string(data.size()) + ", not the " + std::to_string(*length) +
                                       " of " + std::to_string(width) + "×" + std::to_string(height) + " " + std::string(to_string(layout)) + " " +
                                       std::string(to_string(sample)));
    }
    CodecImage out;
    out.width_ = width;
    out.height_ = height;
    out.layout_ = layout;
    out.sample_ = sample;
    out.data_ = std::move(data);
    return out;
}

CodecImage CodecImage::from_normalized(std::uint32_t width, std::uint32_t height, Layout layout, Sample sample, std::span<const float> values) {
    return from_bytes(width, height, layout, sample, quantize(values, sample));
}

float CodecImage::value(std::size_t i) const {
    const char* p = data_.data();
    switch (sample_) {
    case Sample::U8: return static_cast<unsigned char>(p[i]) / 255.0f;
    case Sample::U16: {
        std::uint16_t v = 0;
        std::memcpy(&v, p + i * 2, 2);
        return v / 65535.0f;
    }
    case Sample::F16: {
        std::uint16_t v = 0;
        std::memcpy(&v, p + i * 2, 2);
        return float_from_half(v);
    }
    case Sample::F32: {
        float v = 0;
        std::memcpy(&v, p + i * 4, 4);
        return v;
    }
    }
    return 0;
}

float CodecImage::at(std::uint32_t x, std::uint32_t y, unsigned c) const {
    return value((std::size_t(y) * width_ + x) * channels(layout_) + c);
}

std::vector<float> CodecImage::normalized() const {
    std::vector<float> out(sample_count());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = value(i);
    return out;
}

bool CodecImage::has_translucency() const {
    if (!has_alpha(layout_)) return false;
    const unsigned ch = channels(layout_);
    for (std::size_t p = 0; p < pixel_count(); ++p) {
        if (value(p * ch + ch - 1) < 1.0f) return true;
    }
    return false;
}

bool CodecImage::has_out_of_range() const {
    if (!is_float(sample_)) return false;
    for (std::size_t i = 0; i < sample_count(); ++i) {
        const float v = value(i);
        if (!(v >= 0.0f && v <= 1.0f)) return true;
    }
    return false;
}

CodecImage CodecImage::convert(Layout layout, Sample sample) const {
    if (layout == layout_ && sample == sample_) return *this;
    CodecImage out;
    out.width_ = width_;
    out.height_ = height_;
    out.layout_ = layout;
    out.sample_ = sample;
    if (sample == sample_ && !has_alpha(layout_) && layout == with_alpha(layout_)) {
        // (an opaque picture given an alpha: its samples copied, an opaque alpha after each pixel)
        const std::size_t bps = sample_bytes(sample), from = channels(layout_) * bps;
        const std::string one = quantize(std::array<float, 1>{1.0f}, sample);
        out.data_.reserve(pixel_count() * (from + bps));
        for (std::size_t p = 0; p < pixel_count(); ++p) {
            out.data_.append(data_, p * from, from);
            out.data_ += one;
        }
    } else if (layout == layout_) {
        out.data_ = quantize(normalized(), sample);
    } else {
        out.data_ = quantize(convert_layout(normalized(), layout_, layout), sample);
    }
    if (same_model(layout, layout_)) out.icc = icc;
    out.meta = meta;
    return out;
}

CodecImage CodecImage::crop(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) const {
    if (std::uint64_t(x) + w > width_ || std::uint64_t(y) + h > height_) throw core::Error("value", "the part is not inside the picture");
    CodecImage out = blank(w, h, layout_, sample_);
    const std::size_t bpp = channels(layout_) * sample_bytes(sample_);
    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(out.data_.data() + std::size_t(row) * w * bpp, data_.data() + ((std::size_t(y) + row) * width_ + x) * bpp, std::size_t(w) * bpp);
    }
    out.icc = icc;
    out.meta = meta;
    return out;
}

}  // namespace genko::render::codecs
