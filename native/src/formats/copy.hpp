#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "formats/page_picture.hpp"
#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/options.hpp"
#include "render/codecs/resize.hpp"

namespace genko::formats::detail {
class PartFile;  // (formats/output.hpp)
}

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
// window's (app/session.hpp Session::save_copy). F-a widens the formats the codecs write.
//
// PC4 F-c (書き出し形式…, クイック書き出し, レイヤーを書き出し, `genko copy`; PhotoCraft's export_dialog.rs and
// layer_menu_cmds.rs layer_document, MIT): the same copy at another size — the page drawn at the resolution its scale asks
// (its book's × scale; a width or a height asked exactly: drawn at the nearest resolution, then made that size by the
// resample chosen), its resolution in the file the one that keeps its size on paper —, cut to its finished size (area),
// one layer alone (drawn over nothing, cut to what it holds; without transparency — or in a format without alpha, JPEG's
// and the others' — over white), its colours converted to sRGB when asked, the metadata chosen (none; the copyright and
// contact; all — the book's File Info and the page's EXIF, upright, its thumbnail gone and its sizes the picture's —; a
// PSD's, a PSB's and a .pcraft's book trimmed alike), JPEG's, a lossy WebP's and AVIF's quality in 1..100. Every picture format's colours go one way (encode_for):
// what the format cannot hold of the page's colours — a profile, CMYK — through Little CMS to sRGB, the same for every
// format. A picture past the codecs' limits (render/codecs/limits.hpp: 120 million pixels, 100000 a side, its bytes and
// the work's within 2 GiB, counted as 8 bytes a pixel a byte of depth: a measure, not the heap's peak) is refused before it
// is drawn (core::Error("image_too_large")).

namespace genko::formats {

// (PC4 F-c) Which of the book's metadata a picture's file carries: none; the copyright and contact (Photoshop's
// 著作権情報と連絡先情報: dc:rights, xmpRights:Marked and WebStatement, the author and the author's title, in a packet of
// their own — no camera, no place); all (the book's File Info in the packet it was opened with, and the page's EXIF,
// upright). PhotoCraft's prefs.rs ExportMetadata: none | copyright | all.
enum class Metadata { None, Copyright, All };
std::optional<Metadata> metadata_of(std::string_view id);
std::string_view to_string(Metadata metadata);

struct CopyOptions {
    bool layers = true;          // (PSD, .pcraft) its layers; false: its merged picture alone
    std::optional<int> quality;  // (JPEG) Photoshop's 0..12 (PhotoCraft's round(q / 12 × 99 + 1)); none: 10
    // PC4 F-c:
    std::optional<int> quality_percent;  // 1..100: JPEG's, AVIF's and a lossy WebP's (wins over `quality`); a WebP given a quality is lossy
    double scale = 100.0;                // percent, 1..1000: drawn at the book's resolution × scale
    std::optional<std::uint32_t> width;  // px asked exactly (wins over scale; one alone: the other in proportion)
    std::optional<std::uint32_t> height;
    render::codecs::ResizeFilter resample = render::codecs::ResizeFilter::Bicubic;
    std::string area = "paper";          // paper | bleed | trim (仕上がりで切る)
    std::optional<std::string> layer;    // that layer (its id) alone, cut to what it holds
    bool transparency = true;            // (a layer) false: over white
    std::optional<Metadata> metadata;    // none: the book's File Info as コピーを保存… writes it
    bool srgb = false;                   // the colours converted to sRGB (no profile in the file)
    bool editable_text = false;          // (PSD, PSB) the lines kept as text layers (PSD-01)
};

struct CopyResult {
    std::filesystem::path file;
    std::vector<std::string> notes;  // what the file could not keep of the page, in Japanese
    // (PC4 F-c) the picture written (0: a PSD, a PSB or a .pcraft of the page's own size) and the file's bytes
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t bytes = 0;
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
    // (PC4 F-f: a .pcraft) the file written beside `file`, moved over it by write_planned (removed when the plan is let go)
    std::shared_ptr<detail::PartFile> part;
    bool editable_text = false;            // (PC4 F-c: a PSD's lines as text layers)
    std::uint32_t width = 0, height = 0;   // (PC4 F-c) the picture's
};
PlannedCopy plan_copy(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const CopyOptions& options = {});
CopyResult write_planned(const PlannedCopy& plan);

// plan_copy, then write_planned. render::NotYetPorted("image_write") for an extension it does not write yet;
// core::Error("page_not_loaded") for a page not read yet; the exports' errors.
CopyResult write_copy(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const CopyOptions& options = {});

// Photoshop's JPEG quality (0..12) as libjpeg's (1..100).
int jpeg_quality_of(int photoshop);

// --- PC4 F-c ---

// The size a page's picture is written at with these options (a layer's: its page's — what it holds is known only once it
// is drawn), its resolution in the file, and the resolution it is drawn at.
struct CopySize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double dpi = 0;     // the file's (the size on paper kept)
    int drawn_dpi = 0;  // the page drawn at
};
CopySize copy_size(const core::Document& doc, std::size_t page, const CopyOptions& options = {});
// The size checks a copy as `extension` makes before anything is drawn (core::Error("image_too_large")): the codecs'
// limits of what is drawn and written, and what the format holds (GIF's, an icon's, WebP's …) — estimate_copy's too.
void check_copy_size(const core::Document& doc, std::size_t page, std::string_view extension, const CopyOptions& options = {});
// The page's EXIF a copy carries with all its metadata (a picture it was opened from; none for any other): upright, the
// camera's thumbnail gone and the sizes it says the written picture's (render::codecs::exif_for_copy).
std::optional<std::string> copy_exif(const core::Document& doc, const core::Page& page, std::uint32_t width, std::uint32_t height);

// The picture a copy writes: the page — or its layer alone — at the options' size and area, with the metadata chosen and
// its resolution; its colours as the page has them (encode_for converts them). The size is checked against the codecs'
// limits before anything is drawn.
PagePicture copy_picture(const core::Document& doc, std::size_t page, const CopyOptions& options = {});

// The picture as `format` writes it, by one rule for every way a picture goes out: converted to sRGB (Little CMS, in bands)
// when `srgb`, or when the format holds neither its profile nor its CMYK (OpenEXR and Radiance HDR: as linear light of
// sRGB's primaries, an opened OpenEXR's linear-srgb page as it is); what the file does not keep said in `notes` (the codecs'
// losses of the picture as it is).
std::string encode_for(render::codecs::CodecImage image, render::codecs::Format format, const render::codecs::EncodeOptions& encode, bool srgb,
                       std::vector<std::string>& notes);
// The picture's colours in sRGB (its profile's, its CMYK's, a grey's through its grey): RGB(A) — `grey` a grey picture's
// grey(A) —, at its depth (`linear`: floats of linear light with the built-in linear-srgb), no profile. Little CMS in bands:
// never the picture held again whole as floats.
render::codecs::CodecImage srgb_picture(const render::codecs::CodecImage& picture, bool linear, bool grey);

// The EncodeOptions of a copy's options (its quality, a WebP lossy when it is given one).
render::codecs::EncodeOptions encode_options(const CopyOptions& options, std::string_view extension);

// Pages (indices) each a file in `folder`: <file_stem(page)>.<extension> (p001.png, cover_front.png …), each made and
// written in turn (one page's picture held at a time); a page not read yet is core::Error("page_not_loaded"). The files
// written before one that fails stay.
std::vector<CopyResult> copy_pages(const core::Document& doc, const std::vector<std::size_t>& pages, const std::filesystem::path& folder,
                                   std::string_view extension, const CopyOptions& options = {});

// What a copy would be, written nowhere: exact (the file made, counted and let go) or, with `proxy` (a long side in pixels:
// 書き出し形式…'s estimate, fast on a large page), from the picture made at most that big, its bytes scaled by its pixels
// (PhotoCraft's export_dialog.rs estimate).
struct CopyEstimate {
    std::uint64_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool exact = true;
    std::vector<std::string> notes;
};
CopyEstimate estimate_copy(const core::Document& doc, std::size_t page, std::string_view extension, const CopyOptions& options = {},
                           std::optional<std::uint32_t> proxy = std::nullopt);

}  // namespace genko::formats
