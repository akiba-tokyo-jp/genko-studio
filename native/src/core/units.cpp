// 単位・定規 (PC5 H-b): core/units.hpp. PhotoCraft v0.2.0's prefs.rs Unit::from_px / to_px (MIT), in the book's mm.

#include "core/units.hpp"

#include <algorithm>

namespace genko::core::units {

namespace {

constexpr double kMmPerInch = 25.4;

struct Row {
    Unit unit;
    const char* key;
    const char* label;
    const char* suffix;
    int decimals;
};

constexpr Row kRows[] = {
    {Unit::Px, "px", "pixel", "px", 0},          {Unit::In, "in", "inch", "in", 2},
    {Unit::Cm, "cm", "cm", "cm", 2},             {Unit::Mm, "mm", "mm", "mm", 1},
    {Unit::Pt, "pt", "pt", "pt", 1},             {Unit::Pica, "pica", "pica", "pica", 2},
    {Unit::Percent, "percent", "%", "%", 1},     {Unit::Columns, "columns", "列", "列", 2},
};

const Row& row_of(Unit unit) {
    for (const Row& row : kRows) {
        if (row.unit == unit) return row;
    }
    return kRows[3];
}

}  // namespace

const std::vector<Unit>& ruler_units() {
    static const std::vector<Unit> all{Unit::Px, Unit::In, Unit::Cm, Unit::Mm, Unit::Pt, Unit::Pica, Unit::Percent};
    return all;
}

std::string key_of(Unit unit) { return row_of(unit).key; }

std::optional<Unit> unit_of(std::string_view key) {
    for (const Row& row : kRows) {
        if (key == row.key) return row.unit;
    }
    return std::nullopt;
}

std::string label_of(Unit unit) { return row_of(unit).label; }
std::string suffix_of(Unit unit) { return row_of(unit).suffix; }
int decimals_of(Unit unit) { return row_of(unit).decimals; }

double from_mm(double mm, Unit unit, const Basis& basis) {
    const double inches = mm / kMmPerInch;
    const double ppi = std::max(basis.points_per_inch, 1e-6);
    switch (unit) {
        case Unit::Px: return inches * std::max(basis.dpi, 1e-6);
        case Unit::In: return inches;
        case Unit::Cm: return mm / 10.0;
        case Unit::Mm: return mm;
        case Unit::Pt: return inches * ppi;
        case Unit::Pica: return inches * ppi / 12.0;
        case Unit::Percent: return mm / std::max(basis.extent_mm, 1e-6) * 100.0;
        case Unit::Columns: {
            // (n columns: n × the column and n − 1 gutters — Photoshop's 列)
            const double points = inches * ppi;
            const double column = std::max(basis.column_pt, 1e-6);
            if (points <= column) return points / column;  // (less than one: a part of a column, no gutter)
            return (points + basis.gutter_pt) / std::max(column + basis.gutter_pt, 1e-6);
        }
    }
    return mm;
}

double to_mm(double value, Unit unit, const Basis& basis) {
    const double ppi = std::max(basis.points_per_inch, 1e-6);
    switch (unit) {
        case Unit::Px: return value / std::max(basis.dpi, 1e-6) * kMmPerInch;
        case Unit::In: return value * kMmPerInch;
        case Unit::Cm: return value * 10.0;
        case Unit::Mm: return value;
        case Unit::Pt: return value / ppi * kMmPerInch;
        case Unit::Pica: return value * 12.0 / ppi * kMmPerInch;
        case Unit::Percent: return value * basis.extent_mm / 100.0;
        case Unit::Columns: {
            const double points = value * basis.column_pt + std::max(0.0, value - 1.0) * basis.gutter_pt;
            return points / ppi * kMmPerInch;
        }
    }
    return value;
}

double points_per_inch(std::string_view point_size) { return point_size == "traditional" ? 72.27 : 72.0; }

}  // namespace genko::core::units
