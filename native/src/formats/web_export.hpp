#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/copy.hpp"
#include "render/codecs/resize.hpp"
#include "render/codecs/web.hpp"
#include "render/quantize.hpp"

// Web 用に保存（従来）… (file.export.saveForWebLegacy), 書き出しの環境設定… (file.export.exportPreferences) and クイック書き出し
// (file.export.quickExportAsPng, file.export.quickExport, layer.quickExportAsPng) — PC4 F-c, SPEC.md FILE-01; PhotoCraft
// v0.2.0 crates/engine/src/web_cmds.rs (MIT): its settings, presets, optimiser and the quick export that heeds the export
// preferences.
//
// Save for Web optimises a page — or a layer alone, cut to what it holds — as GIF, PNG-8, PNG-24, JPEG or WBMP: the picture
// at its size (formats/copy.hpp's copy_picture: the page drawn at the resolution the size asks), in sRGB's colours (a page
// of a profile with them converted, or kept and its profile embedded; a grey or CMYK page always through Little CMS), as
// 8-bit RGBA; colours reduced by render/quantize's palettes (a large picture's palette from an even sample of about a
// million pixels) and mapped through an inverse colour map (each 4×4×4 cell of RGB resolved once), dithered as asked;
// edges over the matte; Web Snap moving palette entries near the web-safe cube onto it. Without a file, only the size of
// what it would write (the dialog's estimate: its picture at most `proxy` long, scaled back by the pixels). The file is
// written as it is made, beside its place, then moved in (render/codecs/web's writers hand their bytes to a sink): a
// picture past the codecs' limits is refused before it is drawn. Slices and their HTML table are PC5's.

namespace genko::formats {

enum class WebFormat { Gif, Png8, Png24, Jpeg, Wbmp };
// gif | png8 | png24 | jpeg | wbmp (and PhotoCraft's other names: png-8, png, png-24, png32, jpg), or none.
std::optional<WebFormat> web_format_of(std::string_view id);
std::string_view to_string(WebFormat format);  // gif, png8, png24, jpeg, wbmp
std::string_view web_extension(WebFormat format);  // gif, png, png, jpg, wbmp

// Which metadata Save for Web puts in a JPEG and a PNG-24 (Photoshop's 4 of them; GIF, PNG-8 and WBMP hold none).
enum class WebMetadata { None, Copyright, CopyrightAndContact, All };

struct WebSettings {
    WebFormat format = WebFormat::Png24;
    render::quantize::PaletteKind palette = render::quantize::PaletteKind::Selective;
    std::size_t colors = 256;  // 2..256
    render::quantize::Dither dither = render::quantize::Dither::Diffusion;
    float dither_amount = 0.88f;  // 0..1
    bool transparency = true;
    std::optional<std::array<float, 3>> matte = std::array<float, 3>{1.0f, 1.0f, 1.0f};  // none: no matte (alpha cut at ½)
    bool interlaced = false;
    float web_snap = 0.0f;  // 0..1
    int quality = 60;       // 0..100 (JPEG)
    bool progressive = false;
    bool optimized = true;
    bool embed_icc = false;
    WebMetadata metadata = WebMetadata::Copyright;
    bool convert_to_srgb = true;

    bool operator==(const WebSettings&) const = default;
};

// Photoshop's named presets (PhotoCraft's 12, in its order) and their settings; JPEG Maximum and JPEG Very High are known by
// name too.
std::span<const std::string_view> web_presets();
std::optional<WebSettings> web_preset(std::string_view name);
// PhotoCraft's parameters ({"preset"?, "format"?, "palette"?, "colors"?, "dither"?, "ditherAmount"? 0..100, "transparency"?,
// "matte"? "#rrggbb"|"none", "interlaced"?, "webSnap"? 0..100, "quality"? 0..100, "progressive"?, "optimized"?, "embedIcc"?,
// "metadata"? none|copyright|copyrightAndContact|all, "convertToSrgb"?}): the preset first, then the keys given; a value
// this does not know is core::Error("value") naming it. to_json is its inverse.
WebSettings web_settings_from_json(const core::Json& params);
core::Json to_json(const WebSettings& settings);

// The picture's size ({"percent"?, "width"?, "height"?, "resample"? bicubic|bilinear|nearest}): none of them, its own.
struct WebSize {
    std::optional<double> percent;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    render::codecs::ResizeFilter resample = render::codecs::ResizeFilter::Bicubic;
};
WebSize web_size_from_json(const core::Json& params);

// A straight 8-bit RGBA picture, rows from the top.
struct Rgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> px;
};

struct Optimized {
    std::uint64_t bytes = 0;
    std::string ext;  // gif, png, jpg, wbmp
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::size_t> colors;   // (GIF, PNG-8, WBMP) the palette's entries
    std::vector<std::uint8_t> preview;   // (asked) what the file shows, straight RGBA8
};
// The picture optimised as the settings say, its bytes handed to `sink` (none: only counted); `icc` embedded in a JPEG or a
// PNG-24 when embed_icc, `xmp` put in them, `exif` in a JPEG, `dpi` their resolution.
Optimized optimize(const Rgba8& picture, const WebSettings& settings, const std::optional<std::string>& icc, const std::optional<std::string>& xmp,
                   double dpi, bool preview, const render::codecs::web::Sink* sink = nullptr, const std::optional<std::string>& exif = std::nullopt);

// The size of a file whose proxy made `bytes` and that has `ratio` times its pixels: what a file has whatever its size (from
// the same file of one pixel) and the rest scaled by the pixels (the dialog's estimates).
std::uint64_t estimated_bytes(const Rgba8& proxy, const WebSettings& settings, const std::optional<std::string>& icc,
                              const std::optional<std::string>& xmp, double dpi, std::uint64_t bytes, double ratio,
                              const std::optional<std::string>& exif = std::nullopt);

// The picture Save for Web works on: the page (or its layer alone, cut to what it holds) at the size, its colours sRGB's
// (`icc` none) or, an RGB page of a profile kept as it is, its own (`icc` its profile); `proxy`: at most that long. A page
// larger than its format holds (GIF's 65535, JPEG's 65500 a side) is refused before it is drawn.
struct WebPicture {
    Rgba8 rgba;
    std::optional<std::string> icc;
    // (a JPEG with all the metadata: the page's EXIF as a copy carries it — formats::copy_exif —, as Photoshop's Save for
    // Web puts it in a JPEG alone)
    std::optional<std::string> exif;
    double dpi = 72;
    std::uint32_t full_width = 0;   // (proxy) the size the file would be
    std::uint32_t full_height = 0;
    std::vector<std::string> notes;
};
WebPicture web_picture(const core::Document& doc, std::size_t page, const WebSettings& settings, const WebSize& size = {},
                       const std::optional<std::string>& layer = std::nullopt, std::optional<std::uint32_t> proxy = std::nullopt);

// The XMP the metadata choice puts in (PhotoCraft's web_xmp): none; the copyright, its status and URL (Copyright); with the
// author and the author's title (CopyrightAndContact) — a packet of their own —; the book's File Info (All).
std::optional<std::string> web_xmp(const core::Document& doc, WebMetadata metadata);

struct WebResult {
    std::optional<std::filesystem::path> file;  // (none: the estimate)
    std::string ext;
    std::uint64_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::size_t> colors;
    bool exact = true;
    std::vector<std::string> notes;
};
// Web 用に保存: the page optimised and written to `dest` (whole or not at all), or — none — only its size (`proxy`: from the
// picture made at most that long, scaled back by its pixels).
WebResult save_for_web(const core::Document& doc, std::size_t page, const WebSettings& settings, const WebSize& size = {},
                       const std::optional<std::filesystem::path>& dest = std::nullopt, const std::optional<std::string>& layer = std::nullopt,
                       std::optional<std::uint32_t> proxy = std::nullopt);

// --- 書き出しの環境設定 and クイック書き出し ---

enum class QuickLocation { Ask, SameFolder, Exports };
std::optional<QuickLocation> quick_location_of(std::string_view id);  // ask | sameFolder | exports
std::string_view to_string(QuickLocation location);

// The export preferences (PhotoCraft's prefs.rs Export, Photoshop's 書き出しの環境設定): クイック書き出しの形式 png | jpg | gif |
// webp, its place (ask each time; beside the book; the book's export folder — <name>_書き出し beside it, 原稿を書き出す…'s),
// the JPEG's quality (1..100), the metadata (none | copyright: the copyright and contact | all) and whether the colours are
// converted to sRGB.
struct ExportPreferences {
    std::string format = "png";
    QuickLocation location = QuickLocation::Ask;
    int jpeg_quality = 85;
    Metadata metadata = Metadata::Copyright;
    bool srgb = true;

    bool operator==(const ExportPreferences&) const = default;
};
inline constexpr std::array<std::string_view, 4> kQuickFormats{"png", "jpg", "gif", "webp"};

// The file a quick export writes without asking — <book>_<page>[_<layer>].<format> beside the book, or in its export
// folder —, or none (asking each time, or a book never saved: the window asks). (The coordinator's P3-8) A file already
// there is never the place: <name>-2.<format>, -3 … the first free one.
std::optional<std::filesystem::path> quick_export_place(const ExportPreferences& prefs, const std::optional<std::filesystem::path>& book,
                                                        const core::Page& page, const core::Layer* layer = nullptr);
// The quick export (PhotoCraft's quick_export): png, jpg and gif through Save for Web's optimiser (PNG-24 — a layer's
// transparency kept —, JPEG at the quality over white, GIF of 256 colours dithered), webp through the codecs (lossless);
// the metadata and the colours as the preferences say.
CopyResult quick_export(const core::Document& doc, std::size_t page, const std::filesystem::path& dest, const ExportPreferences& prefs,
                        const std::optional<std::string>& layer = std::nullopt);

}  // namespace genko::formats
