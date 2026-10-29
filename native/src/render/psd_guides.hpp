#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/model.hpp"
#include "render/psd_production.hpp"

// PC5 H-b (節 H 2.2, SPEC.md VIEW-01「ガイド」・FILE-01「PSD・PSB」): a PSD's guides as the page's — PhotoCraft v0.2.0's
// psd_import.rs parse_guides and psd_export.rs guides_resource (MIT, native/third_party/photocraft/NOTICE.md). The image
// resource 1032 (グリッドとガイド) holds a version (1), the grid's cycle across and down (Photoshop's 576), the count of
// guides, then each guide: its place in 1/32 of the file's pixel (a signed 32-bit number) and its way (0 an upright guide —
// its x —, 1 a guide across — its y).
//
// Read (render/psd_production's row 1032, render/import_psd): when the file is opened as a page of its own, its guides
// (version 1; up to 4096; none more than the canvas's size off it) become the page's guide rulers (kind guide, axis "v" or
// "h", at mm), those the page has already (within 0.01 mm) left out. A PSD read onto a page (import_psd) brings none, as
// Photoshop's 配置. Psd-keep@1 never kept 1032 (the grid and guides): nothing else changes.
// Written (formats/psd_export): the page's guide rulers (mm at the picture's resolution → pixels × 32, rounded) — none on
// the page: no 1032 (the file as before, byte for byte).

namespace genko::render::psd_guides {

inline constexpr int kResource = 1032;

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);

}  // namespace genko::render::psd_guides
