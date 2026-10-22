#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/model.hpp"

// The book's File Info in the files it is written to (PC4 F-b, file-info@1; SPEC.md FILE-01): an XMP packet — its
// title, author, author's title, description, keywords and copyright written into the packet it was opened with (the
// packet's other properties kept as they were) — that the exports (原稿を書き出す… and `genko export`: PNG, TIFF, PSD, the
// JPEG of sns) and コピーを保存… put in each file. A book with no File Info writes its files as before (byte for byte).

namespace genko::formats {

// The packet the book's files carry, or none for a book with no File Info. Its dc:title is the book's title — the
// packet's as it was while the book of a file keeps the title it was opened with (its file's name). The packet it was opened with is read from
// the book in memory (Document::native_files) or its assets (Document::asset_dir); one that cannot be read is made anew.
std::optional<std::string> book_xmp(const core::Document& doc);

// The file's bytes with the packet put in, as Photoshop puts it (PhotoCraft's codecs write it so):
//   - PNG: an iTXt chunk "XML:com.adobe.xmp" (uncompressed) before the first IDAT;
//   - JPEG: an APP1 segment "http://ns.adobe.com/xap/1.0/" after SOI and the JFIF header (a packet past one segment,
//     65502 bytes, is not put in: the bytes as they were);
//   - PSD and PSB: image resource 1060 after the others.
// (A TIFF's is its writer's: TiffSave::xmp, tag 700.)
std::string png_with_xmp(std::string png, std::string_view xmp);
std::string jpeg_with_xmp(std::string jpeg, std::string_view xmp);
std::string psd_with_xmp(std::string psd, std::string_view xmp);

}  // namespace genko::formats
