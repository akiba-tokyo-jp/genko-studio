// The guides' windows (PC5 H-b): app/guide_dialogs.hpp. 新規ガイドレイアウトを作成… as Photoshop's (PhotoCraft v0.2.0's
// view.newGuideLayout parameters, MIT), its lengths in the ruler's unit.

#include "app/guide_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/book_dialogs.hpp"
#include "app/config.hpp"
#include "app/theme.hpp"
#include "app/view_units.hpp"
#include "core/error.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr double kMaxMm = 100000;

QDialogButtonBox* ok_cancel(QDialog* dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QDoubleSpinBox* length_box(const QString& name, core::units::Unit unit, const core::units::Basis& basis, double mm, bool at_least_zero) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(name);
    view_units::set_unit(box, unit, basis, mm, kMaxMm);
    if (at_least_zero) box->setMinimum(0);
    return box;
}

double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

}  // namespace

// --- 新規ガイド… -------------------------------------------------------------------------------------------------------

NewGuideDialog::NewGuideDialog(QWidget* parent, const core::Page& page, core::units::Unit unit, const core::units::Basis& across_basis,
                               const core::units::Basis& down_basis, const QString& axis, std::optional<double> at_mm)
    : QDialog(parent), unit_(unit), across_basis_(across_basis), down_basis_(down_basis) {
    setObjectName(QStringLiteral("new_guide_dialog"));
    setWindowTitle(QStringLiteral("新規ガイド"));
    across = new QRadioButton(QStringLiteral("水平方向"));
    across->setObjectName(QStringLiteral("guide_across"));
    upright = new QRadioButton(QStringLiteral("垂直方向"));
    upright->setObjectName(QStringLiteral("guide_upright"));
    was_across_ = axis != QLatin1String("v");
    (was_across_ ? across : upright)->setChecked(true);
    const double fallback = was_across_ ? page.spec.height_mm.value() / 2 : page.spec.width_mm.value() / 2;
    position = length_box(QStringLiteral("guide_position"), unit, was_across_ ? down_basis_ : across_basis_, at_mm.value_or(fallback), false);
    auto* ways = new QWidget;
    auto* line = new QHBoxLayout(ways);
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(across);
    line->addWidget(upright);
    line->addStretch(1);
    connect(across, &QRadioButton::toggled, this, [this] { follow(); });
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("方向"), ways);
    form->addRow(QStringLiteral("位置"), position);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

void NewGuideDialog::follow() {
    // (the place kept as a length; a percent is of the other side now)
    const bool now_across = across->isChecked();
    if (now_across == was_across_) return;
    const double mm = view_units::mm_of(position, unit_, was_across_ ? down_basis_ : across_basis_);
    was_across_ = now_across;
    view_units::set_unit(position, unit_, now_across ? down_basis_ : across_basis_, mm, kMaxMm);
}

QString NewGuideDialog::axis() const { return across->isChecked() ? QStringLiteral("h") : QStringLiteral("v"); }

double NewGuideDialog::at_mm() const { return r3(view_units::mm_of(position, unit_, across->isChecked() ? down_basis_ : across_basis_)); }

// --- 新規ガイドレイアウトを作成… -----------------------------------------------------------------------------------------------

GuideLayoutDialog::GuideLayoutDialog(QWidget* parent, const core::Page& page, core::units::Unit unit, const core::units::Basis& across_basis,
                                     const core::units::Basis& down_basis, std::function<void(std::optional<core::GuideLayout>)> preview)
    : QDialog(parent), page_(page), unit_(unit), across_basis_(across_basis), down_basis_(down_basis), preview_(std::move(preview)) {
    setObjectName(QStringLiteral("guide_layout_dialog"));
    setWindowTitle(QStringLiteral("新規ガイドレイアウトを作成"));
    preset = new QComboBox;
    preset->setObjectName(QStringLiteral("guide_layout_preset"));
    save = new QPushButton(QStringLiteral("プリセットを保存…"));
    save->setObjectName(QStringLiteral("guide_layout_save"));
    remove = new QPushButton(QStringLiteral("プリセットを削除"));
    remove->setObjectName(QStringLiteral("guide_layout_delete"));
    from = new QComboBox;
    from->setObjectName(QStringLiteral("guide_layout_from"));
    from->addItem(QStringLiteral("用紙（カンバス）"), QStringLiteral("paper"));
    from->addItem(QStringLiteral("仕上がり線"), QStringLiteral("trim"));
    from->addItem(QStringLiteral("基本枠"), QStringLiteral("frame"));
    columns_on = new QCheckBox(QStringLiteral("列"));
    columns_on->setObjectName(QStringLiteral("guide_layout_columns_on"));
    columns_on->setChecked(true);
    columns = new QSpinBox;
    columns->setObjectName(QStringLiteral("guide_layout_columns"));
    columns->setRange(1, 100);
    columns->setValue(4);
    width = length_box(QStringLiteral("guide_layout_width"), unit, across_basis, 0, true);
    width->setSpecialValueText(QStringLiteral("自動"));
    gutter = length_box(QStringLiteral("guide_layout_gutter"), unit, across_basis, 5, true);
    rows_on = new QCheckBox(QStringLiteral("行数"));
    rows_on->setObjectName(QStringLiteral("guide_layout_rows_on"));
    rows = new QSpinBox;
    rows->setObjectName(QStringLiteral("guide_layout_rows"));
    rows->setRange(1, 100);
    rows->setValue(4);
    height = length_box(QStringLiteral("guide_layout_height"), unit, down_basis, 0, true);
    height->setSpecialValueText(QStringLiteral("自動"));
    row_gutter = length_box(QStringLiteral("guide_layout_row_gutter"), unit, down_basis, 5, true);
    margin_on = new QCheckBox(QStringLiteral("マージン"));
    margin_on->setObjectName(QStringLiteral("guide_layout_margin_on"));
    top = length_box(QStringLiteral("guide_layout_top"), unit, down_basis, 0, false);
    left = length_box(QStringLiteral("guide_layout_left"), unit, across_basis, 0, false);
    bottom = length_box(QStringLiteral("guide_layout_bottom"), unit, down_basis, 0, false);
    right = length_box(QStringLiteral("guide_layout_right"), unit, across_basis, 0, false);
    center = new QCheckBox(QStringLiteral("列を中央揃え"));
    center->setObjectName(QStringLiteral("guide_layout_center"));
    center->setToolTip(QStringLiteral("幅を決めた列を、マージンの内側の真ん中に寄せます"));
    clear = new QCheckBox(QStringLiteral("既存のガイドを消去"));
    clear->setObjectName(QStringLiteral("guide_layout_clear"));
    show_preview = new QCheckBox(QStringLiteral("プレビュー"));
    show_preview->setObjectName(QStringLiteral("guide_layout_preview"));
    show_preview->setChecked(true);
    note = new QLabel;
    note->setObjectName(QStringLiteral("guide_layout_note"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    const auto row = [](std::initializer_list<QWidget*> widgets) {
        auto* host = new QWidget;
        auto* line = new QHBoxLayout(host);
        line->setContentsMargins(0, 0, 0, 0);
        for (QWidget* w : widgets) line->addWidget(w);
        line->addStretch(1);
        return host;
    };
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("プリセット"), row({preset, save, remove}));
    form->addRow(QStringLiteral("基準"), from);
    form->addRow(columns_on);
    form->addRow(QStringLiteral("数"), columns);
    form->addRow(QStringLiteral("幅"), width);
    form->addRow(QStringLiteral("間隔"), gutter);
    form->addRow(rows_on);
    form->addRow(QStringLiteral("数"), rows);
    form->addRow(QStringLiteral("高さ"), height);
    form->addRow(QStringLiteral("間隔"), row_gutter);
    form->addRow(margin_on);
    form->addRow(QStringLiteral("上"), top);
    form->addRow(QStringLiteral("左"), left);
    form->addRow(QStringLiteral("下"), bottom);
    form->addRow(QStringLiteral("右"), right);
    form->addRow(row({center, clear}));
    form->addRow(show_preview);
    form->addRow(note);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
    for (QCheckBox* box : {columns_on, rows_on, margin_on, center, clear, show_preview}) connect(box, &QCheckBox::toggled, this, [this] { changed(); });
    for (QSpinBox* box : {columns, rows}) connect(box, &QSpinBox::valueChanged, this, [this] { changed(); });
    for (QDoubleSpinBox* box : {width, gutter, height, row_gutter, top, left, bottom, right})
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    connect(from, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(preset, &QComboBox::activated, this, [this](int) {
        const QString name = preset->currentData().toString();
        if (name.isEmpty()) return;
        const QString kept = settings()->value(QStringLiteral("guide_layouts/") + name).toString();
        try {
            put(Json::parse(kept.toStdString()));
        } catch (const std::exception&) {
        }
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (const auto name = ask::get_text(this, QStringLiteral("プリセットを保存"), QStringLiteral("名前"), preset->currentData().toString());
            name && !name->trimmed().isEmpty()) {
            save_preset(name->trimmed());
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] { delete_preset(preset->currentData().toString()); });
    connect(preset, &QComboBox::currentIndexChanged, this, [this](int i) { remove->setEnabled(i > 0); });
    connect(this, &QDialog::finished, this, [this] {
        if (preview_) preview_(std::nullopt);
    });
    fill_presets(QString());
    changed();
}

Json GuideLayoutDialog::fields() const {
    Json out = Json::object();
    const auto mm = [this](const QDoubleSpinBox* box, const core::units::Basis& basis) { return r3(view_units::mm_of(box, unit_, basis)); };
    if (columns_on->isChecked()) {
        Json c{{"count", columns->value()}, {"gutter_mm", mm(gutter, across_basis_)}};
        if (width->value() > width->minimum()) c["width_mm"] = mm(width, across_basis_);
        out["columns"] = std::move(c);
    }
    if (rows_on->isChecked()) {
        Json r{{"count", rows->value()}, {"gutter_mm", mm(row_gutter, down_basis_)}};
        if (height->value() > height->minimum()) r["height_mm"] = mm(height, down_basis_);
        out["rows"] = std::move(r);
    }
    if (margin_on->isChecked()) {
        out["margin_mm"] = Json::array({mm(top, down_basis_), mm(left, across_basis_), mm(bottom, down_basis_), mm(right, across_basis_)});
    }
    if (center->isChecked()) out["center_columns"] = true;
    if (clear->isChecked()) out["clear"] = true;
    const QString base = from->currentData().toString();
    if (base != QLatin1String("paper")) out["from"] = base.toStdString();
    return out;
}

Json GuideLayoutDialog::op() const {
    Json op{{"op", "add_guide_layout"}, {"page", page_.index.json()}};
    const Json chosen = fields();  // (held: a temporary's items() would outlive it)
    for (const auto& [key, value] : chosen.items()) op[key] = value;
    return op;
}

std::optional<core::GuideLayout> GuideLayoutDialog::layout(QString* why) const {
    try {
        return core::guide_layout(page_, op());
    } catch (const std::exception& error) {
        if (why != nullptr) *why = QString::fromUtf8(error.what());
        return std::nullopt;
    }
}

void GuideLayoutDialog::changed() {
    if (putting_) return;
    for (QWidget* w : std::initializer_list<QWidget*>{columns, width, gutter, center}) w->setEnabled(columns_on->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{rows, height, row_gutter}) w->setEnabled(rows_on->isChecked());
    for (QWidget* w : std::initializer_list<QWidget*>{top, left, bottom, right}) w->setEnabled(margin_on->isChecked());
    QString why;
    const auto made = layout(&why);
    if (QPushButton* ok = buttons->button(QDialogButtonBox::Ok)) ok->setEnabled(made.has_value());
    note->setText(made ? QStringLiteral("縦のガイド %1 本・横のガイド %2 本を足します（すでにある所には足しません）").arg(made->down.size()).arg(made->across.size())
                       : QStringLiteral("列・行数・マージンのどれかを入れます"));
    if (preview_) preview_(show_preview->isChecked() ? made : std::nullopt);
}

void GuideLayoutDialog::put(const Json& settings) {
    putting_ = true;
    // (put back however it ends — a preset of the wrong kinds throws midway: the review's P3-7)
    struct Done {
        GuideLayoutDialog* dialog;
        ~Done() { dialog->putting_ = false; }
    } done{this};
    const auto set = [this](QDoubleSpinBox* box, const core::units::Basis& basis, double mm) { view_units::set_unit(box, unit_, basis, mm, kMaxMm); };
    const Json* c = settings.contains("columns") && settings["columns"].is_object() ? &settings["columns"] : nullptr;
    columns_on->setChecked(c != nullptr);
    if (c != nullptr) {
        columns->setValue(c->value("count", 4));
        set(width, across_basis_, c->value("width_mm", 0.0));
        set(gutter, across_basis_, c->value("gutter_mm", 0.0));
    }
    const Json* r = settings.contains("rows") && settings["rows"].is_object() ? &settings["rows"] : nullptr;
    rows_on->setChecked(r != nullptr);
    if (r != nullptr) {
        rows->setValue(r->value("count", 4));
        set(height, down_basis_, r->value("height_mm", 0.0));
        set(row_gutter, down_basis_, r->value("gutter_mm", 0.0));
    }
    const Json m = settings.value("margin_mm", Json());
    margin_on->setChecked(!m.is_null());
    if (m.is_array() && m.size() == 4) {
        set(top, down_basis_, m[0].get<double>());
        set(left, across_basis_, m[1].get<double>());
        set(bottom, down_basis_, m[2].get<double>());
        set(right, across_basis_, m[3].get<double>());
    } else if (m.is_number()) {
        set(top, down_basis_, m.get<double>());
        set(left, across_basis_, m.get<double>());
        set(bottom, down_basis_, m.get<double>());
        set(right, across_basis_, m.get<double>());
    }
    for (QDoubleSpinBox* box : {width, gutter, height, row_gutter}) box->setMinimum(0);
    center->setChecked(settings.value("center_columns", false));
    clear->setChecked(settings.value("clear", false));
    from->setCurrentIndex(std::max(0, from->findData(QString::fromStdString(settings.value("from", std::string("paper"))))));
    putting_ = false;
    changed();
}

void GuideLayoutDialog::fill_presets(const QString& chosen) {
    const QSignalBlocker quiet(preset);
    preset->clear();
    preset->addItem(QStringLiteral("カスタム"), QString());
    const auto store = settings();
    store->beginGroup(QStringLiteral("guide_layouts"));
    const QStringList names = store->childKeys();
    store->endGroup();
    for (const QString& name : names) preset->addItem(name, name);
    preset->setCurrentIndex(std::max(0, preset->findData(chosen)));
    remove->setEnabled(preset->currentIndex() > 0);
}

void GuideLayoutDialog::save_preset(const QString& given) {
    // (a "/" would make a group of the settings, never listed: the full-width one in its place)
    QString name = given;
    name.replace(QLatin1Char('/'), QStringLiteral("／")).replace(QLatin1Char('\\'), QStringLiteral("＼"));
    settings()->setValue(QStringLiteral("guide_layouts/") + name, QString::fromStdString(fields().dump()));
    fill_presets(name);
}

void GuideLayoutDialog::delete_preset(const QString& name) {
    if (name.isEmpty()) return;
    settings()->remove(QStringLiteral("guide_layouts/") + name);
    fill_presets(QString());
}

}  // namespace genko::app
