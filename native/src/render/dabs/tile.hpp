#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// PhotoCraft v0.2.0 crates/paint/src/tile.rs (MIT: native/third_party/photocraft/NOTICE.md): grey bitmaps for sampled
// tips and texture patterns (16-bit values, 1 = full), their mip pyramid and a tiling pattern sampled with wrap-around.
// A book keeps them as grey PNGs (brushes@1: Document::brush_tips); the upstream JSON form (base64 of the
// little-endian samples) is read for define_brush's "format": "photocraft".

namespace genko::render::dabs {

struct GrayTile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> data;  // row by row

    static GrayTile from_fn(std::uint32_t width, std::uint32_t height, const std::function<float(std::uint32_t, std::uint32_t)>& f);
    float get(std::uint32_t x, std::uint32_t y) const { return static_cast<float>(data[std::size_t(y) * width + x]) / 65535.0f; }
    std::vector<float> to_f32() const;
    bool valid() const { return width > 0 && height > 0 && data.size() == std::size_t(width) * height; }
    friend bool operator==(const GrayTile&, const GrayTile&) = default;
};

// 0..1 to 16 bits (rounded half up, clamped).
std::uint16_t q16(float v);

// A grey PNG of 8 or 16 bits (what the book keeps) as a tile; core::Error("value") for any other picture.
GrayTile tile_of_png(std::string_view png);
// The tile as a 16-bit grey PNG (an 8-bit one when every value is a whole step of 257: the same picture, smaller).
std::string png_of_tile(const GrayTile& tile);
// Upstream's JSON form {"width", "height", "data": base64 of little-endian u16 | [0..1 floats]}; core::Error("value")
// for one that does not hold width × height samples.
GrayTile tile_of_json_form(std::uint32_t width, std::uint32_t height, const std::string* base64, const std::vector<double>* floats);

// A float mip pyramid of a tile, for sampling it at any scale without aliasing.
struct Mips {
    struct Level {
        std::size_t width = 0, height = 0;
        std::vector<float> values;
    };
    std::vector<Level> levels;  // (level 0 is the tile itself)

    explicit Mips(const GrayTile& tile);
    // The level for drawing the tile `px` pixels across its larger side (whole halvings: no library log).
    std::size_t level_for(float px) const;
    // Bilinear at u, v in 0..1 of the level (0 outside).
    float sample_clamped(std::size_t level, float u, float v) const;
};

// A tiling float pattern (a texture), sampled with wrap-around.
struct PatternImage {
    std::size_t width = 0, height = 0;
    std::vector<float> data;
    float sample_wrap(double x, double y) const;
};

}  // namespace genko::render::dabs
