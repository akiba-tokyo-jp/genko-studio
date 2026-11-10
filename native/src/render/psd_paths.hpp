#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "core/vector.hpp"

// vector@1 in a PSD (PC3 C-a, the user's decision 3: always read and written; SPEC.md COLOR-07「PSD のパス」):
// PhotoCraft v0.2.0's psd/src/path.rs and io/src/vector_map.rs (MIT, native/third_party/photocraft/NOTICE.md) — a
// path as Photoshop's 26-byte path records (Adobe's "Path resource format": a fill rule record, the initial fill
// record — the inversion —, then each subpath's length record — bytes 2–3 its operation, as psd-tools documents it:
// 1 combine, 2 subtract, 3 intersect, 0 exclude — and its knots, each its control point before, its anchor and its
// control point after as signed 8.24 fixed fractions of the canvas's height then width), the saved paths (image
// resources 2000–2997, each named by its resource's name), the work path (1025), the clipping path (2999: its name and
// flatness) and a layer's vector mask (the tagged block vmsk: version 3, flags — 1 inverted, 2 not linked, 4 disabled —
// and the path). The page's mm are fractions of its paper (the canvas is the page).

namespace genko::render::psd_paths {

inline constexpr unsigned kSavedFirst = 2000;
inline constexpr unsigned kSavedLast = 2997;
inline constexpr unsigned kWorkPath = 1025;
inline constexpr unsigned kClippingPath = 2999;

// The path's records (`width_mm` × `height_mm` the paper). A path's fill rule is not in them (Photoshop fills each
// subpath by the non-zero rule).
std::string records(const core::vector::Path& path, double width_mm, double height_mm);
// The path the records hold (a knot without its length record starts or extends a subpath, leniently). Nothing for
// data that is not records (shorter than one).
std::optional<core::vector::Path> path_of(std::string_view data, double width_mm, double height_mm);

// A layer's vmsk block's payload, and the vector mask one holds (its density 1 and feather 0: those are the layer's
// mask parameters).
std::string vmsk(const core::vector::VectorMask& mask, double width_mm, double height_mm);
std::optional<core::vector::VectorMask> vector_mask_of(std::string_view data, double width_mm, double height_mm);

// A resource name's bytes (a Pascal string: at most 255, ASCII as it is and other letters as UTF-8, cut at a letter's
// end) and back (UTF-8 when it is, else each byte a Latin-1 letter).
std::string resource_name(const std::string& name);
std::string name_of(std::string_view bytes);

// The page's path resources, each "8BIM", its id, its name (a Pascal string padded to even), its size and its data
// padded to even: the saved paths (2000 on, at most 998), the work path, the clipping path. "" for a page of none.
std::string resources(const core::Page& page);
// A clipping path resource's payload (its name and its flatness as 16.16 fixed) and back.
std::string clipping(const core::vector::ClippingPath& clip);
std::optional<core::vector::ClippingPath> clipping_of(std::string_view data);

}  // namespace genko::render::psd_paths
