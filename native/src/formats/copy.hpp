#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"

// コピーを保存… (file.saveACopy, PC4 F-b; PhotoCraft's file_cmds.rs save_a_copy; SPEC.md FILE-01): one page of the book as
// one file of the format its extension names, the book unchanged (its place, its name and its history as they were). PC4
// F-b's formats, and F-a's codecs:
//   - .png, .jpg / .jpeg, and since F-a .webp, .gif, .bmp, .tga, .ico, the Netpbm ones (.pnm .pbm .pgm .ppm .pam .pfm),
//     .qoi, .exr, .hdr, .avif: the page's picture at its resolution (page_picture) through the codecs
//     (render::codecs::encode: what the format holds of its depth, colours, profile, resolution and the book's XMP; the
//     losses said in the notes of the page's picture as it is, as the codecs' plan makes them; Netpbm's kind as its
//     extension names it) — for OpenEXR and Radiance HDR its colours as linear light of sRGB's primaries (Little CMS from
//     its profile; a grey page's OpenEXR its grey; an opened OpenEXR's linear-srgb page as it is), for a format that holds
//     neither its profile nor its CMYK its colours as sRGB's through its profile;
//   - .tif / .tiff: the page at its depth as the exports write a page's TIFF (an RGB page's values or an 8-bit picture,
//     a grey or CMYK page's inks, with its profile);
//   - .psd: the page with its layers as the exports write a page's PSD; `layers` false, its merged picture alone; (PC4
//     F-d) .psb the same as a PSB (a page past 30000 px a side or 2 GiB is no PSD: core::Error("psd_too_large"));
//   - .pcraft (PC4 F-f): the page as PhotoCraft's document (formats/pcraft_write.hpp); `layers` false, its merged picture
//     alone;
// each with the book's File Info (formats/file_xmp.hpp; none for a book with none). A .genko copy of the book is the
// window's (app/session.hpp Session::save_copy). F-c widens this (書き出し形式…, the scale, the metadata, `genko copy`) and
// F-a the formats the codecs write.

namespace genko::formats {

struct CopyOptions {
    bool layers = true;          // (PSD, .pcraft) its layers; false: its merged picture alone
    std::optional<int> quality;  // (JPEG) Photoshop's 0..12 (PhotoCraft's round(q / 12 × 99 + 1)); none: 10
};

struct CopyResult {
    std::filesystem::path file;
    std::vector<std::string> notes;  // what the file could not keep of the page, in Japanese
};

// The extensions write_copy writes (lower case, no dot), the default first.
std::span<const std::string_view> copy_extensions();

// The copy made in memory before anything is written (plan_copy: the page's picture and the file's bytes — the heavy
// work, on any thread —, and what the file will not keep of the page, said before it is written), then written
// (write_planned: the bytes put in place, or the PSD written; the file as it was until then).
struct PlannedCopy {
    std::filesystem::path file;
    std::vector<std::string> notes;         // what the file will not keep of the page, in Japanese
    std::string bytes;                      // (PNG, JPEG, TIFF) the file's bytes
    std::optional<core::Document> psd_book; // (PSD) the book whose page is written as the PSD — the merged one's own book
    std::size_t psd_page = 0;
    bool psb = false;                       // (PC4 F-d) a PSB
    int dpi = 0;
};
PlannedCopy plan_copy(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const CopyOptions& options = {});
CopyResult write_planned(const PlannedCopy& plan);

// plan_copy, then write_planned. render::NotYetPorted("image_write") for an extension it does not write yet;
// core::Error("page_not_loaded") for a page not read yet; the exports' errors.
CopyResult write_copy(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const CopyOptions& options = {});

// Photoshop's JPEG quality (0..12) as libjpeg's (1..100).
int jpeg_quality_of(int photoshop);

}  // namespace genko::formats
