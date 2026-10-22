// 修復の道具 (PC3 B4, SPEC.md COLOR-06「修復」): スポット修復ブラシツール, 修復ブラシツール, コピースタンプツール (S) and ヒストリー
// ブラシツール in the window and on the canvas — the tools and their settings (直径・硬さ・不透明度・流量・間隔・描画モード, 調整あり,
// サンプル, 種類), a stroke made their op (render/ops_retouch), the clone source set by Alt+click and shown on the canvas (its
// cross and the overlay at the brush), コピーソース panel's dock, and the History Brush's source state (履歴's ◆ row) put in
// the book's assets as the op's `from`. コンテンツに応じた塗りつぶし… is content_aware_window.cpp.

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <map>

#include "app/canvas.hpp"
#include "app/clone_source_panel.hpp"
#include "app/fields.hpp"
#include "app/history_panel.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "core/strokes.hpp"
#include "render/image_ops.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/retouch_dabs.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"

namespace genko::app {

using core::Json;

namespace {

const std::vector<std::pair<std::string, QString>>& blend_words() {
    static const std::vector<std::pair<std::string, QString>> words{
        {"normal", QStringLiteral("通常")},           {"multiply", QStringLiteral("乗算")},
        {"screen", QStringLiteral("スクリーン")},     {"add", QStringLiteral("加算（発光）")},
        {"overlay", QStringLiteral("オーバーレイ")},  {"darken", QStringLiteral("比較（暗）")},
        {"lighten", QStringLiteral("比較（明）")},    {"color_burn", QStringLiteral("焼き込みカラー")},
        {"linear_burn", QStringLiteral("焼き込み（リニア）")}, {"color_dodge", QStringLiteral("覆い焼きカラー")},
        {"soft_light", QStringLiteral("ソフトライト")}, {"hard_light", QStringLiteral("ハードライト")},
        {"difference", QStringLiteral("差の絶対値")}, {"exclusion", QStringLiteral("除外")},
        {"subtract", QStringLiteral("減算")},         {"divide", QStringLiteral("除算")},
        {"hue", QStringLiteral("色相")},             {"saturation", QStringLiteral("彩度")},
        {"color", QStringLiteral("カラー")},          {"luminosity", QStringLiteral("輝度")}};
    return words;
}

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::hint(label);
    return label;
}

Json mm_pair(const QPointF& p) { return Json::array({std::round(p.x() * 1000.0) / 1000.0, std::round(p.y() * 1000.0) / 1000.0}); }

// The History Brush's `from` last put in the book's assets, by window: the state, page and layer it is of.
struct HistoryFrom {
    DocPtr state;
    std::string page_id, layer_id;
    Json from;
};
std::map<const MainWindow*, HistoryFrom>& history_froms() {
    static std::map<const MainWindow*, HistoryFrom> all;
    return all;
}

}  // namespace

// --- the canvas ----------------------------------------------------------------------------------------------------------

bool PageCanvas::is_retouch_tool(const QString& tool) {
    return tool == QLatin1String("spotheal") || tool == QLatin1String("heal") || tool == QLatin1String("clone") ||
           tool == QLatin1String("historybrush");
}

bool PageCanvas::is_clone_tool(const QString& tool) { return tool == QLatin1String("heal") || tool == QLatin1String("clone"); }

bool PageCanvas::retouch_press(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    if (!is_clone_tool(tool_) || (modifiers & Qt::AltModifier) == 0) return false;
    emit cloneSourcePicked(mm.x(), mm.y());
    update();
    return true;
}

void PageCanvas::draw_retouch(QPainter& painter) const {
    if (!is_clone_tool(tool_) || !clone_shown.source) return;
    const QPointF source = *clone_shown.source;
    const std::optional<QPointF> at = !stroke_.empty() ? std::optional<QPointF>(QPointF(stroke_.back().x, stroke_.back().y)) : hover_;
    // the destination paired with the source: 調整あり's, the stroke's start, or (before a stroke) the pointer
    std::optional<QPointF> anchor = clone_shown.anchor;
    if (!anchor && !stroke_.empty()) anchor = QPointF(stroke_.front().x, stroke_.front().y);
    const QPointF a = anchor.value_or(at.value_or(source));
    const std::array<double, 4>& m = clone_shown.m;
    const auto read = [&](const QPointF& d) {  // (where a destination point reads the source)
        const double dx = d.x() - a.x(), dy = d.y() - a.y();
        return QPointF(source.x() + m[0] * dx + m[1] * dy, source.y() + m[2] * dx + m[3] * dy);
    };
    // the overlay: the sampled picture laid at the brush as the source lies there
    const bool hidden = clone_shown.auto_hide && !stroke_.empty();
    if (clone_shown.overlay && !hidden && !clone_shown.picture.isNull() && at) {
        const double det = m[0] * m[3] - m[1] * m[2];
        if (std::abs(det) > 1e-12) {
            // (source → destination: d = A + f (s − S), f the inverse of m; then the page's mm to the view)
            const std::array<double, 4> f{m[3] / det, -m[1] / det, -m[2] / det, m[0] / det};
            const double sc = view_.scale;
            const QTransform onto(sc * f[0], sc * f[2], sc * f[1], sc * f[3], view_.pan.x() + sc * (a.x() - (f[0] * source.x() + f[1] * source.y())),
                                  view_.pan.y() + sc * (a.y() - (f[2] * source.x() + f[3] * source.y())));
            painter.save();
            if (clone_shown.clipped) {
                QPainterPath brush;
                const double r = std::max(2.0, retouch_mm / 2 * sc);
                brush.addEllipse(pt(at->x(), at->y()), r, r);
                painter.setClipPath(brush);
            }
            painter.setOpacity(std::clamp(clone_shown.opacity, 0.0, 1.0));
            const QString& blend = clone_shown.blend;
            painter.setCompositionMode(blend == QLatin1String("darken")       ? QPainter::CompositionMode_Darken
                                       : blend == QLatin1String("lighten")    ? QPainter::CompositionMode_Lighten
                                       : blend == QLatin1String("difference") ? QPainter::CompositionMode_Difference
                                                                              : QPainter::CompositionMode_SourceOver);
            painter.setTransform(onto, true);
            painter.drawImage(clone_shown.picture_mm, clone_shown.picture);
            painter.restore();
        }
    }
    // the source's cross: where the pointer reads (where the source was set, until a stroke pairs it)
    const QPointF c = pt(at && (anchor || !stroke_.empty()) ? read(*at).x() : source.x(), at && (anchor || !stroke_.empty()) ? read(*at).y() : source.y());
    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (const auto& [colour, width] : {std::pair{QColor(255, 255, 255, 200), 3.0}, std::pair{theme::accent(), 1.2}}) {
        painter.setPen(QPen(colour, width));
        painter.drawLine(c + QPointF(-7, 0), c + QPointF(7, 0));
        painter.drawLine(c + QPointF(0, -7), c + QPointF(0, 7));
    }
    painter.restore();
}

// --- the window: tools, settings, strokes ------------------------------------------------------------------------------

void MainWindow::build_retouch_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const QString& name, const QList<QKeySequence>& keys, const QString& tip) {
        QAction* act = make(QString::fromLatin1(attribute), title, [this, name] { choose_tool(name); }, keys, tip, true);
        act->setAutoRepeat(false);
        tools_->addAction(act);
        tool_actions_[name] = act;
    };
    tool("act_spot_healing", QStringLiteral("スポット修復ブラシツール"), QStringLiteral("spotheal"), {},
         QStringLiteral("なぞった所のしみやごみを、まわりの絵から作って埋めます（コピー元は要りません）"));
    tool("act_healing", QStringLiteral("修復ブラシツール"), QStringLiteral("heal"), {},
         QStringLiteral("Alt+クリックで決めたコピー元の模様を、なぞった所の色と明るさになじませて写します"));
    tool("act_clone_stamp", QStringLiteral("コピースタンプツール"), QStringLiteral("clone"), {QKeySequence(QStringLiteral("S"))},
         QStringLiteral("Alt+クリックで決めたコピー元を、なぞった所にそのまま写します"));
    tool("act_history_brush", QStringLiteral("ヒストリーブラシツール"), QStringLiteral("historybrush"), {},
         QStringLiteral("なぞった所を、履歴で選んだ状態（◆）の絵に塗り戻します"));
    make("act_content_aware_fill", QStringLiteral("コンテンツに応じた塗りつぶし…"), [this] { content_aware_fill_dialog(); }, {},
         QStringLiteral("選択範囲を、まわりの絵から作って埋めます（描く先のペイントのレイヤー）"));
    connect(canvas_, &PageCanvas::cloneSourcePicked, this, &MainWindow::clone_source_picked);
}

void MainWindow::build_retouch_pages(ToolSettings* ts) {
    RetouchSettings& s = retouch_settings;
    auto* size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("retouch_size"));
    size->setRange(0.2, 200);
    size->setSingleStep(0.5);
    size->setSuffix(QStringLiteral(" mm"));
    size->setValue(s.size_mm);
    connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        retouch_settings.size_mm = v;
        canvas_->retouch_mm = v;
        canvas_->update();
    });
    canvas_->retouch_mm = s.size_mm;
    const auto percent = [this](const char* name, int lo, int hi, double value, double RetouchSettings::*field) {
        auto* box = new QSpinBox;
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(lo, hi);
        box->setSuffix(QStringLiteral(" %"));
        box->setValue(static_cast<int>(value));
        connect(box, &QSpinBox::valueChanged, this, [this, field](int v) { retouch_settings.*field = v; });
        return box;
    };
    QSpinBox* hardness = percent("retouch_hardness", 0, 100, s.hardness, &RetouchSettings::hardness);
    QSpinBox* opacity = percent("retouch_opacity", 1, 100, s.opacity, &RetouchSettings::opacity);
    QSpinBox* flow = percent("retouch_flow", 1, 100, s.flow, &RetouchSettings::flow);
    QSpinBox* spacing = percent("retouch_spacing", 1, 1000, s.spacing, &RetouchSettings::spacing);
    auto* blend = new QComboBox;
    blend->setObjectName(QStringLiteral("retouch_blend"));
    for (const auto& [name, words] : blend_words()) blend->addItem(words, QString::fromStdString(name));
    connect(blend, &QComboBox::currentIndexChanged, this, [this, blend] { retouch_settings.blend = blend->currentData().toString().toStdString(); });
    auto* aligned = new QCheckBox(QStringLiteral("調整あり"));
    aligned->setObjectName(QStringLiteral("retouch_aligned"));
    aligned->setToolTip(QStringLiteral("オン: 筆を離しても、コピー元とのずれを保つ。オフ: なぞるたびに、決めたコピー元から写し始める"));
    aligned->setChecked(s.aligned);
    connect(aligned, &QCheckBox::toggled, this, [this](bool on) {
        retouch_settings.aligned = on;
        clone_sources_follow();
    });
    auto* sample = new QComboBox;
    sample->setObjectName(QStringLiteral("retouch_sample"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("現在のレイヤー"), QStringLiteral("current")},
                                     {QStringLiteral("現在のレイヤー以下"), QStringLiteral("current_and_below")},
                                     {QStringLiteral("すべてのレイヤー"), QStringLiteral("all")}})
        sample->addItem(label, key);
    sample->setToolTip(QStringLiteral("写す元: 描く先のレイヤーだけ・描く先とその下のレイヤー・ページの見えるとおり"));
    connect(sample, &QComboBox::currentIndexChanged, this, [this, sample] {
        retouch_settings.sample = sample->currentData().toString().toStdString();
        clone_overlay_picture();
    });
    auto* type = new QComboBox;
    type->setObjectName(QStringLiteral("retouch_spot_type"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("コンテンツに応じる"), QStringLiteral("content_aware")},
                                     {QStringLiteral("テクスチャを作成"), QStringLiteral("create_texture")},
                                     {QStringLiteral("近似色に合わせる"), QStringLiteral("proximity_match")}})
        type->addItem(label, key);
    connect(type, &QComboBox::currentIndexChanged, this, [this, type] { retouch_settings.spot_type = type->currentData().toString().toStdString(); });
    // The fields are shared by the four tools' pages: a widget lives in one page, so each tool shows the one page and the
    // rows that do not apply to it are hidden when it is chosen (ToolSettings shows the page of the tool in hand).
    auto* widget = new QWidget;
    widget->setObjectName(QStringLiteral("retouch_page"));
    auto* form = new QFormLayout(widget);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->addRow(QStringLiteral("直径"), slider_for(size, true));
    form->addRow(QStringLiteral("硬さ"), hardness);
    form->addRow(QStringLiteral("不透明度"), opacity);
    form->addRow(QStringLiteral("流量"), flow);
    form->addRow(QStringLiteral("間隔"), spacing);
    form->addRow(QStringLiteral("描画モード"), blend);
    form->addRow(QString(), aligned);
    form->addRow(QStringLiteral("サンプル"), sample);
    form->addRow(QStringLiteral("種類"), type);
    QLabel* note = hint(QString());
    form->addRow(note);
    // (what each tool shows: 調整あり and サンプル with the clone tools, 種類 with the spot healing brush)
    const auto show_for = [form, aligned, sample, type, blend, note](const QString& tool) {
        const bool clone = PageCanvas::is_clone_tool(tool);
        form->setRowVisible(aligned, clone);
        form->setRowVisible(sample, clone);
        form->setRowVisible(type, tool == QLatin1String("spotheal"));
        form->setRowVisible(blend, tool != QLatin1String("spotheal"));
        note->setText(clone ? QStringLiteral("Alt キーを押しながらクリックして、コピー元を決めます（コピーソース パネルで 5 つまで）。選択範囲があればその中だけ")
                      : tool == QLatin1String("historybrush")
                          ? QStringLiteral("塗り戻す状態は、履歴 パネルの ◆ の行（右クリックで選ぶ。既定は（はじめ））")
                          : QStringLiteral("しみやごみの上をなぞると、まわりの絵から作って埋めます。選択範囲があればその中だけ"));
    };
    for (const char* tool : {"spotheal", "heal", "clone", "historybrush"}) {
        const QString name = QString::fromLatin1(tool);
        connect(tool_actions_.at(name), &QAction::toggled, widget, [show_for, name](bool on) {
            if (on) show_for(name);
        });
    }
    show_for(QStringLiteral("spotheal"));
    ts->add({QStringLiteral("spotheal"), QStringLiteral("heal"), QStringLiteral("clone"), QStringLiteral("historybrush")}, widget);
}

bool MainWindow::retouch_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer) {
    if (!PageCanvas::is_retouch_tool(stroke.tool) || stroke.points.empty()) return false;
    canvas_->stroke_dropped();
    const RetouchSettings& s = retouch_settings;
    // (the pressure does not change the brush, as Photoshop's retouching tools by default: each point at full size)
    Json points = Json::array();
    for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({p.x, p.y}));
    const std::string name = stroke.tool == QLatin1String("spotheal") ? "spot_healing"
                             : stroke.tool == QLatin1String("heal")   ? "healing_brush"
                             : stroke.tool == QLatin1String("clone")  ? "clone_stamp"
                                                                      : "history_brush";
    Json op{{"op", name},           {"page", page.index.json()}, {"layer_id", layer.id}, {"points", points}, {"width_mm", s.size_mm},
            {"hardness", s.hardness}, {"opacity", s.opacity},    {"flow", s.flow},       {"spacing", s.spacing}};
    if (s.blend != "normal" && name != "spot_healing") op["blend"] = s.blend;
    if (const auto area = selection_area()) op["area"] = *area;
    const bool clone = PageCanvas::is_clone_tool(stroke.tool);
    const QPointF first(stroke.points.front().x, stroke.points.front().y);
    CloneSource& slot = clone_sources.now();
    if (name == "spot_healing") {
        op["type"] = s.spot_type;
    } else if (clone) {
        if (!slot.source) {
            flash(QStringLiteral("先にコピー元を決めます（Alt キーを押しながらクリック）"), 4000);
            return true;
        }
        op["source_mm"] = mm_pair(*slot.source);
        op["anchor_mm"] = mm_pair(s.aligned && slot.anchor ? *slot.anchor : first);
        op["aligned"] = s.aligned;
        op["sample"] = s.sample;
        if (slot.width != 100 || slot.height != 100 || slot.rotation != 0 || slot.flip_h || slot.flip_v) {
            op["transform"] = Json{{"scale", Json::array({slot.width, slot.height})}, {"rotation", slot.rotation}, {"flip_h", slot.flip_h}, {"flip_v", slot.flip_v}};
        }
    } else {
        const std::optional<Json> from = history_brush_from(page, layer);
        if (!from) return true;
        op["from"] = *from;
    }
    const bool done = apply_ops(Json::array({op}));
    if (done && clone && s.aligned && !slot.anchor) slot.anchor = first;  // (調整あり: the first stroke pairs the source)
    if (clone) {
        clone_sources_follow();
        clone_overlay_picture();  // (the overlay shows what was painted too)
    }
    return true;
}

// --- the clone source ----------------------------------------------------------------------------------------------------

void MainWindow::clone_source_picked(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    CloneSource& slot = clone_sources.now();
    slot.source = QPointF(x_mm, y_mm);
    slot.anchor.reset();  // (a new source pairs with the next stroke)
    slot.page_id = page != nullptr ? page->id : std::string();
    const core::Layer* layer = target_layer();
    slot.layer_id = layer != nullptr ? layer->id : std::string();
    flash(QStringLiteral("コピー元を決めました（ソース %1: %2, %3 mm）").arg(clone_sources.active + 1).arg(x_mm, 0, 'f', 1).arg(y_mm, 0, 'f', 1), 3000);
    clone_sources_follow();
    clone_overlay_picture();
}

void MainWindow::clone_sources_follow() {
    const CloneSource& slot = clone_sources.now();
    PageCanvas::CloneShown& shown = canvas_->clone_shown;
    shown.source = slot.source;
    shown.anchor = retouch_settings.aligned ? slot.anchor : std::nullopt;
    shown.m = render::retouch::clone_matrix(slot.width, slot.height, slot.rotation, slot.flip_h, slot.flip_v);
    shown.overlay = clone_sources.overlay;
    shown.opacity = clone_sources.opacity / 100.0;
    shown.clipped = clone_sources.clipped;
    shown.auto_hide = clone_sources.auto_hide;
    shown.blend = QString::fromStdString(clone_sources.blend);
    if (shown.invert != clone_sources.invert && !shown.picture.isNull()) shown.picture.invertPixels(QImage::InvertRgb);
    shown.invert = clone_sources.invert;
    if (clone_source_panel_ != nullptr) clone_source_panel_->refresh();
    canvas_->update();
}

void MainWindow::clone_overlay_picture() {
    // the picture of what the clone tools sample — the layer drawn on, or the page as it shows — for the overlay (drawn on
    // a worker, as the move tool's)
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr || !clone_sources.overlay) return;
    const int dpi = std::max(24, std::min(150, static_cast<int>(std::nearbyint(canvas_->scale() * 25.4))));
    const auto doc = session_->snapshot();
    const std::size_t index = static_cast<std::size_t>(page_index_);
    const std::string layer_id = layer->id;
    const bool whole = retouch_settings.sample != "current";
    const bool invert = clone_sources.invert;
    const std::uint64_t ticket = ++clone_ticket_;
    auto* watcher = new QFutureWatcher<std::pair<QImage, QRectF>>(this);
    connect(watcher, &QFutureWatcher<std::pair<QImage, QRectF>>::finished, this, [this, watcher, ticket, invert] {
        auto [image, where] = watcher->result();
        watcher->deleteLater();
        if (ticket != clone_ticket_ || image.isNull()) return;
        if (invert != clone_sources.invert) image.invertPixels(QImage::InvertRgb);
        canvas_->clone_shown.picture = image;
        canvas_->clone_shown.picture_mm = where;
        canvas_->update();
    });
    watcher->setFuture(QtConcurrent::run([doc, index, layer_id, dpi, whole, invert]() -> std::pair<QImage, QRectF> {
        try {
            const core::Page& p = doc->page(index);
            render::Image drawn;
            if (whole) {
                render::RenderOptions options = render::proof_options();
                options.onion = false;
                drawn = render::render_page(p, dpi, options, doc.get()).image;
            } else {
                for (const core::Layer& l : p.layers) {
                    if (l.id == layer_id) drawn = render::layer_image(p, l, dpi, doc.get(), true);
                }
            }
            if (drawn.empty()) return {};
            const render::Image rgba = drawn.mode() == "RGBA" ? drawn : drawn.convert("RGBA");
            const std::string bytes = rgba.tobytes();
            QImage q = QImage(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888).copy();
            if (invert) q.invertPixels(QImage::InvertRgb);
            const double mm = 25.4 / dpi;
            return {q, QRectF(0, 0, rgba.width() * mm, rgba.height() * mm)};
        } catch (const std::exception&) {
        }
        return {};
    }));
}

void MainWindow::build_clone_source_dock() {
    clone_source_panel_ = new CloneSourcePanel(this);
    clone_source_dock_ = new QDockWidget(QStringLiteral("コピーソース"), this);
    clone_source_dock_->setObjectName(QStringLiteral("コピーソース"));
    clone_source_dock_->setWidget(clone_source_panel_);
    clone_source_dock_->setMinimumWidth(200);
    clone_source_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, clone_source_dock_);
    clone_source_dock_->hide();  // (for retouching only: 表示 → コピーソース brings it)
    view_menu_->addAction(clone_source_dock_->toggleViewAction());
    connect(clone_source_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown && clone_source_panel_ != nullptr) clone_source_panel_->refresh();
    });
}

// --- the History Brush's source ----------------------------------------------------------------------------------------

std::optional<Json> MainWindow::history_brush_from(const core::Page& page, const core::Layer& layer) {
    if (!session_->path()) {
        flash(QStringLiteral("ヒストリーブラシの前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        return std::nullopt;
    }
    const int row = history_brush_row.value_or(0);
    const std::optional<DocPtr> state = session_->state_at(static_cast<std::size_t>(std::max(row, 0)));
    if (!state || !*state) {
        flash(QStringLiteral("履歴の ◆ の状態は、もう原稿に残っていません。履歴 パネルの右クリックで選び直します"), 6000, true);
        return std::nullopt;
    }
    const core::Document& was = **state;
    HistoryFrom& known = history_froms()[this];
    if (known.state == *state && known.page_id == page.id && known.layer_id == layer.id && !known.from.is_null()) return known.from;
    const core::Page* then = nullptr;
    for (std::size_t i = 0; i < was.pages.size(); ++i) {
        if (!was.is_deferred(i) && was.page(i).id == page.id) then = &was.page(i);
    }
    const core::Layer* old = nullptr;
    if (then != nullptr) {
        for (const core::Layer& l : then->layers) {
            if (l.id == layer.id) old = &l;
        }
    }
    if (old == nullptr) {
        flash(QStringLiteral("履歴の ◆ の状態に、このレイヤーはありません（後で作ったレイヤーです）"), 5000);
        return std::nullopt;
    }
    using Kind = render::image_ops::Pixels::Kind;
    // (the kind the op works the layer as: a precise layer's — precise pen lines become precise pixels —, an ink page's
    // inks, else 8 bits)
    const Kind kind = layer.color_raster || (!page.color && core::has_color_strokes(layer)) ? Kind::Precise
                      : page.color && !layer.raster_png                                    ? Kind::Inks
                                                                                            : Kind::Eight;
    const bool marks = old->stroke_count() > 0 || !old->patches.empty();
    try {
        storage::AssetStore store(*session_->path());
        std::string bytes;
        if (kind == Kind::Precise && old->color_raster && !marks) {
            bytes = *old->color_raster;
        } else if (kind == Kind::Inks && old->ink_raster && !old->raster_png && !marks) {
            bytes = *old->ink_raster;
        } else if (kind == Kind::Eight && old->raster_png && !old->raster_png->empty() && !marks) {
            bytes = *old->raster_png;
        } else {
            const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(was, *then, *old);
            if (p.kind() != kind) {
                flash(QStringLiteral("履歴の ◆ の状態のこのレイヤーは、今と種類が違うので塗り戻せません"), 5000);
                return std::nullopt;
            }
            bytes = kind == Kind::Precise ? render::image_ops::to_precise(p)
                    : kind == Kind::Inks  ? render::image_ops::to_inks(p)
                                          : render::write_png(render::image_ops::to_image(p));
        }
        Json from;
        if (kind == Kind::Precise) {  // (in tiles, as the book saves them: a layer of any size)
            std::vector<std::string> refs;
            for (const std::string& tile : core::color_tiles(bytes)) refs.push_back(store.put_bytes(tile, core::kColorRasterSuffix));
            from = core::tiled_metadata(core::ColorRasterView(bytes), refs, core::color_raster_space(page.rgb));
        } else {
            from = store.put_bytes(bytes, kind == Kind::Inks ? std::string(core::kInkRasterSuffix) : std::string(".png"));
        }
        known = HistoryFrom{*state, page.id, layer.id, from};
        return from;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return std::nullopt;
    }
}

}  // namespace genko::app
