#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// プリント… and 1 部プリント's sheet on the paper (PC5 I-f, SPEC.md AUTO-01「プリント」; PhotoCraft v0.2.0's
// engine/src/print_cmds.rs 33-206 and 257-370, MIT: the paper sizes, the marks' shapes and places, the layout, the colour
// handling — its PDF writer and `lp` are not used: Qt prints, app/print_dialog.cpp). Everything here is in millimetres from
// the paper's top-left corner; the window, the command line's `genko print` (and its --dry-run) draw what place() says.

namespace genko::formats::print {

// A paper of the command line (`--paper`): its name and size (portrait).
struct Paper {
    std::string name;
    double width_mm = 0.0;
    double height_mm = 0.0;
};
// a3, a4, a5, b4, b5 (JIS), letter, legal, tabloid, 4x6, 5x7.
const std::vector<Paper>& papers();
// A paper by its name (any case) or "<w>x<h>" in mm (each 25..2000). core::Error("value") for another.
Paper paper_of(std::string_view text);

// The marks put round the picture (Photoshop's プリント… → トリムマーク・センタートンボ・レジストレーションマーク・説明・ラベル).
struct Marks {
    bool corner = false;        // コーナーのトンボ (outside the picture, on the finished size's corners)
    bool center = false;        // センターのトンボ
    bool registration = false;  // レジストレーションマーク
    bool description = false;   // 説明 (the book's File Info's description, under the picture)
    bool labels = false;        // ラベル (the book's title and the page numbers, above the picture)
    bool lines() const { return corner || center || registration; }
    bool any() const { return lines() || description || labels; }
    bool operator==(const Marks&) const = default;
};
// "corner,center,registration,description,labels" (any of them; "" none). core::Error("value") for another word.
Marks marks_of(std::string_view list);
std::string marks_text(const Marks& marks);

// カラーマネジメント: the printer manages the colours (the page's colours as the screen's sRGB, as before) or Genko does —
// the page's own colours (an RGB page's values in its profile, a grey or CMYK page's inks; else the screen's sRGB) through
// Little CMS into the printer's profile, by the rendering intent, with black point compensation or not. Qt hands the
// printer an RGB picture: the printer's profile is an RGB one (a CMYK or grey profile is refused, and why is said).
struct Color {
    bool genko = false;
    std::string profile;               // a built-in profile's id (display-p3, adobe-rgb-compat …) or an ICC file's path
    std::string intent = "relative";   // perceptual | relative | saturation | absolute
    bool bpc = true;
    bool operator==(const Color&) const = default;
};
// The printer's profile's bytes (none: Little CMS's sRGB) once checked: core::Error("value") for one not found, not read by
// Little CMS, or not an RGB profile; an intent that is not one of the four.
std::optional<std::string> printer_profile(const Color& color);

// プリント…'s settings (the window's, the command line's; 1 部プリント's last ones: QSettings print/last).
struct Setup {
    std::string area = "trim";   // trim | bleed | paper (the whole paper with Genko's own crop marks drawn on the page)
    std::string scale = "fit";   // fit (用紙に合わせる) | actual (原寸) | percent (拡大・縮小)
    double percent = 100.0;      // (percent) 1..1000
    bool center = true;          // else at left_mm, top_mm from the paper's top-left corner
    double left_mm = 0.0;
    double top_mm = 0.0;
    bool spreads = false;
    Marks marks;
    Color color;
    int copies = 1;              // 1..999
    std::optional<int> dpi;      // (the command line's --dpi) the pages drawn at; none: the printer's (150..600)
    bool operator==(const Setup&) const = default;

    // {"area", "scale", "percent", "center", "left_mm", "top_mm", "spreads", "marks": [..], "color": {"genko", "profile",
    // "intent", "bpc"}, "copies"} — and back (what cannot be read keeps its default: the settings of an older build).
    core::Json json() const;
    static Setup from_json(const core::Json& value);
};
// What stops a setup before anything is drawn: core::Error("value") (an area, a scale, a percent, a copy count, a
// position outside the paper's metre, a dpi; the colours: printer_profile).
void check(const Setup& setup);

struct RectMm {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};
struct Line {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};
struct Circle {
    double x = 0.0, y = 0.0, r = 0.0;
};
struct Text {
    std::string kind;  // label | description
    std::string text;
    double x = 0.0, y = 0.0;  // the baseline's left end
};

// The marks' lines are this wide, the texts this big (PhotoCraft's 0.3 pt and 8 pt Helvetica).
inline constexpr double kMarkWidthMm = 0.3 * 25.4 / 72.0;
inline constexpr double kTextPt = 8.0;

// One sheet on the paper: the picture's box (its pixels at `dpi` drawn there), the finished size's box in it (the marks
// are on its edges), whether the picture goes past the printable part, and the marks.
struct Sheet {
    std::vector<std::int64_t> pages;
    int width_px = 0, height_px = 0, dpi = 0;
    RectMm paper, printable, image, trim;
    double scale = 1.0;    // 1.0: the page's real size
    bool outside = false;  // part of the picture is past the printable part (cut there)
    std::vector<Line> lines;
    std::vector<Circle> circles;
    std::vector<Text> texts;
    // {"pages", "pixels": [w, h], "dpi", "image_mm": [x, y, w, h], "trim_mm", "scale" (%), "outside", "marks":
    // {"lines": [[x0, y0, x1, y1]…], "circles": [[x, y, r]…]}, "texts": [{"kind", "text", "at_mm": [x, y]}]} (mm, 3 decimals)
    core::Json json() const;
};

// Where a picture of w × h pixels at `dpi` — its finished size `trim_px` in it (pixels) — goes on the paper whose printable
// part is `printable`, as the setup asks: fitted (inside the printable part less the room the marks take), at its size or
// scaled; centred on the printable part or at the position given; the marks round it.
Sheet place(int width_px, int height_px, int dpi, const RectMm& trim_px, const RectMm& paper, const RectMm& printable, const Setup& setup,
            const std::string& label = {}, const std::string& description = {});

// The finished size inside a page's picture cut to `area` at `dpi` (pixels).
RectMm trim_in(const core::Page& page, std::string_view area, int dpi);
// The label of a sheet's pages (the book's title and their numbers), and the description (the book's File Info's, or "").
std::string label_of(const core::Document& doc, const std::vector<const core::Page*>& pages);
std::string description_of(const core::Document& doc);

// A page as it prints at `dpi` (render_page in print mode, Genko's crop marks for the whole paper), cut to `area` — its
// colours handled as `color` says: by the printer, the screen's sRGB; by Genko, the printer profile's RGB ("RGB").
render::Image page_picture(const core::Document& doc, const core::Page& page, std::string_view area, int dpi, const Color& color = {});

}  // namespace genko::formats::print
