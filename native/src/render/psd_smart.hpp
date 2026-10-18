#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// smart-objects@1 (PC3 A3, the user's decision 3: a PSD's smart objects are always read and written; SPEC.md COLOR-05
// 「PSD のスマートオブジェクト」): a placed layer's block — SoLd and SoLE (a versioned descriptor) or PlLd (the older form
// Photoshop writes beside SoLd) — and the files a PSD embeds for them (its global lnk2, lnk3 and lnkD blocks: liFD items
// holding a file's bytes, liFE naming an external one, liFA an alias), read and written. Upstream PhotoCraft's
// io/src/blocks.rs (parse_smart, parse_placed_warp) and io/src/linked.rs; the writing of a fresh placed layer is this
// build's own (upstream writes back only what it read). render/psd reads the blocks out of a file, render/import_psd
// makes smart objects of them, formats/psd_export writes them.

namespace genko::render::psd {

// A placed layer as its block says.
struct Placed {
    std::string key;    // "SoLd", "SoLE" or "PlLd"
    std::string uuid;   // its contents' id among the linked files (SoLd's Idnt, PlLd's unique id)
    std::string placed; // (SoLd) the layer's own id ("placed")
    // Where its contents' box lands on the canvas, in pixels: top left, top right, bottom right, bottom left (x, y) —
    // SoLd's nonAffineTransform when it has one, else its Trnf; PlLd's transform.
    std::array<double, 8> corners{};
    bool has_corners = false;
    double width = 0, height = 0;  // (SoLd) the size of the contents its corners and warp are of (Sz); 0 when not said
    double resolution = 0;         // (SoLd) its Rslt in dpi; 0 when not said
    int type = 0;                  // (its placed layer type: 0 unknown, 1 vector, 2 raster, 3 image stack)
    // Its warp, in the units of `width` and `height` (Genko's form, docs/cpp-migration/schema-v4.md): {"style": "arc" |
    // "arcLower" | … | "custom", "bend", "h_distort", "v_distort", "vertical", "bounds": [left, top, right, bottom],
    // "mesh"?: {"us": [0, 1], "vs": [0, 1], "points": [[x, y] × 16]}}; none when it has no warp (warpNone, or one that
    // leaves the contents as they are).
    std::optional<core::Json> warp;
    bool filters = false;  // (SoLd) Photoshop's smart filters on it (filterFX): its picture shows them, this build has none
};

// The block read (its payload, as the layer record holds it); none when it cannot be read: another kind or version, a
// descriptor cut short, no id.
std::optional<Placed> read_placed(std::string_view key, std::string_view data);

// A placed layer whose corners are a parallelogram is its contents moved by a matrix (within `tolerance` pixels).
bool parallelogram(const std::array<double, 8>& corners, double tolerance = 1e-6);

// A file of the PSD's linked-layer blocks.
struct LinkedItem {
    std::string kind;           // liFD (the file's bytes), liFE (an external file), liFA (an alias)
    std::uint32_t version = 0;  // 1 to 7
    std::string uuid;
    std::string name;           // its file's name (UTF-8)
    std::string type;           // its file type (4 characters: "png ", "JPEG", "8BPS", …)
    std::string_view item;      // the whole item (after its length), within the block's data
    std::string_view data;      // liFD: the file's bytes, within the item
};
// Every item of one block's data — each an 8-byte length, the item, padded to 4 bytes —; an item that cannot be read,
// and what follows one cut short, are left out.
std::vector<LinkedItem> read_linked(std::string_view block);
// One liFD item (version 2, no open descriptor) as a block holds it: its length, the item, padded to 4 bytes.
std::string linked_item(std::string_view uuid, std::string_view name, std::string_view type, std::string_view bytes);
// An item as it was read (LinkedItem::item), framed again: its length before it, padded to 4 bytes.
std::string framed_item(std::string_view item);
// The 4-character file type a linked file of a format (png, jpeg, tiff, webp, bmp, gif, psd; a PSB: "psb") is said to be.
std::string file_type(std::string_view format);

// A placed layer written by this build.
struct PlacedOut {
    std::string uuid;    // its contents' id (Idnt; the linked file's)
    std::string placed;  // its own id
    std::array<double, 8> corners{};  // as Placed::corners, on the canvas written
    double width = 0, height = 0;     // its contents' size in pixels (Sz)
    double resolution = 72;           // its contents' dpi (Rslt)
    int type = 2;
    std::optional<core::Json> warp;   // Genko's form, in the contents' pixels
};
// SoLd's payload (soLD, version 4, its descriptor: Idnt, placed, PgNm, totalPages, Crop, frameStep, duration, frameCount,
// Annt, Type, Trnf, nonAffineTransform, warp, Sz, Rslt, comp, compInfo) and PlLd's (plcL, version 3: the id, the page,
// the corners and the warp), each padded to 4 bytes — Photoshop writes both for a placed layer.
std::string sold_block(const PlacedOut& placed);
std::string plld_block(const PlacedOut& placed);

// An id for contents written by this build, as Photoshop's are made (8-4-4-4-12 lowercase hex digits), the same for the
// same `seed` (a file written again is the same bytes).
std::string uuid_of(std::string_view seed);

}  // namespace genko::render::psd
