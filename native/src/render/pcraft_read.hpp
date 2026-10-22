#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// A PhotoCraft document (.pcraft, format_version 1: PhotoCraft v0.2.0 crates/format, MIT) read as a Genko page (PC4 F-f;
// SPEC.md FILE-01; design section F 2.8). A .pcraft is one picture: it becomes a book of one page in memory, its paper the
// picture's pixels at its resolution (as formats/open_file opens a picture), its layers Genko's —
//   - pixel layers at the page's depth (an 8-bit RGB page's PNG, a precise page's GKCR, a grey, CMYK or Lab page's GKIR),
//     their pixels as they are (never resampled; those beyond the canvas cut), groups as folders (their layers after them,
//     as the PSD's), adjustment layers as correction layers, fill layers (a colour, a gradient, a pattern of the book's),
//     shape layers as pen layers of one shape part, smart objects of an embedded file or .pcraft as smart objects (their
//     smart filters PhotoCraft's commands this build reads: core::from_upstream), each with its masks, vector mask, link,
//     locks, clipping, opacity and blend;
//   - text layers as their pixels (upstream's picture of them), linked smart objects and what cannot be read as Genko's
//     (a shape this build cannot hold, an embedded file it does not read) as their pixels too;
//   - its channels and Quick Mask (channels@1), saved, work and clipping paths (vector@1), guides (rulers of kind guide),
//     patterns (patterns@1), colour table, duotone inks, ICC profile, XMP (file-info@1) and EXIF, the PSD's blocks and
//     resources it carried (psd-keep@1); the page's native_source says it was a .pcraft;
// and what Genko has no place for — layer effects, Blend If, left-out channels, layer comps, notes, slices, variables,
// the timeline, measurements, text styles, a group's own opacity, blend and mask, labels, a video layer's frames — left
// out and said in `notes` (in Japanese), never silently (the user's design 8-5 (a): PC5 takes them in as it adds them).
//
// What is untrusted is checked before anything is made of it: the manifest's size, the format version (a newer one
// refused: core::Error("unsupported_version")), each tile's and blob's hash, size and zstd frame, the canvas's size against
// the picture limits and each layer's pixels against the memory budget, and all the bytes unpacked together against
// ReadLimits — before they are allocated (a zip or zstd bomb is refused, not unpacked). A broken file is
// core::Error("format", "the .pcraft is broken (…)"); one too large core::Error("image_too_large").

namespace genko::render::pcraft {

struct ReadLimits {
    std::uint64_t max_manifest_bytes = 32ull << 20;   // manifest.json
    std::uint64_t max_blob_bytes = 256ull << 20;      // one blob unpacked (an embedded file: render::smart's limit)
    std::uint64_t max_unpacked_bytes = 8ull << 30;    // every tile and blob unpacked, together (upstream: 16 GiB)
    int depth = 0;                                    // (a .pcraft inside a smart object of one: how deep)
};

struct Read {
    core::Document doc;              // a book of one page, in memory (no folder)
    std::vector<std::string> notes;  // what it could not keep, in Japanese
    std::uint64_t unpacked = 0;      // the bytes its tiles and blobs unpacked to (a .pcraft inside one counts in the outer's)
};

// Bytes that look like a .pcraft written as a zip (a zip whose first file is manifest.json: how PhotoCraft and Genko write
// them).
bool is_pcraft(std::string_view bytes);

// A .pcraft (its zip bytes; or a .pcraft folder) as a book named `title`. The errors above.
Read read(std::string_view bytes, std::string_view title, const ReadLimits& limits = {});
Read read_folder(const std::filesystem::path& folder, std::string_view title, const ReadLimits& limits = {});

// Its manifest alone, checked (version, size, JSON): what a quick look needs (genko inspect, the tests).
core::Json read_manifest(std::string_view bytes);

// 埋め込みを配置 of a .pcraft (design 2.8): the page read made the contents of a smart object of `doc` — its page the
// contents' page (PC3's contents of kind page), what its page uses (patterns, lookup tables, files of its own smart objects
// and their contents) brought into `doc` —; the new contents' id. `name` the file's name. The errors above.
std::string place_as_contents(core::Document& doc, std::string_view bytes, std::string_view name, std::vector<std::string>& notes,
                              const ReadLimits& limits = {});

// The window reads a .pcraft on a worker thread before it places it (the reading is the heavy part, never on the window's
// thread): the read kept here by its bytes' asset ref ("sha256:…"), taken — once — by place_as_contents for the same bytes
// (the op), which otherwise reads them itself (the command line, MCP). One at a time; thread-safe.
void remember_read(const std::string& ref, Read read);

}  // namespace genko::render::pcraft
