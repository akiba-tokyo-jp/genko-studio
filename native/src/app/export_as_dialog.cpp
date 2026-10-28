// 書き出しの環境設定…, 書き出し形式… and レイヤーからファイル… (app/export_as_dialog.hpp).

#include "app/export_as_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/look.hpp"
#include "app/theme.hpp"
#include "formats/page_picture.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace {

constexpr std::uint32_t kEstimateSide = 512;  // (PhotoCraft's proxy: the estimate from a picture at most this long)
constexpr std::uint32_t kPreviewSide = 300;

QComboBox* choices(std::initializer_list<std::pair<QString, QString>> items, const QString& current) {
    auto* box = new QComboBox;
    for (const auto& [text, data] : items) box->addItem(text, data);
    if (const int at = box->findData(current); at >= 0) box->setCurrentIndex(at);
    return box;
}

// A codecs' picture as the screen shows it: sRGB, 8-bit RGBA.
QImage shown(const render::codecs::CodecImage& picture) {
    using render::codecs::Layout;
    render::codecs::CodecImage image = picture;
    if ((image.icc && !image.icc->empty()) || !render::codecs::is_rgb(image.layout())) image = formats::srgb_picture(image, false, false);
    const render::codecs::CodecImage rgba = image.convert(Layout::Rgba, render::codecs::Sample::U8);
    QImage out(static_cast<int>(rgba.width()), static_cast<int>(rgba.height()), QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y) {
        std::memcpy(out.scanLine(y), rgba.data().data() + std::size_t(y) * rgba.width() * 4, std::size_t(rgba.width()) * 4);
    }
    return out;
}

}  // namespace

QString human_bytes(std::uint64_t bytes) {
    if (bytes < 1024) return QStringLiteral("%1 バイト").arg(bytes);
    const double kb = double(bytes) / 1024.0;
    if (kb < 1024.0) return QStringLiteral("%1 KB").arg(kb, 0, 'f', kb < 10 ? 1 : 0);
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

// --- 書き出しの環境設定 ---------------------------------------------------------------------------------------------

formats::ExportPreferences export_preferences() {
    const auto store = settings();
    formats::ExportPreferences prefs;
    const std::string format = store->value(QStringLiteral("export/quick_format"), QStringLiteral("png")).toString().toStdString();
    if (std::find(formats::kQuickFormats.begin(), formats::kQuickFormats.end(), format) != formats::kQuickFormats.end()) prefs.format = format;
    if (const auto where = formats::quick_location_of(store->value(QStringLiteral("export/quick_location"), QStringLiteral("ask")).toString().toStdString())) {
        prefs.location = *where;
    }
    bool ok = false;
    const int quality = store->value(QStringLiteral("export/jpeg_quality"), 85).toInt(&ok);
    if (ok) prefs.jpeg_quality = std::clamp(quality, 1, 100);
    if (const auto metadata = formats::metadata_of(store->value(QStringLiteral("export/metadata"), QStringLiteral("copyright")).toString().toStdString())) {
        prefs.metadata = *metadata;
    }
    prefs.srgb = store->value(QStringLiteral("export/srgb"), QStringLiteral("true")).toString() != QLatin1String("false");
    return prefs;
}

void set_export_preferences(const formats::ExportPreferences& prefs) {
    const auto store = settings();
    store->setValue(QStringLiteral("export/quick_format"), QString::fromStdString(prefs.format));
    store->setValue(QStringLiteral("export/quick_location"), QString::fromLatin1(formats::to_string(prefs.location).data()));
    store->setValue(QStringLiteral("export/jpeg_quality"), prefs.jpeg_quality);
    store->setValue(QStringLiteral("export/metadata"), QString::fromLatin1(formats::to_string(prefs.metadata).data()));
    store->setValue(QStringLiteral("export/srgb"), prefs.srgb ? QStringLiteral("true") : QStringLiteral("false"));
}

ExportPreferencesForm::ExportPreferencesForm(const formats::ExportPreferences& prefs, QWidget* parent) : QWidget(parent) {
    format = choices({{QStringLiteral("PNG"), QStringLiteral("png")}, {QStringLiteral("JPG"), QStringLiteral("jpg")},
                      {QStringLiteral("GIF"), QStringLiteral("gif")}, {QStringLiteral("WebP"), QStringLiteral("webp")}},
                     QString::fromStdString(prefs.format));
    location = choices({{QStringLiteral("毎回たずねる"), QStringLiteral("ask")},
                        {QStringLiteral("原稿と同じフォルダー"), QStringLiteral("sameFolder")},
                        {QStringLiteral("原稿の書き出しフォルダー（原稿名_書き出し）"), QStringLiteral("exports")}},
                       QString::fromLatin1(formats::to_string(prefs.location).data()));
    quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(prefs.jpeg_quality);
    metadata = choices({{QStringLiteral("なし"), QStringLiteral("none")}, {QStringLiteral("著作権情報と連絡先情報"), QStringLiteral("copyright")},
                        {QStringLiteral("すべて"), QStringLiteral("all")}},
                       QString::fromLatin1(formats::to_string(prefs.metadata).data()));
    metadata->setToolTip(QStringLiteral("すべて: 本のファイル情報と、写真から開いたページの EXIF（カメラの情報・位置情報（GPS）も含みます。縮小画像は外します）"));
    srgb = new QCheckBox(QStringLiteral("sRGB に変換"));
    srgb->setChecked(prefs.srgb);
    for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{
             {"format", format}, {"location", location}, {"quality", quality}, {"metadata", metadata}, {"srgb", srgb}}) {
        widget->setObjectName(QStringLiteral("prefs_export_%1").arg(QString::fromLatin1(name)));
    }
    location->setToolTip(QStringLiteral("原稿と同じフォルダー・原稿の書き出しフォルダーは、保存した原稿のときに使います（まだ保存していない原稿は、毎回たずねます）"));
    QFormLayout* rows = look::form();
    setLayout(rows);
    rows->addRow(look::section(QStringLiteral("クイック書き出し")));
    rows->addRow(QStringLiteral("形式"), format);
    rows->addRow(QStringLiteral("書き出し先"), location);
    rows->addRow(QStringLiteral("JPG の画質"), quality);
    rows->addRow(look::section(QStringLiteral("メタデータと色")));
    rows->addRow(QStringLiteral("メタデータ"), metadata);
    rows->addRow(QString(), srgb);
    look::quiet_labels(rows);
}

formats::ExportPreferences ExportPreferencesForm::value() const {
    formats::ExportPreferences prefs;
    prefs.format = format->currentData().toString().toStdString();
    prefs.location = formats::quick_location_of(location->currentData().toString().toStdString()).value_or(formats::QuickLocation::Ask);
    prefs.jpeg_quality = quality->value();
    prefs.metadata = formats::metadata_of(metadata->currentData().toString().toStdString()).value_or(formats::Metadata::Copyright);
    prefs.srgb = srgb->isChecked();
    return prefs;
}

ExportPreferencesDialog::ExportPreferencesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("exportPreferencesDialog"));
    setWindowTitle(QStringLiteral("書き出しの環境設定"));
    form = new ExportPreferencesForm(export_preferences());
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        set_export_preferences(form->value());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* body = new QVBoxLayout;
    body->addWidget(form);
    look::frame(this, look::header(QStringLiteral("書き出しの環境設定"), QStringLiteral("クイック書き出しの形式・書き出し先・画質・メタデータ（このパソコンの設定）")), body,
                nullptr, look::footer(buttons));
}

// --- 書き出し形式… ---------------------------------------------------------------------------------------------------

ExportAsDialog::ExportAsDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page, std::optional<std::string> layer)
    : QDialog(parent), doc_(std::move(doc)), page_(page), layer_(std::move(layer)) {
    setObjectName(QStringLiteral("exportAsDialog"));
    setWindowTitle(QStringLiteral("書き出し形式"));
    target = new QComboBox;
    if (layer_) {
        QString name = QString::fromStdString(*layer_);
        for (const core::Layer& l : doc_->page(page_).layers) {
            if (l.id == *layer_) name = QString::fromStdString(l.title);
        }
        target->addItem(QStringLiteral("レイヤー「%1」だけ（中身の範囲で切る）").arg(name), QStringLiteral("layer"));
    } else {
        target->addItem(QStringLiteral("このページ"), QStringLiteral("page"));
        target->addItem(QStringLiteral("すべてのページ（フォルダーに 1 ページずつ）"), QStringLiteral("all"));
    }
    format = choices({{QStringLiteral("PNG"), QStringLiteral("png")}, {QStringLiteral("JPEG"), QStringLiteral("jpg")}, {QStringLiteral("GIF"), QStringLiteral("gif")},
                      {QStringLiteral("WebP"), QStringLiteral("webp")}, {QStringLiteral("TIFF"), QStringLiteral("tif")}, {QStringLiteral("AVIF"), QStringLiteral("avif")}},
                     QStringLiteral("png"));
    quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(85);
    lossless = new QCheckBox(QStringLiteral("可逆圧縮"));
    lossless->setChecked(true);
    transparency = new QCheckBox(QStringLiteral("透明部分"));
    transparency->setChecked(true);
    transparency->setToolTip(QStringLiteral("オフにすると、透明な所を白にします（ページはいつも紙の上の絵です）"));
    scale = new QDoubleSpinBox;
    scale->setRange(1.0, 1000.0);
    scale->setDecimals(1);
    scale->setSuffix(QStringLiteral(" %"));
    scale->setValue(100.0);
    width = new QSpinBox;
    height = new QSpinBox;
    for (QSpinBox* side : {width, height}) {
        side->setRange(1, 100000);
        side->setSuffix(QStringLiteral(" px"));
    }
    resample = choices({{QStringLiteral("バイキュービック"), QStringLiteral("bicubic")}, {QStringLiteral("バイリニア"), QStringLiteral("bilinear")},
                        {QStringLiteral("ニアレストネイバー"), QStringLiteral("nearest")}},
                       QStringLiteral("bicubic"));
    trim = new QCheckBox(QStringLiteral("仕上がりで切る（裁ち落としを含めない）"));
    metadata = choices({{QStringLiteral("なし"), QStringLiteral("none")}, {QStringLiteral("著作権情報と連絡先情報"), QStringLiteral("copyright")},
                        {QStringLiteral("すべて"), QStringLiteral("all")}},
                       QString::fromLatin1(formats::to_string(export_preferences().metadata).data()));
    metadata->setToolTip(QStringLiteral("すべて: 本のファイル情報と、写真から開いたページの EXIF（カメラの情報・位置情報（GPS）も含みます。縮小画像は外します）"));
    srgb = new QCheckBox(QStringLiteral("sRGB に変換"));
    srgb->setChecked(true);
    preview = new QLabel;
    preview->setObjectName(QStringLiteral("export_as_preview"));
    preview->setFixedSize(int(kPreviewSide), int(kPreviewSide));
    preview->setAlignment(Qt::AlignCenter);
    estimate = new QLabel(QStringLiteral("見積もっています…"));
    estimate->setObjectName(QStringLiteral("export_as_estimate"));
    notes = new QLabel;
    notes->setWordWrap(true);
    theme::role(notes, "hint");
    for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{
             {"target", target}, {"format", format}, {"quality", quality}, {"lossless", lossless}, {"transparency", transparency},
             {"scale", scale}, {"width", width}, {"height", height}, {"resample", resample}, {"trim", trim}, {"metadata", metadata},
             {"srgb", srgb}}) {
        widget->setObjectName(QStringLiteral("export_as_%1").arg(QString::fromLatin1(name)));
    }
    if (layer_) trim->setEnabled(false);
    else transparency->setEnabled(false);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す…"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QFormLayout* rows = look::form();
    rows->addRow(look::section(QStringLiteral("ファイル設定")));
    rows->addRow(QStringLiteral("書き出すもの"), target);
    rows->addRow(QStringLiteral("形式"), format);
    rows->addRow(QStringLiteral("画質"), quality);
    rows->addRow(QString(), lossless);
    rows->addRow(QString(), transparency);
    rows->addRow(look::section(QStringLiteral("画像サイズ")));
    rows->addRow(QStringLiteral("拡大・縮小"), scale);
    rows->addRow(QStringLiteral("幅"), width);
    rows->addRow(QStringLiteral("高さ"), height);
    rows->addRow(QStringLiteral("再サンプル"), resample);
    rows->addRow(QString(), trim);
    rows->addRow(look::section(QStringLiteral("メタデータと色")));
    rows->addRow(QStringLiteral("メタデータ"), metadata);
    rows->addRow(QString(), srgb);
    look::quiet_labels(rows);
    auto* side = new QVBoxLayout;
    side->addWidget(preview);
    side->addWidget(estimate);
    side->addWidget(notes);
    side->addStretch(1);
    auto* card = new QWidget;
    card->setLayout(side);
    look::frame(this, look::header(QStringLiteral("書き出し形式"), QStringLiteral("形式・大きさ・画質を選んで、1 枚の画像に書き出します")), rows,
                look::card(card, QStringLiteral("プレビュー")), look::footer(buttons));

    timer_.setSingleShot(true);
    timer_.setInterval(150);  // (a moment after the last change: one estimate for a run of keys)
    connect(&timer_, &QTimer::timeout, this, &ExportAsDialog::start);
    watcher_ = new QFutureWatcher<Result>(this);
    connect(watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = watcher_->result();
        ++estimates_;
        if (pending_) {  // (a change came in while it was made: that one shown next)
            pending_ = false;
            start();
            return;
        }
        show_result(result);
    });
    for (QComboBox* box : {target, format, resample, metadata}) connect(box, &QComboBox::currentIndexChanged, this, &ExportAsDialog::changed);
    for (QCheckBox* box : {lossless, transparency, trim, srgb}) connect(box, &QCheckBox::toggled, this, &ExportAsDialog::changed);
    connect(quality, &QSpinBox::valueChanged, this, &ExportAsDialog::changed);
    connect(scale, &QDoubleSpinBox::valueChanged, this, [this] { size_edited(0); });
    connect(width, &QSpinBox::valueChanged, this, [this] { size_edited(1); });
    connect(height, &QSpinBox::valueChanged, this, [this] { size_edited(2); });
    size_edited(0);
}

ExportAsDialog::~ExportAsDialog() {
    // (the worker holds the book itself: let go, its estimate never shown — the window closes at once)
    if (watcher_ != nullptr) watcher_->disconnect(this);
}

QString ExportAsDialog::extension() const { return format->currentData().toString(); }

bool ExportAsDialog::all_pages() const { return target->currentData().toString() == QLatin1String("all"); }

bool ExportAsDialog::estimating() const { return timer_.isActive() || (watcher_ != nullptr && watcher_->isRunning()); }

formats::CopyOptions ExportAsDialog::options() const {
    formats::CopyOptions o;
    const QString ext = extension();
    o.layer = layer_;
    if (ext == QLatin1String("jpg") || ext == QLatin1String("avif") || (ext == QLatin1String("webp") && !lossless->isChecked())) o.quality_percent = quality->value();
    o.transparency = !layer_ || transparency->isChecked();
    if (sized_by_ == 1) o.width = static_cast<std::uint32_t>(width->value());
    else if (sized_by_ == 2) o.height = static_cast<std::uint32_t>(height->value());
    else o.scale = scale->value();
    o.resample = render::codecs::resize_filter(resample->currentData().toString().toStdString()).value_or(render::codecs::ResizeFilter::Bicubic);
    if (!layer_ && trim->isChecked()) o.area = "trim";
    o.metadata = formats::metadata_of(metadata->currentData().toString().toStdString());
    o.srgb = srgb->isChecked();
    return o;
}

void ExportAsDialog::size_edited(int which) {
    sized_by_ = which;
    // (the other two follow: the page's size at that scale — a layer's size is known when it is drawn)
    try {
        formats::CopyOptions o = options();
        o.layer.reset();
        const formats::CopySize size = formats::copy_size(*doc_, page_, o);
        const QSignalBlocker a(scale), b(width), c(height);
        if (which != 1) width->setValue(static_cast<int>(size.width));
        if (which != 2) height->setValue(static_cast<int>(size.height));
        if (which != 0) {
            const auto [w100, h100] = formats::page_picture_size(doc_->page(page_), std::max(1, static_cast<int>(std::lround(doc_->page(page_).spec.dpi.value()))), o.area);
            scale->setValue(100.0 * double(size.width) / double(std::max<std::uint32_t>(1, w100)));
        }
    } catch (const std::exception&) {
        // (a size the page cannot be: the estimate says why)
    }
    if (layer_) {
        width->setEnabled(true);
        height->setEnabled(true);
    }
    changed();
}

void ExportAsDialog::changed() {
    const QString ext = extension();
    quality->setEnabled(ext == QLatin1String("jpg") || ext == QLatin1String("avif") || (ext == QLatin1String("webp") && !lossless->isChecked()));
    lossless->setVisible(ext == QLatin1String("webp"));
    if (layer_) {
        // (PC4 F-c's review P2-1: a JPEG holds no alpha — the layer written over white —: 透明部分 greyed and off, as it was
        // when another format is chosen again)
        const bool holds = ext != QLatin1String("jpg");
        if (holds != transparency->isEnabled()) {
            const QSignalBlocker quiet(transparency);
            if (!holds) {
                transparency_asked_ = transparency->isChecked();
                transparency->setChecked(false);
            } else {
                transparency->setChecked(transparency_asked_);
            }
            transparency->setEnabled(holds);
        }
    }
    estimate->setText(QStringLiteral("見積もっています…"));
    timer_.start();
}

void ExportAsDialog::start() {
    if (watcher_->isRunning()) {
        pending_ = true;
        return;
    }
    // (on a worker, from small pictures: the estimate from one at most 512 px long, the preview at most 300 — a large
    // page never makes the window wait)
    const std::shared_ptr<const core::Document> doc = doc_;
    const std::size_t page = page_;
    const formats::CopyOptions o = options();
    const std::string ext = extension().toStdString();
    watcher_->setFuture(QtConcurrent::run([doc, page, o, ext]() -> Result {
        Result r;
        try {
            const formats::CopyEstimate e = formats::estimate_copy(*doc, page, ext, o, kEstimateSide);
            r.bytes = e.bytes;
            r.width = e.width;
            r.height = e.height;
            for (const std::string& note : e.notes) r.notes << QString::fromStdString(note);
            formats::CopyOptions small = o;
            const double k = double(kPreviewSide) / double(std::max<std::uint32_t>(1, std::max(e.width, e.height)));
            if (k < 1.0) {
                small.width.reset();
                small.height.reset();
                small.scale = std::clamp(formats::copy_size(*doc, page, o).dpi / std::max(1.0, doc->page(page).spec.dpi.value()) * 100.0 * k, 1.0, 1000.0);
            }
            small.metadata = formats::Metadata::None;
            r.picture = shown(formats::copy_picture(*doc, page, small).image);
            r.ok = true;
        } catch (const std::exception& error) {
            r.error = QString::fromUtf8(error.what());
        }
        return r;
    }));
}

void ExportAsDialog::show_result(const Result& result) {
    if (!result.ok) {
        estimate->setText(wording::error(result.error));
        preview->clear();
        notes->clear();
        return;
    }
    const QString pages = all_pages() ? QStringLiteral("・1 ページあたり") : QString();
    estimate->setText(QStringLiteral("%1  約 %2（%3 × %4 px%5）")
                          .arg(extension().toUpper(), human_bytes(result.bytes))
                          .arg(result.width)
                          .arg(result.height)
                          .arg(pages));
    QPixmap pixmap = QPixmap::fromImage(result.picture);
    preview->setPixmap(pixmap.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    notes->setText(result.notes.isEmpty() ? QString() : QStringLiteral("このファイルには残せないもの:\n・") + result.notes.join(QStringLiteral("\n・")));
}

// --- レイヤーからファイル… ------------------------------------------------------------------------------------------

LayersToFilesDialog::LayersToFilesDialog(QWidget* parent, const QString& where, const QString& name) : QDialog(parent) {
    setObjectName(QStringLiteral("layersToFilesDialog"));
    setWindowTitle(QStringLiteral("レイヤーからファイル"));
    folder = new QLineEdit(QDir::toNativeSeparators(where));
    folder->setObjectName(QStringLiteral("layers_folder"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString chosen = ask::existing_dir(this, QStringLiteral("書き出し先"), folder->text());
        if (!chosen.isEmpty()) folder->setText(QDir::toNativeSeparators(chosen));
    });
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(folder, 1);
    line->addWidget(pick);
    auto* where_row = new QWidget;
    where_row->setLayout(line);
    prefix = new QLineEdit(name);
    prefix->setObjectName(QStringLiteral("layers_prefix"));
    visible_only = new QCheckBox(QStringLiteral("表示中のレイヤーのみ"));
    visible_only->setObjectName(QStringLiteral("layers_visible_only"));
    visible_only->setChecked(true);
    format = choices({{QStringLiteral("PNG"), QStringLiteral("png")}, {QStringLiteral("JPEG"), QStringLiteral("jpeg")}, {QStringLiteral("TIFF"), QStringLiteral("tiff")},
                      {QStringLiteral("PSD"), QStringLiteral("psd")}, {QStringLiteral("WebP"), QStringLiteral("webp")}},
                     QStringLiteral("png"));
    format->setObjectName(QStringLiteral("layers_format"));
    quality = new QSpinBox;
    quality->setObjectName(QStringLiteral("layers_quality"));
    quality->setRange(0, 12);
    quality->setValue(10);
    quality->setToolTip(QStringLiteral("JPEG の画質（0〜12。12 が最高）"));
    connect(format, &QComboBox::currentIndexChanged, this, [this] { quality->setEnabled(format->currentData().toString() == QLatin1String("jpeg")); });
    quality->setEnabled(false);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("書き出し先"), where_row);
    rows->addRow(QStringLiteral("ファイル名の接頭辞"), prefix);
    rows->addRow(QString(), visible_only);
    rows->addRow(QStringLiteral("ファイル形式"), format);
    rows->addRow(QStringLiteral("JPEG の画質"), quality);
    look::quiet_labels(rows);
    look::frame(this, look::header(QStringLiteral("レイヤーからファイル"), QStringLiteral("ページごとのフォルダーに、レイヤーを 1 枚ずつ書き出します")), rows, nullptr,
                look::footer(buttons));
}

formats::LayerFiles LayersToFilesDialog::files() const {
    formats::LayerFiles files;
    files.format = format->currentData().toString().toStdString();
    files.prefix = prefix->text().trimmed().toStdString();
    files.visible_only = visible_only->isChecked();
    if (files.format == "jpeg") files.quality = quality->value();
    return files;
}

}  // namespace genko::app
