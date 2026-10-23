// Web 用に保存（従来）… (app/save_for_web_dialog.hpp).

#include "app/save_for_web_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>

#include "app/export_as_dialog.hpp"
#include "app/look.hpp"
#include "app/theme.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace {

constexpr std::uint32_t kProxySide = 1024;  // (the panes and the estimate from the page at most this long)

QComboBox* choices(std::initializer_list<std::pair<QString, QString>> items, const QString& current) {
    auto* box = new QComboBox;
    for (const auto& [text, data] : items) box->addItem(text, data);
    if (const int at = box->findData(current); at >= 0) box->setCurrentIndex(at);
    return box;
}

QImage image_of(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h) {
    QImage out(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y) std::memcpy(out.scanLine(y), rgba.data() + std::size_t(y) * w * 4, std::size_t(w) * 4);
    return out;
}

QString format_name(formats::WebFormat format) {
    switch (format) {
    case formats::WebFormat::Gif: return QStringLiteral("GIF");
    case formats::WebFormat::Png8: return QStringLiteral("PNG-8");
    case formats::WebFormat::Png24: return QStringLiteral("PNG-24");
    case formats::WebFormat::Jpeg: return QStringLiteral("JPEG");
    case formats::WebFormat::Wbmp: return QStringLiteral("WBMP");
    }
    return QStringLiteral("PNG-24");
}

// 4 アップ's two others: fewer colours, a lower quality (a PNG-24 as PNG-8 and JPEG; a WBMP dithered otherwise).
std::vector<formats::WebSettings> variants(const formats::WebSettings& st) {
    std::vector<formats::WebSettings> out(2, st);
    switch (st.format) {
    case formats::WebFormat::Gif:
    case formats::WebFormat::Png8:
        out[0].colors = std::max<std::size_t>(2, st.colors / 2);
        out[1].colors = std::max<std::size_t>(2, st.colors / 4);
        break;
    case formats::WebFormat::Jpeg:
        out[0].quality = std::max(1, st.quality / 2);
        out[1].quality = std::max(1, st.quality / 4);
        break;
    case formats::WebFormat::Png24:
        out[0].format = formats::WebFormat::Png8;
        out[0].colors = 128;
        out[1].format = formats::WebFormat::Jpeg;
        out[1].quality = 60;
        break;
    case formats::WebFormat::Wbmp:
        out[0].dither = render::quantize::Dither::Pattern;
        out[1].dither = render::quantize::Dither::None;
        break;
    }
    return out;
}

QString caption_of(const formats::WebSettings& st, std::uint64_t bytes, const std::optional<std::size_t>& colors) {
    QString text = QStringLiteral("%1  約 %2").arg(format_name(st.format), human_bytes(bytes));
    if (st.format == formats::WebFormat::Jpeg) text += QStringLiteral("（画質 %1）").arg(st.quality);
    else if (colors) text += QStringLiteral("（%1 色）").arg(*colors);
    return text;
}

}  // namespace

SaveForWebDialog::SaveForWebDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page)
    : QDialog(parent), doc_(std::move(doc)), page_(page) {
    setObjectName(QStringLiteral("saveForWebDialog"));
    setWindowTitle(QStringLiteral("Web 用に保存（従来）"));
    views = new QTabWidget;
    views->setObjectName(QStringLiteral("web_views"));
    for (int i = 0; i < 4; ++i) {
        auto* picture = new QLabel;
        picture->setAlignment(Qt::AlignCenter);
        picture->setMinimumSize(160, 120);
        auto* caption = new QLabel;
        theme::role(caption, "hint");
        pictures.push_back(picture);
        captions.push_back(caption);
    }
    const auto pane = [](std::initializer_list<std::pair<QLabel*, QLabel*>> panes, int columns) {
        auto* sheet = new QWidget;
        auto* grid = new QGridLayout(sheet);
        int i = 0;
        for (const auto& [picture, caption] : panes) {
            auto* box = new QVBoxLayout;
            box->addWidget(picture, 1);
            box->addWidget(caption);
            grid->addLayout(box, i / columns, i % columns);
            ++i;
        }
        return sheet;
    };
    // (each pane's labels live in one tab at a time: the tabs are made again as they are chosen)
    views->addTab(new QWidget, QStringLiteral("元の画像"));
    views->addTab(new QWidget, QStringLiteral("最適化"));
    views->addTab(new QWidget, QStringLiteral("2 アップ"));
    views->addTab(new QWidget, QStringLiteral("4 アップ"));
    const auto lay_out = [this, pane](int tab) {
        for (QLabel* l : pictures) l->setParent(nullptr);
        for (QLabel* l : captions) l->setParent(nullptr);
        QWidget* made = tab == 0   ? pane({{pictures[0], captions[0]}}, 1)
                        : tab == 1 ? pane({{pictures[1], captions[1]}}, 1)
                        : tab == 2 ? pane({{pictures[0], captions[0]}, {pictures[1], captions[1]}}, 2)
                                   : pane({{pictures[0], captions[0]}, {pictures[1], captions[1]}, {pictures[2], captions[2]}, {pictures[3], captions[3]}}, 2);
        const QSignalBlocker quiet(views);
        const QString title = views->tabText(tab);
        QWidget* old = views->widget(tab);
        views->removeTab(tab);
        views->insertTab(tab, made, title);
        views->setCurrentIndex(tab);
        delete old;
    };
    connect(views, &QTabWidget::currentChanged, this, lay_out);
    lay_out(2);  // (2 アップ first, as Photoshop opens)

    preset = new QComboBox;
    preset->addItem(QStringLiteral("（カスタム）"), QString());
    for (const std::string_view name : formats::web_presets()) preset->addItem(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())), QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    format = choices({{QStringLiteral("GIF"), QStringLiteral("gif")}, {QStringLiteral("PNG-8"), QStringLiteral("png8")}, {QStringLiteral("PNG-24"), QStringLiteral("png24")},
                      {QStringLiteral("JPEG"), QStringLiteral("jpeg")}, {QStringLiteral("WBMP"), QStringLiteral("wbmp")}},
                     QStringLiteral("png24"));
    palette = choices({{QStringLiteral("知覚的"), QStringLiteral("perceptual")}, {QStringLiteral("割り付け"), QStringLiteral("selective")},
                       {QStringLiteral("適応"), QStringLiteral("adaptive")}, {QStringLiteral("制限（Web）"), QStringLiteral("restrictive")},
                       {QStringLiteral("すべての色"), QStringLiteral("exact")}, {QStringLiteral("Mac OS"), QStringLiteral("systemMac")},
                       {QStringLiteral("Windows"), QStringLiteral("systemWindows")}, {QStringLiteral("均等"), QStringLiteral("uniform")}},
                      QStringLiteral("selective"));
    colors = new QSpinBox;
    colors->setRange(2, 256);
    colors->setValue(256);
    dither = choices({{QStringLiteral("ディザなし"), QStringLiteral("none")}, {QStringLiteral("拡散"), QStringLiteral("diffusion")},
                      {QStringLiteral("パターン"), QStringLiteral("pattern")}, {QStringLiteral("ノイズ"), QStringLiteral("noise")}},
                     QStringLiteral("diffusion"));
    dither_amount = new QSpinBox;
    dither_amount->setRange(0, 100);
    dither_amount->setSuffix(QStringLiteral(" %"));
    dither_amount->setValue(88);
    transparency = new QCheckBox(QStringLiteral("透明部分"));
    transparency->setChecked(true);
    matte = choices({{QStringLiteral("白"), QStringLiteral("#ffffff")}, {QStringLiteral("黒"), QStringLiteral("#000000")}, {QStringLiteral("なし"), QStringLiteral("none")}},
                    QStringLiteral("#ffffff"));
    interlaced = new QCheckBox(QStringLiteral("インターレース"));
    web_snap = new QSpinBox;
    web_snap->setRange(0, 100);
    web_snap->setSuffix(QStringLiteral(" %"));
    quality = new QSpinBox;
    quality->setRange(0, 100);
    quality->setValue(60);
    progressive = new QCheckBox(QStringLiteral("プログレッシブ"));
    optimized = new QCheckBox(QStringLiteral("最適化"));
    optimized->setChecked(true);
    embed_icc = new QCheckBox(QStringLiteral("ICC プロファイルを埋め込む"));
    metadata = choices({{QStringLiteral("なし"), QStringLiteral("none")}, {QStringLiteral("著作権"), QStringLiteral("copyright")},
                        {QStringLiteral("著作権情報と連絡先情報"), QStringLiteral("copyrightAndContact")}, {QStringLiteral("すべて"), QStringLiteral("all")}},
                       QStringLiteral("copyright"));
    srgb = new QCheckBox(QStringLiteral("sRGB に変換"));
    srgb->setChecked(true);
    percent = new QDoubleSpinBox;
    percent->setRange(1.0, 1000.0);
    percent->setDecimals(1);
    percent->setSuffix(QStringLiteral(" %"));
    percent->setValue(100.0);
    resample = choices({{QStringLiteral("バイキュービック"), QStringLiteral("bicubic")}, {QStringLiteral("バイリニア"), QStringLiteral("bilinear")},
                        {QStringLiteral("ニアレストネイバー"), QStringLiteral("nearest")}},
                       QStringLiteral("bicubic"));
    estimate = new QLabel(QStringLiteral("見積もっています…"));
    estimate->setObjectName(QStringLiteral("web_estimate"));
    for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{
             {"preset", preset}, {"format", format}, {"palette", palette}, {"colors", colors}, {"dither", dither}, {"dither_amount", dither_amount},
             {"transparency", transparency}, {"matte", matte}, {"interlaced", interlaced}, {"web_snap", web_snap}, {"quality", quality},
             {"progressive", progressive}, {"optimized", optimized}, {"embed_icc", embed_icc}, {"metadata", metadata}, {"srgb", srgb},
             {"percent", percent}, {"resample", resample}}) {
        widget->setObjectName(QStringLiteral("web_%1").arg(QString::fromLatin1(name)));
    }
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存…"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("プリセット"), preset);
    rows->addRow(QStringLiteral("形式"), format);
    rows->addRow(look::section(QStringLiteral("色")));
    rows->addRow(QStringLiteral("減色アルゴリズム"), palette);
    rows->addRow(QStringLiteral("カラー"), colors);
    rows->addRow(QStringLiteral("ディザ"), dither);
    rows->addRow(QStringLiteral("ディザの量"), dither_amount);
    rows->addRow(QString(), transparency);
    rows->addRow(QStringLiteral("マット"), matte);
    rows->addRow(QStringLiteral("Web スナップ"), web_snap);
    rows->addRow(QString(), interlaced);
    rows->addRow(look::section(QStringLiteral("JPEG")));
    rows->addRow(QStringLiteral("画質"), quality);
    rows->addRow(QString(), progressive);
    rows->addRow(QString(), optimized);
    rows->addRow(look::section(QStringLiteral("メタデータと色")));
    rows->addRow(QString(), embed_icc);
    rows->addRow(QStringLiteral("メタデータ"), metadata);
    rows->addRow(QString(), srgb);
    rows->addRow(look::section(QStringLiteral("画像サイズ")));
    rows->addRow(QStringLiteral("パーセント"), percent);
    rows->addRow(QStringLiteral("画質（再サンプル）"), resample);
    look::quiet_labels(rows);
    auto* left = new QVBoxLayout;
    left->addWidget(views, 1);
    left->addWidget(estimate);
    auto* panes = new QWidget;
    panes->setLayout(left);
    auto* settings_side = new QWidget;
    settings_side->setLayout(rows);
    auto* body = new QHBoxLayout;
    body->addWidget(panes, 3);
    body->addWidget(settings_side, 2);
    look::frame(this, look::header(QStringLiteral("Web 用に保存（従来）"), QStringLiteral("色数や画質を見比べて、Web 用に小さく書き出します")), body, nullptr,
                look::footer(buttons));
    look::fit_to_screen(this, QSize(1100, 760));

    timer_.setSingleShot(true);
    timer_.setInterval(150);
    connect(&timer_, &QTimer::timeout, this, &SaveForWebDialog::start);
    watcher_ = new QFutureWatcher<Result>(this);
    connect(watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = watcher_->result();
        ++previews_;
        if (pending_) {
            pending_ = false;
            start();
            return;
        }
        show_result(result);
    });
    connect(preset, &QComboBox::currentIndexChanged, this, &SaveForWebDialog::preset_chosen);
    for (QComboBox* box : {format, palette, dither, matte, metadata, resample}) connect(box, &QComboBox::currentIndexChanged, this, &SaveForWebDialog::changed);
    for (QSpinBox* box : {colors, dither_amount, web_snap, quality}) connect(box, &QSpinBox::valueChanged, this, &SaveForWebDialog::changed);
    for (QCheckBox* box : {transparency, interlaced, progressive, optimized, embed_icc, srgb}) connect(box, &QCheckBox::toggled, this, &SaveForWebDialog::changed);
    connect(percent, &QDoubleSpinBox::valueChanged, this, &SaveForWebDialog::changed);
    preset->setCurrentIndex(preset->findData(QStringLiteral("PNG-24")));
    preset_chosen();
}

SaveForWebDialog::~SaveForWebDialog() {
    if (watcher_ != nullptr) watcher_->waitForFinished();  // (the worker reads the book this dialog holds)
}

bool SaveForWebDialog::working() const { return timer_.isActive() || (watcher_ != nullptr && watcher_->isRunning()); }

formats::WebSettings SaveForWebDialog::settings() const {
    core::Json params = core::Json::object();
    params["format"] = format->currentData().toString().toStdString();
    params["palette"] = palette->currentData().toString().toStdString();
    params["colors"] = colors->value();
    params["dither"] = dither->currentData().toString().toStdString();
    params["ditherAmount"] = dither_amount->value();
    params["transparency"] = transparency->isChecked();
    params["matte"] = matte->currentData().toString().toStdString();
    params["interlaced"] = interlaced->isChecked();
    params["webSnap"] = web_snap->value();
    params["quality"] = quality->value();
    params["progressive"] = progressive->isChecked();
    params["optimized"] = optimized->isChecked();
    params["embedIcc"] = embed_icc->isChecked();
    params["metadata"] = metadata->currentData().toString().toStdString();
    params["convertToSrgb"] = srgb->isChecked();
    return formats::web_settings_from_json(params);
}

formats::WebSize SaveForWebDialog::size() const {
    formats::WebSize out;
    if (percent->value() != 100.0) out.percent = percent->value();
    out.resample = render::codecs::resize_filter(resample->currentData().toString().toStdString()).value_or(render::codecs::ResizeFilter::Bicubic);
    return out;
}

QString SaveForWebDialog::extension() const { return QString::fromLatin1(formats::web_extension(settings().format).data()); }

void SaveForWebDialog::preset_chosen() {
    const QString name = preset->currentData().toString();
    if (name.isEmpty()) return;
    const auto found = formats::web_preset(name.toStdString());
    if (!found) return;
    presetting_ = true;
    const core::Json params = formats::to_json(*found);
    const auto set_box = [](QComboBox* box, const std::string& value) {
        if (const int at = box->findData(QString::fromStdString(value)); at >= 0) box->setCurrentIndex(at);
    };
    set_box(format, params["format"].get<std::string>());
    set_box(palette, params["palette"].get<std::string>());
    colors->setValue(static_cast<int>(params["colors"].get<std::int64_t>()));
    set_box(dither, params["dither"].get<std::string>());
    dither_amount->setValue(static_cast<int>(params["ditherAmount"].get<double>()));
    transparency->setChecked(params["transparency"].get<bool>());
    set_box(matte, params["matte"].get<std::string>());
    interlaced->setChecked(params["interlaced"].get<bool>());
    web_snap->setValue(static_cast<int>(params["webSnap"].get<double>()));
    quality->setValue(static_cast<int>(params["quality"].get<std::int64_t>()));
    progressive->setChecked(params["progressive"].get<bool>());
    optimized->setChecked(params["optimized"].get<bool>());
    embed_icc->setChecked(params["embedIcc"].get<bool>());
    set_box(metadata, params["metadata"].get<std::string>());
    srgb->setChecked(params["convertToSrgb"].get<bool>());
    presetting_ = false;
    changed();
}

void SaveForWebDialog::changed() {
    if (presetting_) return;
    // (a setting changed by hand is no preset's any more)
    if (const int at = preset->currentIndex(); at > 0) {
        const auto found = formats::web_preset(preset->currentData().toString().toStdString());
        if (found && !(*found == settings())) {
            const QSignalBlocker quiet(preset);
            preset->setCurrentIndex(0);
        }
    }
    const formats::WebFormat f = settings().format;
    const bool indexed = f == formats::WebFormat::Gif || f == formats::WebFormat::Png8;
    for (QWidget* w : std::initializer_list<QWidget*>{palette, colors, web_snap}) w->setEnabled(indexed);
    dither->setEnabled(indexed || f == formats::WebFormat::Wbmp);
    dither_amount->setEnabled(indexed);
    for (QWidget* w : std::initializer_list<QWidget*>{quality, progressive, optimized}) w->setEnabled(f == formats::WebFormat::Jpeg);
    transparency->setEnabled(f != formats::WebFormat::Jpeg && f != formats::WebFormat::Wbmp);
    interlaced->setEnabled(f != formats::WebFormat::Jpeg && f != formats::WebFormat::Wbmp);
    embed_icc->setEnabled(f == formats::WebFormat::Jpeg || f == formats::WebFormat::Png24);
    metadata->setEnabled(f == formats::WebFormat::Jpeg || f == formats::WebFormat::Png24);
    estimate->setText(QStringLiteral("見積もっています…"));
    timer_.start();
}

void SaveForWebDialog::start() {
    if (watcher_->isRunning()) {
        pending_ = true;
        return;
    }
    const std::shared_ptr<const core::Document> doc = doc_;
    const std::size_t page = page_;
    formats::WebSettings st;
    try {
        st = settings();
    } catch (const std::exception& error) {
        estimate->setText(wording::error(QString::fromUtf8(error.what())));
        return;
    }
    const formats::WebSize sz = size();
    watcher_->setFuture(QtConcurrent::run([doc, page, st, sz]() -> Result {
        Result r;
        try {
            // (the page drawn once, at most 1024 px long; each pane optimised from it, its size scaled back to the file's)
            const formats::WebPicture picture = formats::web_picture(*doc, page, st, sz, std::nullopt, kProxySide);
            const double ratio = double(picture.full_width) * picture.full_height / (double(picture.rgba.width) * picture.rgba.height);
            std::optional<std::string> icc = picture.icc;
            const std::optional<std::string> xmp = formats::web_xmp(*doc, st.metadata);
            r.panes.push_back(Pane{image_of(picture.rgba.px, picture.rgba.width, picture.rgba.height),
                                   QStringLiteral("元の画像（%1 × %2 px）").arg(picture.full_width).arg(picture.full_height)});
            std::vector<formats::WebSettings> all{st};
            for (const formats::WebSettings& v : variants(st)) all.push_back(v);
            for (std::size_t i = 0; i < all.size(); ++i) {
                const formats::Optimized o = formats::optimize(picture.rgba, all[i], icc, xmp, picture.dpi, true, nullptr);
                const std::uint64_t bytes = formats::estimated_bytes(picture.rgba, all[i], icc, xmp, picture.dpi, o.bytes, ratio);
                if (i == 0) r.bytes = bytes;
                r.panes.push_back(Pane{image_of(o.preview, o.width, o.height), caption_of(all[i], bytes, o.colors)});
            }
            r.ok = true;
        } catch (const std::exception& error) {
            r.error = QString::fromUtf8(error.what());
        }
        return r;
    }));
}

void SaveForWebDialog::show_result(const Result& result) {
    if (!result.ok) {
        estimate->setText(wording::error(result.error));
        for (QLabel* p : pictures) p->clear();
        return;
    }
    for (std::size_t i = 0; i < pictures.size() && i < result.panes.size(); ++i) {
        const QSize room = pictures[i]->size().expandedTo(QSize(160, 120));
        pictures[i]->setPixmap(QPixmap::fromImage(result.panes[i].picture).scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        captions[i]->setText(result.panes[i].caption);
    }
    estimate->setText(result.panes.size() > 1 ? result.panes[1].caption : QString());
}

}  // namespace genko::app
