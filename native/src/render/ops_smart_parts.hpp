#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/smart_decode.hpp"

// smart-objects@1: what the smart objects' ops share (render/ops_smart.cpp, PC3 A0) with the ops of their contents and
// links (render/ops_smart_contents.cpp, PC3 A1).

namespace genko::render::smart_ops {

// The asset store's file of a ref, whatever its picture's suffix (an op's "asset"): its bytes and suffix (OpError when
// the book has none, or not the bytes its name says).
std::pair<std::string, std::string> asset_file(const core::Document& doc, const std::string& ref);
// The contents' colour use an op asks for ("embedded_profile" keep, convert or assign, "icc" or "profile", "intent",
// "bpc"), for a file of these properties.
core::Json colour_use(core::Document& doc, const core::Json& op, const smart::FileInfo& info);
// An id of contents the book does not hold yet.
std::string fresh_contents_id(const core::Document& doc);
// A linked file's record (core::smart::link_json) from an op's "link" ({"path", "relative"?, "sha256"?, "bytes"?}):
// its sha256 and size those of `bytes` (the file as given) unless it says. OpError when it is not a link's.
core::Json link_of(const core::Json& op, std::string_view bytes);
// rasterize_layer's work on one layer (the `i`th of the `at`th page): its look as a paint layer's pixels at 200 dpi —
// an 8-bit page's PNG, a precise page's raster, an ink page's inks —, its mask and settings kept.
void rasterize(core::Document& doc, std::size_t at, std::size_t i);
// smart_convert's work on a placed picture (PC3 C-e: one turned with its page): a smart object of its file, `id` its
// layer's id, its matrix the place it is laid at; its panel, finish and where it came from kept.
core::Layer placed_to_smart(core::Document& doc, core::Layer placed, const std::string& id);
// A page of more than 8 bits (RGB in precise colour) shows a smart object's file of more bits only within what such a page
// keeps whole (core::kColorRasterMaxPixels), as smart_place takes one: OpError ("the picture is too large to keep its 16
// or 32 bits on this page …") for a larger one on it. A page made so (set_color_mode, independent review A P1-1) asks it.
void fits_deep_page(const core::Document& doc, const core::Page& page);

}  // namespace genko::render::smart_ops
