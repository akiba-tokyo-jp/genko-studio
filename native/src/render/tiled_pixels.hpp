#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/command_bus.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"

// PC4 E-0 (SPEC.md COLOR-09, ARCHITECTURE.md「描く道具の土台」): the sparse working copy of a paint layer the brushes that
// change pixels work on (E5: Dodge, Burn, Sponge, Blur, Sharpen, Smudge, the Mixer Brush, the Background Eraser, Color
// Replacement; retouch::apply_dab_stroke) — the tiles of the layer's own grid a stroke reaches, kTile pixels square
// (smaller at the grid's right and bottom edges), each held as the layer's own values (image_ops::Pixels: 8 bits,
// precise, inks), read through the windowed read (image_ops::layer_pixels(…, boxes): the layer read once for them all)
// and written back the same way, only the pixels edited (each one edit() gave): the rest of the layer keeps its bytes (a
// clear pixel of an ink layer its inks too). A stroke across a 600 dpi
// B4 page holds only the tiles along it, never the page in floats; the tiles are held in the imaging budget (past it:
// too_large, nothing changed).

namespace genko::render::tiled {

inline constexpr int kTile = 256;

class TiledPixels {
public:
    // The layer `id` of the page at `at` (made ready as image_ops::layer_pixels makes it: refused when locked or not a
    // paint layer; its lines and fills drawn in when it is read), nothing of it held yet.
    TiledPixels(core::OpContext& c, std::size_t at, std::string id);
    // Tiles over a grid of that size, of `like`'s kind (its colours, precision and inks), none held: a copy to work on
    // (each tile made clear when it is reached), or a test's.
    TiledPixels(Size grid, const image_ops::Pixels& like);
    TiledPixels(const TiledPixels& other);
    TiledPixels& operator=(const TiledPixels&) = delete;
    TiledPixels(TiledPixels&&) noexcept;
    ~TiledPixels();

    Size grid() const { return grid_; }
    image_ops::Pixels::Kind kind() const;
    unsigned colours() const;
    unsigned all() const { return colours() + 1; }

    // The tiles the boxes reach (clamped to the grid) held: those not held yet read from the layer (on a copy: clear),
    // all in one read. How many were read.
    std::size_t reach(const std::vector<Box>& boxes);
    // Whether the pixel's tile is held.
    bool holds(int x, int y) const;
    // A pixel of a held tile (core::Error("value") for one that is not held): its values, colours then the alpha. edit()
    // marks it edited (its tile changed).
    const float* at(int x, int y) const;
    float* edit(int x, int y);
    // The boxes of the tiles held, and of those changed (in the order of the grid's rows).
    std::vector<Box> held() const;
    std::vector<Box> changed() const;
    // The pixels edited written back into the layer — those edit() gave, in one write (every other pixel keeps its
    // bytes) —, and the box they cover (none: empty). On a copy: core::Error("value").
    Box put(core::Document& doc) const;

private:
    struct Tile {
        Box box;
        image_ops::Pixels pixels;
        Box edited;  // (the box of its pixels edited; empty: none)
        std::vector<std::uint8_t> marks;  // (one a pixel of the tile, row by row: 1 edited; empty until the first)
    };
    std::int64_t key(int tx, int ty) const { return std::int64_t(ty) * columns_ + tx; }
    const Tile* tile_at(int x, int y) const;

    core::OpContext* context_ = nullptr;
    std::size_t at_ = 0;
    std::string id_;
    Size grid_;
    int columns_ = 0;
    std::unique_ptr<image_ops::Pixels> like_;  // (a pixel of the layer's kind)
    std::unordered_map<std::int64_t, Tile> tiles_;
};

// The boxes on a grid that `boxes` reach (each grown by `margin` pixels), as kTile tiles: what TiledPixels::reach reads.
std::vector<Box> tiles_reached(Size grid, const std::vector<Box>& boxes, int margin = 0);

}  // namespace genko::render::tiled
