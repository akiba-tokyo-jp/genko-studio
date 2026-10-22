#pragma once

#include "core/json.hpp"
#include "render/area_kinds.hpp"
#include "render/image.hpp"

// PC3 B3 (SPEC.md COLOR-06「賢い選択」「選択とマスク」): the smart selection's area kinds (render/area_kinds' table) —
// PhotoCraft v0.2.0's engine/src/smartselect_cmds.rs and select_extra_cmds.rs (select_sky) over algo/src/segment and
// algo/src/matting (render/segment, render/matting), MIT (native/third_party/photocraft/NOTICE.md). Each reads the
// colours it analyses — a layer's own (`layer`: image_ops::shown_layer_pixels) or the page as it shows
// (image_ops::page_pixels, the working 200 dpi) — as RGB 0..1 (a precise page's values, HDR clipped; an ink page's colours
// its inks mean; a clear pixel over mid grey), so an 8-bit, a 16-bit and a CMYK page of the same picture give the same
// area. The work is on the grid the area is asked at (area_kinds::Context: its dpi and size — the selection's 200 dpi now,
// a page's finer grid later), the colours stretched to it; lengths are mm. None found: an empty mask (the area is empty).
// The window solves them on a worker thread (segment::StopScope stops one).

namespace genko::render::select_smart {

// {"subject": {"layer"?} | true}: Select › Subject — the most salient object (superpixel saliency seeding a GrabCut).
Image subject(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"sky": {"threshold" (0..100 = 50: stricter higher), "softness" (0..100 = 50), "layer"?}}: Select › Sky.
Image sky(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"object": {"rect_mm": [x, y, w, h], "layer"?}}: the Object Selection tool's rectangle (GrabCut from it).
Image object(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"quick": {"points_mm": [[x, y], …], "size_mm" (the brush's diameter, = 4), "enhance_edge" (= false), "layer"?}}: the
// Quick Selection tool's stroke — what it grows to (the window adds it to the selection, or takes it away).
Image quick(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"focus": {"range" (0..1 = 0.5: In-Focus Range), "noise" (0..1 = 0: Image Noise Level), "layer"?}}: Select › Focus Area.
Image focus(const core::Json& value, const core::Json& area, const area_kinds::Context& c);
// {"refine": {"area", "radius_mm" (= 0), "smart_radius" (= false), "smooth" (0..100 = 0), "feather_mm" (= 0), "contrast"
// (0..100 = 0), "shift_edge" (−100..100 = 0), "layer"?}}: Select and Mask's edge (Refine Edge) of the area — the guided
// filter's soft edge within the radius (the picture read only then), then smoothed, feathered, the contrast raised and
// the edge shifted.
Image refine(const core::Json& value, const core::Json& area, const area_kinds::Context& c);

}  // namespace genko::render::select_smart
