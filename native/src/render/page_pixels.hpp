#pragma once

#include <cstdint>
#include <string>

#include "core/model.hpp"
#include "core/page_geometry.hpp"
#include "render/resample.hpp"

// PC3 節 C (C-e, SPEC.md COLOR-07「判型」): the pixels of a page moved with it by the page size ops (render/ops_pagesize.cpp,
// through core/page_geometry's map_page and its PixelHooks) — a paint layer's 8-bit, precise and ink pixels, a layer's mask,
// a smart object's filter mask, the pictures of its patches, the page's channels and its saved mask areas. Each lies
// stretched over the page on a grid of its own; it keeps its resolution (its pixels per mm across and down) on the new
// paper.
//
// A move by whole pixels and a turn by a right angle or a flip move the pixels themselves (their values exact). A move by
// part of a pixel goes to the nearest whole pixel of each grid (half a pixel at most: 0.064 mm on the paint layers' 200 dpi,
// 0.085 mm on a mask's 150 dpi), so the pixels are never drawn again for it.

namespace genko::render::page_pixels {

// How the pixels go with the page's map.
struct How {
    enum class Kind {
        Stretch,  // not drawn again: the pixels lie over the new paper as they are (Image Size without resampling)
        Resize,   // drawn again at the new size, by `filter` (masks and channels bilinear; Image Size)
        Turn,     // a turn by a right angle or a flip of the whole page (`turn`): the pixels moved
        Shift,    // a move (Canvas Size, Crop, Trim, Reveal All): each grid by whole pixels of its own
        Warp,     // any other map (Arbitrary, a tilted crop): projected with `interp`, outside clear (a mask's edge carried on)
    };
    Kind kind = Kind::Stretch;
    resample::Filter filter = resample::Filter::Bicubic;
    resample::Interp interp = resample::Interp::Bicubic;
    resample::Turn turn = resample::Turn::FlipH;
};

// set_page_spec's map (the old basic frame onto the new one, x and y each by its own scale, as pagespec._Map moves a
// point) as a map of the page. OpError for a basic frame of no finite size.
core::page_geometry::Mapping frame_map(const core::Rect& old_frame, const core::Rect& new_frame);
// How set_page_spec moves pixels at their own resolution (render/bookpages, color_edit, ink_edit): projected, bilinear.
How frame_how();

// The hooks map_page moves the pixels with: `before` and `after` the page's paper before and after the op (the map takes
// the one onto the other). A smart object's filter mask is read from and written into `doc`'s files.
core::page_geometry::PixelHooks hooks(core::Document& doc, const How& how, const core::PageSpec& before, const core::PageSpec& after);

// A grid of `width` × `height` over the paper `before` on the paper `after`: its size there (its resolution kept; a quarter
// turn swaps it). Each side at least 1.
struct Grid {
    int width = 0;
    int height = 0;
};
Grid grid_after(const How& how, int width, int height, const core::PageSpec& before, const core::PageSpec& after);

// The ways each kind of pixels goes (the hooks' work; each also the tests'): `m` the page's map (mm).
// An 8-bit paint layer's PNG (as RGBA), a layer mask's or filter mask's PNG ("L", its edge carried on past it).
std::string move_png(const std::string& png, const How& how, const core::page_geometry::Mapping& m, const core::PageSpec& before,
                     const core::PageSpec& after);
std::string move_mask(const std::string& png, const How& how, const core::page_geometry::Mapping& m, const core::PageSpec& before,
                      const core::PageSpec& after);
// A precise layer's raster (GKCR: u16, or f32 with its colours above 1 kept).
std::string move_precise(const std::string& gkcr, const How& how, const core::page_geometry::Mapping& m, const core::PageSpec& before,
                         const core::PageSpec& after);
// An ink layer's raster or a channel's (GKIR): outside clear (no ink, a channel 0).
std::string move_inks(const std::string& gkir, const How& how, const core::page_geometry::Mapping& m, const core::PageSpec& before,
                      const core::PageSpec& after);

}  // namespace genko::render::page_pixels
