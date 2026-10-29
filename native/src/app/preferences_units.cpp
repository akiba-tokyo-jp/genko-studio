// 環境設定 → 単位・定規 and ガイド・グリッド・スライス (PC5 H-b, edit.preferences.unitsAndRulers and guidesGridAndSlices; SPEC.md
// VIEW-01; PhotoCraft v0.2.0's prefs.rs UnitsAndRulers and GuidesGridAndSlices, MIT): their pages in the dialog and what
// 決める keeps (core/prefs_table's keys units/… and guides/…). The slices' two fields join the second page with 節 J.

#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/look.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/view_units.hpp"
#include "core/prefs_table.hpp"

namespace genko::app::preferences {

namespace {

core::Json pref(const char* path) {
    const auto store = settings();
    return core::prefs::value(*store, path);
}

QString text_of(const char* path) { return QString::fromStdString(pref(path).get<std::string>()); }

QComboBox* choices(const QString& name, std::initializer_list<std::pair<const char*, const char*>> items, const QString& current) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    for (const auto& [label, value] : items) box->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    box->setCurrentIndex(std::max(0, box->findData(current)));
    return box;
}

QDoubleSpinBox* number_box(const QString& name, double lo, double hi, double value, const QString& suffix, int decimals) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(name);
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setValue(value);
    return box;
}

void show_colour(QPushButton* button, const QColor& colour) {
    button->setProperty("colour", colour);
    QPixmap swatch(28, 14);
    swatch.fill(colour);
    button->setIcon(QIcon(swatch));
    button->setText(colour.name());
}

QPushButton* colour_button(QWidget* page, const QString& name, const char* path) {
    auto* button = new QPushButton;
    button->setObjectName(name);
    show_colour(button, QColor(text_of(path)));
    QObject::connect(button, &QPushButton::clicked, page, [page, button] {
        if (const auto chosen = ask::colour(page, button->property("colour").value<QColor>(), QStringLiteral("色を選ぶ"))) show_colour(button, *chosen);
    });
    return button;
}

const std::initializer_list<std::pair<const char*, const char*>> kStyles{{"ライン", "lines"}, {"破線", "dashedLines"}, {"点線", "dots"}};

// The grid's unit (a length's: a page's pixels and percent have no page here).
core::units::Unit grid_unit(const QComboBox* box) {
    return core::units::unit_of(box->currentData().toString().toStdString()).value_or(core::units::Unit::Mm);
}

core::units::Basis no_page() {
    core::units::Basis basis;
    basis.points_per_inch = view_units::points_per_inch();
    return basis;
}

}  // namespace

std::pair<QWidget*, QWidget*> unit_pages(QWidget* parent) {
    // --- 単位・定規
    auto* units = new QWidget(parent);
    units->setObjectName(QStringLiteral("prefs_units_page"));
    {
        QFormLayout* rows = look::form();
        units->setLayout(rows);
        auto* ruler = view_units::unit_box(view_units::ruler());
        ruler->setObjectName(QStringLiteral("prefs_ruler_unit"));
        auto* type = choices(QStringLiteral("prefs_type_unit"), {{"pt", "pt"}, {"pixel", "px"}, {"mm", "mm"}}, text_of("unitsAndRulers.typeUnits"));
        auto* column = number_box(QStringLiteral("prefs_column_width"), 1, 10000, view_units::column_pt(), QStringLiteral(" pt"), 1);
        auto* gutter = number_box(QStringLiteral("prefs_gutter"), 0, 10000, view_units::gutter_pt(), QStringLiteral(" pt"), 1);
        auto* print = number_box(QStringLiteral("prefs_print_resolution"), 1, 10000, view_units::print_dpi(), QStringLiteral(" pixel/inch"), 0);
        auto* screen = number_box(QStringLiteral("prefs_screen_resolution"), 1, 10000, view_units::screen_dpi(), QStringLiteral(" pixel/inch"), 0);
        auto* points = choices(QStringLiteral("prefs_point_size"), {{"PostScript（72 pt/inch）", "postscript"}, {"従来の単位（72.27 pt/inch）", "traditional"}},
                               text_of("unitsAndRulers.pointSize"));
        rows->addRow(look::section(QStringLiteral("単位")));
        rows->addRow(QStringLiteral("定規"), ruler);
        rows->addRow(QStringLiteral("文字"), type);
        rows->addRow(look::section(QStringLiteral("段組のサイズ")));
        rows->addRow(QStringLiteral("幅"), column);
        rows->addRow(QStringLiteral("間隔"), gutter);
        rows->addRow(look::section(QStringLiteral("新規ドキュメントプリセットの解像度")));
        rows->addRow(QStringLiteral("印刷解像度"), print);
        rows->addRow(QStringLiteral("画面解像度"), screen);
        rows->addRow(look::section(QStringLiteral("ポイント/パイカの単位")));
        rows->addRow(QString(), points);
        auto* hint = new QLabel(QStringLiteral("定規・ガイド・グリッド・情報パネル・画像解像度…・カンバスサイズ… の長さがこの単位になります（原稿は mm のまま）。"
                                               "ペンの太さなど、漫画の道具の mm の欄は変わりません。画面解像度はプリントサイズで使います。段組のサイズは単位「列」で使います。"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        rows->addRow(QString(), hint);
        look::quiet_labels(rows);
    }
    // --- ガイド・グリッド・スライス
    auto* guides = new QWidget(parent);
    guides->setObjectName(QStringLiteral("prefs_guides_page"));
    {
        QFormLayout* rows = look::form();
        guides->setLayout(rows);
        auto* guide_colour = colour_button(guides, QStringLiteral("prefs_guide_color"), "guidesGridAndSlices.guideColor");
        auto* guide_style = choices(QStringLiteral("prefs_guide_style"), kStyles, text_of("guidesGridAndSlices.guideStyle"));
        auto* smart_colour = colour_button(guides, QStringLiteral("prefs_smart_color"), "guidesGridAndSlices.smartGuideColor");
        auto* grid_colour = colour_button(guides, QStringLiteral("prefs_grid_color"), "guidesGridAndSlices.gridColor");
        auto* grid_style = choices(QStringLiteral("prefs_grid_style"), kStyles, text_of("guidesGridAndSlices.gridStyle"));
        auto* unit = choices(QStringLiteral("prefs_grid_unit"), {{"mm", "mm"}, {"cm", "cm"}, {"inch", "in"}, {"pt", "pt"}, {"pica", "pica"}},
                             text_of("guidesGridAndSlices.gridUnit"));
        const int parts = pref("guidesGridAndSlices.subdivisions").get<int>();
        const double every_mm = pref("guidesGridAndSlices.gridlineEvery").get<double>() * parts;
        auto* every = new QDoubleSpinBox;
        every->setObjectName(QStringLiteral("prefs_grid_every"));
        every->setDecimals(3);
        every->setRange(0.001, 100000);
        every->setValue(core::units::from_mm(every_mm, grid_unit(unit), no_page()));
        // (the number kept as a length when the unit is changed)
        unit->setProperty("was", unit->currentData());
        QObject::connect(unit, &QComboBox::currentIndexChanged, guides, [unit, every] {
            const auto was = core::units::unit_of(unit->property("was").toString().toStdString()).value_or(core::units::Unit::Mm);
            const double mm = core::units::to_mm(every->value(), was, no_page());
            every->setValue(core::units::from_mm(mm, grid_unit(unit), no_page()));
            unit->setProperty("was", unit->currentData());
        });
        auto* subdivisions = new QSpinBox;
        subdivisions->setObjectName(QStringLiteral("prefs_grid_subdivisions"));
        subdivisions->setRange(1, 100);
        subdivisions->setValue(parts);
        auto* every_row = new QWidget;
        auto* line = new QHBoxLayout(every_row);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(every, 1);
        line->addWidget(unit);
        rows->addRow(look::section(QStringLiteral("ガイド")));
        rows->addRow(QStringLiteral("カラー"), guide_colour);
        rows->addRow(QStringLiteral("線の種類"), guide_style);
        rows->addRow(look::section(QStringLiteral("スマートガイド")));
        rows->addRow(QStringLiteral("カラー"), smart_colour);
        rows->addRow(look::section(QStringLiteral("グリッド")));
        rows->addRow(QStringLiteral("カラー"), grid_colour);
        rows->addRow(QStringLiteral("線の種類"), grid_style);
        rows->addRow(QStringLiteral("グリッド線"), every_row);
        rows->addRow(QStringLiteral("分割数"), subdivisions);
        auto* hint = new QLabel(QStringLiteral("グリッド線は太い線の間隔、分割数はその間を割る数です（細い線にスナップします）。"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        rows->addRow(QString(), hint);
        look::quiet_labels(rows);
    }
    return {units, guides};
}

void save_unit_pages(QWidget* units, QWidget* guides) {
    const auto store = settings();
    if (units != nullptr) {
        store->setValue(QStringLiteral("units/ruler"), units->findChild<QComboBox*>(QStringLiteral("prefs_ruler_unit"))->currentData());
        store->setValue(QStringLiteral("units/type"), units->findChild<QComboBox*>(QStringLiteral("prefs_type_unit"))->currentData());
        for (const auto& [name, key] : {std::pair{"prefs_column_width", "units/column_width"}, {"prefs_gutter", "units/gutter"},
                                        {"prefs_print_resolution", "units/print_resolution"}, {"prefs_screen_resolution", "units/screen_resolution"}}) {
            store->setValue(QString::fromLatin1(key), units->findChild<QDoubleSpinBox*>(QString::fromLatin1(name))->value());
        }
        store->setValue(QStringLiteral("units/point_size"), units->findChild<QComboBox*>(QStringLiteral("prefs_point_size"))->currentData());
    }
    if (guides != nullptr) {
        for (const auto& [name, key] : {std::pair{"prefs_guide_color", "guides/guide_color"}, {"prefs_smart_color", "guides/smart_color"},
                                        {"prefs_grid_color", "guides/grid_color"}}) {
            store->setValue(QString::fromLatin1(key), guides->findChild<QPushButton*>(QString::fromLatin1(name))->property("colour").value<QColor>().name());
        }
        store->setValue(QStringLiteral("guides/guide_style"), guides->findChild<QComboBox*>(QStringLiteral("prefs_guide_style"))->currentData());
        store->setValue(QStringLiteral("guides/grid_style"), guides->findChild<QComboBox*>(QStringLiteral("prefs_grid_style"))->currentData());
        const auto* unit = guides->findChild<QComboBox*>(QStringLiteral("prefs_grid_unit"));
        const int parts = guides->findChild<QSpinBox*>(QStringLiteral("prefs_grid_subdivisions"))->value();
        const double every_mm = core::units::to_mm(guides->findChild<QDoubleSpinBox*>(QStringLiteral("prefs_grid_every"))->value(), grid_unit(unit), no_page());
        store->setValue(QStringLiteral("guides/grid_unit"), unit->currentData());
        store->setValue(QStringLiteral("guides/grid_subdivisions"), parts);
        // (the thin lines' step — what snaps — within the table's range)
        store->setValue(QStringLiteral("guides/grid_mm"), std::clamp(std::round(every_mm / std::max(1, parts) * 1000.0) / 1000.0, 0.5, 100.0));
    }
}

}  // namespace genko::app::preferences
