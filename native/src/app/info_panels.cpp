// ヒストグラム パネルと情報パネル (PC2d): app/info_panels.hpp.

#include "app/info_panels.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QPushButton>
#include <QFormLayout>
#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tiles.hpp"
#include "app/view_units.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "render/color_canvas.hpp"
#include "render/ink.hpp"
#include "render/inspect.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace {

// The page drawn for its histogram: a picture of about this many pixels across (upstream's cache, tone.rs:85).
constexpr double kAcross = 384.0;

// The layer's own pixels at their own size, as the screen shows them ("RGBA"): its raster (precise: its 8-bit picture;
// of inks: the colours they mean), else none.
std::optional<render::Image> own_pixels(const core::Page& page, const core::Layer& layer) {
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        const render::Size size{static_cast<int>(view.width()), static_cast<int>(view.height())};
        return render::color_raster_preview(view, size, render::Box{0, 0, size.width, size.height}, render::RgbSpace::of(page));
    }
    if (layer.ink_raster && page.color) return render::ink::to_rgba8(core::decode_ink_raster(*layer.ink_raster), *page.color);
    if (layer.raster_png && !layer.raster_png->empty() && layer.kind != core::LayerKind::Placed)
        return render::open_image(*layer.raster_png, render::kPillowOpenLimits).convert("RGBA");
    return std::nullopt;
}

QString number(double v) { return QString::number(v, 'f', 2); }

}  // namespace

Histogram histogram_of(const render::Image& picture) {
    Histogram out;
    const render::Image rgba = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    const std::string bytes = rgba.tobytes();
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + i);
        if (p[3] == 0) continue;
        ++out.pixels;
        ++out.bins[Histogram::kRed][p[0]];
        ++out.bins[Histogram::kGreen][p[1]];
        ++out.bins[Histogram::kBlue][p[2]];
        ++out.bins[Histogram::kRgb][(static_cast<unsigned>(p[0]) + p[1] + p[2]) / 3];
        ++out.bins[Histogram::kLuminosity][(p[0] * 299u + p[1] * 587u + p[2] * 114u + 500u) / 1000u];
    }
    return out;
}

HistogramStats stats_of(const std::array<std::uint64_t, 256>& bins) {
    HistogramStats out;
    for (const std::uint64_t v : bins) out.pixels += v;
    if (out.pixels == 0) return out;
    double sum = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) sum += static_cast<double>(i) * static_cast<double>(bins[i]);
    out.mean = sum / static_cast<double>(out.pixels);
    double var = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) var += (static_cast<double>(i) - out.mean) * (static_cast<double>(i) - out.mean) * static_cast<double>(bins[i]);
    out.deviation = std::sqrt(var / static_cast<double>(out.pixels));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) {  // (the first level half the pixels reach: upstream's median)
        seen += bins[i];
        if (seen * 2 >= out.pixels) {
            out.median = static_cast<int>(i);
            break;
        }
    }
    return out;
}

// --- ヒストグラム -------------------------------------------------------------------------------------------------------

class HistogramGraph : public QWidget {
public:
    explicit HistogramGraph(HistogramPanel* panel) : panel_(panel) {
        setMinimumHeight(100);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        const int shown = panel_->channel_->currentIndex();
        const auto draw = [&](const std::array<std::uint64_t, 256>& bins, const QColor& colour) {
            // (scaled to a high level of the counts, so one spike — the paper's white — does not flatten the rest)
            std::vector<std::uint64_t> sorted(bins.begin(), bins.end());
            std::sort(sorted.begin(), sorted.end());
            const double top = std::max<double>(1.0, static_cast<double>(sorted[250]) * 1.1);
            p.setPen(QPen(colour, 1));
            for (int i = 0; i < 256; ++i) {
                const double x = (i + 0.5) / 256.0 * width();
                const double h = std::min(1.0, static_cast<double>(bins[static_cast<std::size_t>(i)]) / top) * (height() - 2);
                if (h > 0) p.drawLine(QPointF(x, height() - 1), QPointF(x, height() - 1 - h));
            }
        };
        const Histogram& h = panel_->histogram_;
        if (shown == 5) {  // (カラー: red, green and blue laid over each other)
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            draw(h.bins[Histogram::kRed], QColor(235, 80, 80));
            draw(h.bins[Histogram::kGreen], QColor(80, 205, 95));
            draw(h.bins[Histogram::kBlue], QColor(90, 140, 255));
            return;
        }
        static const std::array<QColor, 5> colours{QColor(110, 110, 110), QColor(235, 80, 80), QColor(80, 205, 95), QColor(90, 140, 255),
                                                   QColor(70, 70, 70)};
        draw(h.bins[static_cast<std::size_t>(std::clamp(shown, 0, 4))], colours[static_cast<std::size_t>(std::clamp(shown, 0, 4))]);
    }

private:
    HistogramPanel* panel_;
};

HistogramPanel::HistogramPanel(MainWindow* window) : window_(window) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* form = new QFormLayout;
    source_ = new QComboBox;
    source_->addItems({QStringLiteral("ページ全体"), QStringLiteral("描く先のレイヤー")});
    source_->setToolTip(QStringLiteral("数える絵: ページの見え（小さな解像度で）か、描く先のレイヤーだけ（その画素）"));
    form->addRow(QStringLiteral("元"), source_);
    channel_ = new QComboBox;
    channel_->addItems({QStringLiteral("RGB"), QStringLiteral("レッド"), QStringLiteral("グリーン"), QStringLiteral("ブルー"),
                        QStringLiteral("輝度"), QStringLiteral("カラー")});
    form->addRow(QStringLiteral("チャンネル"), channel_);
    layout->addLayout(form);
    graph_ = new HistogramGraph(this);
    layout->addWidget(graph_);
    auto* figures = new QFormLayout;
    const char* names[] = {"平均", "標準偏差", "中央値", "ピクセル数"};
    for (std::size_t i = 0; i < figures_.size(); ++i) {
        figures_[i] = new QLabel(QStringLiteral("—"));
        figures_[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        figures->addRow(QString::fromUtf8(names[i]), figures_[i]);
    }
    layout->addLayout(figures);
    layout->addStretch(1);
    connect(source_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(channel_, &QComboBox::currentIndexChanged, this, [this] {
        show_figures();
        graph_->update();
    });
    // (followed while shown: made again a moment after what it counts has changed — the page, the layer drawn on)
    watch_ = new QTimer(this);
    watch_->setInterval(300);
    connect(watch_, &QTimer::timeout, this, [this] {
        if (isVisible()) follow();
    });
    watch_->start();
}

void HistogramPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    follow();
}

void HistogramPanel::follow() {
    const core::Page* page = window_->current_page();
    const core::Layer* layer = source_->currentIndex() == 1 ? window_->target_layer() : nullptr;
    // (a page or layer changed is a new block: an edit never changes one in place)
    const void* now = layer != nullptr ? static_cast<const void*>(layer) : static_cast<const void*>(page);
    if (now == counted_ && source_->currentIndex() == counted_source_) return;
    refresh();
}

void HistogramPanel::refresh() {
    histogram_ = {};
    const core::Page* page = window_->current_page();
    counted_source_ = source_->currentIndex();
    counted_ = nullptr;
    if (page != nullptr) {
        try {
            if (source_->currentIndex() == 1) {
                const core::Layer* layer = window_->target_layer();
                counted_ = layer;
                if (layer != nullptr) {
                    const auto own = own_pixels(*page, *layer);
                    const int dpi = std::max(10, static_cast<int>(kAcross * 25.4 / std::max(page->spec.width_mm.value(), page->spec.height_mm.value())));
                    histogram_ = histogram_of(own ? *own : render::layer_image(*page, *layer, dpi, &window_->book(), true));
                }
            } else {
                counted_ = page;
                const int dpi = std::max(10, static_cast<int>(kAcross * 25.4 / std::max(page->spec.width_mm.value(), page->spec.height_mm.value())));
                render::RenderOptions options;
                options.mode = "proof";
                options.skip_unported = true;
                histogram_ = histogram_of(render::render_page(*page, dpi, options, &window_->book()).image);
            }
        } catch (const std::exception&) {  // (a page that cannot be drawn now: nothing counted)
            histogram_ = {};
        }
    }
    show_figures();
    graph_->update();
}

void HistogramPanel::show_figures() {
    const int shown = channel_->currentIndex();
    const HistogramStats s = stats_of(histogram_.bins[static_cast<std::size_t>(shown == 5 ? Histogram::kRgb : std::clamp(shown, 0, 4))]);
    const bool any = s.pixels > 0;
    figures_[0]->setText(any ? number(s.mean) : QStringLiteral("—"));
    figures_[1]->setText(any ? number(s.deviation) : QStringLiteral("—"));
    figures_[2]->setText(any ? QString::number(s.median) : QStringLiteral("—"));
    figures_[3]->setText(QString::number(histogram_.pixels));
}

// --- 情報 -----------------------------------------------------------------------------------------------------------

InfoPanel::InfoPanel(MainWindow* window) : window_(window) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(6, 6, 6, 6);
    const auto label = [](const char* name) {
        auto* l = new QLabel(QStringLiteral("—"));
        l->setObjectName(QString::fromLatin1(name));
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setWordWrap(true);
        return l;
    };
    place_ = label("infoPlace");
    screen_ = label("infoScreen");
    values_ = label("infoValues");
    proof_ = label("infoProof");
    selection_ = label("infoSelection");
    ruler_ = label("infoRuler");
    form->addRow(QStringLiteral("位置"), place_);
    form->addRow(QStringLiteral("画面の色"), screen_);
    form->addRow(QStringLiteral("ページの値"), values_);
    form->addRow(QStringLiteral("校正の値"), proof_);
    form->addRow(QStringLiteral("選択範囲"), selection_);
    form->addRow(QStringLiteral("ものさし"), ruler_);
    ruler_->setToolTip(QStringLiteral("ものさしツールの線: 角度（A）・長さ（L1・分度器の L2）・幅（W）・高さ（H）・始点（X・Y）"));
    place_->setToolTip(QStringLiteral("ポインターの位置（環境設定の定規の単位と、原稿の解像度の画素）"));
    values_->setToolTip(QStringLiteral("ページの色モードの値（genko inspect --pixel・ステータスバーと同じ）"));
    proof_->setToolTip(QStringLiteral("色の校正のプロファイルでの値（CMYK はインキの %）"));
}

void InfoPanel::show_at(std::optional<QPointF> mm) {
    const core::Page* page = window_->current_page();
    at_.reset();
    if (page == nullptr || !mm || mm->x() < 0 || mm->y() < 0 || mm->x() > page->spec.width_mm.value() || mm->y() > page->spec.height_mm.value()) {
        for (QLabel* l : {place_, screen_, values_, proof_}) l->setText(QStringLiteral("—"));
        return;
    }
    const double dpi = page->spec.dpi.truthy() ? page->spec.dpi.value() : 350.0;
    // (PC5 H-b: in 環境設定 → 単位・定規's unit — mm as before, with the page's pixels)
    const core::units::Unit unit = view_units::ruler();
    if (unit == core::units::Unit::Mm) {
        place_->setText(QStringLiteral("X %1 mm（%2 px）  Y %3 mm（%4 px）")
                            .arg(mm->x(), 0, 'f', 1)
                            .arg(std::floor(mm->x() / 25.4 * dpi), 0, 'f', 0)
                            .arg(mm->y(), 0, 'f', 1)
                            .arg(std::floor(mm->y() / 25.4 * dpi), 0, 'f', 0));
    } else if (unit == core::units::Unit::Px) {
        place_->setText(QStringLiteral("X %1 px  Y %2 px").arg(std::floor(mm->x() / 25.4 * dpi), 0, 'f', 0).arg(std::floor(mm->y() / 25.4 * dpi), 0, 'f', 0));
    } else {
        place_->setText(QStringLiteral("X %1（%2 px）  Y %3（%4 px）")
                            .arg(view_units::words(mm->x(), unit, view_units::basis(page, true)))
                            .arg(std::floor(mm->x() / 25.4 * dpi), 0, 'f', 0)
                            .arg(view_units::words(mm->y(), unit, view_units::basis(page, false)))
                            .arg(std::floor(mm->y() / 25.4 * dpi), 0, 'f', 0));
    }
    try {
        // (as the status bar reads it: the page drawn at the book's resolution, 300 dpi at most)
        const double at_dpi = window_->book().spec.dpi.truthy() ? std::min(300.0, window_->book().spec.dpi.value()) : 300.0;
        at_ = render::pixel_at(*page, &window_->book(), mm->x(), mm->y(), std::nullopt, static_cast<int>(std::max(1.0, at_dpi)));
    } catch (const std::exception&) {
        for (QLabel* l : {screen_, values_, proof_}) l->setText(QStringLiteral("—"));
        return;
    }
    const core::Json& srgb = at_->at("srgb");
    const int r = srgb[0].get<int>(), g = srgb[1].get<int>(), b = srgb[2].get<int>();
    screen_->setText(QStringLiteral("R %1  G %2  B %3（%4）").arg(r).arg(g).arg(b).arg(QColor(r, g, b).name()));
    const QString words = (page->color || page->rgb) ? MainWindow::pixel_words(*page, *at_) : QString();
    values_->setText(words.isEmpty() ? QStringLiteral("R %1  G %2  B %3（sRGB・8 bit）").arg(r).arg(g).arg(b) : words);
    try {
        const core::Json proof = render::proof::proof_values(window_->canvas()->renderer().proof_setup(), r, g, b);
        QStringList parts;
        const std::string space = proof.at("space").get<std::string>();
        const core::Json& values = proof.at("values");
        static const QStringList cmyk{QStringLiteral("C"), QStringLiteral("M"), QStringLiteral("Y"), QStringLiteral("K")};
        static const QStringList rgb{QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")};
        for (std::size_t c = 0; c < values.size(); ++c) {
            const double v = values[c].get<double>();
            if (space == "cmyk") parts << QStringLiteral("%1 %2%").arg(cmyk[static_cast<qsizetype>(c)]).arg(std::lround(v * 100));
            else if (space == "gray") parts << QStringLiteral("K %1%").arg(std::lround((1 - v) * 100));
            else parts << QStringLiteral("%1 %2").arg(rgb[static_cast<qsizetype>(c)]).arg(std::lround(v * 255));
        }
        proof_->setText(parts.join(QStringLiteral("  ")));
    } catch (const std::exception&) {
        proof_->setText(QStringLiteral("—"));
    }
}

void InfoPanel::show_ruler(const std::optional<core::Json>& r) {
    if (!r) {
        ruler_->setText(QStringLiteral("—"));
        return;
    }
    const auto num = [&](const char* key) { return r->at(key).get<double>(); };
    QString text = QStringLiteral("A %1°  L1 %2 mm（%3 px）").arg(num("angle"), 0, 'f', 1).arg(num("l1_mm"), 0, 'f', 2).arg(num("l1"), 0, 'f', 0);
    if (r->at("protractor").get<bool>()) text += QStringLiteral("  L2 %1 mm（%2 px）").arg(num("l2_mm"), 0, 'f', 2).arg(num("l2"), 0, 'f', 0);
    text += QStringLiteral("\nW %1  H %2 mm  X %3  Y %4 mm").arg(num("w_mm"), 0, 'f', 2).arg(num("h_mm"), 0, 'f', 2).arg(num("x_mm"), 0, 'f', 2).arg(num("y_mm"), 0, 'f', 2);
    if (r->at("units").get<std::string>() != "pixels") {
        text += QStringLiteral("\n計測スケールで %1 %2").arg(num("length"), 0, 'f', 2).arg(QString::fromStdString(r->at("units").get<std::string>()));
    }
    ruler_->setText(text);
}

void InfoPanel::show_selection() {
    const auto& chosen = window_->canvas()->selection();
    const core::Page* page = window_->current_page();
    if (!chosen || chosen->outline.empty() || page == nullptr) {
        selection_->setText(QStringLiteral("なし"));
        return;
    }
    QPolygonF outline;
    for (const QPointF& q : chosen->outline) outline << q;
    const QRectF box = outline.boundingRect();
    const double dpi = page->spec.dpi.truthy() ? page->spec.dpi.value() : 350.0;
    const core::units::Unit unit = view_units::ruler();  // (PC5 H-b)
    if (unit == core::units::Unit::Mm) {
        selection_->setText(QStringLiteral("幅 %1 mm（%2 px）  高さ %3 mm（%4 px）")
                                .arg(box.width(), 0, 'f', 1)
                                .arg(std::lround(box.width() / 25.4 * dpi))
                                .arg(box.height(), 0, 'f', 1)
                                .arg(std::lround(box.height() / 25.4 * dpi)));
    } else if (unit == core::units::Unit::Px) {
        selection_->setText(QStringLiteral("幅 %1 px  高さ %2 px").arg(std::lround(box.width() / 25.4 * dpi)).arg(std::lround(box.height() / 25.4 * dpi)));
    } else {
        selection_->setText(QStringLiteral("幅 %1（%2 px）  高さ %3（%4 px）")
                                .arg(view_units::words(box.width(), unit, view_units::basis(page, true)))
                                .arg(std::lround(box.width() / 25.4 * dpi))
                                .arg(view_units::words(box.height(), unit, view_units::basis(page, false)))
                                .arg(std::lround(box.height() / 25.4 * dpi)));
    }
}

// --- the window's side -----------------------------------------------------------------------------------------------

namespace {

// A profile's information (render::proof::profile_info) in the dialog's words.
QString profile_text(const core::Json& info) {
    if (info.contains("description") && info.at("description").is_null())  // (a page whose plates have none)
        return QStringLiteral("プロファイルはありません（マルチチャンネルのページの版はプロファイルを持ちません）");
    QStringList rows;
    const auto text = [&](const char* key) { return info.contains(key) && info.at(key).is_string() ? QString::fromStdString(info.at(key).get<std::string>()) : QString(); };
    static const std::map<std::string, QString> spaces{{"rgb", QStringLiteral("RGB")}, {"gray", QStringLiteral("グレー")}, {"cmyk", QStringLiteral("CMYK")},
                                                       {"lab", QStringLiteral("Lab")}};
    static const std::map<std::string, QString> classes{{"display", QStringLiteral("ディスプレイ")}, {"output", QStringLiteral("出力（印刷）")},
                                                        {"input", QStringLiteral("入力")}, {"colorspace", QStringLiteral("色空間")},
                                                        {"abstract", QStringLiteral("抽象")}, {"link", QStringLiteral("デバイスリンク")},
                                                        {"named", QStringLiteral("名前付きの色")}};
    const auto named = [](const std::map<std::string, QString>& words, const QString& key) {
        const auto it = words.find(key.toStdString());
        return it != words.end() ? it->second : key;
    };
    rows << QStringLiteral("説明: %1").arg(text("description"));
    rows << QStringLiteral("色空間: %1").arg(named(spaces, text("space")));
    rows << QStringLiteral("種類: %1").arg(named(classes, text("class")));
    rows << QStringLiteral("ICC の版: %1").arg(text("version"));
    if (info.contains("white") && info.at("white").is_array()) {
        const core::Json& w = info.at("white");
        rows << QStringLiteral("白色点（XYZ）: %1, %2, %3").arg(w[0].get<double>(), 0, 'f', 4).arg(w[1].get<double>(), 0, 'f', 4).arg(w[2].get<double>(), 0, 'f', 4);
    }
    if (!text("trc").isEmpty()) rows << QStringLiteral("トーンカーブ: %1").arg(text("trc"));
    rows << QStringLiteral("変換の表（LUT）: %1").arg(info.value("lut", false) ? QStringLiteral("あり") : QStringLiteral("なし"));
    QStringList intents;
    for (const core::Json& i : info.value("intents", core::Json::array())) intents << QString::fromStdString(i.get<std::string>());
    rows << QStringLiteral("変換の意図: %1").arg(intents.join(QStringLiteral("・")));
    const std::int64_t bytes = info.value("bytes", std::int64_t{0});
    rows << QStringLiteral("大きさ: %1").arg(bytes > 0 ? QStringLiteral("%1 バイト").arg(bytes) : QStringLiteral("（Little CMS 内蔵）"));
    return rows.join(QLatin1Char('\n'));
}

}  // namespace

void MainWindow::build_info_docks() {
    histogram_panel_ = new HistogramPanel(this);
    histogram_dock_ = new QDockWidget(QStringLiteral("ヒストグラム"), this);
    histogram_dock_->setObjectName(QStringLiteral("ヒストグラム"));
    histogram_dock_->setWidget(histogram_panel_);
    histogram_dock_->setMinimumWidth(200);
    histogram_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, histogram_dock_);
    tabifyDockWidget(layers_dock_, histogram_dock_);
    histogram_dock_->hide();  // (for colour work: ウィンドウ → ヒストグラム brings it, as チャンネル)
    view_menu_->addAction(histogram_dock_->toggleViewAction());
    info_panel_ = new InfoPanel(this);
    info_dock_ = new QDockWidget(QStringLiteral("情報"), this);
    info_dock_->setObjectName(QStringLiteral("情報"));
    info_dock_->setWidget(info_panel_);
    info_dock_->setMinimumWidth(200);
    info_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, info_dock_);
    tabifyDockWidget(layers_dock_, info_dock_);
    info_dock_->hide();
    QAction* info = info_dock_->toggleViewAction();
    info->setShortcut(QKeySequence(QStringLiteral("F8")));  // (upstream's window.panel.info)
    view_menu_->addAction(info);
    layers_dock_->raise();
    // (the info follows the pointer and the selection while it is shown)
    connect(canvas_, &PageCanvas::pointerOver, this, [this](double x_mm, double y_mm) {
        if (info_dock_->isVisible()) info_panel_->show_at(QPointF(x_mm, y_mm));
    });
    connect(canvas_, &PageCanvas::changed, this, [this] {
        if (info_dock_->isVisible()) info_panel_->show_selection();
    });
    connect(info_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) info_panel_->show_selection();
    });
}

void MainWindow::profile_info_dialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("profile_info_dialog"));
    dialog.setWindowTitle(QStringLiteral("プロファイルの情報"));
    auto* choice = new QComboBox;
    choice->setObjectName(QStringLiteral("profile_choice"));
    if (const core::Page* page = current_page()) {
        choice->addItem(QStringLiteral("このページのプロファイル（%1 ページ）").arg(QString::fromStdString(page->index.repr())), QString::fromStdString(page->index.repr()));
    }
    for (const core::BuiltinProfile& b : core::builtin_profiles()) {
        choice->addItem(QString::fromUtf8(b.description.data(), static_cast<qsizetype>(b.description.size())),
                        QString::fromUtf8(b.id.data(), static_cast<qsizetype>(b.id.size())));
    }
    auto* shown = new QLabel;
    shown->setObjectName(QStringLiteral("profile_info"));
    shown->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shown->setWordWrap(true);
    shown->setMinimumWidth(360);
    const auto show = [this, choice, shown] {
        try {
            shown->setText(profile_text(render::proof::profile_info_of(book(), choice->currentData().toString().toStdString())));
        } catch (const std::exception& error) {
            shown->setText(QStringLiteral("読めません: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        }
    };
    connect(choice, &QComboBox::currentIndexChanged, &dialog, show);
    show();
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("プロファイル"), choice);
    form->addRow(shown);
    look::quiet_labels(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    look::frame(&dialog, look::header(QStringLiteral("プロファイルの情報"), QStringLiteral("ICC プロファイルが自分について言うこと（見るだけ）")), form, nullptr,
                look::footer(buttons));
    ask::exec(&dialog);
}

}  // namespace genko::app
