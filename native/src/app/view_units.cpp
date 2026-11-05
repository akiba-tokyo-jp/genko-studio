// 単位・定規 as the screen uses it (PC5 H-b): app/view_units.hpp.

#include "app/view_units.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>

#include <algorithm>
#include <cmath>

#include "app/config.hpp"
#include "core/prefs_table.hpp"

namespace genko::app::view_units {

namespace {

core::Json pref(const char* path) {
    const auto store = settings();
    return core::prefs::value(*store, path);
}

}  // namespace

core::units::Unit ruler() {
    return core::units::unit_of(pref("unitsAndRulers.rulers").get<std::string>()).value_or(core::units::Unit::Mm);
}

core::units::Unit type_unit() {
    return core::units::unit_of(pref("unitsAndRulers.typeUnits").get<std::string>()).value_or(core::units::Unit::Pt);
}

double points_per_inch() { return core::units::points_per_inch(pref("unitsAndRulers.pointSize").get<std::string>()); }
double screen_dpi() { return pref("unitsAndRulers.screenResolution").get<double>(); }
double print_dpi() { return pref("unitsAndRulers.printResolution").get<double>(); }
double column_pt() { return pref("unitsAndRulers.columnWidth").get<double>(); }
double gutter_pt() { return pref("unitsAndRulers.gutter").get<double>(); }

core::units::Basis basis(const core::Page* page, bool across) {
    core::units::Basis b;
    b.points_per_inch = points_per_inch();
    b.column_pt = column_pt();
    b.gutter_pt = gutter_pt();
    if (page != nullptr) {
        b.dpi = page->spec.dpi.value();
        b.extent_mm = across ? page->spec.width_mm.value() : page->spec.height_mm.value();
    }
    return b;
}

QString words(double mm, core::units::Unit unit, const core::units::Basis& basis) {
    return QStringLiteral("%1 %2")
        .arg(core::units::from_mm(mm, unit, basis), 0, 'f', core::units::decimals_of(unit))
        .arg(QString::fromStdString(core::units::suffix_of(unit)));
}

void set_unit(QDoubleSpinBox* box, core::units::Unit unit, const core::units::Basis& basis, double mm, double max_mm) {
    const QSignalBlocker quiet(box);
    const double top = core::units::from_mm(max_mm, unit, basis);
    box->setDecimals(std::max(2, core::units::decimals_of(unit)));
    box->setRange(-top, top);
    box->setSuffix(QStringLiteral(" ") + QString::fromStdString(core::units::suffix_of(unit)));
    box->setValue(core::units::from_mm(mm, unit, basis));
}

double mm_of(const QDoubleSpinBox* box, core::units::Unit unit, const core::units::Basis& basis) {
    return core::units::to_mm(box->value(), unit, basis);
}

QComboBox* unit_box(core::units::Unit chosen, bool with_columns) {
    auto* box = new QComboBox;
    std::vector<core::units::Unit> all = core::units::ruler_units();
    if (with_columns) all.push_back(core::units::Unit::Columns);
    for (const core::units::Unit u : all) {
        box->addItem(QString::fromStdString(core::units::label_of(u)), QString::fromStdString(core::units::key_of(u)));
    }
    box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(core::units::key_of(chosen)))));
    return box;
}

}  // namespace genko::app::view_units
