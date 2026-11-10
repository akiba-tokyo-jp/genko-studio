#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "formats/copy.hpp"
#include "formats/page_picture.hpp"

// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」; the user's decision 3: アートボード＝ページ): what the window and the
// command line write and read of the book's artboards — never an op (ops never read or write a path) —, after PhotoCraft
// v0.2.0's engine/src/artboard_cmds.rs (artboards_to_files, raster_pdf, artboards_to_pdf: 336-430) and crates/io (a PSD's
// artboards), MIT (native/third_party/photocraft/NOTICE.md). An artboard is a page, so each is written as the page's
// exports write it (write_copy: its depth, profile, File Info).

namespace genko::formats {

// The book's artboard pages (their positions), in the book's order; `names` those chosen (none: every one).
// core::Error("value") for a book without artboards, a name no artboard has, or a page not read yet ("page_not_loaded").
std::vector<std::size_t> artboard_pages(const core::Document& doc, const std::vector<std::string>& names = {});

// The page's picture as page_picture makes it — an artboard whose background is transparent with its alpha: what its layers
// and lines leave of the paper, found as trim_page finds it (drawn over a white and a black paper; where they differ
// nothing covers it), on an 8-bit RGB page (a page of precise colour or inks: over white, said in the notes).
PagePicture artboard_picture(const core::Page& page, const core::Document& doc, int dpi, std::string_view area = "paper");

// ファイル → 書き出し → アートボードからファイル… (file.export.artboardsToFiles): each artboard — or those chosen — written as one
// file by write_copy: <prefix>_<the artboard's name>.<extension> in `folder` (the prefix none: the book's title; "" given:
// the name alone; a name's marks a path or a file system takes made "_", cut by whole characters to 240 bytes; a name met
// again given _2, _3 …). The files written before one that fails stay.
struct ArtboardFiles {
    std::string extension = "png";            // one of copy_extensions()
    std::optional<std::string> prefix;        // none: the book's title (無題 when it has none)
    std::vector<std::string> artboards;       // none: every one
    CopyOptions options;                      // (JPEG's quality 0..12 in options.quality)
    // (the coordinator's P3-8, write_guard) each file's place before it is written: throws to write it and those after it not
    std::function<void(const std::filesystem::path&)> check;
};
std::vector<CopyResult> export_artboards(const core::Document& doc, const std::filesystem::path& folder, const ArtboardFiles& files);

// ファイル → 書き出し → アートボードから PDF… (file.export.artboardsToPdf): one PDF, a page an artboard — or those chosen —, each
// its picture as JPEG (Photoshop's quality 0..12, sRGB; a transparent background over white) on a PDF page of its size on
// paper (its pixels at its resolution).
struct ArtboardPdf {
    std::vector<std::string> artboards;  // none: every one
    int quality = 10;                    // 0..12
    std::function<void(const std::filesystem::path&)> check;
};
struct ArtboardPdfResult {
    std::filesystem::path file;
    std::size_t pages = 0;
    std::uint64_t bytes = 0;
    std::vector<std::string> notes;
};
ArtboardPdfResult export_artboards_pdf(const core::Document& doc, const std::filesystem::path& file, const ArtboardPdf& pdf);

// コピーを保存 / アートボードからファイル… の「アートボードを 1 つの PSD に」: the artboards — or those chosen — as one PSD (a PSB when
// `psb`), each artboard a group carrying its artb block (its board's place on the canvas, preset and background) — Photoshop's
// artboards. Each lies where its origin_px says (none: right of the ones placed, 100 pixels apart: PhotoCraft's GAP), the
// canvas their bounds. The artboards are to be of one colour mode and profile (core::Error("value") otherwise); of 8-bit and
// deeper RGB pages of sRGB, every one is made the deepest's depth first (on a copy). What goes on one page only — a page's
// guides, notes, comps, slices, channels and paths — is the first artboard's (said in the notes).
struct ArtboardPsd {
    std::vector<std::string> artboards;  // none: every one
    bool psb = false;
    std::function<void(const std::filesystem::path&)> check;
};
CopyResult export_artboards_psd(const core::Document& doc, const std::filesystem::path& file, const ArtboardPsd& psd);

// (open_file) A PSD's artboards — its top-level groups that keep an artb block (import_psd keeps it as a block it does not
// read) — each made a page of the book, an artboard: its layers (the group's, the group gone), the page cut to its board,
// its name the group's, its preset, background and place on the canvas kept. The layers outside every artboard make one
// page more (アートボードの外), last. What names layers over the whole file — its layer comps and slices — and the blocks the
// file kept are left (said in `notes`). A book of no artboards as it is.
void split_psd_artboards(core::Document& doc, std::vector<std::string>& notes);
// (open_file) The same for a .pcraft's artboards (render::pcraft::Read::artboards: each top-level group's id and PhotoCraft's
// Artboard — its rect, background and preset; one that cannot be read left a group, said).
void split_pcraft_artboards(core::Document& doc, const std::vector<std::pair<std::string, core::Json>>& artboards, std::vector<std::string>& notes);

}  // namespace genko::formats
