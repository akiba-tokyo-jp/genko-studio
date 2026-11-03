// HDR Pro に統合…'s two windows (PC5 I-h): see app/hdr_dialog.hpp.

#include "app/hdr_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <tuple>

#include "app/ask.hpp"
#include "core/json.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace {

QDialogButtonBox* ok_cancel(QDialog* dialog, const QString& ok, std::function<std::optional<QString>()> problem) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(ok);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, problem] {
        if (const auto why = problem ? problem() : std::optional<QString>()) {
            ask::warning(dialog, dialog->windowTitle(), *why);
            return;
        }
        dialog->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QDoubleSpinBox* spin(QWidget* parent, double lo, double hi, double value, int decimals, double step) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(value);
    return box;
}

}  // namespace

// --- the exposures ----------------------------------------------------------------------------------------------------------

HdrFilesDialog::HdrFilesDialog(QWidget* parent, std::vector<std::pair<QString, int>> open) : QDialog(parent), open_(std::move(open)) {
    setObjectName(QStringLiteral("hdrFilesDialog"));
    setWindowTitle(QStringLiteral("HDR Pro に統合"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("ソースファイル: 統合する露出違いの画像を 2〜%1 枚選んでください").arg(service::automate::kHdrMaxImages), this));
    auto* form = new QFormLayout;
    use = new QComboBox(this);
    use->addItem(QStringLiteral("ファイル"), QStringLiteral("files"));
    use->addItem(QStringLiteral("フォルダー"), QStringLiteral("folder"));
    form->addRow(QStringLiteral("使用"), use);
    layout->addLayout(form);
    auto* folder_row = new QWidget(this);
    auto* folder_layout = new QHBoxLayout(folder_row);
    folder_layout->setContentsMargins(0, 0, 0, 0);
    folder = new QLineEdit(folder_row);
    auto* pick = new QPushButton(QStringLiteral("参照…"), folder_row);
    folder_layout->addWidget(folder, 1);
    folder_layout->addWidget(pick);
    layout->addWidget(folder_row);
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString chosen = ask::existing_dir(this, QStringLiteral("ソースのフォルダー"), folder->text());
        if (!chosen.isEmpty()) folder->setText(QDir::toNativeSeparators(chosen));
    });
    auto* files_row = new QWidget(this);
    auto* files_layout = new QHBoxLayout(files_row);
    files_layout->setContentsMargins(0, 0, 0, 0);
    files = new QListWidget(files_row);
    auto* buttons = new QVBoxLayout;
    auto* add = new QPushButton(QStringLiteral("参照…"), files_row);
    auto* remove = new QPushButton(QStringLiteral("削除"), files_row);
    auto* add_open = new QPushButton(QStringLiteral("開いているファイルを追加"), files_row);
    add_open->setEnabled(!open_.empty());
    for (QPushButton* b : {add, remove, add_open}) buttons->addWidget(b);
    buttons->addStretch(1);
    files_layout->addWidget(files, 1);
    files_layout->addLayout(buttons);
    layout->addWidget(files_row);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString chosen = ask::open_path(this, QStringLiteral("ファイルを追加"));
        if (chosen.isEmpty()) return;
        auto* item = new QListWidgetItem(QDir::toNativeSeparators(chosen), files);
        item->setData(Qt::UserRole, QStringLiteral("file:") + chosen);
    });
    connect(remove, &QPushButton::clicked, this, [this] { delete files->currentItem(); });
    connect(add_open, &QPushButton::clicked, this, [this] {
        for (const auto& [title, tab] : open_) {
            const QString data = QStringLiteral("open:%1").arg(tab);
            bool there = false;
            for (int i = 0; i < files->count(); ++i) there = there || files->item(i)->data(Qt::UserRole).toString() == data;
            if (there) continue;
            auto* item = new QListWidgetItem(QStringLiteral("%1（開いているファイル）").arg(title), files);
            item->setData(Qt::UserRole, data);
        }
    });
    align = new QCheckBox(QStringLiteral("可能な場合はソース画像を自動的に配置"), this);
    align->setChecked(true);
    layout->addWidget(align);
    const auto shown = [this, folder_row, files_row] {
        const bool by_folder = use->currentData().toString() == QLatin1String("folder");
        folder_row->setVisible(by_folder);
        files_row->setVisible(!by_folder);
    };
    connect(use, &QComboBox::currentIndexChanged, this, shown);
    shown();
    layout->addWidget(ok_cancel(this, QStringLiteral("OK"), [this] { return problem(); }));
}

std::optional<QString> HdrFilesDialog::problem() const {
    if (use->currentData().toString() == QLatin1String("folder")) {
        if (folder->text().trimmed().isEmpty()) return QStringLiteral("ソースのフォルダーを選んでください");
        return std::nullopt;
    }
    if (files->count() < static_cast<int>(service::automate::kHdrMinImages)) return QStringLiteral("露出違いの画像を 2 枚以上選んでください");
    if (files->count() > static_cast<int>(service::automate::kHdrMaxImages))
        return QStringLiteral("統合できる画像は %1 枚までです").arg(service::automate::kHdrMaxImages);
    return std::nullopt;
}

// --- HDR Pro に統合 ---------------------------------------------------------------------------------------------------------

HdrDialog::HdrDialog(QWidget* parent, std::vector<render::hdr::Shot> shots, bool align) : QDialog(parent), shots_(std::move(shots)), align_(align) {
    setObjectName(QStringLiteral("hdrDialog"));
    setWindowTitle(QStringLiteral("HDR Pro に統合"));
    auto* layout = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    preview = new QLabel(this);
    preview->setObjectName(QStringLiteral("hdr_preview"));
    preview->setMinimumSize(360, 240);
    preview->setAlignment(Qt::AlignCenter);
    preview->setWordWrap(true);
    left->addWidget(preview, 1);
    images = new QTableWidget(static_cast<int>(shots_.size()), 2, this);
    images->setHorizontalHeaderLabels({QStringLiteral("画像"), QStringLiteral("EV")});
    images->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    images->verticalHeader()->setVisible(false);
    images->setToolTip(QStringLiteral("EV を書き換えると、その値で統合します（書き換えなければ EXIF か画像から推定）"));
    for (std::size_t i = 0; i < shots_.size(); ++i) {
        auto* name = new QTableWidgetItem(QString::fromStdString(shots_[i].name));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        images->setItem(static_cast<int>(i), 0, name);
        images->setItem(static_cast<int>(i), 1, new QTableWidgetItem(QString()));
    }
    left->addWidget(images);
    layout->addLayout(left, 1);

    auto* right = new QVBoxLayout;
    auto* ghost_row = new QHBoxLayout;
    ghosts = new QCheckBox(QStringLiteral("ゴーストを除去"), this);
    base = new QComboBox(this);
    base->addItem(QStringLiteral("自動"), -1);
    for (std::size_t i = 0; i < shots_.size(); ++i) base->addItem(QString::fromStdString(shots_[i].name), static_cast<int>(i));
    base->setToolTip(QStringLiteral("ゴーストを除く所に使う画像"));
    ghost_row->addWidget(ghosts);
    ghost_row->addWidget(base, 1);
    right->addLayout(ghost_row);
    auto* form = new QFormLayout;
    mode = new QComboBox(this);
    mode->addItem(QStringLiteral("32 bit"), 32);
    mode->addItem(QStringLiteral("16 bit"), 16);
    mode->addItem(QStringLiteral("8 bit"), 8);
    mode->setCurrentIndex(1);  // (Photoshop's: 16 bit, ローカル適応)
    form->addRow(QStringLiteral("モード"), mode);
    method = new QComboBox(this);
    method->addItem(QStringLiteral("ローカル適応"), QStringLiteral("local_adaptation"));
    method->addItem(QStringLiteral("ハイライト圧縮"), QStringLiteral("highlight_compression"));
    method->addItem(QStringLiteral("露光量とガンマ"), QStringLiteral("exposure_gamma"));
    method->addItem(QStringLiteral("ヒストグラムの平均化"), QStringLiteral("equalize_histogram"));
    form->addRow(QStringLiteral("方法"), method);
    right->addLayout(form);

    // (ローカル適応: エッジ光彩, トーンとディテール, 詳細 — Photoshop's groups and ranges)
    toning_ = new QWidget(this);
    auto* toning = new QVBoxLayout(toning_);
    toning->setContentsMargins(0, 0, 0, 0);
    local_group_ = new QWidget(toning_);
    auto* local_layout = new QVBoxLayout(local_group_);
    local_layout->setContentsMargins(0, 0, 0, 0);
    const auto group = [&](const QString& title, std::initializer_list<std::tuple<const char*, QString, double, double, double, int, double>> rows) {
        auto* box = new QGroupBox(title, local_group_);
        auto* f = new QFormLayout(box);
        for (const auto& [key, label, lo, hi, value, decimals, step] : rows) {
            QDoubleSpinBox* s = spin(box, lo, hi, value, decimals, step);
            s->setObjectName(QStringLiteral("hdr_") + QString::fromLatin1(key));
            local[key] = s;
            f->addRow(label, s);
        }
        local_layout->addWidget(box);
    };
    group(QStringLiteral("エッジ光彩"), {{"radius", QStringLiteral("半径（pixel）"), 1, 500, 7, 0, 1}, {"strength", QStringLiteral("強さ"), 0.1, 4, 0.52, 2, 0.01}});
    group(QStringLiteral("トーンとディテール"), {{"gamma", QStringLiteral("ガンマ"), 0.1, 2, 1, 2, 0.01},
                                                 {"exposure", QStringLiteral("露光量"), -5, 5, 0, 2, 0.05},
                                                 {"detail", QStringLiteral("ディテール（%）"), -100, 300, 30, 0, 1}});
    group(QStringLiteral("詳細"), {{"shadow", QStringLiteral("シャドウ（%）"), -100, 100, 0, 0, 1},
                                   {"highlight", QStringLiteral("ハイライト（%）"), -100, 100, 0, 0, 1},
                                   {"vibrance", QStringLiteral("自然な彩度（%）"), -100, 100, 0, 0, 1},
                                   {"saturation", QStringLiteral("彩度（%）"), -100, 100, 20, 0, 1}});
    toning->addWidget(local_group_);
    gamma_group_ = new QWidget(toning_);
    auto* gamma_form = new QFormLayout(gamma_group_);
    gamma_form->setContentsMargins(0, 0, 0, 0);
    exposure = spin(gamma_group_, -20, 20, 0, 2, 0.05);
    gamma = spin(gamma_group_, 0.1, 10, 1, 2, 0.01);
    gamma_form->addRow(QStringLiteral("露光量"), exposure);
    gamma_form->addRow(QStringLiteral("ガンマ"), gamma);
    toning->addWidget(gamma_group_);
    right->addWidget(toning_);
    view_row_ = new QWidget(this);
    auto* view_form = new QFormLayout(view_row_);
    view_form->setContentsMargins(0, 0, 0, 0);
    view = spin(view_row_, -20, 20, 0, 2, 0.1);
    view->setToolTip(QStringLiteral("プレビューの見え方だけ（32 bit の値は変わりません。開いた後は 表示 → 32 bit プレビューオプション…）"));
    view_form->addRow(QStringLiteral("プレビューの露光量（EV）"), view);
    right->addWidget(view_row_);
    right->addStretch(1);
    right->addWidget(ok_cancel(this, QStringLiteral("OK"), nullptr));
    layout->addLayout(right);

    later_ = new QTimer(this);
    later_->setSingleShot(true);
    later_->setInterval(150);
    connect(later_, &QTimer::timeout, this, [this] { refresh(); });
    const auto changed = [this] {
        shown_rows();
        later_->start();
    };
    connect(mode, &QComboBox::currentIndexChanged, this, changed);
    connect(method, &QComboBox::currentIndexChanged, this, changed);
    connect(ghosts, &QCheckBox::toggled, this, changed);
    connect(base, &QComboBox::currentIndexChanged, this, changed);
    for (QDoubleSpinBox* s : {exposure, gamma, view}) connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    for (auto& [key, s] : local) connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    connect(images, &QTableWidget::cellChanged, this, [this, changed](int, int column) {
        if (filling_ || column != 1) return;
        ev_given_ = true;
        changed();
    });
    shown_rows();
    refresh();
}

void HdrDialog::shown_rows() {
    const int depth = mode->currentData().toInt();
    const QString m = method->currentData().toString();
    method->setEnabled(depth != 32);
    toning_->setVisible(depth != 32);
    local_group_->setVisible(m == QLatin1String("local_adaptation"));
    gamma_group_->setVisible(m == QLatin1String("exposure_gamma"));
    view_row_->setVisible(depth == 32);
    base->setEnabled(ghosts->isChecked());
}

service::automate::HdrSettings HdrDialog::settings() const {
    service::automate::HdrSettings s;
    s.align = align_;
    s.depth = mode->currentData().toInt();
    s.remove_ghosts = ghosts->isChecked();
    if (s.remove_ghosts && base->currentData().toInt() >= 0) s.ghost_base = static_cast<std::size_t>(base->currentData().toInt());
    if (ev_given_) {
        std::vector<double> ev;
        for (int r = 0; r < images->rowCount(); ++r) {
            bool ok = false;
            const double v = images->item(r, 1) != nullptr ? images->item(r, 1)->text().trimmed().toDouble(&ok) : 0.0;
            ev.push_back(ok && std::isfinite(v) ? v : 0.0);
        }
        s.ev = std::move(ev);
    }
    core::Json values = core::Json::object();
    const QString m = method->currentData().toString();
    if (m == QLatin1String("local_adaptation")) {
        for (const auto& [key, box] : local) values[key] = box->value();
    } else if (m == QLatin1String("exposure_gamma")) {
        values["exposure"] = exposure->value();
        values["gamma"] = gamma->value();
    }
    s.tone = service::automate::hdr_tone(m.toStdString(), values);
    return s;
}

void HdrDialog::refresh() {
    later_->stop();
    try {
        const service::automate::HdrSettings s = settings();
        std::string key = std::to_string(s.remove_ghosts) + "/" + (s.ghost_base ? std::to_string(*s.ghost_base) : std::string("-"));
        if (s.ev)
            for (const double v : *s.ev) key += "/" + std::to_string(v);
        if (!radiance_ || key != radiance_key_) {
            radiance_.reset();
            radiance_ = service::automate::hdr_radiance(shots_, s);
            radiance_key_ = key;
            if (!ev_given_) {
                // (the EVs the merge found: given back to the table, which the person may correct)
                filling_ = true;
                const core::Json& ev = radiance_->report["ev"];
                for (int r = 0; r < images->rowCount() && r < static_cast<int>(ev.size()); ++r)
                    images->item(r, 1)->setText(QString::number(ev[static_cast<std::size_t>(r)].get<double>(), 'f', 2));
                filling_ = false;
            }
        }
        const render::codecs::CodecImage shown = service::automate::hdr_display(*radiance_, s, view->value());
        shown_ = QImage(reinterpret_cast<const uchar*>(shown.data().data()), static_cast<int>(shown.width()), static_cast<int>(shown.height()),
                        static_cast<qsizetype>(shown.width()) * 3, QImage::Format_RGB888)
                     .copy();
        said_.clear();
        preview->setPixmap(QPixmap::fromImage(shown_.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    } catch (const std::exception& error) {
        shown_ = QImage();
        said_ = wording::error(QString::fromUtf8(error.what()));
        preview->setPixmap(QPixmap());
        preview->setText(said_);
    }
}

}  // namespace genko::app
