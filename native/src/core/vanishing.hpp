#pragma once

#include <cstddef>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// vanishing-point@1 (PC4 節 D6, SPEC.md COLOR-08「フィルター」・schema-v4.md「消点」): the planes of フィルター → 消点… a
// page keeps (as Photoshop keeps them with the document: opened again, the planes are there to work on). The page's
// "vanishing" — held in Page::extra under that key, written back as it was read —:
//   {"planes": [{"corners_mm": [[x, y] × 4]},                                   // the first: four corners on the page (mm),
//               {"from": i, "edge": "top"|"right"|"bottom"|"left", "angle": 90, "depth": 1}, …],   // clockwise from its
//    "focal_mm"?: f}                                                            // top left; each later one its own four
//                                                                               // corners or torn off an earlier one's edge
// at most kVanishingMostPlanes. A plane's corners make a convex quad; a plane torn off names an earlier one, its angle
// (degrees about the edge: 90 perpendicular) and depth (of the parent's side across the edge) finite. A page whose planes
// are not so opens the book read-only. The geometry (the camera, the planes lifted, torn off, pasting and cloning on them):
// render/vanishing.

namespace genko::core {

inline constexpr std::string_view kVanishingFeature = "vanishing-point@1";
inline constexpr std::string_view kVanishingKey = "vanishing";  // (the page's key, in Page::extra)
inline constexpr std::size_t kVanishingMostPlanes = 32;

// The planes in their normal form ({"planes": […], "focal_mm"?}: each plane's keys in this order, the defaults filled —
// angle 90, depth 1 —, depth within 0.05..20, angle within −360..360). OpError says what is not well formed.
Json read_vanishing(const Json& value);
// The page's planes (none: it has none).
const Json* page_vanishing(const Page& page);
// Does a page the book writes keep planes? Are they well formed (core::Error("value") says what is not)?
bool uses_vanishing(const Document& doc);
void validate_vanishing(const Document& doc);

}  // namespace genko::core
