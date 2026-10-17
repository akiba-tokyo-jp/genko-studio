#include "app/mode_dialogs.hpp"

#include <QCheckBox>
#include <QAbstractButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

#include "app/ask.hpp"
#include "app/book_dialogs.hpp"
#include "core/ink.hpp"
#include "render/quantize.hpp"

namespace genko::app {

using core::Json;

namespace {

// The page in front or every page (and the pages to come), as ページの色モード… offers them.
QComboBox* scope_box(const core::Page* page, const QString& name) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    if (page != nullptr) box->addItem(QStringLiteral("このページ（%1 ページ）").arg(QString::fromStdString(page->index.repr())), QStringLiteral("page"));
    box->addItem(QStringLiteral("すべてのページと、これから足すページ"), QStringLiteral("all"));
    return box;
}

// The op's "page": the page in front's number, or none (every page and the book's default).
void put_scope(Json& op, const QComboBox* scope, const core::Page* page) {
    if (page != nullptr && scope->currentData().toString() == QLatin1String("page")) op["page"] = page->index.json();
}

QComboBox* combo(const QString& name, std::initializer_list<std::pair<const char*, const char*>> items) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    for (const auto& [label, data] : items) box->addItem(QString::fromUtf8(label), QString::fromLatin1(data));
    return box;
}

QString data_of(const QComboBox* box) { return box->currentData().toString(); }

QDialogButtonBox* ok_cancel(QDialog* dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QString hex(const std::array<int, 3>& c) { return QColor(c[0], c[1], c[2]).name(); }

void paint_swatch(QAbstractButton* button, const std::array<int, 3>& c) {
    button->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #808080;").arg(hex(c)));
    button->setToolTip(QStringLiteral("%1（%2, %3, %4）").arg(hex(c)).arg(c[0]).arg(c[1]).arg(c[2]));
}

// PhotoCraft's default duotone inks (mode_cmds.rs default_inks), as render/ops_ink writes them.
const std::array<std::pair<const char*, std::array<int, 3>>, 4>& default_inks() {
    static const std::array<std::pair<const char*, std::array<int, 3>>, 4> kInks{
        {{"Black", {0, 0, 0}}, {"Warm Brown", {158, 97, 46}}, {"Gold", {219, 173, 46}}, {"Slate Blue", {69, 97, 148}}}};
    return kInks;
}

QString curve_text(const std::vector<core::CurvePoint>& curve) {
    QStringList points;
    const auto number = [](double v) { return QString::number(v, 'g', 6); };
    for (const core::CurvePoint& p : curve) points << QStringLiteral("%1:%2").arg(number(p[0]), number(p[1]));
    return points.join(QStringLiteral(", "));
}

// A curve as written ("in:out, in:out, …", 0..100), or none when it cannot be read.
std::optional<Json> curve_of(const QString& text) {
    Json out = Json::array();
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList pair = part.trimmed().split(QLatin1Char(':'));
        if (pair.size() != 2) return std::nullopt;
        bool a = false, b = false;
        const double in = pair[0].trimmed().toDouble(&a), value = pair[1].trimmed().toDouble(&b);
        if (!a || !b || !std::isfinite(in) || !std::isfinite(value) || in < 0 || in > 100 || value < 0 || value > 100) return std::nullopt;
        out.push_back(Json::array({in, value}));
    }
    if (out.size() < 2 || out.size() > core::kCurvePointsMax) return std::nullopt;
    return out;
}

}  // namespace

// --- インデックスカラー ---------------------------------------------------------------------------------------------------

IndexedDialog::IndexedDialog(QWidget* parent, const core::Page* page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("indexed_dialog"));
    setWindowTitle(QStringLiteral("インデックスカラー"));
    scope = scope_box(page, QStringLiteral("indexed_scope"));
    palette = combo(QStringLiteral("indexed_palette"), {{"ローカル（特定）", "selective"},
                                                         {"ローカル（知覚的）", "perceptual"},
                                                         {"ローカル（割り付け）", "adaptive"},
                                                         {"正確（256 色まで）", "exact"},
                                                         {"システム（Mac OS）", "systemMac"},
                                                         {"システム（Windows）", "systemWindows"},
                                                         {"Web", "web"},
                                                         {"均等", "uniform"}});
    colors = new QSpinBox;
    colors->setObjectName(QStringLiteral("indexed_colors"));
    colors->setRange(2, 256);
    colors->setValue(256);
    forced = combo(QStringLiteral("indexed_forced"), {{"白黒", "blackWhite"}, {"なし", "none"}, {"原色", "primaries"}, {"Web", "web"}});
    transparency = new QCheckBox(QStringLiteral("透明部分を残す"));
    transparency->setObjectName(QStringLiteral("indexed_transparency"));
    transparency->setChecked(true);
    dither = combo(QStringLiteral("indexed_dither"), {{"拡散", "diffusion"}, {"なし", "none"}, {"パターン", "pattern"}, {"ノイズ", "noise"}});
    amount = new QSpinBox;
    amount->setObjectName(QStringLiteral("indexed_amount"));
    amount->setRange(0, 100);
    amount->setValue(75);
    amount->setSuffix(QStringLiteral(" %"));
    auto* note = new QLabel(QStringLiteral("ページの画素レイヤーを 1 枚に統合してから、選んだ色数の表（カラーテーブル）の色にします（取り消せます）。"
                                           "コマ・台詞・フキダシは要素のまま残り、表の色で表示します。"));
    note->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("パレット"), palette);
    form->addRow(QStringLiteral("色数"), colors);
    form->addRow(QStringLiteral("強制する色"), forced);
    form->addRow(QString(), transparency);
    form->addRow(QStringLiteral("ディザ"), dither);
    form->addRow(QStringLiteral("ディザの量"), amount);
    form->addRow(note);
    const auto enable = [this] {
        const QString kind = data_of(palette);
        colors->setEnabled(kind == QLatin1String("selective") || kind == QLatin1String("perceptual") || kind == QLatin1String("adaptive") ||
                           kind == QLatin1String("uniform"));
        amount->setEnabled(data_of(dither) != QLatin1String("none"));
    };
    connect(palette, &QComboBox::currentIndexChanged, this, enable);
    connect(dither, &QComboBox::currentIndexChanged, this, enable);
    enable();
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

Json IndexedDialog::op() const {
    Json op{{"op", "to_indexed"}};
    put_scope(op, scope, page_);
    op["palette"] = data_of(palette).toStdString();
    op["colors"] = colors->value();
    op["forced"] = data_of(forced).toStdString();
    op["transparency"] = transparency->isChecked();
    op["dither"] = data_of(dither).toStdString();
    op["amount"] = amount->value();
    return op;
}

// --- カラーテーブル ---------------------------------------------------------------------------------------------------

ColorTableDialog::ColorTableDialog(QWidget* parent, const core::Page& page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("color_table_dialog"));
    setWindowTitle(QStringLiteral("カラーテーブル"));
    const core::ColorTable table = page.rgb && page.rgb->table ? *page.rgb->table : core::ColorTable{};
    colors_ = table.colors;
    preset = combo(QStringLiteral("color_table_preset"), {{"カスタム", "custom"},
                                                           {"黒体", "blackBody"},
                                                           {"グレースケール", "grayscale"},
                                                           {"スペクトル", "spectrum"},
                                                           {"システム（Mac OS）", "systemMac"},
                                                           {"システム（Windows）", "systemWindows"},
                                                           {"Web", "web"}});
    transparent = new QComboBox;
    transparent->setObjectName(QStringLiteral("color_table_transparent"));
    auto* grid_box = new QWidget;
    auto* grid = new QGridLayout(grid_box);
    grid->setSpacing(1);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 256; ++i) {
        auto* cell = new QToolButton;
        cell->setObjectName(QStringLiteral("color_table_cell_%1").arg(i));
        cell->setFixedSize(16, 16);
        connect(cell, &QToolButton::clicked, this, [this, i] {
            if (i >= static_cast<int>(colors_.size())) return;
            const std::array<int, 3>& c = colors_[static_cast<std::size_t>(i)];
            if (const auto chosen = ask::colour(this, QColor(c[0], c[1], c[2]), QStringLiteral("%1 番の色").arg(i))) {
                set_entry(i, {chosen->red(), chosen->green(), chosen->blue()});
            }
        });
        grid->addWidget(cell, i / 16, i % 16);
        cells.push_back(cell);
    }
    connect(preset, &QComboBox::currentIndexChanged, this, [this] {
        if (const auto named = render::quantize::preset_table(data_of(preset).toStdString())) {
            colors_.clear();
            for (const auto& c : *named) colors_.push_back({c[0], c[1], c[2]});
            paint();
        }
    });
    paint();
    if (table.transparent) transparent->setCurrentIndex(std::max(0, transparent->findData(*table.transparent)));
    auto* note = new QLabel(QStringLiteral("画素は表の番号を保ち、その番号の新しい色になります（取り消せます）。色をクリックすると変えられます。"));
    note->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("表"), preset);
    form->addRow(grid_box);
    form->addRow(QStringLiteral("透明にする色"), transparent);
    form->addRow(note);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

void ColorTableDialog::set_entry(int index, const std::array<int, 3>& colour) {
    if (index < 0 || index >= static_cast<int>(colors_.size())) return;
    colors_[static_cast<std::size_t>(index)] = colour;
    paint();
}

void ColorTableDialog::paint() {
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const bool used = i < colors_.size();
        cells[i]->setEnabled(used);
        if (used) paint_swatch(cells[i], colors_[i]);
        else cells[i]->setStyleSheet(QStringLiteral("background-color: transparent; border: 1px dotted #c0c0c0;"));
    }
    const QVariant was = transparent->currentData();
    transparent->clear();
    transparent->addItem(QStringLiteral("なし"), QVariant());
    for (std::size_t i = 0; i < colors_.size(); ++i) transparent->addItem(QStringLiteral("%1 番（%2）").arg(i).arg(hex(colors_[i])), static_cast<int>(i));
    if (was.isValid()) transparent->setCurrentIndex(std::max(0, transparent->findData(was)));
}

Json ColorTableDialog::op() const {
    Json colours = Json::array();
    for (const auto& c : colors_) colours.push_back(Json::array({c[0], c[1], c[2]}));
    const QVariant clear = transparent->currentData();
    return Json{{"op", "set_color_table"}, {"page", page_.index.json()}, {"table", "custom"}, {"colors", std::move(colours)},
                {"transparent", clear.isValid() ? Json(clear.toInt()) : Json()}};
}

// --- 2 階調 --------------------------------------------------------------------------------------------------------------

BitmapDialog::BitmapDialog(QWidget* parent, const core::Page* page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("bitmap_dialog"));
    setWindowTitle(QStringLiteral("2 階調"));
    scope = scope_box(page, QStringLiteral("bitmap_scope"));
    method = combo(QStringLiteral("bitmap_method"),
                   {{"誤差拡散法", "diffusion"}, {"50% を基準に 2 階調", "threshold"}, {"パターンディザ", "pattern"}, {"ハーフトーンスクリーン", "halftone"}});
    frequency = new QDoubleSpinBox;
    frequency->setObjectName(QStringLiteral("bitmap_frequency"));
    frequency->setRange(1, 999);
    frequency->setDecimals(1);
    frequency->setValue(53);
    frequency->setSuffix(QStringLiteral(" 線/インチ"));
    angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("bitmap_angle"));
    angle->setRange(-180, 180);
    angle->setDecimals(1);
    angle->setValue(45);
    angle->setSuffix(QStringLiteral(" 度"));
    shape = combo(QStringLiteral("bitmap_shape"),
                  {{"円", "round"}, {"楕円", "ellipse"}, {"線", "line"}, {"正方形", "square"}, {"ひし形", "diamond"}, {"十字", "cross"}});
    auto* note = new QLabel(QStringLiteral("グレースケールのページの画素レイヤーを 1 枚に統合し、白の上で黒と白の 2 階調にします（取り消せます）。"
                                           "コマ・台詞は要素のまま残り、2 階調で表示します。"));
    note->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("方法"), method);
    form->addRow(QStringLiteral("線数"), frequency);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QStringLiteral("網点の形"), shape);
    form->addRow(note);
    const auto enable = [this] {
        const bool halftone = data_of(method) == QLatin1String("halftone");
        frequency->setEnabled(halftone);
        angle->setEnabled(halftone);
        shape->setEnabled(halftone);
    };
    connect(method, &QComboBox::currentIndexChanged, this, enable);
    enable();
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

Json BitmapDialog::op() const {
    Json op{{"op", "to_bitmap"}};
    put_scope(op, scope, page_);
    op["method"] = data_of(method).toStdString();
    op["frequency"] = frequency->value();
    op["angle"] = angle->value();
    op["shape"] = data_of(shape).toStdString();
    return op;
}

// --- ダブルトーン -----------------------------------------------------------------------------------------------------

DuotoneDialog::DuotoneDialog(QWidget* parent, const core::Page* page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("duotone_dialog"));
    setWindowTitle(QStringLiteral("ダブルトーン"));
    scope = scope_box(page, QStringLiteral("duotone_scope"));
    type = combo(QStringLiteral("duotone_type"),
                 {{"モノトーン", "monotone"}, {"ダブルトーン", "duotone"}, {"トライトーン", "tritone"}, {"クワッドトーン", "quadtone"}});
    type->setCurrentIndex(1);
    const std::vector<core::DuotoneInk> now = page != nullptr && page->color ? page->color->duotone : std::vector<core::DuotoneInk>{};
    if (!now.empty()) type->setCurrentIndex(static_cast<int>(std::min<std::size_t>(now.size(), 4)) - 1);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("種類"), type);
    for (std::size_t k = 0; k < 4; ++k) {
        const bool own = k < now.size();
        chosen_[k] = own ? now[k].color : default_inks()[k].second;
        names[k] = new QLineEdit(own ? QString::fromStdString(now[k].name) : QString::fromUtf8(default_inks()[k].first));
        names[k]->setObjectName(QStringLiteral("duotone_name_%1").arg(k + 1));
        names[k]->setMaxLength(static_cast<int>(core::kInkNameMax));
        colours[k] = new QPushButton(QStringLiteral("色…"));
        colours[k]->setObjectName(QStringLiteral("duotone_colour_%1").arg(k + 1));
        paint_swatch(colours[k], chosen_[k]);
        connect(colours[k], &QPushButton::clicked, this, [this, k] {
            const auto chosen = ask::colour(this, QColor(chosen_[k][0], chosen_[k][1], chosen_[k][2]), QStringLiteral("インキ %1 の色").arg(k + 1));
            if (!chosen) return;
            chosen_[k] = {chosen->red(), chosen->green(), chosen->blue()};
            paint_swatch(colours[k], chosen_[k]);
        });
        curves[k] = new QLineEdit(curve_text(own ? now[k].curve : core::DuotoneInk{}.curve));
        curves[k]->setObjectName(QStringLiteral("duotone_curve_%1").arg(k + 1));
        curves[k]->setToolTip(QStringLiteral("グレーの濃さ（0〜100）とインキの濃さ（0〜100）の組を「入力:出力」でカンマ区切りに 2〜13 点"));
        connect(curves[k], &QLineEdit::textChanged, this, [this] { enable(); });
        auto* row = new QWidget;
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(names[k], 2);
        line->addWidget(colours[k]);
        line->addWidget(curves[k], 3);
        form->addRow(QStringLiteral("インキ %1").arg(k + 1), row);
    }
    auto* note = new QLabel(QStringLiteral("グレースケールの画素はそのままで、インキの色とカーブで刷った見え方にします（取り消せます）。"
                                           "グレースケールに戻すとインキが外れます。"));
    note->setWordWrap(true);
    form->addRow(note);
    connect(type, &QComboBox::currentIndexChanged, this, [this] { enable(); });
    buttons = ok_cancel(this);
    enable();
    lay_out_dialog(this, form, buttons);
}

void DuotoneDialog::enable() {
    const int n = type->currentIndex() + 1;
    for (int k = 0; k < 4; ++k) {
        names[static_cast<std::size_t>(k)]->setEnabled(k < n);
        colours[static_cast<std::size_t>(k)]->setEnabled(k < n);
        curves[static_cast<std::size_t>(k)]->setEnabled(k < n);
    }
    if (buttons != nullptr) buttons->button(QDialogButtonBox::Ok)->setEnabled(problem().isEmpty());
}

QString DuotoneDialog::problem() const {
    const int n = type->currentIndex() + 1;
    for (int k = 0; k < n; ++k) {
        if (names[static_cast<std::size_t>(k)]->text().trimmed().isEmpty()) return QStringLiteral("インキ %1 の名前を入れます").arg(k + 1);
        if (!curve_of(curves[static_cast<std::size_t>(k)]->text())) {
            return QStringLiteral("インキ %1 のカーブが読めません（0〜100 の「入力:出力」を 2〜13 点）").arg(k + 1);
        }
    }
    return {};
}

Json DuotoneDialog::op() const {
    Json op{{"op", "set_duotone"}};
    put_scope(op, scope, page_);
    op["type"] = data_of(type).toStdString();
    Json inks = Json::array();
    for (int k = 0; k <= type->currentIndex(); ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        Json ink{{"name", names[at]->text().trimmed().toStdString()}, {"color", Json::array({chosen_[at][0], chosen_[at][1], chosen_[at][2]})}};
        if (const auto curve = curve_of(curves[at]->text())) ink["curve"] = *curve;
        inks.push_back(std::move(ink));
    }
    op["inks"] = std::move(inks);
    return op;
}

}  // namespace genko::app
