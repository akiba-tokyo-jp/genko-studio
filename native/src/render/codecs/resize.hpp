#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "render/codecs/image.hpp"

// A codecs' picture made another size (PC4 F-c, SPEC.md FILE-01: 書き出し形式… and Web 用に保存's 画像サイズ, a page drawn at
// the resolution nearest to the size asked and made exactly that size): separable filters, as Pillow and Image Size resize —
// nearest neighbour (each pixel the nearest one), bilinear (a tent), bicubic (Catmull-Rom, Keys' a = −0.5) —, a reduction's
// filter widened by its factor so it does not alias; colours under an alpha filtered premultiplied (no dark fringe); every
// depth and layout, a float picture's values past 1 kept. Rows are made one at a time from a window of filtered rows: the
// picture is never held again as floats.

namespace genko::render::codecs {

enum class ResizeFilter { Nearest, Bilinear, Bicubic };
// "nearest" | "bilinear" | "bicubic" (Photoshop's ニアレストネイバー, バイリニア, バイキュービック), or none.
std::optional<ResizeFilter> resize_filter(std::string_view id);

// The picture at width × height (both at least 1; the same size: the picture as it is), its profile and metadata kept.
CodecImage resized(const CodecImage& picture, std::uint32_t width, std::uint32_t height, ResizeFilter filter = ResizeFilter::Bicubic);

}  // namespace genko::render::codecs
