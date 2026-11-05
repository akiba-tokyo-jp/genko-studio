#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 単位・定規 (PC5 H-b, edit.preferences.unitsAndRulers, SPEC.md VIEW-01; PhotoCraft v0.2.0's prefs.rs Unit, MIT —
// native/third_party/photocraft/NOTICE.md): the lengths the screen shows in the unit the person chose — 定規 (the scales
// along the canvas), the guides' windows, the grid, the 情報 panel, 画像解像度… and カンバスサイズ…. The book keeps its mm
// (as before: an op never reads the unit); a length is turned into the unit and back at the screen. Pixels are the page's
// (its resolution), percent a side of the page (or of what the window sizes), points PostScript's 72 to the inch or the
// traditional 72.27 (環境設定 → 単位・定規 → ポイント/パイカの単位).

namespace genko::core::units {

enum class Unit { Px, In, Cm, Mm, Pt, Pica, Percent, Columns };

// The units 定規 can be in (Photoshop's order: pixel, inch, cm, mm, pt, pica, %).
const std::vector<Unit>& ruler_units();
// The value kept in the settings ("px", "in", "cm", "mm", "pt", "pica", "percent", "columns"), and back (none: not one).
std::string key_of(Unit unit);
std::optional<Unit> unit_of(std::string_view key);
// The screen's word for it (Photoshop's: pixel, inch, cm, mm, pt, pica, %, 列) and the short suffix of a number in it.
std::string label_of(Unit unit);
std::string suffix_of(Unit unit);
// How many decimals a number in it is shown with (upstream's: pixels none, pt / % / mm one, the rest two).
int decimals_of(Unit unit);

// What a length's unit needs besides the length: the page's resolution (px), the side it is a percent of (mm), the points
// to the inch, and a column's width and the gutter between columns (points: 段組の幅・間隔).
struct Basis {
    double dpi = 600;
    double extent_mm = 100;
    double points_per_inch = 72;
    double column_pt = 180;
    double gutter_pt = 12;
};

// A length in mm in the unit, and a length in the unit in mm.
double from_mm(double mm, Unit unit, const Basis& basis);
double to_mm(double value, Unit unit, const Basis& basis);

// Points to the inch: "postscript" 72, "traditional" 72.27.
double points_per_inch(std::string_view point_size);

}  // namespace genko::core::units
