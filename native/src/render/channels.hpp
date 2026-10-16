#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// channels@1 (SPEC.md COLOR-03, docs/cpp-migration/schema-v4.md「チャンネル」): a page's alpha and spot channels as the
// drawing build works them — their planes, the spots printed over the page as it shows, the channel panel's view (the
// colour planes shown, the alpha channels laid over), and the pixel ops working a channel or one colour plane instead of
// a layer's colour (the ops' "channel": render/ops_registry.cpp wraps every pixel op with these).

namespace genko::render {
struct RenderOptions;
struct RenderResult;
}  // namespace genko::render

namespace genko::render::channels {

// A channel's pixels: its density (0..1) at each pixel, rows from the top; the plane lies stretched over the page.
struct Plane {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;
};

// A plane of zeros over the page at `dpi` (its paper's size in pixels there).
Plane blank(const core::Page& page, int dpi);
// The channel's plane (a channel without a raster: zeros at its kind's dpi, core::channel_dpi).
Plane plane_of(const core::Page& page, const core::Channel& channel);
// The plane at another size (bilinear by the pixels' centres, the edges carried on).
Plane resized(const Plane& plane, std::uint32_t width, std::uint32_t height);
// Its value where the page's point (fx, fy) is (each 0..1 across the page), bilinear as resized().
float sample(const Plane& plane, double fx, double fy);
// A channel's density where the page's point (fx, fy) is (bilinear as sample()), read from its raster decoded once while
// it is drawn (the drawing's cache, by the raster's bytes: the screen's tiles and the exports read the same channel); 0
// for a channel without a raster.
// A raster that cannot be read (the book is read-only then, its problem reported where it was read) reads 0. The decoded
// rasters are held at their own depth, a few at a time within a budget.
class Density {
public:
    explicit Density(const core::Channel& channel);
    float at(double fx, double fy) const;

private:
    std::shared_ptr<const void> pixels_;
};
// The channel as an "L" picture of `size` over the page (255 × its density at each pixel's centre).
Image mask_of(const core::Channel& channel, Size size);
// The channel made small (the panel's picture): an "L" picture of `size`, each pixel the mean of the raster's pixels it
// covers, read row by row (not decoded whole, not kept). 0 for a channel without a raster or one that cannot be read.
Image thumbnail(const core::Channel& channel, Size size);

// The plane as the channel's GKIR Mask raster at the depth (8 and 16 rounded, 32 as floats), or null when it is all 0.
core::Bytes encode(const Plane& plane, int depth);
// The plane as an "L" picture of `size` (255 × density, sampled at each pixel's centre): the page at that size.
Image mask_image(const Plane& plane, Size size);
// An "L" picture's values as a plane (value / 255).
Plane from_mask(const Image& mask);

// set_page_spec: a channel's raster (over the old paper) moved as the page's layers move — the old basic frame onto the
// new, as pagespec._Map moves a point — over the new paper at the resolution it had, bilinear, its edge carried on.
core::Bytes relayout(const core::Bytes& raster, const core::PageSpec& old_paper, const core::PageSpec& paper, const core::Rect& old_frame,
                     const core::Rect& new_frame);
// Each page's channels at its depth (after an op changed it: set_color_mode, convert_profile): 8 → 16 → 32 exact, down
// rounded.
void follow_depth(core::Document& doc);

// A colour plane of the page as it shows (proof, the spots not printed), at 200 dpi: its values 0..1 as the panel shows a
// plate alone (RGB its value; CMYK 1 − its ink; Lab L/100, (a + 128)/255, (b + 128)/255; grey its lightness) — or, for
// `plate` none, the page's lightness (0.299 R + 0.587 G + 0.114 B).
Plane page_plane(const core::Page& page, const core::Document* episode, std::optional<std::size_t> plate);

// The spot channels printed over the picture of the page as it shows (`image`: "RGB" or "RGBA", the part `area` of a page
// of `size` pixels): each spot's density v in its ink c (sRGB) and solidity s takes a colour p to
// (1 − s·v)·p·(1 − v + v·c) + s·v·c (PhotoCraft's compose/multichannel.rs). `shown`: the spot channels printed (their
// ids; none: every one).
void print_spots(Image& image, const core::Page& page, const Box& area, Size size, const std::optional<std::vector<std::string>>& shown);

// The channel panel's view of the page (RenderOptions::planes, overlays, spots) laid on what was drawn: the colour planes
// hidden left out (one plane alone shown in grey; none: the first alpha channel shown, in grey, else white), the spots
// printed, the alpha channels shown laid over in their colours. Nothing for a page without channels and with every plane
// shown. (The same pixels for a part of the page as for the whole.)
void finish(RenderResult& result, const core::Page& page, int dpi, const RenderOptions& options);

// --- the pixel ops' "channel" ---------------------------------------------------------------------------------------

// What an op's "channel" names on its page: a channel's index (an alpha or spot channel, by its id or name), a colour
// plate's (red, cyan, … — the page's), or the composite ("composite", or the page's one plate on a grey page): nothing to
// do. core::OpError for anything else.
struct Target {
    std::optional<std::size_t> channel;
    std::optional<std::size_t> plate;
};
std::optional<Target> target_of(const core::Document& doc, const core::Json& op);

// An op working a channel: the channel lent as a hidden paint layer of its own — an opaque grey picture of its density
// over the page at the paint layers' 200 dpi (raster::kWorkingDpi: every pixel op draws there, whatever the channel's own
// resolution; a larger channel lent as the mean of the pixels each covers), outside the panels —, the op's layer
// pointing at it. Taken back after the op: the pixels the op changed (their grey, over black where it made them clear, by
// a grey page's rules: Genko's blacks none, white full) put back into the channel at its own resolution, each of its
// pixels by the share of it they cover (bilinear when the channel is finer); a pixel of the channel no changed pixel
// reaches keeps its value, and a channel the op left as it was its very bytes. The changed pixels come back at 8 bits
// (a channel of 16 or 32 bits: precision() says so).
class ChannelLending {
public:
    ChannelLending(core::Document& doc, std::size_t page, std::size_t channel, core::Json& op, std::string_view name);
    // Whether the op changed the channel.
    bool take_back(core::Document& doc) const;
    // The channel's depth (8, 16, 32): above 8, what the op changed was drawn at 8 bits.
    int depth() const { return depth_; }

private:
    std::string page_id_;
    std::string channel_id_;
    std::string layer_id_;
    std::string lent_;  // (the RGBA bytes lent, at 200 dpi)
    Size lent_size_;
    Size own_size_;     // (the channel's raster's, or the size it is made at)
    int depth_ = 8;
};

// An op working one colour plate of its layer: after it, the layer's other plates and its alpha put back as they were
// (where the op changed a pixel). A pen layer has no plates: core::OpError.
class PlateKeep {
public:
    PlateKeep(const core::Document& doc, std::size_t page, std::size_t plate, const core::Json& op, std::string_view name);
    void restore(core::Document& doc) const;

private:
    std::string page_id_;
    std::string layer_id_;
    std::size_t plate_ = 0;
    std::optional<core::Layer> before_;
};

}  // namespace genko::render::channels
