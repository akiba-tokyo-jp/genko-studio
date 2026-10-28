#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model.hpp"
#include "render/codecs/image.hpp"

// A page or a layer as one picture at its depth (PC4 F-0, SPEC.md FILE-01): what コピーを保存…, 書き出し形式…, クイック書き出し
// and レイヤーを書き出し write through the codecs (F-b, F-c). One rule for the three ways a page composites, the rule the
// exports already write a page's TIFF and PSD by (formats/export.cpp's inks and values):
//   - an 8-bit RGB page: its print picture (render_page), RGB 8 bits — sRGB, no profile;
//   - an RGB page of a colour model (color-model@1: 16 or 32 bits, a profile): its values in its profile (render_page's
//     precise, detail::page_values) at its depth — RGB 8, 16 or 32 bits (floats) —, its profile;
//   - a grey or CMYK page: its inks at its depth (render_page's inks) — grey or CMYK, 8, 16 or 32 bits —, its profile.
// What the codecs' picture holds no layout for is the picture as the screen shows it (RGB 8 bits, said in the notes): a Lab
// page, an indexed, bitmap, duotone or multichannel page. A page is opaque (its paper under it).

namespace genko::formats {

struct PagePicture {
    render::codecs::CodecImage image;
    std::vector<std::string> notes;  // what the picture could not keep, in Japanese
};

// The page at `dpi`, cut to `area` (paper | bleed | trim, as the exports cut it). render::NotYetPorted for what this build
// does not draw yet, core::Error("image_too_large") for a page too large at that resolution.
PagePicture page_picture(const core::Page& page, const core::Document& doc, int dpi, std::string_view area = "paper");
// (PC4 F-c) The size page_picture makes at `dpi` (its paper, or the area cut), without drawing it: what 書き出し形式… shows
// and the size a picture is checked against before it is drawn.
std::pair<std::uint32_t, std::uint32_t> page_picture_size(const core::Page& page, int dpi, std::string_view area = "paper");

// One layer alone over nothing, the page's size at `dpi`: its own pixels at the page's depth when they are drawn as they
// are on the page's grid at that resolution — a paint layer of precise pixels (RGBA 16 or 32 bits) or inks (grey or CMYK
// with an alpha) whose raster is that size, and nothing on it (no lines, parts, mask, vector mask or effect: a layer an
// opened picture made) —, else its picture as the layered exports draw it (render::layer_image: RGBA 8 bits).
PagePicture layer_picture(const core::Page& page, const core::Layer& layer, const core::Document& doc, int dpi);

}  // namespace genko::formats
