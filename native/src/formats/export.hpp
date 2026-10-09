#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// The exports (Python's genko/export.py, profiles.py, pack.py and psd.py's writer), writing the same files: the same
// names, and the same bytes where Python's are fixed (PNG, TIFF, JPEG, PDF, PSD, the text files; formats/pillow_save.hpp
// writes as Pillow writes). Where Python puts the time in a file, so does this: the sRGB profile embedded in RGB files
// (Little CMS stamps it when it is made), an EPUB's zip entries and its dcterms:modified.
//
// The pages are drawn with render::render_page (the caller registers the book's own brushes first, as Python's reader
// does); what this build does not draw yet stops the export with render::NotYetPorted. Python's ValueError is
// core::PyValueError, OSError core::Error("io"), KeyError core::Error("key").
//
// Every file of an export is written beside its place and moved there when the whole export has been made
// (formats/output.hpp): a failed export leaves none of them. The folders are made as Python makes them.

namespace genko::formats {

namespace fs = std::filesystem;

// export.safe_name: a file name that works on Windows, macOS and Linux (Japanese is kept): <>:"/\|?* and control
// characters made _, spaces around and dots and spaces at the end taken off, a reserved name (CON, COM1, …) given a
// leading _, at most 80 characters; `fallback` for nothing left.
std::string safe_name(std::string_view text, std::string_view fallback = "genko");
// export.stem: f"{safe_name(title)}_ep{episode:02d}" (PyValueError for an episode number that is a float).
std::string stem(const core::Document& episode);

inline constexpr std::array<std::string_view, 3> kAreas{"paper", "bleed", "trim"};
inline constexpr std::array<std::string_view, 5> kColors{"auto", "rgb", "cmyk", "gray", "bitonal"};
inline constexpr int kKindleLongEdge = 2560;  // px: what Amazon asks of comic pages

// export.crop_to: the page's picture cut to the paper (as it is), the bleed or the finished size.
render::Image crop_to(const render::Image& image, const core::Page& page, std::string_view area, int dpi);
// export.page_color: the colour a page is written in; auto: a monochrome book's pages in grey, its covers and a colour
// book's pages in RGB.
std::string page_color(const core::Page& page, std::string_view color);

// export.export_png_sequence: every page as `mode` draws it, <stem>_<p001|cover_…>.png.
std::vector<fs::path> export_png_sequence(const core::Document& episode, const fs::path& dest, int working_dpi = 150,
                                          std::string_view mode = "print");

// export.export_print: the print pages. fmt png | png1 | tiff | pdf | cmyk; dpi (nothing or 0: the book's, else 600);
// area paper (with crop marks when `crop_marks`) | bleed | trim; color auto | rgb (sRGB, its profile embedded) | cmyk
// (TIFF or PDF, through the CMYK profile `icc` when given) | gray | bitonal; `screen` ({lpi, angle, shape, …}: the
// greys of a black and white page as dots). PyValueError with Python's messages for a wrong area, colour, format or
// screen, and for a profile that is not a CMYK printing profile.
std::vector<fs::path> export_print(const core::Document& episode, const fs::path& dest, std::string fmt = "png",
                                   std::optional<std::int64_t> dpi = std::nullopt, int threshold = 180, bool crop_marks = true,
                                   std::string_view area = "paper", std::string_view color = "auto", const std::string& icc = {},
                                   const std::optional<core::Json>& screen = std::nullopt);

// export.export_layers: every page's layers (render::page_layers) as transparent PNGs, a folder per page
// (<stem>_<p001>/01_紙.png, …), a layer's mask applied, cut to the area.
std::vector<fs::path> export_layers(const core::Document& episode, const fs::path& dest, int dpi = 350, std::string_view area = "paper");

// export.export_strip: the pages that are not covers one under another, as one PNG at `dest`.
fs::path export_strip(const core::Document& episode, const fs::path& dest, int dpi = 150);

// export.export_epub: EPUB 3, fixed layout, one page picture per spine item (the front cover first, the back cover
// last; a jacket's or a band's covers cut out of it), the pages cut to the finished size and their tones flat greys
// (`dots`: the print's dots). `long_edge` scales every page to that many pixels on its long side; `kindle` adds what
// Kindle devices read; `gray` and `jpeg` as they say.
fs::path export_epub(const core::Document& episode, const fs::path& dest, int dpi = 150, bool kindle = false,
                     std::optional<int> long_edge = std::nullopt, bool gray = false, bool jpeg = false, bool dots = false);
// export.export_kindle: export_epub for Kindle (JPEG, every page `long_edge` px on its long side; `gray`: nothing for
// a monochrome book in grey).
fs::path export_kindle(const core::Document& episode, const fs::path& dest, int long_edge = kKindleLongEdge,
                       std::optional<bool> gray = std::nullopt, bool dots = false);

// profiles.trimmed: the page as read on screen (no bleed, no dots, no crop marks).
render::Image trimmed(const core::Page& page, const core::Document& episode, int dpi);
// profiles.export_webtoon: every page `width_px` wide, stacked (`gap_px` apart) and cut into slices of at most
// `max_height` px: 001.png, 002.png, … (fmt "jpeg": .jpg at `quality`), sRGB, optimized.
std::vector<fs::path> export_webtoon(const core::Document& episode, const fs::path& dest, int width_px = 800, int max_height = 1280,
                                     int gap_px = 0, std::string_view fmt = "png", int quality = 92);
// profiles.export_sns: one picture a page with its long edge at `long_edge`; `spreads` adds each spread as one picture.
std::vector<fs::path> export_sns(const core::Document& episode, const fs::path& dest, int long_edge = 2048, std::string_view fmt = "jpeg",
                                 int quality = 92, bool spreads = false);

// pack.export_pack: the submission pack at the print resolution: 1-bit TIFFs, PNGs, list.csv and README.txt.
std::vector<fs::path> export_pack(const core::Document& episode, const fs::path& dest, std::string_view preset = "shueisha",
                                  std::optional<std::int64_t> dpi = std::nullopt);

// psd.export_page_psd: the page as a layered PSD (the printed page as its merged picture, render::page_layers as its
// layers, each cut to what it holds, PackBits); dpi nothing or 0: the book's, else 600.
fs::path export_page_psd(const core::Document& episode, const core::Page& page, const fs::path& dest,
                         std::optional<std::int64_t> dpi = std::nullopt);
// psd.export_psd_pages: one PSD a page in the folder `dest` (<stem>_p001.psd, …).
std::vector<fs::path> export_psd_pages(const core::Document& episode, const fs::path& dest, std::optional<std::int64_t> dpi = std::nullopt);
// psd.export_psd: one page (page number `page`) as a PSD at `dest` (core::Error("key") when there is no such page:
// Python's StopIteration).
fs::path export_psd(const core::Document& episode, const fs::path& dest, std::int64_t dpi = 150, std::int64_t page = 1);

namespace detail {
// f"{value:0{width}d}": the sign counted in the width (PyValueError for a float, as Python's format).
std::string padded(const core::Num& value, std::size_t width);
// Text as Path.write_text writes it ("\n" as "\r\n" on Windows).
std::string native_text(std::string text);
// Path(dest) / name as pathlib joins them ("." / "a" is "a").
fs::path join(const fs::path& dest, std::string_view name);
// Path.suffix: the name's last ".x" (not a leading dot, not a trailing one), "" for none.
std::string suffix(const fs::path& path);
// Path.with_suffix(suffix): the last suffix replaced (profiles._save: "Vol.2_ep01" becomes "Vol.jpg").
fs::path with_suffix(const fs::path& path, std::string_view suffix);
}  // namespace detail

}  // namespace genko::formats
