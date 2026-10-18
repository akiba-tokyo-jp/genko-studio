#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/page.hpp"

// smart-objects@1 (PC3 A0, docs/cpp-migration/ARCHITECTURE.md §4a「スマートオブジェクト」): a smart object drawn on its page.
// Its contents are drawn as they are — a page of its own with its layers alone (render_contents: no paper, panels' borders,
// lines or nombres, over nothing), or a file read from its bytes (render/smart_decode) through its colour use — then laid
// on the page's pixels through its matrix in one resampling: whole pixels moved as they are, a picture's box as a placed
// picture's (the same pixels as place_asset gives it), anything else through its interpolation. What it makes is the
// layer's picture: the page draws it as a layer's (its mask, opacity, blend, clipping, effects, screen and colour).
// A part of the page gets the same pixels as the whole page cut there. A smart object that draws exactly as a layer of
// its own (core::smart::plain_layer) is drawn as that layer before any of this (render/page.cpp).
//
// What later steps of PC3 add — perspective and warp (A5), stack modes (A4), smart filters and their mask (A2), a PSD's
// picture (A3) — is not drawn yet: NotYetPorted("smart:<what>").

namespace genko::render {
struct ColorPicture;
struct InkPicture;
class RgbSpace;
namespace detail {
struct Ctx;
}
}  // namespace genko::render

namespace genko::render::smart {

// The smart object's picture over `area` of the page as the 8-bit page draws it ("RGBA", sRGB; nothing when none of it is
// there): cut to its panel (clip_to) and, a picture's, with the monochrome finish in print (as a placed picture's).
std::optional<Image> picture_8bit(const detail::Ctx& ctx, const core::Layer& layer, const Box& area);

// On a page in precise colour: its picture in the canvas's space where it holds more than 8 bits — a file of 16 or 32 bits,
// contents in precise colour moved by whole pixels —; nothing otherwise (picture_8bit is drawn as an 8-bit picture is).
std::optional<ColorPicture> picture_precise(const detail::Ctx& ctx, const core::Layer& layer, const Box& area, const RgbSpace& space);

// On a CMYK, Lab, grey or multichannel page: its picture in the page's inks where it holds inks of its own — a CMYK, grey
// or Lab file, contents of the page's colour mode moved by whole pixels —; nothing otherwise (picture_8bit is drawn as an
// 8-bit picture is, by the page's rules).
std::optional<InkPicture> picture_inks(const detail::Ctx& ctx, const core::Layer& layer, const Box& area);

// Does the page composite in precise colour for its smart objects: an RGB page of more than 8 bits showing a file of more
// than 8 bits, or contents that composite so (a page whose layers alone would not)?
bool needs_precision(const core::Page& page, const core::Document* doc, bool print);

// The smart object's contents' file as the 8-bit page draws it: open_image's "RGBA" (a placed picture's) through its colour
// use (keep: its numbers; embedded, assign: from that profile to sRGB). Read from the book's assets (or the files ops
// were given, Document::smart_files) and remembered. core::Error when it cannot be read.
std::shared_ptr<const Image> file_picture(const core::Document& doc, const core::SmartContents& contents);
// The file's bytes (null when the book does not hold them).
core::Bytes file_bytes(const core::Document& doc, const core::SmartContents& contents);

// The pictures remembered between tiles, forgotten (clear_render_caches), and how many pixels they hold.
void clear_caches();
std::int64_t cache_pixels();

// How many times contents were drawn whole or a file decoded (not found in the caches), for tests.
struct Counts {
    std::uint64_t contents_drawn = 0;
    std::uint64_t files_decoded = 0;
};
Counts counts();

}  // namespace genko::render::smart
