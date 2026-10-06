#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "core/json.hpp"
#include "core/model.hpp"

namespace genko::core {
inline constexpr std::string_view kColorRasterFeature = "native.color_raster_v1";
inline constexpr std::string_view kColorRasterSuffix = ".colorrgba";
inline constexpr std::size_t kColorRasterMaxPixels = 4 * 1024 * 1024;
inline constexpr std::size_t kColorRasterBookBytes = 256 * 1024 * 1024;
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
void validate_color_document(const Document& doc);
} // namespace genko::core
