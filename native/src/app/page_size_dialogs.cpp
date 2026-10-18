#include "app/page_size_dialogs.hpp"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/book_dialogs.hpp"
#include "app/theme.hpp"

namespace genko::app {

using core::Json;

namespace {

// The page in front or every page.
QComboBox* scope_box(const core::Page& page, const QString& name, bool all_first) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    box->addItem(QStringLiteral("このページ（%1 ページ）").arg(QString::fromStdString(page.index.repr())), QStringLiteral("page"));
    box->addItem(QStringLiteral("すべてのページ"), QStringLiteral("all"));
    box->setCurrentIndex(all_first ? 1 : 0);
    return box;
}

// The op's pages: the page in front's number, or every page.
void put_scope(Json& op, const QComboBox* scope, const core::Page& page) {
    if (scope->currentData().toString() == QLatin1String("page")) {
        op["page"] = page.index.json();
    } else {
        op["pages"] = "all";
    }
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

QDoubleSpinBox* mm_box(const QString& name, double lo, double hi, double value) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(name);
    box->setRange(lo, hi);
    box->setDecimals(2);
    box->setSuffix(QStringLiteral(" mm"));
    box->setValue(value);
    return box;
}

// Where the finished size goes when the paper changes.
QComboBox* trim_box(const QString& name) {
    QComboBox* box = combo(name, {{"中身と一緒に残す（新しい用紙からはみ出すなら断る）", "keep"}, {"新しい用紙に合わせる", "fit"}});
    box->setToolTip(QStringLiteral("仕上がり・裁ち落とし・基本枠は、ふだんはページの中身と一緒に元の所に残ります。"
                                   "新しい用紙に合わせると、仕上がりを用紙いっぱい（裁ち落としの分を残して）にし、余白は入るように縮めます"));
    return box;
}

void put_trim(Json& op, const QComboBox* trim) {
    if (data_of(trim) == QLatin1String("fit")) op["trim"] = "fit";
}

double r2(double v) { return std::round(v * 100.0) / 100.0; }

}  // namespace

// --- 画像解像度 ---------------------------------------------------------------------------------------------------------

ImageSizeDialog::ImageSizeDialog(QWidget* parent, const core::Page& page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("image_size_dialog"));
    setWindowTitle(QStringLiteral("画像解像度"));
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    ratio_ = h > 0 ? w / h : 1;
    scope = scope_box(page, QStringLiteral("image_size_scope"), true);
    unit = combo(QStringLiteral("image_size_unit"), {{"mm", "mm"}, {"pixel（解像度で）", "px"}});
    width = mm_box(QStringLiteral("image_size_width"), 1, 100000, w);
    height = mm_box(QStringLiteral("image_size_height"), 1, 100000, h);
    keep_ratio = new QCheckBox(QStringLiteral("縦横比を固定"));
    keep_ratio->setObjectName(QStringLiteral("image_size_ratio"));
    keep_ratio->setChecked(true);
    dpi = new QSpinBox;
    dpi->setObjectName(QStringLiteral("image_size_dpi"));
    dpi->setRange(1, 10000);
    dpi->setValue(static_cast<int>(std::lround(page.spec.dpi.value())));
    dpi->setSuffix(QStringLiteral(" dpi"));
    resample = new QCheckBox(QStringLiteral("再サンプル"));
    resample->setObjectName(QStringLiteral("image_size_resample"));
    resample->setChecked(true);
    resample->setToolTip(QStringLiteral("外すと画素の数を保ったまま解像度だけを変えます（用紙の mm がそのぶん変わります。全ページのとき）"));
    method = combo(QStringLiteral("image_size_method"), {{"バイキュービック法（滑らかなグラデーション）", "bicubic"},
                                                         {"バイリニア法", "bilinear"},
                                                         {"ニアレストネイバー法（ハードな輪郭）", "nearest"},
                                                         {"Lanczos 法", "lanczos"},
                                                         {"ディテールを保持（拡大）", "preserve_details"}});
    summary = new QLabel;
    summary->setObjectName(QStringLiteral("image_size_summary"));
    summary->setWordWrap(true);
    theme::role(summary, "hint");
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("単位"), unit);
    form->addRow(QStringLiteral("幅"), width);
    form->addRow(QStringLiteral("高さ"), height);
    form->addRow(QString(), keep_ratio);
    form->addRow(QStringLiteral("解像度"), dpi);
    form->addRow(QString(), resample);
    form->addRow(QStringLiteral("再サンプルの方法"), method);
    form->addRow(summary);
    connect(width, &QDoubleSpinBox::valueChanged, this, [this] { follow(true); });
    connect(height, &QDoubleSpinBox::valueChanged, this, [this] { follow(false); });
    connect(unit, &QComboBox::currentIndexChanged, this, [this] { show_unit(); });
    const auto enable = [this] {
        // (the resolution is the whole book's; without resampling the size follows it)
        const bool all = data_of(scope) == QLatin1String("all");
        dpi->setEnabled(all);
        resample->setEnabled(all);
        if (!all) resample->setChecked(true);
        const bool sizes = resample->isChecked();
        width->setEnabled(sizes);
        height->setEnabled(sizes);
        unit->setEnabled(sizes);
        method->setEnabled(sizes);
        follow(true);
    };
    connect(scope, &QComboBox::currentIndexChanged, this, enable);
    connect(resample, &QCheckBox::toggled, this, enable);
    connect(dpi, &QSpinBox::valueChanged, this, [this] { show_unit(); });
    enable();
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

double ImageSizeDialog::mm_of(double value) const { return unit_now_ == QLatin1String("px") ? value / dpi->value() * 25.4 : value; }
double ImageSizeDialog::value_of(double mm) const { return unit_now_ == QLatin1String("px") ? mm / 25.4 * dpi->value() : mm; }

void ImageSizeDialog::show_unit() {
    // (the same size in the unit chosen; without resampling, the pixels kept: the mm follow the resolution)
    const double page_dpi = page_.spec.dpi.value();
    double w_mm = mm_of(width->value()), h_mm = mm_of(height->value());
    if (!resample->isChecked()) {
        w_mm = page_.spec.width_mm.value() * page_dpi / dpi->value();
        h_mm = page_.spec.height_mm.value() * page_dpi / dpi->value();
    }
    unit_now_ = data_of(unit);
    following_ = true;
    const QString suffix = unit_now_ == QLatin1String("px") ? QStringLiteral(" px") : QStringLiteral(" mm");
    for (QDoubleSpinBox* box : {width, height}) {
        box->setSuffix(suffix);
        box->setDecimals(unit_now_ == QLatin1String("px") ? 0 : 2);
    }
    width->setValue(value_of(w_mm));
    height->setValue(value_of(h_mm));
    following_ = false;
    summary->setText(QStringLiteral("新しい用紙: %1 × %2 mm（%3 × %4 pixel、%5 dpi）")
                         .arg(r2(w_mm))
                         .arg(r2(h_mm))
                         .arg(std::lround(w_mm / 25.4 * dpi->value()))
                         .arg(std::lround(h_mm / 25.4 * dpi->value()))
                         .arg(dpi->value()));
}

void ImageSizeDialog::follow(bool from_width) {
    if (following_) return;
    if (keep_ratio->isChecked()) {
        following_ = true;
        if (from_width) {
            height->setValue(value_of(mm_of(width->value()) / ratio_));
        } else {
            width->setValue(value_of(mm_of(height->value()) * ratio_));
        }
        following_ = false;
    }
    show_unit();
}

Json ImageSizeDialog::op() const {
    Json op{{"op", "resize_pages"}};
    put_scope(op, scope, page_);
    const bool dpi_changed = dpi->isEnabled() && dpi->value() != static_cast<int>(std::lround(page_.spec.dpi.value()));
    if (!resample->isChecked()) {
        op["resample"] = "none";
        op["dpi"] = dpi->value();
        return op;
    }
    op["resample"] = data_of(method).toStdString();
    const bool px = data_of(unit) == QLatin1String("px");
    op[px ? "width_px" : "width_mm"] = px ? std::round(width->value()) : r2(width->value());
    if (!keep_ratio->isChecked()) op[px ? "height_px" : "height_mm"] = px ? std::round(height->value()) : r2(height->value());
    if (dpi_changed) op["dpi"] = dpi->value();
    return op;
}

// --- カンバスサイズ -------------------------------------------------------------------------------------------------------

namespace {

constexpr std::array<const char*, 9> kAnchors{"top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right"};
constexpr std::array<const char*, 9> kAnchorMarks{"↖", "↑", "↗", "←", "●", "→", "↙", "↓", "↘"};
constexpr std::array<const char*, 9> kAnchorNames{"左上", "上", "右上", "左", "中央", "右", "左下", "下", "右下"};

}  // namespace

CanvasSizeDialog::CanvasSizeDialog(QWidget* parent, const core::Page& page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("canvas_size_dialog"));
    setWindowTitle(QStringLiteral("カンバスサイズ"));
    scope = scope_box(page, QStringLiteral("canvas_size_scope"), true);
    width = mm_box(QStringLiteral("canvas_size_width"), -100000, 100000, page.spec.width_mm.value());
    height = mm_box(QStringLiteral("canvas_size_height"), -100000, 100000, page.spec.height_mm.value());
    relative = new QCheckBox(QStringLiteral("相対"));
    relative->setObjectName(QStringLiteral("canvas_size_relative"));
    relative->setToolTip(QStringLiteral("今の用紙に足す（引く）大きさで決めます"));
    connect(relative, &QCheckBox::toggled, this, [this](bool on) {
        width->setValue(on ? 0.0 : page_.spec.width_mm.value());
        height->setValue(on ? 0.0 : page_.spec.height_mm.value());
    });
    auto* grid_host = new QWidget;
    auto* grid = new QGridLayout(grid_host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);
    for (std::size_t i = 0; i < kAnchors.size(); ++i) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("canvas_anchor_%1").arg(QString::fromLatin1(kAnchors[i])));
        button->setText(QString::fromUtf8(kAnchorMarks[i]));
        button->setToolTip(QString::fromUtf8(kAnchorNames[i]));
        button->setCheckable(true);
        const QString name = QString::fromLatin1(kAnchors[i]);
        connect(button, &QToolButton::clicked, this, [this, name] { choose_anchor(name); });
        grid->addWidget(button, static_cast<int>(i / 3), static_cast<int>(i % 3));
        anchors[i] = button;
    }
    choose_anchor(QStringLiteral("center"));
    extension = combo(QStringLiteral("canvas_size_extension"), {{"透明", "transparent"}, {"白", "white"}, {"黒", "black"}, {"その他（色を選ぶ）", "colour"}});
    extension->setToolTip(QStringLiteral("広げた所を、ページのいちばん下のペイントレイヤーにこの色で塗ります（透明: 用紙が見えます）"));
    colour = new QToolButton;
    colour->setObjectName(QStringLiteral("canvas_size_colour"));
    colour->setText(QStringLiteral("色…"));
    connect(colour, &QToolButton::clicked, this, [this] {
        if (const auto chosen = ask::colour(this, QColor(chosen_[0], chosen_[1], chosen_[2]), QStringLiteral("カンバス拡張カラー"))) {
            chosen_ = {chosen->red(), chosen->green(), chosen->blue()};
            extension->setCurrentIndex(extension->findData(QStringLiteral("colour")));
        }
    });
    trim = trim_box(QStringLiteral("canvas_size_trim"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("幅"), width);
    form->addRow(QStringLiteral("高さ"), height);
    form->addRow(QString(), relative);
    form->addRow(QStringLiteral("基準位置"), grid_host);
    form->addRow(QStringLiteral("カンバス拡張カラー"), extension);
    form->addRow(QString(), colour);
    form->addRow(QStringLiteral("仕上がり"), trim);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

void CanvasSizeDialog::choose_anchor(const QString& anchor) {
    anchor_ = anchor;
    for (std::size_t i = 0; i < kAnchors.size(); ++i) anchors[i]->setChecked(QString::fromLatin1(kAnchors[i]) == anchor);
}

Json CanvasSizeDialog::op() const {
    Json op{{"op", "set_canvas_size"}};
    put_scope(op, scope, page_);
    op["width_mm"] = r2(width->value());
    op["height_mm"] = r2(height->value());
    if (relative->isChecked()) op["relative"] = true;
    op["anchor"] = anchor_.toStdString();
    const QString how = data_of(extension);
    if (how == QLatin1String("white")) op["extension"] = Json::array({255, 255, 255});
    else if (how == QLatin1String("black")) op["extension"] = Json::array({0, 0, 0});
    else if (how == QLatin1String("colour")) op["extension"] = Json::array({chosen_[0], chosen_[1], chosen_[2]});
    else op["extension"] = "transparent";
    put_trim(op, trim);
    return op;
}

// --- 角度入力 -----------------------------------------------------------------------------------------------------------

RotateAngleDialog::RotateAngleDialog(QWidget* parent, const core::Page& page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("rotate_angle_dialog"));
    setWindowTitle(QStringLiteral("カンバスの回転"));
    scope = scope_box(page, QStringLiteral("rotate_scope"), false);
    angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("rotate_angle"));
    angle->setRange(-359.99, 359.99);
    angle->setDecimals(2);
    angle->setSuffix(QStringLiteral("°"));
    direction = combo(QStringLiteral("rotate_direction"), {{"時計回り", "cw"}, {"反時計回り", "ccw"}});
    interp = combo(QStringLiteral("rotate_interp"), {{"バイキュービック法", "bicubic"}, {"バイリニア法", "bilinear"}, {"ニアレストネイバー法", "nearest"}});
    auto* note = new QLabel(QStringLiteral("用紙は回したページが入る大きさに広がり、仕上がりは用紙の真ん中に置きます。台詞の文字は立てたまま、"
                                           "コマは自由な形に、ガイドは同じ所を通る線の定規になります（取り消せます）"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QString(), direction);
    form->addRow(QStringLiteral("補間方法"), interp);
    form->addRow(note);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

Json RotateAngleDialog::op() const {
    Json op{{"op", "rotate_pages"}};
    put_scope(op, scope, page_);
    const double a = std::round(angle->value() * 100.0) / 100.0;
    op["angle"] = data_of(direction) == QLatin1String("ccw") ? -a : a;
    op["interp"] = data_of(interp).toStdString();
    op["text"] = "upright";
    return op;
}

// --- トリミング -----------------------------------------------------------------------------------------------------------

TrimDialog::TrimDialog(QWidget* parent, const core::Page& page) : QDialog(parent), page_(page) {
    setObjectName(QStringLiteral("trim_dialog"));
    setWindowTitle(QStringLiteral("トリミング"));
    scope = scope_box(page, QStringLiteral("trim_scope"), false);
    based_on = combo(QStringLiteral("trim_based_on"), {{"透明ピクセル", "transparent"}, {"左上のピクセルの色", "top_left"}, {"右下のピクセルの色", "bottom_right"}});
    based_on->setToolTip(QStringLiteral("透明ピクセル: 用紙の色の上に何も描いていない所を切り取ります"));
    const auto side = [this](const QString& name, const QString& label) {
        auto* box = new QCheckBox(label);
        box->setObjectName(name);
        box->setChecked(true);
        return box;
    };
    top = side(QStringLiteral("trim_top"), QStringLiteral("上"));
    bottom = side(QStringLiteral("trim_bottom"), QStringLiteral("下"));
    left = side(QStringLiteral("trim_left"), QStringLiteral("左"));
    right = side(QStringLiteral("trim_right"), QStringLiteral("右"));
    auto* sides = new QWidget;
    auto* row = new QGridLayout(sides);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(top, 0, 0);
    row->addWidget(bottom, 0, 1);
    row->addWidget(left, 0, 2);
    row->addWidget(right, 0, 3);
    trim = trim_box(QStringLiteral("trim_trim"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("基準"), based_on);
    form->addRow(QStringLiteral("切り取る側"), sides);
    form->addRow(QStringLiteral("仕上がり"), trim);
    buttons = ok_cancel(this);
    lay_out_dialog(this, form, buttons);
}

Json TrimDialog::op() const {
    Json op{{"op", "trim_page"}};
    put_scope(op, scope, page_);
    op["based_on"] = data_of(based_on).toStdString();
    op["top"] = top->isChecked();
    op["bottom"] = bottom->isChecked();
    op["left"] = left->isChecked();
    op["right"] = right->isChecked();
    put_trim(op, trim);
    return op;
}

}  // namespace genko::app
