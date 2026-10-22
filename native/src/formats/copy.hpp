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
// F-b's formats:
//   - .png, .jpg / .jpeg: the page's picture at its resolution (page_picture) through the codecs (render::codecs::encode:
//     what the format holds of its depth and colours; the losses said in the notes, as the codecs' plan makes them);
//   - .tif / .tiff: the page at its depth as the exports write a page's TIFF (an RGB page's values or an 8-bit picture,
//     a grey or CMYK page's inks, with its profile);
//   - .psd: the page with its layers as the exports write a page's PSD; `layers` false, its merged picture alone;
// each with the book's File Info (formats/file_xmp.hpp; none for a book with none). A .genko copy of the book is the
// window's (app/session.hpp Session::save_copy). F-c widens this (書き出し形式…, the scale, the metadata, `genko copy`) and
// F-a the formats the codecs write.

namespace genko::formats {

struct CopyOptions {
    bool layers = true;          // (PSD) its layers; false: its merged picture alone
    std::optional<int> quality;  // (JPEG) Photoshop's 0..12 (PhotoCraft's round(q / 12 × 99 + 1)); none: 10
};

struct CopyResult {
    std::filesystem::path file;
    std::vector<std::string> notes;  // what the file could not keep of the page, in Japanese
};

// The extensions write_copy writes (lower case, no dot), the default first.
std::span<const std::string_view> copy_extensions();

// render::NotYetPorted("image_write") for an extension it does not write yet; core::Error("page_not_loaded") for a page
// not read yet; the exports' errors.
CopyResult write_copy(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const CopyOptions& options = {});

// Photoshop's JPEG quality (0..12) as libjpeg's (1..100).
int jpeg_quality_of(int photoshop);

}  // namespace genko::formats
