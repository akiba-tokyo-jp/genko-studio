// The windows of ファイル → 自動処理 ▸ and スクリプト ▸ (PC5 I-e, SPEC.md AUTO-01): app/automate_dialogs.hpp.

#include "app/automate_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>
#include <map>

#include "app/ask.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "service/actions.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;
namespace automate = service::automate;
namespace events = service::script_events;

namespace {

fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

const QString kStepsFilter = QStringLiteral("手順 (*.json *.txt *.genkoscript);;すべてのファイル (*)");

// A line of a path with its 選択… button (a folder, or a file to read).
QWidget* path_row(QWidget* parent, QLineEdit*& edit, const QString& caption, bool folder, const QString& filter = {}) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit(row);
    auto* pick = new QPushButton(QStringLiteral("選択…"), row);
    layout->addWidget(edit, 1);
    layout->addWidget(pick);
    QLineEdit* line = edit;
    QObject::connect(pick, &QPushButton::clicked, row, [row, line, caption, folder, filter] {
        const QString chosen = folder ? ask::existing_dir(row, caption, line->text()) : ask::open_path(row, caption, filter);
        if (!chosen.isEmpty()) line->setText(QDir::toNativeSeparators(chosen));
    });
    return row;
}

// The formats write_copy writes (same: the picture's own).
QComboBox* format_box(QWidget* parent, bool same) {
    auto* box = new QComboBox(parent);
    if (same) box->addItem(QStringLiteral("元のファイルと同じ"), QStringLiteral("same"));
    for (const std::string_view ext : formats::copy_extensions()) {
        const QString e = QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size()));
        box->addItem(e.toUpper(), e);
    }
    return box;
}

QSpinBox* quality_box(QWidget* parent, int value) {
    auto* box = new QSpinBox(parent);
    box->setRange(0, 12);
    box->setValue(value);
    box->setToolTip(QStringLiteral("JPEG の画質（0〜12。12 が最高）"));
    return box;
}

// 実行: the オートアクション (and a steps file); `none`: a first item for no steps.
QWidget* steps_row(QWidget* parent, StepsChoice& choice, bool none) {
    auto* row = new QWidget(parent);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    choice.box = new QComboBox(row);
    if (none) choice.box->addItem(QStringLiteral("（なし）"), QString());
    const Json saved = service::actions::load();
    for (const auto& [name, item] : saved.items()) {
        choice.box->addItem(QStringLiteral("アクション: %1").arg(QString::fromStdString(name)), QStringLiteral("action:") + QString::fromStdString(name));
    }
    choice.box->addItem(QStringLiteral("手順のファイル…"), QStringLiteral("file"));
    layout->addWidget(choice.box);
    QWidget* file = path_row(row, choice.file, QStringLiteral("手順のファイル"), false, kStepsFilter);
    layout->addWidget(file);
    QComboBox* box = choice.box;
    const auto shown = [box, file] { file->setVisible(box->currentData().toString() == QLatin1String("file")); };
    QObject::connect(box, &QComboBox::currentIndexChanged, row, shown);
    shown();
    return row;
}

QWidget* source_row(QWidget* parent, SourceChoice& choice, bool open) {
    auto* group = new QGroupBox(QStringLiteral("ソース"), parent);
    auto* layout = new QVBoxLayout(group);
    choice.box = new QComboBox(group);
    choice.box->addItem(QStringLiteral("フォルダー"), QStringLiteral("folder"));
    choice.box->addItem(QStringLiteral("ファイル"), QStringLiteral("files"));
    if (open) choice.box->addItem(QStringLiteral("開いている原稿"), QStringLiteral("open"));
    layout->addWidget(choice.box);
    QWidget* folder = path_row(group, choice.folder, QStringLiteral("ソースのフォルダー"), true);
    layout->addWidget(folder);
    choice.recurse = new QCheckBox(QStringLiteral("すべてのサブフォルダーを含める"), group);
    layout->addWidget(choice.recurse);
    auto* files = new QWidget(group);
    auto* files_layout = new QHBoxLayout(files);
    files_layout->setContentsMargins(0, 0, 0, 0);
    choice.files = new QListWidget(files);
    auto* buttons = new QVBoxLayout;
    auto* add = new QPushButton(QStringLiteral("追加…"), files);
    auto* remove = new QPushButton(QStringLiteral("削除"), files);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    files_layout->addWidget(choice.files, 1);
    files_layout->addLayout(buttons);
    layout->addWidget(files);
    QListWidget* list = choice.files;
    QObject::connect(add, &QPushButton::clicked, group, [group, list] {
        const QString chosen = ask::open_path(group, QStringLiteral("ファイルを追加"));
        if (!chosen.isEmpty()) list->addItem(QDir::toNativeSeparators(chosen));
    });
    QObject::connect(remove, &QPushButton::clicked, group, [list] { delete list->currentItem(); });
    QComboBox* box = choice.box;
    QCheckBox* recurse = choice.recurse;
    const auto shown = [box, folder, recurse, files] {
        const QString kind = box->currentData().toString();
        folder->setVisible(kind == QLatin1String("folder"));
        recurse->setVisible(kind == QLatin1String("folder"));
        files->setVisible(kind == QLatin1String("files"));
    };
    QObject::connect(box, &QComboBox::currentIndexChanged, group, shown);
    shown();
    return group;
}

std::optional<QString> source_problem(const SourceChoice& source) {
    const QString kind = source.kind();
    if (kind == QLatin1String("folder") && source.folder->text().trimmed().isEmpty()) return QStringLiteral("ソースのフォルダーを選んでください");
    if (kind == QLatin1String("files") && source.files->count() == 0) return QStringLiteral("ソースのファイルを追加してください");
    return std::nullopt;
}

QDialogButtonBox* ok_cancel(QDialog* dialog, const QString& ok, std::function<std::optional<QString>()> problem) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(ok);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, problem] {
        if (const auto why = problem()) {
            ask::warning(dialog, dialog->windowTitle(), *why);
            return;
        }
        dialog->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

}  // namespace

// --- 実行・ソース -------------------------------------------------------------------------------------------------------

std::optional<StepsChoice::Chosen> StepsChoice::chosen() const {
    const QString data = box->currentData().toString();
    if (data.isEmpty()) return std::nullopt;
    Chosen out;
    if (data == QLatin1String("file")) {
        const fs::path file = fpath(QDir::fromNativeSeparators(this->file->text().trimmed()));
        out.steps = automate::read_steps_file(file);
        out.name = core::path_to_utf8(file.stem());
        return out;
    }
    out.name = data.mid(QStringLiteral("action:").size()).toStdString();
    const Json saved = service::actions::load();
    const auto found = saved.find(out.name);
    if (found == saved.end()) throw core::Error("not_found", "no action " + out.name);
    out.steps = (*found)["ops"];
    out.recorded = true;
    return out;
}

QString SourceChoice::kind() const { return box->currentData().toString(); }

// --- バッチ… -------------------------------------------------------------------------------------------------------------

BatchDialog::BatchDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("バッチ"));
    auto* layout = new QVBoxLayout(this);
    auto* run = new QGroupBox(QStringLiteral("実行"), this);
    auto* run_layout = new QFormLayout(run);
    run_layout->addRow(QStringLiteral("アクション"), steps_row(run, steps, false));
    layout->addWidget(run);
    layout->addWidget(source_row(this, source, true));
    auto* dest = new QGroupBox(QStringLiteral("実行後"), this);
    auto* dest_layout = new QFormLayout(dest);
    destination = new QComboBox(dest);
    destination->addItem(QStringLiteral("なし"), QStringLiteral("none"));
    destination->addItem(QStringLiteral("保存して閉じる（Genko の原稿だけ）"), QStringLiteral("save"));
    destination->addItem(QStringLiteral("フォルダー"), QStringLiteral("folder"));
    destination->setCurrentIndex(2);
    dest_layout->addRow(QStringLiteral("実行先"), destination);
    QWidget* folder_row = path_row(dest, folder, QStringLiteral("実行先のフォルダー"), true);
    dest_layout->addRow(QStringLiteral("フォルダー"), folder_row);
    prefix = new QLineEdit(dest);
    suffix = new QLineEdit(dest);
    numbered = new QCheckBox(QStringLiteral("3 桁の連番（_001）を付ける"), dest);
    dest_layout->addRow(QStringLiteral("ファイル名の前"), prefix);
    dest_layout->addRow(QStringLiteral("ファイル名の後"), suffix);
    dest_layout->addRow(QString(), numbered);
    format = format_box(dest, true);
    quality = quality_box(dest, 10);
    dest_layout->addRow(QStringLiteral("形式"), format);
    dest_layout->addRow(QStringLiteral("画質（JPEG）"), quality);
    layout->addWidget(dest);
    const auto shown = [this, folder_row] {
        const bool to_folder = destination->currentData().toString() == QLatin1String("folder");
        for (QWidget* w : {folder_row, static_cast<QWidget*>(prefix), static_cast<QWidget*>(suffix), static_cast<QWidget*>(numbered),
                           static_cast<QWidget*>(format), static_cast<QWidget*>(quality)}) {
            w->setEnabled(to_folder);
        }
    };
    connect(destination, &QComboBox::currentIndexChanged, this, shown);
    shown();
    auto* errors = new QFormLayout;
    on_error = new QComboBox(this);
    on_error->addItem(QStringLiteral("エラーで停止"), QStringLiteral("stop"));
    on_error->addItem(QStringLiteral("エラーをログに記録して続ける"), QStringLiteral("log"));
    on_error->setCurrentIndex(1);
    errors->addRow(QStringLiteral("エラー"), on_error);
    layout->addLayout(errors);
    layout->addWidget(ok_cancel(this, QStringLiteral("OK"), [this] { return problem(); }));
}

std::optional<QString> BatchDialog::problem() const {
    if (steps.box->currentData().toString() == QLatin1String("file") && steps.file->text().trimmed().isEmpty()) {
        return QStringLiteral("流す手順のファイルを選んでください");
    }
    if (steps.box->count() == 0) return QStringLiteral("流すアクションか手順のファイルを選んでください");
    if (const auto why = source_problem(source)) return why;
    const QString to = destination->currentData().toString();
    if (to == QLatin1String("folder") && folder->text().trimmed().isEmpty()) return QStringLiteral("実行先のフォルダーを選んでください");
    return std::nullopt;
}

// --- ドロップレットを作成… -------------------------------------------------------------------------------------------------

DropletDialog::DropletDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("ドロップレットを作成"));
    auto* layout = new QVBoxLayout(this);
    auto* save = new QGroupBox(QStringLiteral("ドロップレットを保存"), this);
    auto* save_layout = new QFormLayout(save);
    auto* place_row = new QWidget(save);
    auto* place_layout = new QHBoxLayout(place_row);
    place_layout->setContentsMargins(0, 0, 0, 0);
    place = new QLineEdit(place_row);
    auto* pick = new QPushButton(QStringLiteral("選択…"), place_row);
    place_layout->addWidget(place, 1);
    place_layout->addWidget(pick);
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString chosen = ask::save_path(this, QStringLiteral("ドロップレットを保存"), QStringLiteral("ドロップレット.genkodroplet"),
                                              QStringLiteral("Genko のドロップレット (*.genkodroplet)"));
        if (!chosen.isEmpty()) place->setText(QDir::toNativeSeparators(chosen));
    });
    save_layout->addRow(QStringLiteral("保存先"), place_row);
    launcher = new QCheckBox(QStringLiteral("起動ファイルも作る（ファイルをドロップして実行）"), save);
    launcher->setChecked(true);
    save_layout->addRow(QString(), launcher);
    layout->addWidget(save);
    auto* run = new QGroupBox(QStringLiteral("実行"), this);
    auto* run_layout = new QFormLayout(run);
    run_layout->addRow(QStringLiteral("アクション"), steps_row(run, steps, false));
    layout->addWidget(run);
    auto* dest = new QGroupBox(QStringLiteral("実行後"), this);
    auto* dest_layout = new QFormLayout(dest);
    own_folder = new QCheckBox(QStringLiteral("フォルダーを決めておく（決めないときは、ファイルの隣の droplet-output）"), dest);
    dest_layout->addRow(QString(), own_folder);
    QWidget* folder_row = path_row(dest, folder, QStringLiteral("実行先のフォルダー"), true);
    dest_layout->addRow(QStringLiteral("フォルダー"), folder_row);
    connect(own_folder, &QCheckBox::toggled, folder_row, &QWidget::setEnabled);
    folder_row->setEnabled(false);
    prefix = new QLineEdit(dest);
    suffix = new QLineEdit(dest);
    dest_layout->addRow(QStringLiteral("ファイル名の前"), prefix);
    dest_layout->addRow(QStringLiteral("ファイル名の後"), suffix);
    format = format_box(dest, true);
    quality = quality_box(dest, 10);
    dest_layout->addRow(QStringLiteral("形式"), format);
    dest_layout->addRow(QStringLiteral("画質（JPEG）"), quality);
    layout->addWidget(dest);
    layout->addWidget(ok_cancel(this, QStringLiteral("作成"), [this] { return problem(); }));
}

std::optional<QString> DropletDialog::problem() const {
    if (place->text().trimmed().isEmpty()) return QStringLiteral("ドロップレットの保存先を選んでください");
    if (steps.box->currentData().toString() == QLatin1String("file") && steps.file->text().trimmed().isEmpty()) {
        return QStringLiteral("流す手順のファイルを選んでください");
    }
    if (own_folder->isChecked() && folder->text().trimmed().isEmpty()) return QStringLiteral("実行先のフォルダーを選んでください");
    return std::nullopt;
}

// --- 画像プロセッサー… ---------------------------------------------------------------------------------------------------

ImageProcessorDialog::ImageProcessorDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("画像プロセッサー"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("① 処理する画像を選択"), this));
    layout->addWidget(source_row(this, source, false));
    layout->addWidget(new QLabel(QStringLiteral("② 画像を保存する場所を選択（形式ごとのフォルダーに書きます）"), this));
    layout->addWidget(path_row(this, folder, QStringLiteral("保存先のフォルダー"), true));
    auto* kinds = new QGroupBox(QStringLiteral("③ ファイル形式"), this);
    auto* kinds_layout = new QFormLayout(kinds);
    jpeg = new QCheckBox(QStringLiteral("JPEG 形式で保存"), kinds);
    jpeg->setChecked(true);
    quality = quality_box(kinds, 8);
    kinds_layout->addRow(jpeg, quality);
    psd = new QCheckBox(QStringLiteral("PSD 形式で保存"), kinds);
    tiff = new QCheckBox(QStringLiteral("TIFF 形式で保存"), kinds);
    png = new QCheckBox(QStringLiteral("PNG 形式で保存"), kinds);
    kinds_layout->addRow(psd);
    kinds_layout->addRow(tiff);
    kinds_layout->addRow(png);
    fit = new QCheckBox(QStringLiteral("サイズに合わせる（拡大はしません）"), kinds);
    auto* size = new QWidget(kinds);
    auto* size_layout = new QHBoxLayout(size);
    size_layout->setContentsMargins(0, 0, 0, 0);
    width = new QSpinBox(size);
    height = new QSpinBox(size);
    for (QSpinBox* box : {width, height}) {
        box->setRange(1, 100000);
        box->setValue(1200);
        box->setSuffix(QStringLiteral(" px"));
    }
    size_layout->addWidget(new QLabel(QStringLiteral("W:"), size));
    size_layout->addWidget(width);
    size_layout->addWidget(new QLabel(QStringLiteral("H:"), size));
    size_layout->addWidget(height);
    kinds_layout->addRow(fit, size);
    connect(fit, &QCheckBox::toggled, size, &QWidget::setEnabled);
    size->setEnabled(false);
    layout->addWidget(kinds);
    auto* more = new QGroupBox(QStringLiteral("④ 環境設定"), this);
    auto* more_layout = new QFormLayout(more);
    run_action = new QCheckBox(QStringLiteral("アクションを実行"), more);
    QWidget* action_row = steps_row(more, steps, false);
    more_layout->addRow(run_action, action_row);
    connect(run_action, &QCheckBox::toggled, action_row, &QWidget::setEnabled);
    action_row->setEnabled(false);
    srgb = new QCheckBox(QStringLiteral("プロファイルを sRGB に変換"), more);
    more_layout->addRow(srgb);
    layout->addWidget(more);
    layout->addWidget(ok_cancel(this, QStringLiteral("実行"), [this] { return problem(); }));
}

service::automate::ImageProcessor ImageProcessorDialog::processor() const {
    service::automate::ImageProcessor out;
    if (jpeg->isChecked()) out.formats.push_back("jpg");
    if (psd->isChecked()) out.formats.push_back("psd");
    if (tiff->isChecked()) out.formats.push_back("tif");
    if (png->isChecked()) out.formats.push_back("png");
    out.quality = quality->value();
    if (fit->isChecked()) {
        out.width_px = width->value();
        out.height_px = height->value();
    }
    out.to_srgb = srgb->isChecked();
    return out;
}

std::optional<QString> ImageProcessorDialog::problem() const {
    if (const auto why = source_problem(source)) return why;
    if (folder->text().trimmed().isEmpty()) return QStringLiteral("保存先のフォルダーを選んでください");
    if (!jpeg->isChecked() && !psd->isChecked() && !tiff->isChecked() && !png->isChecked()) return QStringLiteral("保存するファイル形式を 1 つ以上選んでください");
    if (run_action->isChecked() && steps.box->currentData().toString() == QLatin1String("file") && steps.file->text().trimmed().isEmpty()) {
        return QStringLiteral("流す手順のファイルを選んでください");
    }
    return std::nullopt;
}

// --- 画像を合わせる… -----------------------------------------------------------------------------------------------------

FitImageDialog::FitImageDialog(QWidget* parent, int width_px, int height_px) : QDialog(parent) {
    setWindowTitle(QStringLiteral("画像を合わせる"));
    auto* layout = new QFormLayout(this);
    width = new QSpinBox(this);
    height = new QSpinBox(this);
    width->setRange(1, 100000);
    height->setRange(1, 100000);
    width->setValue(width_px);
    height->setValue(height_px);
    width->setSuffix(QStringLiteral(" pixel"));
    height->setSuffix(QStringLiteral(" pixel"));
    layout->addRow(QStringLiteral("幅"), width);
    layout->addRow(QStringLiteral("高さ"), height);
    dont_enlarge = new QCheckBox(QStringLiteral("拡大しない"), this);
    layout->addRow(QString(), dont_enlarge);
    resample = new QComboBox(this);
    for (const auto& [id, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"bicubic", "バイキュービック法"}, {"bilinear", "バイリニア法"}, {"nearest", "ニアレストネイバー法"}, {"lanczos", "Lanczos 法"},
             {"preserve_details", "ディテールを保持"}}) {
        resample->addItem(QString::fromUtf8(name), QString::fromLatin1(id));
    }
    layout->addRow(QStringLiteral("再サンプル"), resample);
    layout->addRow(ok_cancel(this, QStringLiteral("OK"), [] { return std::optional<QString>(); }));
}

Json FitImageDialog::step(const Json& page) const {
    Json out{{"op", "fit_image"}, {"page", page}, {"width_px", width->value()}, {"height_px", height->value()},
             {"resample", resample->currentData().toString().toStdString()}};
    if (dont_enlarge->isChecked()) out["dont_enlarge"] = true;
    return out;
}

// --- 条件付きモード変更… -------------------------------------------------------------------------------------------------

ModeChangeDialog::ModeChangeDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("条件付きモード変更"));
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("変換元のモード"), this);
    auto* grid = new QVBoxLayout(group);
    static const std::map<std::string, QString> kNames = {
        {"bitmap", QStringLiteral("モノクロ 2 階調")}, {"gray", QStringLiteral("グレースケール")},   {"duotone", QStringLiteral("ダブルトーン")},
        {"indexed", QStringLiteral("インデックスカラー")}, {"rgb", QStringLiteral("RGB カラー")},     {"cmyk", QStringLiteral("CMYK カラー")},
        {"lab", QStringLiteral("Lab カラー")},             {"multichannel", QStringLiteral("マルチチャンネル")}};
    for (const std::string& mode : service::automate::mode_names()) {
        auto* box = new QCheckBox(kNames.at(mode), group);
        box->setChecked(true);
        grid->addWidget(box);
        from.emplace_back(mode, box);
    }
    auto* buttons = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("すべて"), group);
    auto* none = new QPushButton(QStringLiteral("なし"), group);
    buttons->addWidget(all);
    buttons->addWidget(none);
    buttons->addStretch(1);
    grid->addLayout(buttons);
    connect(all, &QPushButton::clicked, this, [this] {
        for (auto& [mode, box] : from) box->setChecked(true);
    });
    connect(none, &QPushButton::clicked, this, [this] {
        for (auto& [mode, box] : from) box->setChecked(false);
    });
    layout->addWidget(group);
    auto* target = new QFormLayout;
    to = new QComboBox(this);
    to->addItem(QStringLiteral("RGB カラー"), QStringLiteral("rgb"));
    to->addItem(QStringLiteral("CMYK カラー"), QStringLiteral("cmyk"));
    to->addItem(QStringLiteral("Lab カラー"), QStringLiteral("lab"));
    to->addItem(QStringLiteral("グレースケール"), QStringLiteral("gray"));
    target->addRow(QStringLiteral("変換後のモード"), to);
    layout->addLayout(target);
    layout->addWidget(ok_cancel(this, QStringLiteral("OK"), [this] { return problem(); }));
}

Json ModeChangeDialog::step(const Json& page) const {
    Json modes = Json::array();
    for (const auto& [mode, box] : from) {
        if (box->isChecked()) modes.push_back(mode);
    }
    return Json{{"op", "conditional_mode_change"}, {"page", page}, {"from", modes}, {"to", to->currentData().toString().toStdString()}};
}

std::optional<QString> ModeChangeDialog::problem() const {
    for (const auto& [mode, box] : from) {
        if (box->isChecked()) return std::nullopt;
    }
    return QStringLiteral("変換元のモードを 1 つ以上選んでください");
}

// --- レンズ補正… ---------------------------------------------------------------------------------------------------------

LensBatchDialog::LensBatchDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("レンズ補正"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(source_row(this, source, false));
    auto* dest = new QGroupBox(QStringLiteral("保存先"), this);
    auto* dest_layout = new QFormLayout(dest);
    dest_layout->addRow(QStringLiteral("フォルダー"), path_row(dest, folder, QStringLiteral("保存先のフォルダー"), true));
    format = format_box(dest, true);
    quality = quality_box(dest, 10);
    dest_layout->addRow(QStringLiteral("形式"), format);
    dest_layout->addRow(QStringLiteral("画質（JPEG）"), quality);
    layout->addWidget(dest);
    auto* fix = new QGroupBox(QStringLiteral("補正（レンズプロファイルは各画像の EXIF の焦点距離から自動で選びます）"), this);
    auto* fix_layout = new QFormLayout(fix);
    distortion = new QCheckBox(QStringLiteral("幾何学的歪み"), fix);
    chromatic = new QCheckBox(QStringLiteral("色収差"), fix);
    vignette = new QCheckBox(QStringLiteral("周辺光量"), fix);
    auto_scale = new QCheckBox(QStringLiteral("画像を自動的に拡大・縮小"), fix);
    for (QCheckBox* box : {distortion, chromatic, vignette, auto_scale}) {
        box->setChecked(true);
        fix_layout->addRow(box);
    }
    edge = new QComboBox(fix);
    edge->addItem(QStringLiteral("透明"), QStringLiteral("transparency"));
    edge->addItem(QStringLiteral("エッジを拡張"), QStringLiteral("extension"));
    edge->addItem(QStringLiteral("黒"), QStringLiteral("black"));
    edge->addItem(QStringLiteral("白"), QStringLiteral("white"));
    edge->setCurrentIndex(1);
    fix_layout->addRow(QStringLiteral("エッジ"), edge);
    layout->addWidget(fix);
    layout->addWidget(ok_cancel(this, QStringLiteral("OK"), [this] { return problem(); }));
}

Json LensBatchDialog::settings() const {
    return Json{{"profile", "auto"},
                {"correct_distortion", distortion->isChecked()},
                {"correct_ca", chromatic->isChecked()},
                {"correct_vignette", vignette->isChecked()},
                {"auto_scale", auto_scale->isChecked()},
                {"edge", edge->currentData().toString().toStdString()}};
}

std::optional<QString> LensBatchDialog::problem() const {
    if (const auto why = source_problem(source)) return why;
    if (folder->text().trimmed().isEmpty()) return QStringLiteral("保存先のフォルダーを選んでください");
    return std::nullopt;
}

// --- スクリプトイベントマネージャー… -------------------------------------------------------------------------------------

ScriptEventsDialog::ScriptEventsDialog(QWidget* parent, events::Table table, const std::optional<std::string>& problem)
    : QDialog(parent), table_(std::move(table)) {
    setWindowTitle(QStringLiteral("スクリプトイベントマネージャー"));
    auto* layout = new QVBoxLayout(this);
    said = new QLabel(this);
    said->setWordWrap(true);
    if (problem) said->setText(QString::fromStdString(*problem));
    layout->addWidget(said);
    enabled = new QCheckBox(QStringLiteral("スクリプト／アクションを実行するイベントを有効にする"), this);
    enabled->setChecked(table_.enabled);
    layout->addWidget(enabled);
    list = new QListWidget(this);
    layout->addWidget(list, 1);
    auto* form = new QFormLayout;
    event = new QComboBox(this);
    for (const events::Event e : events::events()) {
        event->addItem(QString::fromStdString(events::label_of(e)), QString::fromLatin1(events::id_of(e).data(), static_cast<qsizetype>(events::id_of(e).size())));
    }
    form->addRow(QStringLiteral("Genko イベント"), event);
    kind = new QComboBox(this);
    kind->addItem(QStringLiteral("スクリプト"), QStringLiteral("script"));
    kind->addItem(QStringLiteral("アクション"), QStringLiteral("action"));
    form->addRow(QString(), kind);
    QWidget* script_row = path_row(this, script, QStringLiteral("スクリプト（手順のファイル）"), false, kStepsFilter);
    form->addRow(QStringLiteral("スクリプト"), script_row);
    action = new QComboBox(this);
    const Json saved = service::actions::load();
    for (const auto& [name, item] : saved.items()) action->addItem(QString::fromStdString(name));
    form->addRow(QStringLiteral("アクション"), action);
    const auto shown = [this, script_row] {
        const bool file = kind->currentData().toString() == QLatin1String("script");
        script_row->setEnabled(file);
        action->setEnabled(!file);
    };
    connect(kind, &QComboBox::currentIndexChanged, this, shown);
    shown();
    layout->addLayout(form);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("追加"), this);
    auto* remove = new QPushButton(QStringLiteral("削除"), this);
    auto* remove_every = new QPushButton(QStringLiteral("すべてを削除"), this);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(remove_every);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this] { add_chosen(); });
    connect(remove, &QPushButton::clicked, this, [this] { remove_at(list->currentRow()); });
    connect(remove_every, &QPushButton::clicked, this, [this] { remove_all(); });
    layout->addWidget(ok_cancel(this, QStringLiteral("完了"), [] { return std::optional<QString>(); }));
    fill();
}

void ScriptEventsDialog::fill() {
    list->clear();
    for (const events::Binding& b : table_.bindings) {
        const auto e = events::event_of(b.event);
        const QString what = b.script ? QDir::toNativeSeparators(QString::fromStdString(*b.script))
                                      : QStringLiteral("アクション: %1").arg(QString::fromStdString(b.action.value_or(std::string())));
        list->addItem(QStringLiteral("%1: %2").arg(e ? QString::fromStdString(events::label_of(*e)) : QString::fromStdString(b.event), what));
    }
}

bool ScriptEventsDialog::add_chosen() {
    events::Binding binding;
    binding.event = event->currentData().toString().toStdString();
    try {
        if (kind->currentData().toString() == QLatin1String("script")) {
            const QString text = script->text().trimmed();
            if (text.isEmpty()) {
                said->setText(QStringLiteral("スクリプト（手順のファイル）を選んでください"));
                return false;
            }
            const fs::path file = fs::absolute(fpath(QDir::fromNativeSeparators(text)));
            const std::string bytes = storage::read_file_bounded(file, automate::kMaxStepsBytes);
            automate::read_steps(bytes);  // (a file that cannot be steps is not bound)
            binding.script = core::path_to_utf8(file);
            binding.sha256 = storage::sha256_hex(bytes);
            binding.name = core::path_to_utf8(file.stem());
        } else {
            const std::string name = action->currentText().toStdString();
            const Json saved = service::actions::load();
            const auto found = saved.find(name);
            if (name.empty() || found == saved.end()) {
                said->setText(QStringLiteral("アクションを選んでください"));
                return false;
            }
            binding.action = name;
            binding.sha256 = events::action_sha256((*found)["ops"]);
            binding.name = name;
        }
    } catch (const std::exception& error) {
        said->setText(QStringLiteral("結び付けられません: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        return false;
    }
    if (table_.bindings.size() >= events::kMaxBindings) {
        said->setText(QStringLiteral("これ以上は結び付けられません"));
        return false;
    }
    table_.bindings.push_back(std::move(binding));
    enabled->setChecked(true);  // (Photoshop's: adding one turns the events on)
    said->clear();
    fill();
    return true;
}

void ScriptEventsDialog::remove_at(int row) {
    if (row < 0 || row >= static_cast<int>(table_.bindings.size())) return;
    table_.bindings.erase(table_.bindings.begin() + row);
    fill();
}

void ScriptEventsDialog::remove_all() {
    table_.bindings.clear();
    fill();
}

events::Table ScriptEventsDialog::table() const {
    events::Table out = table_;
    out.enabled = enabled->isChecked();
    return out;
}

}  // namespace genko::app
