#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/model.hpp"
#include "render/psd_production.hpp"

// PC5 H-f (pixel-aspect@1, the user's decision 10; SPEC.md VIEW-01「ピクセル縦横比」・FILE-01「PSD・PSB」): a PSD's pixel aspect
// ratio as the page's. The image resource 1064 (Adobe's "Pixel Aspect Ratio") holds a version (1, or 2 — Photoshop's since
// CS2: the NTSC and PAL ratios set right —) and the ratio, the width of a pixel over its height, as a big-endian double.
//
// Read (render/psd_production's row 1064, render/import_psd): when the file is opened as a page of its own, a ratio that is
// not square (0.1..10) becomes the page's pixel_aspect — the block kept (psd-keep@1) with its mark, so the file's own bytes
// are written again while the ratio is the one read. A square one, one read onto a page (import_psd: Photoshop's 配置 brings
// none) or one out of range is not read: kept as before (psd-keep@1), unmarked.
// Written (formats/psd_export): the page's ratio when it is not square (version 2); square: none of its own (an earlier
// file's square 1064, kept unmarked, written back as before — the file as before, byte for byte). The pixels are written as
// they are (square): only the file's word for them changes.

namespace genko::render::psd_pixel_aspect {

inline constexpr int kResource = 1064;

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);
std::string fingerprint(const core::Page& page, const core::Document* doc);
// The resource's data for a ratio (version 2), and the ratio of a resource's data (none: not one — too short, another
// version, not finite).
std::string bytes_of(double ratio);
std::optional<double> ratio_of(std::string_view data);

}  // namespace genko::render::psd_pixel_aspect
