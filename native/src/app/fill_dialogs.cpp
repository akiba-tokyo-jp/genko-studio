// PC3 B2 (edit.fill, edit.stroke): 編集 → 塗りつぶし… and 境界線を描く…, Photoshop's dialogs. The selection (none: the whole
// layer, or for 境界線 the edge of what the layer holds) filled or stroked on the layer drawn on with one op — fill_area,
// outline_area (render/ops_masks) — so one Undo takes it back. (塗りつぶし's パターン — PC4 E3, edit.fill: the chosen pattern
// of the library or the book, its 比率 and 角度 — fills with fill_area's "pattern", a library pattern put into the book by
// add_pattern in the same apply; コンテンツに応じる with コンテンツに応じた塗りつぶし…, B4.)

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSpinBox>

#include <cmath>

#include "app/ask.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/patterns_panel.hpp"
#include "core/patterns.hpp"
#include "core/pynum.hpp"

namespace genko::app {

using core::Json;

namespace {

Json rgb_json(int r, int g, int b) { return Json::array({r, g, b}); }

QDialogButtonBox* ok_cancel(QDialog& dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return buttons;
}

QSpinBox* percent(const char* name, int value) {
    auto* box = new QSpinBox;
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(1, 100);
    box->setSuffix(QStringLiteral(" %"));
    box->setValue(value);
    return box;
}

}  // namespace

void MainWindow::fill_dialog() {
    const core::Page* page = current_page();
    const core::Layer* layer = paint_layer();
    if (page == nullptr || layer == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fill_dialog"));
    dialog.setWindowTitle(QStringLiteral("塗りつぶし"));
    auto* form = new QFormLayout(&dialog);
    auto* contents = new QComboBox;
    contents->setObjectName(QStringLiteral("fill_contents"));
    for (const auto& [label, key] : {std::pair{"描画色", "fore"}, {"背景色", "back"}, {"カラー…", "colour"}, {"パターン", "pattern"},
                                     {"ブラック", "black"}, {"50% グレー", "grey"}, {"ホワイト", "white"}})
        contents->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    auto* opacity = percent("fill_opacity", static_cast<int>(std::lround(pen_.opacity * 100)));
    auto* preserve = new QCheckBox(QStringLiteral("透明部分の保持"));
    preserve->setObjectName(QStringLiteral("fill_preserve"));
    // (PC4 E3: パターン — the chosen pattern, its 比率 and 角度)
    QComboBox* pattern = pattern_combo(book(), current_pattern());
    pattern->setObjectName(QStringLiteral("fill_pattern"));
    auto* scale = new QDoubleSpinBox;
    scale->setObjectName(QStringLiteral("fill_pattern_scale"));
    scale->setRange(core::kPatternScaleMin, core::kPatternScaleMax);
    scale->setDecimals(0);
    scale->setSuffix(QStringLiteral(" %"));
    scale->setValue(100);
    auto* angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("fill_pattern_angle"));
    angle->setRange(-360, 360);
    angle->setDecimals(1);
    angle->setSuffix(QStringLiteral("°"));
    form->addRow(QStringLiteral("内容"), contents);
    form->addRow(QStringLiteral("カスタムパターン"), pattern);
    form->addRow(QStringLiteral("比率"), scale);
    form->addRow(QStringLiteral("角度"), angle);
    const auto pattern_rows = [form, contents, pattern, scale, angle] {
        const bool on = contents->currentData().toString() == QLatin1String("pattern");
        for (QWidget* w : std::initializer_list<QWidget*>{pattern, scale, angle}) form->setRowVisible(w, on);
    };
    pattern_rows();
    connect(contents, &QComboBox::currentIndexChanged, &dialog, pattern_rows);
    form->addRow(QStringLiteral("不透明度"), opacity);
    form->addRow(preserve);
    form->addRow(ok_cancel(dialog));
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const QString key = contents->currentData().toString();
    if (key == QLatin1String("pattern")) {  // (PC4 E3: the pattern laid over the area, from the page's corner)
        Json ops;
        const auto spec = pattern_fill_of(pattern->currentData().toString(), scale->value(), angle->value(), true, ops);
        if (!spec) return;
        set_current_pattern(pattern->currentData().toString());
        const auto area = selection_area();
        Json op{{"op", "fill_area"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", area ? *area : Json{{"all", true}}}, {"pattern", *spec}};
        if (opacity->value() < 100) op["opacity"] = opacity->value() / 100.0;
        if (preserve->isChecked()) op["preserve_transparency"] = true;
        ops.push_back(std::move(op));
        apply_ops(ops);
        return;
    }
    Json rgb;
    if (key == QLatin1String("fore")) {
        rgb = Json(pen_.rgb);
    } else if (key == QLatin1String("back")) {
        const auto& sub = colours_ != nullptr ? colours_->sub_rgb() : Rgb{255, 255, 255};
        rgb = rgb_json(sub[0], sub[1], sub[2]);
    } else if (key == QLatin1String("colour")) {
        const auto& r = pen_.rgb;
        const auto chosen = ask::colour(this, QColor(int(r[0]), int(r[1]), int(r[2])), QStringLiteral("塗りつぶしの色"));
        if (!chosen || !still(asked)) return;
        rgb = rgb_json(chosen->red(), chosen->green(), chosen->blue());
    } else if (key == QLatin1String("black")) {
        rgb = rgb_json(0, 0, 0);
    } else if (key == QLatin1String("grey")) {
        rgb = rgb_json(128, 128, 128);
    } else {
        rgb = rgb_json(255, 255, 255);
    }
    const auto area = selection_area();
    Json op{{"op", "fill_area"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", area ? *area : Json{{"all", true}}}, {"rgb", rgb}};
    if (opacity->value() < 100) op["opacity"] = opacity->value() / 100.0;
    if (preserve->isChecked()) op["preserve_transparency"] = true;
    apply_ops(Json::array({op}));
}

void MainWindow::stroke_dialog() {
    const core::Page* page = current_page();
    const core::Layer* layer = paint_layer();
    if (page == nullptr || layer == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("stroke_dialog"));
    dialog.setWindowTitle(QStringLiteral("境界線を描く"));
    auto* form = new QFormLayout(&dialog);
    auto* width = new QDoubleSpinBox;
    width->setObjectName(QStringLiteral("stroke_width"));
    width->setRange(0.05, 50);
    width->setDecimals(2);
    width->setSuffix(QStringLiteral(" mm"));
    width->setValue(settings()->value(QStringLiteral("stroke/width_mm"), 1.0).toDouble());
    auto* colour = new QPushButton;
    colour->setObjectName(QStringLiteral("stroke_colour"));
    std::array<int, 3> rgb{int(pen_.rgb.size() > 0 ? pen_.rgb[0] : 20), int(pen_.rgb.size() > 1 ? pen_.rgb[1] : 20), int(pen_.rgb.size() > 2 ? pen_.rgb[2] : 20)};
    const auto show_colour = [colour, &rgb] {
        colour->setText(QColor(rgb[0], rgb[1], rgb[2]).name());
        colour->setStyleSheet(QStringLiteral("background:%1;").arg(QColor(rgb[0], rgb[1], rgb[2]).name()));
    };
    show_colour();
    connect(colour, &QPushButton::clicked, &dialog, [this, &rgb, show_colour] {
        if (const auto chosen = ask::colour(this, QColor(rgb[0], rgb[1], rgb[2]), QStringLiteral("境界線の色"))) {
            rgb = {chosen->red(), chosen->green(), chosen->blue()};
            show_colour();
        }
    });
    auto* location = new QComboBox;
    location->setObjectName(QStringLiteral("stroke_location"));
    for (const auto& [label, key] : {std::pair{"内側", "inside"}, {"中央", "center"}, {"外側", "outside"}}) location->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    location->setCurrentIndex(std::max(0, location->findData(settings()->value(QStringLiteral("stroke/location"), QStringLiteral("center")).toString())));
    auto* opacity = percent("stroke_opacity", 100);
    auto* preserve = new QCheckBox(QStringLiteral("透明部分の保持"));
    preserve->setObjectName(QStringLiteral("stroke_preserve"));
    form->addRow(QStringLiteral("幅"), width);
    form->addRow(QStringLiteral("カラー"), colour);
    form->addRow(QStringLiteral("位置"), location);
    form->addRow(QStringLiteral("不透明度"), opacity);
    form->addRow(preserve);
    form->addRow(ok_cancel(dialog));
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    settings()->setValue(QStringLiteral("stroke/width_mm"), width->value());
    settings()->setValue(QStringLiteral("stroke/location"), location->currentData().toString());
    Json op{{"op", "outline_area"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"width_mm", core::py_round(width->value(), 3)},
            {"rgb", rgb_json(rgb[0], rgb[1], rgb[2])}, {"location", location->currentData().toString().toStdString()}};
    if (const auto area = selection_area()) op["area"] = *area;
    if (opacity->value() < 100) op["opacity"] = opacity->value() / 100.0;
    if (preserve->isChecked()) op["preserve_transparency"] = true;
    apply_ops(Json::array({op}));
}

}  // namespace genko::app
