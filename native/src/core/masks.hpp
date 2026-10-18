#pragma once

#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// masks@1 (PC3 節 B, SPEC.md COLOR-05, docs/cpp-migration/schema-v4.md「マスク（masks@1）」): what a layer mask holds
// past its picture — whether it moves with the layer (linked), its density and its feather (Photoshop's Properties) —
// and a page's Quick Mask (an alpha channel marked quick_mask). A mask without them is written as before (its bytes the
// same); a book with them names masks@1, and a build that does not know it opens the book read-only.

namespace genko::core {

inline constexpr std::string_view kMasksFeature = "masks@1";
inline constexpr double kMaskFeatherMax = 100.0;  // (mm)

// Does the mask hold more than its picture and enabled (linked, a density below 1, a feather)?
bool mask_has_settings(const Mask& mask);
// The mask's keys past {"enabled", "asset"}, written into `out` in this order when they are not their defaults: linked,
// density, feather_mm.
void write_mask_settings(const Mask& mask, Json& out);
// Those keys read from a layer's mask object into `mask` (core::Error("value") for one that is not a bool / a number in
// its range).
void read_mask_settings(const Json& raw, Mask& mask);

// Does the book need masks@1 (a mask with settings, a page's Quick Mask)?
bool uses_masks(const Document& doc);
// The book's masks as they must be (core::Error("value") says what is not): a density 0..1, a feather 0..kMaskFeatherMax
// mm, at most one Quick Mask on a page and only an alpha channel. Pages not read yet are not looked at.
void validate_masks(const Document& doc);

// The page's Quick Mask channel (null: none).
const Channel* quick_mask_of(const Page& page);

}  // namespace genko::core
