#pragma once

#include <array>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/warp.hpp"

// masks@1 (PC3 節 B, SPEC.md COLOR-06): a layer mask as the drawing lays it on its layer — the one function the three
// ways (8 bits, precise, inks) and the merges take it through — and a mask lent to a pixel op as a layer to paint
// ("layer_mask": true, render/ops_registry).

namespace genko::render::layer_masks {

// The mask as it shows over `area` of the page drawn at `size`: its picture (`decoded`, "L") stretched over the page by
// `resample`, feathered (a Gaussian of σ = feather_mm / 2 on the page, worked over the area widened by 3σ so its edge is
// right) and its density laid on (1 − density·(1 − m)). A mask of no density or feather: decoded.resize_region(size,
// area, resample), as before them.
Image shown(const core::Page& page, const core::Mask& mask, const Image& decoded, Size size, const Box& area, Resample resample);

// --- the grid and the moves (B2) ----------------------------------------------------------------------------------------

// The grid a page's layer masks are made at (set_layer_mask, paint_mask, mask_from_transparency): 150 dpi on an 8-bit RGB
// page, as every mask before; on a precise (16, 32 bits) or ink page the finest grid of its pixel layers — the working
// 200 dpi at least, 600 dpi at most (the coordinator's 8-2 (b)) —, and so on an 8-bit page whose paper says grid
// "finest" (image-grid@1, PC4 F-b: an opened picture, a new image), its 8-bit pixel layers counted too. A mask made before
// keeps its own picture: it is stretched over the page whatever its size.
int grid_dpi(const core::Page& page);
// The page in pixels at that grid.
Size grid_size(const core::Page& page);

// A linked mask moved with what its layer's pixels in `area` do (transform_area's "mask": "follow"; 節 C's moves of a
// layer): its part the area covers (by the area's coverage) lifted and laid again through `m` (mm, bilinear) over what
// stays; the part it left takes the mask's background — the value most of the mask's edge pixels hold (white for a mask
// that reveals all around, black for one that hides). Its picture's size and its settings are kept.
core::Mask move_mask(const core::Page& page, const core::Mask& mask, const core::Json& area, const std::array<double, 6>& m);
// The same through a free transform (warp.mapping of the area's box), as transform_area's "warp" moves the pixels.
core::Mask warp_mask(const core::Page& page, const core::Mask& mask, const core::Json& area, const warp::Go& go);

// --- "layer_mask": true on a pixel op (B-0) -----------------------------------------------------------------------------

// The pixel ops that may paint a layer's mask instead of its pixels: the channel ops' (core::channel_target_op).
bool lends_to(std::string_view op);

// The op's layer's mask lent to it as a hidden paint layer of its own — an opaque grey picture of the mask at the paint
// layers' 200 dpi (a layer without a mask: one that reveals all first, as Photoshop makes it, at the page's grid_size) —,
// the op's layer pointing at it. Taken back after the op: the pixels it changed (their grey, over black where it made them
// clear) put back into the mask at its own grid, each of its pixels by the share of it they cover; a pixel no changed
// pixel reaches keeps its value, and a mask the op left as it was its very bytes. The mask's settings (linked, density,
// feather) are kept.
class MaskLending {
public:
    MaskLending(core::Document& doc, std::size_t page, core::Json& op, std::string_view name);
    // Whether the op changed the mask.
    bool take_back(core::Document& doc) const;

private:
    std::string page_id_;
    std::string owner_;     // (the layer whose mask it is)
    std::string layer_id_;  // (the layer lent)
    std::string lent_;      // (the RGBA bytes lent)
    Size lent_size_;
    Size own_size_;         // (the mask's picture's)
    std::string own_;       // (its "L" bytes before)
    bool made_ = false;     // (the layer had no mask: one made for the op)
};

}  // namespace genko::render::layer_masks
