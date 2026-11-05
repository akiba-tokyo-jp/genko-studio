#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "render/psd_production.hpp"

// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」, FILE-01「PSD・PSB」): a PSD's slices — the image resource 1050 — as the
// page's, and back: PhotoCraft v0.2.0's crates/psd/src/slices.rs and crates/io/src/slices_map.rs (MIT,
// native/third_party/photocraft/NOTICE.md).
//
// Version 6 (Adobe's documented layout): the bounds (top, left, bottom, right), the group's name (a unicode string), the
// count, then each slice — its id, group id, origin (0 auto, 1 layer-based, 2 user), the layer's lyid (layer-based only),
// name, type (0 no image, 1 image, 2 table), rect (left, top, right, bottom: the file's pixels), URL, target, message, alt,
// whether the cell text is HTML, the cell text, the horizontal and vertical alignment and the background (alpha, red, green,
// blue; alpha 0: none) —, then maybe a versioned descriptor with each slice's outsets (topOutset…, by sliceID). Versions 7
// and 8 are one versioned descriptor (baseName, bounds, slices) — read, not written.
// Reading (render/psd_production's row 1050): the user and layer-based slices onto the page (the auto ones made again),
// placed through the file's canvas on the page (ReadContext::onto), when the page has none yet and keeps no earlier PSD's
// blocks; a layer-based slice whose layer (by its lyid) was not made becomes a user slice where it was. Writing: while the
// page's slices — and where its layer-based ones lie — are as read, the block's own bytes (psd_production's mark); else
// version 6 of the page's slices in the file's pixels, layer-based ones naming their layers' lyids (a layer the file does
// not write: a user slice there), and the auto ones around them, as Photoshop saves them.

namespace genko::render::psd_slices {

inline constexpr int kResource = 1050;

struct FileSlice {
    std::int64_t id = 0;
    std::uint32_t group_id = 0;
    std::uint32_t origin = 2;                 // 0 auto, 1 layer-based, 2 user
    std::optional<std::uint32_t> layer_id;    // (layer-based) the layer's lyid
    std::string name;                         // (UTF-8)
    std::uint32_t kind = 1;                   // 0 no image, 1 image, 2 table
    std::array<std::int32_t, 4> rect{};       // left, top, right, bottom
    std::string url, target, message, alt, cell_text;
    bool cell_text_is_html = false;
    std::uint32_t horizontal_align = 0, vertical_align = 0;
    std::array<std::uint8_t, 4> color{};      // alpha, red, green, blue
    std::array<std::int32_t, 4> outsets{};    // top, left, bottom, right

    bool operator==(const FileSlice&) const = default;
};

struct FileSlices {
    std::uint32_t version = 6;
    std::array<std::int32_t, 4> bounds{};     // top, left, bottom, right
    std::string group;
    std::vector<FileSlice> slices;

    bool operator==(const FileSlices&) const = default;
};

inline constexpr std::size_t kSlicesMost = 4096;  // (slices a resource holds that this build reads; more: kept as it is)

// The resource's slices (none: not one this build reads — broken, another version, more than kSlicesMost).
std::optional<FileSlices> parse(std::string_view data);
// Version 6 of them (the outsets' descriptor after, when any slice has some).
std::string resource(const FileSlices& slices);

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);
std::string fingerprint(const core::Page& page, const core::Document* doc);

}  // namespace genko::render::psd_slices
