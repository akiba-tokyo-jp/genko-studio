// PC5 I-c (app/artboard_dialog.hpp): the artboard's windows. The presets after PhotoCraft v0.2.0's engine/src/artboard_cmds.rs
// (20-31, MIT, native/third_party/photocraft/NOTICE.md) and Genko's finished sizes (core/artboards).

#include "app/artboard_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>

#include "app/ask.hpp"
#include "app/look.hpp"
#include "formats/copy.hpp"
#include "render/op_limits.hpp"

namespace genko::app {

using core::Json;

namespace {

const char* const kBackgrounds[][2] = {{"white", "白"}, {"black", "黒"}, {"transparent", "透明"}, {"custom", "その他…"}};
const char* const kAnchors[][2] = {{"top_left", "左上"}, {"top", "上"},         {"top_right", "右上"}, {"left", "左"},          {"center", "中央"},
                                   {"right", "右"},     {"bottom_left", "左下"}, {"bottom", "下"},     {"bottom_right", "右下"}};

}  // namespace

ArtboardDialog::ArtboardDialog(QWidget* parent, const std::optional<core::Artboard>& now, const QString& given, int width_px, int height_px, double dpi)
    : QDialog(parent), dpi_(dpi), editing_(now.has_value()), start_w_(width_px), start_h_(height_px) {
    setObjectName(QStringLiteral("artboardDialog"));
    setWindowTitle(editing_ ? QStringLiteral("アートボードの設定") : QStringLiteral("新規アートボード"));
    name = new QLineEdit(now ? QString::fromStdString(now->name) : given);
    name->setObjectName(QStringLiteral("artboard_name"));
    preset = new QComboBox;
    preset->setObjectName(QStringLiteral("artboard_preset"));
    preset->addItem(QStringLiteral("カスタム"), QString());
    for (const core::ArtboardPreset& p : core::artboard_presets()) {
        const auto px = core::preset_pixels(p, dpi);
        const QString label = QString::fromUtf8(p.name.data(), static_cast<qsizetype>(p.name.size())) +
                              (p.mm ? QStringLiteral("（%1×%2 mm）").arg(p.width).arg(p.height) : QStringLiteral("（%1×%2 px）").arg(px[0]).arg(px[1]));
        preset->addItem(label, QString::fromUtf8(p.name.data(), static_cast<qsizetype>(p.name.size())));
    }
    if (now && now->preset) preset->setCurrentIndex(std::max(0, preset->findData(QString::fromStdString(*now->preset))));
    width = new QSpinBox;
    width->setObjectName(QStringLiteral("artboard_width"));
    height = new QSpinBox;
    height->setObjectName(QStringLiteral("artboard_height"));
    for (QSpinBox* s : {width, height}) {
        s->setRange(1, static_cast<int>(render::limits::kSide));
        s->setSuffix(QStringLiteral(" px"));
    }
    width->setValue(width_px);
    height->setValue(height_px);
    connect(preset, &QComboBox::currentIndexChanged, this, [this] {
        const core::ArtboardPreset* p = core::artboard_preset(preset->currentData().toString().toStdString());
        if (p == nullptr) return;
        const auto px = core::preset_pixels(*p, dpi_);
        const QSignalBlocker a(width), b(height);
        width->setValue(static_cast<int>(px[0]));
        height->setValue(static_cast<int>(px[1]));
    });
    // (a size given by hand is no more the preset's: カスタム)
    for (QSpinBox* s : {width, height}) {
        connect(s, &QSpinBox::valueChanged, this, [this] {
            const QSignalBlocker quiet(preset);
            preset->setCurrentIndex(0);
        });
    }
    background = new QComboBox;
    background->setObjectName(QStringLiteral("artboard_background"));
    for (const auto& [id, words] : kBackgrounds) background->addItem(QString::fromUtf8(words), QString::fromLatin1(id));
    if (now) background->setCurrentIndex(std::max(0, background->findData(QString::fromStdString(now->background))));
    if (now && now->rgb) rgb_ = *now->rgb;
    colour = new QPushButton;
    colour->setObjectName(QStringLiteral("artboard_colour"));
    connect(colour, &QPushButton::clicked, this, [this] {
        if (const auto got = ask::colour(this, QColor(rgb_[0], rgb_[1], rgb_[2]), QStringLiteral("アートボードの背景の色"))) {
            rgb_ = {got->red(), got->green(), got->blue()};
            show_colour();
        }
    });
    connect(background, &QComboBox::currentIndexChanged, this, [this] { show_colour(); });
    show_colour();
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(editing_ ? QStringLiteral("変える") : QStringLiteral("作る"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("名前"), name);
    rows->addRow(QStringLiteral("プリセット"), preset);
    rows->addRow(QStringLiteral("幅"), width);
    rows->addRow(QStringLiteral("高さ"), height);
    if (editing_) {
        anchor = new QComboBox;
        anchor->setObjectName(QStringLiteral("artboard_anchor"));
        for (const auto& [id, words] : kAnchors) anchor->addItem(QString::fromUtf8(words), QString::fromLatin1(id));
        move_contents = new QCheckBox(QStringLiteral("中身も一緒に動かす"));
        move_contents->setObjectName(QStringLiteral("artboard_move_contents"));
        move_contents->setChecked(true);
        rows->addRow(QStringLiteral("基準位置"), anchor);
        rows->addRow(QString(), move_contents);
    }
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(background, 1);
    line->addWidget(colour);
    auto* back_row = new QWidget;
    back_row->setLayout(line);
    rows->addRow(QStringLiteral("アートボードの背景色"), back_row);
    look::quiet_labels(rows);
    look::frame(this,
                look::header(editing_ ? QStringLiteral("アートボードの設定") : QStringLiteral("新規アートボード"),
                             QStringLiteral("アートボードは 1 ページです。大きさは原稿の解像度（%1 dpi）の pixel で決めます").arg(dpi)),
                rows, nullptr, look::footer(buttons));
}

void ArtboardDialog::show_colour() {
    const bool custom = background->currentData().toString() == QLatin1String("custom");
    colour->setEnabled(custom);
    colour->setText(custom ? QStringLiteral("#%1%2%3")
                                 .arg(rgb_[0], 2, 16, QLatin1Char('0'))
                                 .arg(rgb_[1], 2, 16, QLatin1Char('0'))
                                 .arg(rgb_[2], 2, 16, QLatin1Char('0'))
                           : QStringLiteral("色…"));
}

Json ArtboardDialog::settings() const {
    Json out = Json::object();
    if (!name->text().trimmed().isEmpty()) out["name"] = name->text().trimmed().toStdString();
    if (const QString p = preset->currentData().toString(); !p.isEmpty()) out["preset"] = p.toStdString();
    if (!editing_ || width->value() != start_w_ || height->value() != start_h_ || out.contains("preset")) {
        out["width_px"] = width->value();
        out["height_px"] = height->value();
    }
    const std::string b = background->currentData().toString().toStdString();
    out["background"] = b;
    if (b == "custom") out["rgb"] = Json::array({rgb_[0], rgb_[1], rgb_[2]});
    if (editing_ && out.contains("width_px")) {
        out["anchor"] = anchor->currentData().toString().toStdString();
        out["move_contents"] = move_contents->isChecked();
    }
    return out;
}

ArtboardExportDialog::ArtboardExportDialog(QWidget* parent, bool pdf, const QString& start, const QString& name, const std::vector<std::string>& names)
    : QDialog(parent) {
    setObjectName(pdf ? QStringLiteral("artboardsToPdfDialog") : QStringLiteral("artboardsToFilesDialog"));
    const QString title = pdf ? QStringLiteral("アートボードから PDF") : QStringLiteral("アートボードからファイル");
    setWindowTitle(title);
    where = new QLineEdit(QDir::toNativeSeparators(start));
    where->setObjectName(QStringLiteral("artboards_where"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this, pdf] {
        const QString got = pdf ? ask::save_path(this, QStringLiteral("PDF の書き出し先"), where->text(), QStringLiteral("PDF (*.pdf)"))
                                : ask::existing_dir(this, QStringLiteral("書き出し先"), where->text());
        if (!got.isEmpty()) where->setText(QDir::toNativeSeparators(got));
    });
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(where, 1);
    line->addWidget(pick);
    auto* where_row = new QWidget;
    where_row->setLayout(line);
    artboards = new QListWidget;
    artboards->setObjectName(QStringLiteral("artboards_list"));
    for (const std::string& n : names) {
        auto* item = new QListWidgetItem(QString::fromStdString(n), artboards);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    artboards->setMaximumHeight(160);
    quality = new QSpinBox;
    quality->setObjectName(QStringLiteral("artboards_quality"));
    quality->setRange(0, 12);
    quality->setValue(10);
    quality->setToolTip(QStringLiteral("JPEG の画質（0〜12。12 が最高）"));
    QFormLayout* rows = look::form();
    rows->addRow(pdf ? QStringLiteral("書き出し先（PDF）") : QStringLiteral("書き出し先"), where_row);
    rows->addRow(QStringLiteral("アートボード"), artboards);
    if (!pdf) {
        prefix = new QLineEdit(name);
        prefix->setObjectName(QStringLiteral("artboards_prefix"));
        format = new QComboBox;
        format->setObjectName(QStringLiteral("artboards_format"));
        for (const std::string_view ext : formats::copy_extensions())
            format->addItem(QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size())).toUpper(), QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size())));
        format->setCurrentIndex(std::max(0, format->findData(QStringLiteral("png"))));
        one_psd = new QCheckBox(QStringLiteral("アートボードを 1 つの PSD に"));
        one_psd->setObjectName(QStringLiteral("artboards_one_psd"));
        one_psd->setToolTip(QStringLiteral("選んだアートボードを、Photoshop のアートボードとして 1 つの PSD（PSB）に並べて書きます"));
        const auto kinds = [this] {
            const QString f = format->currentData().toString();
            quality->setEnabled(f == QLatin1String("jpg") || f == QLatin1String("jpeg"));
            one_psd->setEnabled(f == QLatin1String("psd") || f == QLatin1String("psb"));
            if (!one_psd->isEnabled()) one_psd->setChecked(false);
        };
        connect(format, &QComboBox::currentIndexChanged, this, kinds);
        kinds();
        rows->addRow(QStringLiteral("ファイル名の接頭辞"), prefix);
        rows->addRow(QStringLiteral("ファイル形式"), format);
        rows->addRow(QString(), one_psd);
    }
    rows->addRow(QStringLiteral("JPEG の画質"), quality);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    look::quiet_labels(rows);
    look::frame(this,
                look::header(title, pdf ? QStringLiteral("選んだアートボードを 1 枚ずつ JPEG にして、1 つの PDF に書き出します")
                                        : QStringLiteral("選んだアートボードを 1 つずつファイルに書き出します（接頭辞_アートボード名）")),
                rows, nullptr, look::footer(buttons));
}

std::vector<std::string> ArtboardExportDialog::chosen() const {
    std::vector<std::string> out;
    for (int i = 0; i < artboards->count(); ++i) {
        if (artboards->item(i)->checkState() == Qt::Checked) out.push_back(artboards->item(i)->text().toStdString());
    }
    return out;
}

}  // namespace genko::app
