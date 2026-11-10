#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// The exports people start from the app, without Qt (Python's genko/app/exporting.py): the formats and their options,
// the pages to write, and run(), which writes one format into a folder and says what it wrote. Every format can be
// written freely; the checked "正式な書き出し" (official: preflight, the export approval recorded) goes through the
// studio, a layer above this one: run() hands it to RunOptions::official_export after Python's own refusals
// (service::run_export sets it), and refuses it without one instead of writing.

namespace genko::formats {

struct Format {
    std::string_view key;
    std::string_view label;
    std::string_view note;
    std::vector<std::string_view> options;  // dpi, area, width, max_height, long_edge, jpeg, spreads, color, icc, screen
    bool official = false;
};

// exporting.FORMATS, in their order.
const std::vector<Format>& formats();
// exporting.BY_KEY.get(key) (null for none).
const Format* format(std::string_view key);

// exporting.parse_pages: '3-5, 8' → [3, 4, 5, 8] (each once, in order; 、，〜～ read as , and -); PyValueError with
// Python's words ("ページの指定が読めません: …", "N ページはありません（1〜count）", "書き出すページがありません").
std::vector<std::int64_t> parse_pages(std::string_view text, std::int64_t count);

// exporting.subset: the book with only these pages (they keep their numbers; a spread missing its partner is written
// as a single page).
core::Document subset(const core::Document& episode, const std::vector<std::int64_t>& pages);

// exporting.default_dpi: 150 for epub and strip, else the book's (600 when it has none).
std::int64_t default_dpi(const core::Document& episode, std::string_view key);

// color-mode@1 (SPEC.md COLOR-02), color-model@1 (COLOR-03): the export's notes — the words saying how many pages of a
// colour model (CMYK, Lab, grey; RGB of a profile or of 16 or 32 bits) the format (a key of formats() or the command
// line's: pdf, tiff, cmyk, png, …) writes from the colours on screen rather than as their inks or values with this
// colour (auto, rgb, cmyk, gray, bitonal), how many of 32 bits it writes at 16, and how many of those a PDF cuts the HDR
// of (values past 0..1 an RGB or grey page holds, or an exposure correction may make). PDF and TIFF keep a page's inks
// with "auto" (a CMYK page's with "cmyk", a grey page's with "gray" too), an RGB page's values with "auto" or "rgb";
// PSD always (Photoshop's CMYK and Lab at 16 bits); a PDF holds 16 bits at most. run() puts them in its reply's
// "notes"; never converted without a word. (Only CMYK and Lab pages converted: color-mode@1's words.)
std::vector<std::string> export_notes(const core::Document& episode, std::string_view key, std::string_view color);
// The first of them (color-mode@1's: the pages converted), or none.
std::optional<std::string> ink_note(const core::Document& episode, std::string_view key, std::string_view color);
// The same for an animation of one page (a movie holds 8-bit sRGB only): a page of a colour model's, or none.
std::optional<std::string> ink_note(const core::Page& page);

// PC2a (file.export.colorLookupTables, the C++ build's own: `genko export --format cube`, 書き出し → カラールックアップ
// テーブル): the page's visible correction layers, in their order, laid on an identity lattice of `size`³ (held to
// 2..256) — each by its opacity and native.blend, its mask and clipping left out (a LUT is the stack's colour mapping,
// PhotoCraft's file_cmds.rs bake_cube) — as a .cube file (red fastest, values 0..1, 6 decimals). A layer this build does
// not draw (a plugin's, a filter that moves shapes, one not ported yet) is left out and named in `skipped`.
std::string bake_cube(const core::Document& doc, const core::Page& page, int size, const std::string& title,
                      std::vector<std::string>* skipped = nullptr);

struct RunOptions {
    bool official = false;
    std::string actor = "human:user";
    std::optional<std::int64_t> dpi;
    int width = 800;
    int max_height = 1280;
    int long_edge = 2048;
    bool jpeg = false;
    bool spreads = false;
    std::string area = "bleed";
    std::optional<std::vector<std::int64_t>> pages;
    std::string color = "auto";
    std::string icc;
    bool dots = false;
    std::optional<core::Json> screen;
    // (psd, PSD-01) the lines' text kept editable — "image" (as pictures: Python's PSD) or "editable" — and, editable,
    // a line using what PSD text cannot keep drawn as a picture (見た目優先) instead of stopping the export (厳密)
    std::string psd_text = "image";
    bool psd_rasterize = false;
    // The official export itself (HumanService.export, which the studio layer above this one has: service::run_export
    // sets it), called after Python's own refusals with the book's folder; without it the official export is refused
    // (code "not_yet_ported").
    std::function<core::Json(const std::filesystem::path& project)> official_export;
};

// exporting.run: {"ok": true, "files": [...]} or {"ok": false, "error": …} (Python's ValueError and OSError become the
// error; what this build does not do yet, render::NotYetPorted, too, with "code": "not_yet_ported"; a resolution past
// 1 to 100000 dpi, formats::check_dpi). `out` is a folder. The editable PSD (options.psd_text "editable") adds
// "psd_text" (PsdTextReport) to its reply, or stops with "code": "psd_text_unsupported" and the lines ("items"). A
// book whose pages are still being read (Document::deferred: the app's first page first) is not written from:
// core::Error("page_not_loaded") is thrown.
core::Json run(const core::Document& episode, const std::optional<std::filesystem::path>& project, std::string_view key,
               const std::filesystem::path& out, const RunOptions& options = {});

}  // namespace genko::formats
