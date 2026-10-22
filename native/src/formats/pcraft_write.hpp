#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"

// A Genko page written as a PhotoCraft document (.pcraft, format_version 1 — PhotoCraft v0.2.0 crates/format, MIT; PC4 F-f,
// SPEC.md FILE-01, design section F 2.8): one page = one file (a book's pages each a file of their own: genko export
// --format pcraft, <stem>_p001.pcraft …). A ZIP of stored files — manifest.json first, then tiles/<blake3>.zst (256 × 256,
// the layer's samples little-endian, zstd at its fastest level; a tile of nothing but the surface's untouched pixel left
// out, a tile of the same bytes written once), blobs/<blake3>.zst (the profile, the EXIF, the PSD's kept blocks, the files
// of the smart objects), thumb.png (256) and composite/preview.png (1024) — streamed to its sink, never held whole.
//
// What PhotoCraft reads as its own: the page's pixel layers at the page's depth (an 8-bit page's RGBA, a precise page's
// GKCR values, a grey, CMYK or Lab page's inks — copied as they are when a layer is the page's pixels at that resolution,
// else as the layered exports draw it), folders as groups, correction layers as adjustment layers (Genko's own form of
// Python's kinds by the nearest, said), fill layers (a colour, a gradient laid out over the canvas, a pattern of the
// book's), pen layers of one shape part as shape layers (more parts or lines with them: a group), smart objects as smart
// objects of their file (contents of a page: a .pcraft inside, written the same way), with their smart filters PhotoCraft
// has (core::to_upstream) and their mask; each layer's mask, vector mask, link, locks, clipping, opacity and blend; the
// page's channels and Quick Mask, paths, guides, patterns its fills use, colour table, duotone inks, profile, the book's
// File Info as XMP, the EXIF and the PSD's blocks it was opened with. What PhotoCraft has no place for — the pen's lines,
// the panels' borders, the lines of dialogue, the tones, the effect lines, the 3D, the rulers, the paper of a book's page —
// is written as pixel layers of its picture, as a PSD writes them (render::page_layers).

namespace genko::formats {

struct PcraftOptions {
    std::optional<int> dpi;  // the resolution the page is written at (none: the page's own)
};

// The page's .pcraft, its bytes handed to `sink` as they are made. Notes: what the file could not keep, in Japanese.
// core::Error("image_too_large") past what a .pcraft without zip64 holds (4 GiB) or the page's picture limits;
// core::Error("page_not_loaded") for a page not read yet; render::NotYetPorted for what this build does not draw yet.
std::vector<std::string> write_pcraft(const core::Document& doc, std::size_t page, const std::function<void(std::string_view)>& sink,
                                      const PcraftOptions& options = {});
// The same as bytes (a smart object's contents written inside another .pcraft; the tests).
std::string pcraft_bytes(const core::Document& doc, std::size_t page, const PcraftOptions& options = {}, std::vector<std::string>* notes = nullptr);
// Written to `dest` whole or not at all (beside it under another name, then moved into place).
std::vector<std::string> write_pcraft_file(const core::Document& doc, std::size_t page, const std::filesystem::path& dest,
                                           const PcraftOptions& options = {});

}  // namespace genko::formats
