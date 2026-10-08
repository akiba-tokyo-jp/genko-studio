#include "core/color_raster.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>
#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/strokes.hpp"
#include "core/pyconv.hpp"
#include "core/ops_util.hpp"

namespace genko::core {
namespace {
std::uint32_t le(std::string_view b, std::size_t p, unsigned n) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= std::uint32_t(static_cast<unsigned char>(b[p+i])) << (i*8);
    return v;
}
void append_le(std::string& b, std::uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b.push_back(static_cast<char>((v >> (i*8)) & 255));
}
[[noreturn]] void invalid() { throw Error("format", "invalid or oversized high-precision color raster"); }
std::uint32_t dimension(const Json& o, const char* key) {
    if (!o.contains(key) || !o[key].is_number_integer()) invalid();
    const double v = o[key].get<double>();
    if (v < 1 || v > static_cast<double>(kColorRasterMaxPixels)) invalid();
    return static_cast<std::uint32_t>(v);
}
std::size_t pixels(std::uint32_t w, std::uint32_t h) {
    if (!w || !h || w > kColorRasterMaxPixels || h > kColorRasterMaxPixels / w) invalid();
    return std::size_t(w)*h;
}
}
ColorRasterView::ColorRasterView(std::string_view bytes) : bytes_(bytes) {
    if (bytes.size() < 16 || bytes.substr(0, 4) != "GKCR" ||
        static_cast<unsigned char>(bytes[4]) != 1 || (bytes[5] != 2 && bytes[5] != 4) ||
        bytes[6] != 0 || bytes[7] != 0) invalid();
    width_ = le(bytes, 8, 4);
    height_ = le(bytes, 12, 4);
    sample_bytes_ = static_cast<unsigned char>(bytes[5]);
    const auto n = pixels(width_, height_)*4;
    if (bytes.size() != 16 + n*sample_bytes_) invalid();
    if (sample_bytes_ == 4) {
        for (std::size_t i = 0; i < n; ++i) {
            const float v = std::bit_cast<float>(le(bytes_, 16+i*4, 4));
            if (!std::isfinite(v) || (i%4 == 3 && !(0 <= v && v <= 1))) invalid();
        }
    }
}
std::array<double, 4> ColorRasterView::pixel(std::size_t i) const {
    if (i >= std::size_t(width_)*height_) invalid();
    std::array<double, 4> out{};
    for (unsigned c = 0; c < 4; ++c) {
        const auto bits = le(bytes_, 16+(i*4+c)*sample_bytes_, sample_bytes_);
        out[c] = sample_bytes_ == 2 ? bits/65535.0 : static_cast<double>(std::bit_cast<float>(bits));
    }
    return out;
}
Json ColorRasterView::metadata(std::string_view asset) const {
    return Json{{"asset", std::string(asset)}, {"width", width_}, {"height", height_},
                {"precision", sample_bytes_ == 2 ? "u16" : "f32"}, {"space", "srgb"}, {"alpha", "straight"}};
}
void ColorRasterView::check_metadata(const Json& m) const {
    if (!m.is_object() || !m.contains("asset") || !m["asset"].is_string()) invalid();
    const auto expected = metadata(m["asset"].get<std::string>());
    if (m.size() != expected.size()) invalid();
    // Canonical journal snapshots reorder object keys, not the metadata values.
    for (const auto& [key, value] : expected.items())
        if (!m.contains(key) || m[key] != value) invalid();
}
ColorRasterEdit::ColorRasterEdit(std::string bytes)
    : bytes_(std::move(bytes)), view_(bytes_), sample_bytes_(static_cast<unsigned char>(bytes_[5])) {}
void ColorRasterEdit::put(std::size_t i, unsigned c, double v) {
    if (i >= std::size_t(width())*height()) invalid();
    if (!std::isfinite(v) || (c == 3 && !(-1e-12 <= v && v <= 1 + 1e-12))) invalid();
    std::uint32_t bits = 0;
    if (sample_bytes_ == 2) {
        if (v < -1e-12 || v > 1 + 1e-12) invalid();
        bits = static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
    } else {
        if (std::abs(v) > std::numeric_limits<float>::max()) invalid();
        bits = std::bit_cast<std::uint32_t>(static_cast<float>(c == 3 ? std::clamp(v, 0.0, 1.0) : v));
    }
    // (in place: the view reads these same bytes)
    char* at = bytes_.data() + 16 + (i*4 + c)*sample_bytes_;
    for (unsigned k = 0; k < sample_bytes_; ++k) at[k] = static_cast<char>((bits >> (k*8)) & 255);
}
void ColorRasterEdit::set(std::size_t i, const std::array<double, 4>& rgba) {
    for (unsigned c = 0; c < 4; ++c) put(i, c, rgba[c]);
}
void ColorRasterEdit::set_alpha(std::size_t i, double alpha) { put(i, 3, alpha); }
std::string ColorRasterEdit::take() && { return std::move(bytes_); }
std::string encode_color_raster(const Json& op) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if (!op.is_object() || !op.contains("precision") || (op["precision"] != "u16" && op["precision"] != "f32")) invalid();
    const unsigned sample_bytes = op["precision"] == "u16" ? 2 : 4;
    const auto w = dimension(op, "width"), h = dimension(op, "height");
    const auto n = pixels(w, h)*4;
    if (!op.contains("pixels") || !op["pixels"].is_array() || op["pixels"].size() != n) invalid();
    std::string out("GKCR\1\2\0\0", 8);
    out[5] = static_cast<char>(sample_bytes);
    out.reserve(16 + n*sample_bytes);
    append_le(out, w, 4); append_le(out, h, 4);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& sample = op["pixels"][i];
        if (!sample.is_number()) invalid();
        const double v = sample.get<double>();
        if (sample_bytes == 2) {
            if (!sample.is_number_integer() || v < 0 || v > 65535) invalid();
            append_le(out, static_cast<std::uint32_t>(v), 2);
        } else {
            if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max() ||
                (i%4 == 3 && !(0 <= v && v <= 1))) invalid();
            append_le(out, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
        }
    }
    return out;
}
std::string encode_color_pixels(std::uint32_t width, std::uint32_t height, std::string_view precision,
                               const std::function<std::array<double, 4>(std::size_t)>& pixel) {
    if (precision != "u16" && precision != "f32") invalid();
    const unsigned sample_bytes = precision == "u16" ? 2 : 4;
    const auto n = pixels(width, height);
    std::string out("GKCR\1\2\0\0", 8);
    out[5] = static_cast<char>(sample_bytes);
    out.reserve(16 + n * 4 * sample_bytes);
    append_le(out, width, 4); append_le(out, height, 4);
    for (std::size_t i = 0; i < n; ++i) {
        const auto rgba = pixel(i);
        for (unsigned c = 0; c < 4; ++c) {
            const double v = rgba[c];
            if (!std::isfinite(v) || (c == 3 && !(0 <= v && v <= 1))) invalid();
            if (sample_bytes == 2) {
                // A u16 result explicitly quantizes in its original sample format.
                if (v < -1e-12 || v > 1 + 1e-12) invalid();
                append_le(out, static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535)), 2);
            } else {
                if (std::abs(v) > std::numeric_limits<float>::max()) invalid();
                append_le(out, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
            }
        }
    }
    return out;
}
void validate_color_document(const Document& doc) {
    std::size_t bytes = 0;
    std::unordered_set<const std::string*> allocations;
    for (const auto& page : doc.pages) {
        const bool color = std::any_of(page->layers.begin(), page->layers.end(), [](const Layer& l) { return bool(l.color_raster) || has_color_strokes(l); });
        for (const auto& layer : page->layers) {
            if (has_color_strokes(layer)) {
                for (const auto& stroke : layer.strokes->items) if (stroke->color_rgb) validate_stroke_color(*stroke->color_rgb);
                if (layer.kind != LayerKind::Strokes || layer.raster_png || !layer.patches.empty() || layer.mask ||
                    layer.effect || layer.screen || layer.color || layer.panel_each)
                    throw Error("not_yet_ported", "precise stroke layer style is not supported yet");
            }
            if (layer.color_raster) {
                // Charge each live immutable allocation once, not each layer reference.
                if (allocations.insert(layer.color_raster.get()).second) {
                    if (layer.color_raster->size() > kColorRasterBookBytes - bytes)
                        throw Error("value", "high-precision color raster book budget exceeded");
                    bytes += layer.color_raster->size();
                    (void)ColorRasterView(*layer.color_raster);
                }
                if (layer.kind != LayerKind::Raster || layer.role == LayerRole::Tone || layer.raster_png || layer.stroke_count() || !layer.patches.empty() ||
                    layer.panel_clip || layer.effect || layer.screen || layer.color)
                    throw Error("not_yet_ported", "high-precision color layer style is not supported yet");
            }
            if (layer.kind == LayerKind::Adjust && layer.adjust &&
                layer.adjust->value("kind", Json()) == "exposure") (void)Exposure::parse(*layer.adjust);
            if (!color || !layer.visible) continue;
            (void)blend_mode(Json(layer.blend));
            if ((layer.kind == LayerKind::Adjust && layer.blend != "normal") || py_truthy(layer.parent_id) || layer.kind == LayerKind::Tone || layer.role == LayerRole::Tone)
                throw Error("not_yet_ported", "high-precision color composition style is not supported yet");
            // (any other correction is read as the 8-bit page reads it: one it cannot read does nothing)
        }
    }
}
} // namespace genko::core
