#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/resample.hpp"

// smart-objects@1, PC3 A5 (docs/cpp-migration/SPEC.md COLOR-05「スマートオブジェクトの上の変形」): the smart filters that
// bend a smart object's picture — 節 C's Liquify (its strokes replayed on a fresh field), Perspective Warp (its planes) and
// Puppet Warp (its pins, meshed over what the picture shows at the working 200 dpi, so that the screen and print bend it
// alike) — drawn again on the picture below them each time it is drawn (PhotoCraft v0.2.0's distort_cmds.rs:233-272,
// apply_to_surface; MIT, third_party/photocraft/NOTICE.md). Their settings are in the page's mm (the picture is the smart
// object's of the whole page, render/smart_filters), so a smart filter bends what the op would have bent on a paint layer.
// The pixels are drawn through render/resample, premultiplied, in each way's own values (the 8-bit page's, a precise page's
// linear light, a page's inks).

namespace genko::render::smart_bends {

// A bend filter's settings as the smart object keeps them (OpError for what is not well formed): Liquify's {"strokes",
// "mesh_mm"?} as given; a perspective warp's {"planes", "interp"} its planes straightened already (its "straighten" taken
// in); a puppet warp's {"pins", "mode", "density", "expansion_mm", "interp"} — interp nearest, bilinear or bicubic (none:
// bicubic).
core::Json settled(std::string_view kind, const core::Json& params, const core::Page& page);

// The picture of the whole page (`size` its pixels) over a window of them (x0, y0, x1, y1), clear past its edge.
using Reader = std::function<resample::Plane(int x0, int y0, int x1, int y1)>;
// What a bend filter makes of the picture: the window it changes and its pixels there — `whole`: the picture is that
// window, clear elsewhere (a perspective or puppet warp moves all it shows); else the picture as it was past it (Liquify
// moves what its field reaches). None: nothing bent. `shown` is the box of what the picture shows (none: nothing).
// OpError / core::Error("value") for settings this build cannot use (a book changed by hand).
struct Bent {
    resample::Plane plane;
    int x0 = 0, y0 = 0;
    bool whole = true;
};
std::optional<Bent> bend(const core::Page& page, std::string_view kind, const core::Json& params, Size size, const std::optional<Box>& shown,
                         const Reader& read);

}  // namespace genko::render::smart_bends
