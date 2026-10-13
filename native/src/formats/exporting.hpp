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

// color-mode@1 (SPEC.md COLOR-02): the words saying how many CMYK and Lab pages the format (a key of formats() or the
// command line's: pdf, tiff, cmyk, png, …) writes from the colours on screen rather than as their inks with this
// colour (auto, rgb, cmyk, gray, bitonal), or none. PDF and TIFF keep a page's inks with "auto" (a CMYK page's with
// "cmyk" too), PSD always. run() puts them in its reply's "notes"; never converted without a word.
std::optional<std::string> ink_note(const core::Document& episode, std::string_view key, std::string_view color);

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
