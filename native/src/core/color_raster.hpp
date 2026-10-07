#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "core/json.hpp"
#include "core/model.hpp"

namespace genko::core {
inline constexpr std::string_view kColorRasterFeature = "native.color_raster_v1";
inline constexpr std::string_view kColorRasterSuffix = ".colorrgba";
inline constexpr std::size_t kColorRasterBookBytes = 256 * 1024 * 1024;
// Use the existing book byte ceiling (worst-case RGBA f32 plus GKCR header),
// rather than a prototype-only 4M limit. Book/work/allocator budgets stay intact.
inline constexpr std::size_t kColorRasterMaxPixels = (kColorRasterBookBytes - 16) / (4 * 4);
// Immutable, straight-alpha sRGB RGBA. Integers retain every one of their 16 bits.
// GKCR, version=1, sample bytes=2, reserved=0, LE width/height, then LE RGBA samples.
class ColorRasterView {
public:
    explicit ColorRasterView(std::string_view bytes);
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::array<double, 4> pixel(std::size_t i) const;
    Json metadata(std::string_view asset) const;
    void check_metadata(const Json& metadata) const;
private:
    std::string_view bytes_;
    std::uint32_t width_ = 0, height_ = 0;
    unsigned sample_bytes_ = 2;
};
std::string encode_color_raster(const Json& op);
// Serializes computed straight sRGB samples without an RGBA8 intermediate or a large JSON pixel array.
std::string encode_color_pixels(std::uint32_t width, std::uint32_t height, std::string_view precision,
                               const std::function<std::array<double, 4>(std::size_t)>& pixel);
void validate_color_document(const Document& doc);
} // namespace genko::core
