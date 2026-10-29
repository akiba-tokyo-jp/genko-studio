#pragma once

#include <QString>

#include "core/model.hpp"
#include "core/units.hpp"

class QComboBox;
class QDoubleSpinBox;

// 環境設定 → 単位・定規 as the screen uses it (PC5 H-b, edit.preferences.unitsAndRulers, SPEC.md VIEW-01「単位・定規」): the
// ruler's unit (定規, the guides' windows, the grid's spacing, 情報, 画像解像度…, カンバスサイズ…), the points to the inch, the
// screen's resolution (プリントサイズ), a column and its gutter (列). The book's lengths stay mm: a number shown in the unit
// is turned back into mm before it reaches an op. The pen's width and the manga tools' other mm fields keep their mm (節 H
// 8-10 (a), the coordinator's decision).

namespace genko::app::view_units {

core::units::Unit ruler();
core::units::Unit type_unit();
double points_per_inch();
double screen_dpi();
double print_dpi();
double column_pt();
double gutter_pt();

// What a length of this page needs to be in a unit (`across`: along its width — a percent of its width).
core::units::Basis basis(const core::Page* page, bool across);
// A length (mm) as words in the unit: "12.5 mm", "1476 px".
QString words(double mm, core::units::Unit unit, const core::units::Basis& basis);

// A spin box of a length in `unit` (its suffix, decimals and range): the value given in mm and read back in mm.
void set_unit(QDoubleSpinBox* box, core::units::Unit unit, const core::units::Basis& basis, double mm, double max_mm = 100000);
double mm_of(const QDoubleSpinBox* box, core::units::Unit unit, const core::units::Basis& basis);
// The ruler's units (Photoshop's list), the chosen one first selected.
QComboBox* unit_box(core::units::Unit chosen, bool with_columns = false);

}  // namespace genko::app::view_units
