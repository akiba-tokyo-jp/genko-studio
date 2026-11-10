#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/brush_dabs.hpp"
#include "render/dabs/dynamics.hpp"
#include "render/dabs/tile.hpp"
#include "render/image.hpp"

// PhotoCraft v0.2.0 crates/paint/src/render.rs (MIT: native/third_party/photocraft/NOTICE.md): a line's dabs laid at a
// resolution. Each dab is rasterised (its tip: computed round, or a sampled picture through its mips; the noise, a
// texture on each tip, the wet edges, its flow) into a sparse buffer of 64-pixel tiles, where the dabs build up with
// their flow towards each one's opacity ceiling (c ← c + v·(ceiling − c); the wet edges take the largest instead), with
// their colours when the colour dynamics vary them; the line's masks (the dual brush, a texture on the whole line) are
// applied when the coverage is read. That coverage is the line's, before its opacity (Photoshop's 不透明度 and 流量).
//
// Upstream's pixels: a texture pixel is the texture's scale / 300 inch on the page (as a paper's), the noise's grain is
// 1/300 inch, so the screen and the print show the same texture; the anti-aliased edge and the tips are pixels of the
// resolution drawn.

namespace genko::render::dabs {

// A grey mask (texture, dual tip) laid on the coverage v at depth d (every mode keeps v = 0 at 0).
float mask_combine(core::DabMaskMode mode, float v, float t, float d);

// The coverage of a round dab at a distance from its centre (pixels: an anti-aliased edge and the hardness's fall).
float dab_coverage(float d, float radius, float hardness);

// A brush ready to lay dabs at a resolution: its settings with the tips' mips and the texture prepared (brightness,
// contrast, invert).
class BrushContext {
public:
    BrushContext(const core::DabSettings& settings, std::shared_ptr<const Mips> tip, std::shared_ptr<const Mips> dual_tip,
                 std::shared_ptr<const PatternImage> texture, int dpi);

    const core::DabSettings& settings() const { return settings_; }
    int dpi() const { return dpi_; }
    bool textured() const { return texture_ != nullptr; }
    // The box a dab can touch (pixels at dpi; a sampled tip turned needs its corners).
    Box dab_rect(const Dab& d, bool dual) const;
    // The dab over `rect` into `out` (rect's pixels, row by row): its tip × noise × the texture on each tip × the wet
    // edges × its flow.
    void rasterize(const Dab& d, bool dual, const Box& rect, std::vector<float>& out) const;
    // The texture at a pixel (1 without one).
    float texture_at(int x, int y) const;
    // The line's masks on an accumulated coverage at a pixel.
    float stroke_mask(float c, std::optional<float> dual, int x, int y) const;

private:
    core::DabSettings settings_;
    std::shared_ptr<const Mips> tip_, dual_tip_;
    std::shared_ptr<const PatternImage> texture_;
    int dpi_;
    double px_per_mm_;
    double texel_;  // (texture pixels per page pixel)
};

// The texture's picture with its brightness, contrast and invert (render.rs prepare_pattern); none when it is off.
std::shared_ptr<const PatternImage> prepare_pattern(const core::DabTexture& texture, const PatternImage& picture);

// A sparse coverage buffer of 64-pixel tiles, with a straight colour premultiplied by the coverage when asked.
class CoverageMap {
public:
    static constexpr int kTile = 64;
    explicit CoverageMap(bool colour = false) : colour_(colour) {}
    // The values of a dab over `rect` built up towards `ceiling` (or the largest, `use_max`), with its colour.
    void accumulate(const Box& rect, const std::vector<float>& values, float ceiling, bool use_max, const std::array<float, 4>* colour);
    float get(int x, int y) const;
    // The colour laid at a pixel (straight; black where nothing is).
    std::array<float, 3> colour_at(int x, int y) const;
    bool coloured() const { return colour_; }
    bool empty() const { return tiles_.empty(); }
    // The tiles' boxes (pixels), in order.
    std::vector<Box> tile_boxes() const;

    struct Tile {
        int tx = 0, ty = 0;
        std::vector<float> cov;
        std::vector<float> col;  // (3 a pixel, premultiplied)
    };
    const std::unordered_map<std::int64_t, Tile>& tiles() const { return tiles_; }

private:
    static std::int64_t key(int tx, int ty) { return (static_cast<std::int64_t>(ty) << 32) ^ static_cast<std::uint32_t>(tx); }
    const Tile* find(int x, int y, int& index) const;
    bool colour_;
    std::unordered_map<std::int64_t, Tile> tiles_;
};

// A line's dabs laid at a resolution, only over `within` (pixels; the dabs past it are skipped: a pixel's coverage
// is only its own dabs', so a part is drawn as the whole would draw it there).
struct LineCover {
    std::shared_ptr<const BrushContext> ctx;
    CoverageMap cov;
    std::optional<CoverageMap> dual;
    Box box;  // where it can be other than 0 (inside `within`); empty: nothing
    float opacity = 1.0f;  // the brush's opacity, the ceiling of the whole line

    // The line's coverage at a pixel, its masks and opacity applied (0..1).
    float at(int x, int y) const;
    bool coloured() const { return cov.coloured(); }
    std::array<float, 3> colour(int x, int y) const { return cov.colour_at(x, y); }
    // The coverage (as at()) of every pixel of `area`, row by row; and the colours (straight; black where nothing is).
    void fill(const Box& area, std::vector<float>& out) const;
    void fill_colours(const Box& area, std::vector<std::array<float, 3>>& out) const;
};

// Lay `dabs` (and the dual brush's) at the context's resolution over `within`. `coloured`: keep each dab's colour (the
// colour dynamics). core::Error("too_large") past kMaxDabs dabs.
std::shared_ptr<const LineCover> lay(std::shared_ptr<const BrushContext> ctx, const std::vector<Dab>& dabs, const std::vector<Dab>& dual,
                                     const Box& within, bool coloured);

}  // namespace genko::render::dabs
