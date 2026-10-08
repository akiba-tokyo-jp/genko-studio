#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "core/json.hpp"
#include "core/model.hpp"

namespace genko::core {
inline constexpr std::string_view kColorRasterFeature = "native.color_raster_v1";
inline constexpr std::string_view kColorRasterSuffix = ".colorrgba";
// A book's precise rasters saved in tiles (each tile its own content-addressed asset, so a change to part of a
// raster adds only the tiles it changed): metadata {"tiles": [refs, row by row], "tile": side, width, height,
// precision, space, alpha}. A raster saved whole (metadata {"asset": ref, …}) is still read.
inline constexpr std::string_view kColorTilesFeature = "native.color_raster_tiles_v1";
inline constexpr std::uint32_t kColorTile = 256;
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
// A raster's samples changed in place (the eraser, the selection): a copy of its bytes, read and written in its own
// precision. Pixels never set keep their bytes; a u16 sample is rounded to 16 bits, an f32 one kept to 32.
class ColorRasterEdit {
public:
    explicit ColorRasterEdit(std::string bytes);
    ColorRasterEdit(const ColorRasterEdit&) = delete;
    ColorRasterEdit& operator=(const ColorRasterEdit&) = delete;
    std::uint32_t width() const { return view_.width(); }
    std::uint32_t height() const { return view_.height(); }
    bool floating() const { return sample_bytes_ == 4; }
    std::array<double, 4> pixel(std::size_t i) const { return view_.pixel(i); }
    // Straight sRGB RGBA; a u16 raster takes 0..1, an f32 one any finite colour and an alpha in 0..1.
    void set(std::size_t i, const std::array<double, 4>& rgba);
    void set_alpha(std::size_t i, double alpha);
    std::string take() &&;
private:
    void put(std::size_t i, unsigned c, double v);
    std::string bytes_;
    ColorRasterView view_;
    unsigned sample_bytes_;
};
std::string encode_color_raster(const Json& op);
// The raster in tiles of kColorTile pixels square (smaller at its right and bottom edges), row by row, each a raster
// of its own (GKCR, the same precision).
std::vector<std::string> color_tiles(std::string_view raster);
// The tiles put back together: core::Error("format") unless they are the tiles of a raster of that size and
// precision. `tile(i)` gives the i-th tile's bytes.
std::string join_color_tiles(std::uint32_t width, std::uint32_t height, std::string_view precision,
                             const std::function<std::string(std::size_t)>& tile);
// The metadata of a raster saved in tiles (the refs as given), and its check against the raster read.
Json tiled_metadata(const ColorRasterView& raster, const std::vector<std::string>& refs);
void check_tiled_metadata(const ColorRasterView& raster, const Json& metadata);
// Serializes computed straight sRGB samples without an RGBA8 intermediate or a large JSON pixel array.
std::string encode_color_pixels(std::uint32_t width, std::uint32_t height, std::string_view precision,
                               const std::function<std::array<double, 4>(std::size_t)>& pixel);
void validate_color_document(const Document& doc);
} // namespace genko::core
