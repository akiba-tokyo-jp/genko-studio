#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"

// file-info@1 and image-grid@1 (PC4 節 F, docs/cpp-migration/schema-v4.md「ファイル・形式」): what a book keeps of the
// files it was opened from and of Photoshop's File Info, and the mark that its 8-bit pages work at their finest grid.
//
// - The book's `file_info` (ファイル情報…): the author, the author's title, the description, the keywords, the copyright
//   (its notice, status and URL) — PhotoCraft's eight XMP items less the title, which is the book's own `title` — and the
//   XMP packet read with them ("xmp": its asset, .xmp), whose other items are kept as they are. Empty items are not
//   written; a book with none has no `file_info`.
// - A page's `native_source`: the file the page was opened from (its format, name, size, resolution, depth, colours, the
//   EXIF and XMP it carried as assets, .exif and .xmp, and a camera's data). Only pages opened from a file have it.
// - The paper's `grid` ("finest", image-grid@1): an 8-bit page of this paper makes its new pixel layers, selections and
//   masks at the finest grid of its pixel layers (200 to 600 dpi) as a page in precise colour or inks does (the user's
//   decision for a page opened from an image or made as a new image; design 8-2 (b)).
// The book names file-info@1 while its file_info or a page's native_source is there, image-grid@1 while a paper has its
// grid. What is not well formed is refused (core::Error("value")): the reader opens such a book read-only.

namespace genko::core {

struct Document;
struct PageSpec;

inline constexpr std::string_view kFileInfoFeature = "file-info@1";
inline constexpr std::string_view kImageGridFeature = "image-grid@1";
// The suffixes of the metadata kept as assets: an XMP packet (UTF-8) and an EXIF block (TIFF structure, without JPEG's
// "Exif\0\0").
inline constexpr std::string_view kXmpSuffix = ".xmp";
inline constexpr std::string_view kExifSuffix = ".exif";

inline constexpr std::array<std::string_view, 3> kCopyrightStatuses{"unknown", "copyrighted", "public_domain"};
inline constexpr std::size_t kFileInfoTextMax = 2000;  // characters of one item (a keyword too)
inline constexpr std::size_t kKeywordsMax = 200;
// The grids a paper can name (image-grid@1).
inline constexpr std::array<std::string_view, 1> kGrids{"finest"};

struct FileInfo {
    std::string author;            // dc:creator
    std::string author_title;      // photoshop:AuthorsPosition
    std::string description;       // dc:description
    std::vector<std::string> keywords;  // dc:subject
    std::string copyright;         // dc:rights
    std::string copyright_status;  // xmpRights:Marked: unknown | copyrighted | public_domain ("": not said)
    std::string copyright_url;     // xmpRights:WebStatement
    std::string xmp;               // the packet read (an asset ref, "sha256:…"), "" for none

    bool empty() const;
    bool operator==(const FileInfo&) const = default;
};

// {"author"?, "author_title"?, "description"?, "keywords"?, "copyright"?, "copyright_status"?, "copyright_url"?, "xmp"?}
// in that order, the empty ones left out; and back, checked.
Json to_json(const FileInfo& info);
FileInfo file_info_from_json(const Json& value);
void check_file_info(const FileInfo& info);

// --- XMP (PC4 F-b; PhotoCraft v0.2.0 crates/engine/src/file_cmds.rs:330-497 read_file_info / write_file_info, MIT) ---
// The File Info an XMP packet says: dc:creator (its names joined by "; "), photoshop:AuthorsPosition, dc:description,
// dc:subject, dc:rights, xmpRights:Marked (True: copyrighted, False: public_domain, else not said) and
// xmpRights:WebStatement, as elements or as attributes of an rdf:Description; `xmp` left empty. Never throws: what is not
// there is empty.
FileInfo file_info_from_xmp(std::string_view packet);
// Its dc:title (the first rdf:li), "" for none.
std::string xmp_title(std::string_view packet);
// The packet with those eight items rewritten from `info` and the title from `title` — the empty ones removed, the others
// in an rdf:Description of their own at the end (several in a packet are XMP) — and every other property kept as it was;
// a new packet when there is none (or it holds no rdf:RDF). The author and the keywords are split at ";" and "," as
// PhotoCraft writes its Seq and Bag.
std::string write_xmp(std::optional<std::string_view> packet, const FileInfo& info, std::string_view title);

// The formats a page can be opened from (native_source's format) and the colours it says the file held.
inline constexpr std::array<std::string_view, 17> kSourceFormats{"png", "jpeg", "tiff", "webp", "gif", "bmp", "tga", "ico", "pnm",
                                                                  "qoi", "exr", "hdr", "avif", "psd", "psb", "raw", "pcraft"};
inline constexpr std::array<std::string_view, 12> kSourceModes{"gray",    "graya",  "rgb",     "rgba",         "cmyk",    "cmyka",
                                                                "lab",     "laba",   "indexed", "bitmap",       "duotone", "multichannel"};

// What a camera said of a picture (a RAW file's, or an image's EXIF): its maker and model, and what it knew of the shot.
struct CameraInfo {
    std::string make;
    std::string model;
    std::optional<double> iso;
    std::optional<double> exposure;  // seconds
    std::optional<double> aperture;  // f-number
    std::optional<double> focal;     // mm

    bool operator==(const CameraInfo&) const = default;
};

struct NativeSource {
    std::string format;  // kSourceFormats
    std::string name;    // the file's name (no folder)
    std::int64_t width_px = 0;
    std::int64_t height_px = 0;
    std::optional<std::pair<double, double>> dpi;  // the resolution the file said (none: it said none)
    int depth = 8;                                // bits a sample: 1, 8, 16, 32
    std::string mode = "rgb";                     // kSourceModes
    std::string exif;                             // asset refs ("sha256:…"), "" for none
    std::string xmp;
    std::optional<CameraInfo> camera;

    bool operator==(const NativeSource&) const = default;
};

// {"format", "name", "width_px", "height_px", "dpi"?: [x, y], "depth", "mode", "exif"?, "xmp"?, "camera"?: {"make",
// "model", "iso"?, "exposure"?, "aperture"?, "focal"?}} in that order; and back, checked.
Json to_json(const NativeSource& source);
NativeSource native_source_from_json(const Json& value);
void check_native_source(const NativeSource& source);

// A paper's grid read ("finest"), or core::Error("value").
std::string grid_from_json(const Json& value);

// Does the book name file-info@1 (its file_info, a page's native_source, on its pages or a contents page) or
// image-grid@1 (a grid on its paper, a page's or a contents page's)?
bool uses_file_info(const Document& doc);
bool uses_image_grid(const Document& doc);
// The book's file_info, native_sources and grids as they must be (core::Error("value") says what is not). Pages not read
// yet are not looked at.
void validate_file_info(const Document& doc);
// The assets file-info@1 names (each once, in the order met): its XMP and EXIF, with their suffix.
std::vector<std::pair<std::string, std::string_view>> file_info_assets(const Document& doc);

}  // namespace genko::core
