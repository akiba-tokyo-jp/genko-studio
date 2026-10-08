// The drawing tools in the window (M3④-2, Python's main.py: act_picker … act_liquify, act_fill_gaps, act_swap_colour,
// act_transparent, act_thicker / act_thinner, the tool settings of 図形・色混ぜ・ゆがみ・グラデーション, the カラー panel
// and what each tool's drawing becomes: _on_colour_picked, _fill_at, _lasso_filled, _shape_drawn, _gradient,
// _fill_gaps, _nudge_brush, layer_colour_at, and the 透明色 / 色混ぜ / ゆがみ lines of _on_stroke).

#include <QComboBox>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

#include "app/brush_panel.hpp"
#include "app/colours.hpp"
#include "app/fields.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/pynum.hpp"
#include "render/page.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

namespace {

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::role(label, "hint");
    return label;
}

QFormLayout* form_on(QWidget* page) {
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return form;
}

}  // namespace

void MainWindow::build_paint_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const char* name, const QList<QKeySequence>& keys, const QString& tip) {
        const QString key = QString::fromLatin1(name);
        QAction* act = make(QString::fromLatin1(attribute), title, [this, key] { choose_tool(key); }, keys, tip, true);
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[key] = act;
        return act;
    };
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    tool("act_picker", QStringLiteral("スポイト"), "picker", key("I"), QStringLiteral("クリックした所の色をペンの色にします"));
    tool("act_fill", QStringLiteral("塗りつぶし"), "fill", key("G"), QStringLiteral("線で囲まれた所をクリックで塗ります（隙間閉じ・見る範囲はブラシ パネルで）"));
    tool("act_lassofill", QStringLiteral("囲って塗る"), "lassofill", key("Shift+G"), QStringLiteral("ドラッグで囲んだ所を塗ります"));
    tool("act_gradient", QStringLiteral("グラデーション"), "gradient", key("U"), QStringLiteral("ドラッグの向きに色をなめらかに変えて塗る（選択範囲があればその中だけ）"));
    tool("act_shape", QStringLiteral("図形"), "shape", key("O"),
         QStringLiteral("直線・折れ線・曲線・長方形・楕円・多角形を描く（Shift で 45° と正方形。折れ線と曲線はクリックで点、ダブルクリックか Enter で終わり）"));
    tool("act_blend", QStringLiteral("色混ぜ"), "blend", key("Shift+B"), QStringLiteral("ペイントのレイヤーの色をぼかす・指先でのばす・なじませる"));
    tool("act_liquify", QStringLiteral("ゆがみ（指で押す）"), "liquify", key("Shift+L"), QStringLiteral("なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く"));
    make("act_point_thinner", QStringLiteral("選んだ点を細く"), [this] { point_width(0.8); }, key("Ctrl+Alt+["),
         QStringLiteral("線の編集で選んだ制御点のところだけ、線を細くします"));
    make("act_fill_gaps", QStringLiteral("塗り残しを塗る"), [this] { fill_gaps(); }, {},
         QStringLiteral("塗った色の間に残った小さなすき間を、同じ色で塗ります。選択範囲があればその中を、無ければなぞった所を"));
    make("act_swap_colour", QStringLiteral("メインとサブの色を入れ替える"), [this] { colours_->swap(); }, key("X"));
    QAction* transparent = make("act_transparent", QStringLiteral("透明色で描く"), [] {}, {}, QStringLiteral("ペンで描いた所が消える（もう一度で戻る）"), true);
    connect(transparent, &QAction::toggled, this, [this](bool on) {
        if (colours_ != nullptr) colours_->transparent->setChecked(on);
    });
    make("act_thicker", QStringLiteral("太く（ペン・消しゴム）"), [this] { nudge_brush(1); }, key("]"));
    make("act_thinner", QStringLiteral("細く（ペン・消しゴム）"), [this] { nudge_brush(-1); }, key("["));
}

void MainWindow::build_paint_pages(ToolSettings* ts) {
    // 図形
    shape_kind_ = new QComboBox;
    shape_kind_->setObjectName(QStringLiteral("shape_kind"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("直線"), QStringLiteral("line")}, {QStringLiteral("折れ線"), QStringLiteral("polyline")},
                                     {QStringLiteral("曲線"), QStringLiteral("curve")}, {QStringLiteral("長方形"), QStringLiteral("rect")},
                                     {QStringLiteral("楕円"), QStringLiteral("ellipse")}, {QStringLiteral("多角形"), QStringLiteral("polygon")}})
        shape_kind_->addItem(label, key);
    connect(shape_kind_, &QComboBox::activated, this, [this](int) { canvas_->shape_kind = shape_kind_->currentData().toString(); });
    shape_style_ = new QComboBox;
    shape_style_->setObjectName(QStringLiteral("shape_style"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("線"), QStringLiteral("line")}, {QStringLiteral("塗り"), QStringLiteral("fill")},
                                     {QStringLiteral("線と塗り"), QStringLiteral("both")}})
        shape_style_->addItem(label, key);
    shape_sides_ = new QSpinBox;
    shape_sides_->setObjectName(QStringLiteral("shape_sides"));
    shape_sides_->setRange(3, 24);
    shape_sides_->setValue(5);
    connect(shape_sides_, &QSpinBox::valueChanged, this, [this](int v) { canvas_->shape_sides = v; });
    shape_radius_ = new QDoubleSpinBox;
    shape_radius_->setObjectName(QStringLiteral("shape_radius"));
    shape_radius_->setRange(0, 100);
    shape_radius_->setSuffix(QStringLiteral(" mm"));
    auto* shape_page = new QWidget;
    QFormLayout* shl = form_on(shape_page);
    shl->addRow(QStringLiteral("形"), shape_kind_);
    shl->addRow(QStringLiteral("描き方"), shape_style_);
    shl->addRow(QStringLiteral("多角形の角"), shape_sides_);
    shl->addRow(QStringLiteral("長方形の角の丸み"), shape_radius_);
    shl->addRow(hint(QStringLiteral("線の太さと色はペンと同じ。Shift で 45° と正方形。折れ線・曲線はクリックで点を置き、"
                                    "ダブルクリックか Enter で終わり（Shift+Enter で閉じる）。")));
    ts->add({QStringLiteral("shape")}, shape_page);
    // 色混ぜ
    blend_mode_ = new QComboBox;
    blend_mode_->setObjectName(QStringLiteral("blend_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("ぼかし"), QStringLiteral("blur")}, {QStringLiteral("指先（色をのばす）"), QStringLiteral("smudge")},
                                     {QStringLiteral("なじませ"), QStringLiteral("blend")}})
        blend_mode_->addItem(label, key);
    blend_strength_ = new QSpinBox;
    blend_strength_->setObjectName(QStringLiteral("blend_strength"));
    blend_strength_->setRange(5, 100);
    blend_strength_->setSuffix(QStringLiteral(" %"));
    blend_strength_->setValue(60);
    auto* blend_size = new QDoubleSpinBox;
    blend_size->setObjectName(QStringLiteral("blend_size"));
    blend_size->setRange(0.5, 60);
    blend_size->setSuffix(QStringLiteral(" mm"));
    blend_size->setValue(6.0);
    connect(blend_size, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->blend_mm = v; });
    auto* blend_page = new QWidget;
    QFormLayout* bfl = form_on(blend_page);
    bfl->addRow(QStringLiteral("混ぜ方"), blend_mode_);
    bfl->addRow(QStringLiteral("強さ"), blend_strength_);
    bfl->addRow(QStringLiteral("大きさ"), blend_size);
    bfl->addRow(hint(QStringLiteral("ペイントのレイヤーの色を混ぜます（ペンの線は線のまま）。")));
    ts->add({QStringLiteral("blend")}, blend_page);
    // ゆがみ
    liquify_mode_ = new QComboBox;
    liquify_mode_->setObjectName(QStringLiteral("liquify_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("押し流す"), QStringLiteral("push")}, {QStringLiteral("縮める"), QStringLiteral("pinch")},
                                     {QStringLiteral("ふくらませる"), QStringLiteral("bloat")}, {QStringLiteral("右に渦"), QStringLiteral("twirl_cw")},
                                     {QStringLiteral("左に渦"), QStringLiteral("twirl_ccw")}})
        liquify_mode_->addItem(label, key);
    liquify_strength_ = new QSpinBox;
    liquify_strength_->setObjectName(QStringLiteral("liquify_strength"));
    liquify_strength_->setRange(5, 100);
    liquify_strength_->setSuffix(QStringLiteral(" %"));
    liquify_strength_->setValue(60);
    auto* liquify_size = new QDoubleSpinBox;
    liquify_size->setObjectName(QStringLiteral("liquify_size"));
    liquify_size->setRange(0.5, 80);
    liquify_size->setSuffix(QStringLiteral(" mm"));
    liquify_size->setValue(10.0);
    connect(liquify_size, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->blend_mm = v; });
    auto* liquify_page = new QWidget;
    QFormLayout* lfl = form_on(liquify_page);
    lfl->addRow(QStringLiteral("動かし方"), liquify_mode_);
    lfl->addRow(QStringLiteral("強さ"), liquify_strength_);
    lfl->addRow(QStringLiteral("大きさ"), liquify_size);
    lfl->addRow(hint(QStringLiteral("ペンの線は点が動き、線のまま残ります。")));
    ts->add({QStringLiteral("liquify")}, liquify_page);
    // グラデーション
    gradient_mode_ = new QComboBox;
    gradient_mode_->setObjectName(QStringLiteral("gradient_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("ペンの色 → 透明"), QStringLiteral("fade")}, {QStringLiteral("ペンの色 → 白"), QStringLiteral("white")},
                                     {QStringLiteral("黒 → 白"), QStringLiteral("bw")}, {QStringLiteral("円（中心からペンの色 → 透明）"), QStringLiteral("radial")}})
        gradient_mode_->addItem(label, key);
    ts->add({QStringLiteral("gradient")}, action_page({QStringLiteral("色の変わり方"), static_cast<QWidget*>(gradient_mode_)}));
}

void MainWindow::build_colour_dock() {
    colours_ = new ColourPanel(brush_);
    connect(colours_->transparent, &QCheckBox::toggled, this, [this](bool on) {
        if (QAction* act = action("act_transparent"); act->isChecked() != on) act->setChecked(on);
    });
    connect(colours_, &ColourPanel::pick_source_chosen, this, [this](const QString& source) { canvas_->pick_source = source; });
    auto* dock = new QDockWidget(QStringLiteral("カラー"), this);
    dock->setObjectName(QStringLiteral("カラー"));
    auto* scroll = new QScrollArea;  // (a tall panel scrolls on a small screen instead of making the window taller)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(colours_);
    dock->setWidget(scroll);
    dock->setMinimumWidth(200);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(pages_dock_, dock);
    view_menu_->addAction(dock->toggleViewAction());
}

// --- what the tools' drawings become ---------------------------------------------------------------------------------

Json MainWindow::paint_fields() const {
    Json out{{"rgb", pen_.rgb}};
    if (pen_.opacity < 1) out["opacity"] = core::py_round(pen_.opacity, 3);
    return out;
}

std::optional<std::array<int, 3>> MainWindow::layer_colour_at(double x_mm, double y_mm) const {
    // the colour of the layer being drawn on at this point (the eyedropper set to the layer)
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return std::nullopt;
    constexpr int dpi = 100;
    try {
        const render::Image image = render::layer_image(*page, *layer, dpi, &book());
        const auto x = core::py_round_int(x_mm / 25.4 * dpi), y = core::py_round_int(y_mm / 25.4 * dpi);
        if (!(0 <= x && x < image.width() && 0 <= y && y < image.height())) return std::nullopt;
        const std::vector<double> px = image.getpixel(static_cast<int>(x), static_cast<int>(y));
        if (px.size() < 4 || px[3] <= 20) return std::nullopt;
        return std::array<int, 3>{static_cast<int>(px[0]), static_cast<int>(px[1]), static_cast<int>(px[2])};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void MainWindow::colour_picked(const std::array<int, 3>& rgb) {
    brush_->set_colour(rgb);
    flash(QStringLiteral("色を拾いました (%1, %2, %3)").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]), 2000);
}

void MainWindow::fill_at(double x_mm, double y_mm) {
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    Json op{{"op", "fill"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"x_mm", core::py_round(x_mm, 2)},
            {"y_mm", core::py_round(y_mm, 2)}};
    op.update(brush_->fill_fields());
    op.update(paint_fields());
    apply_ops(Json::array({op}));
}

void MainWindow::lasso_filled(const QVector<QPointF>& points) {
    // 囲って塗る, as the brush panel says: the shape, only the closed areas inside it, or the gaps along it
    Json pts = Json::array();
    for (const QPointF& p : points) pts.push_back(Json::array({p.x(), p.y()}));
    const QString mode = brush_->lasso_mode->currentData().toString();
    if (mode == QLatin1String("shape")) {
        const core::Layer* layer = paint_layer();
        if (layer == nullptr) return;
        Json op{{"op", "fill_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", Json{{"poly", pts}}}};
        op.update(paint_fields());
        apply_ops(Json::array({op}));
        return;
    }
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    const core::Page* page = current_page();
    if (mode == QLatin1String("enclosed")) {
        Json op{{"op", "fill_enclosed"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"poly", pts}};
        op.update(brush_->fill_fields());
        op.update(paint_fields());
        apply_ops(Json::array({op}));
        return;
    }
    std::vector<std::array<double, 2>> line;
    for (const QPointF& p : points) line.push_back({p.x(), p.y()});
    if (const auto area = render::selection::stroke_area(line, std::max(1.0, brush_->size->value() * 2))) {
        Json op{{"op", "fill_gaps"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", *area}, {"max_mm", brush_->gap_size->value()}};
        op.update(paint_fields());
        apply_ops(Json::array({op}));
    }
}

void MainWindow::shape_drawn(const Json& shape) {
    const core::Layer* layer = paint_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr) return;
    const QString how = shape_style_->currentData().toString();
    Json op{{"op", "add_shape"}, {"page", page->index.json()}, {"layer_id", layer->id}};
    for (const auto& [key, value] : shape.items()) op[key] = value;
    op["line"] = how == QLatin1String("line") || how == QLatin1String("both");
    op["fill"] = how == QLatin1String("fill") || how == QLatin1String("both");
    op["width_mm"] = canvas_->brush_width_mm;
    op["rgb"] = pen_.rgb;
    if (shape.value("shape", std::string()) == "rect" && shape_radius_->value() != 0.0) op["radius_mm"] = shape_radius_->value();
    apply_ops(Json::array({op}));
}

void MainWindow::gradient(const QPointF& from, const QPointF& to) {
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? paint_layer() : nullptr;
    if (page == nullptr || layer == nullptr) return;
    const Json rgb = pen_.rgb;
    const QString mode = gradient_mode_->currentData().toString();
    Json op{{"op", "gradient_fill"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"from", Json::array({from.x(), from.y()})},
            {"to", Json::array({to.x(), to.y()})}};
    if (mode == QLatin1String("white")) {
        op["rgb_from"] = rgb;
        op["rgb_to"] = Json::array({255, 255, 255});
    } else if (mode == QLatin1String("bw")) {
        op["rgb_from"] = Json::array({20, 20, 20});
        op["rgb_to"] = Json::array({255, 255, 255});
    } else {
        op["rgb_from"] = rgb;
        op["opacity_to"] = 0.0;
        op["shape"] = mode == QLatin1String("radial") ? "radial" : "linear";
    }
    if (const auto area = selection_area()) op["area"] = *area;
    apply_ops(Json::array({op}));
}

void MainWindow::fill_gaps() {
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? paint_layer() : nullptr;
    if (layer == nullptr || page == nullptr) return;
    const auto area = selection_area();
    if (!area) {  // (no range chosen: the tool that traces over the gaps, not the whole layer at once)
        brush_->lasso_mode->setCurrentIndex(std::max(0, brush_->lasso_mode->findData(QStringLiteral("gaps"))));
        choose_tool(QStringLiteral("lassofill"));
        flash(QStringLiteral("塗り残しの所をなぞると、そこだけ塗ります（囲って塗る・なぞった所の塗り残し）。"
                             "レイヤー全体なら、先にすべて選択（Ctrl+A）してから"),
              6000);
        return;
    }
    apply_ops(Json::array({Json{{"op", "fill_gaps"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"max_mm", brush_->gap_size->value()},
                                {"area", *area}}}));
}

void MainWindow::nudge_brush(int step) {
    static const std::vector<double> sizes{0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0, 15.0, 20.0};
    if (canvas_->tool() == QLatin1String("eraser")) {
        std::size_t i = 0;  // (the nearest size; the first of two as near)
        for (std::size_t k = 1; k < sizes.size(); ++k)
            if (std::abs(sizes[k] - eraser_mm_) < std::abs(sizes[i] - eraser_mm_)) i = k;
        const auto at = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(i) + step, 0, static_cast<std::ptrdiff_t>(sizes.size()) - 1);
        eraser_mm_ = sizes[static_cast<std::size_t>(at)];
        canvas_->eraser_mm = eraser_mm_;
        {
            const QSignalBlocker quiet(eraser_size_);
            eraser_size_->setValue(eraser_mm_);
        }
        flash(QStringLiteral("消しゴムの太さ %1 mm").arg(QString::fromStdString(core::py_format_g(eraser_mm_))), 2000);
        canvas_->update();
        return;
    }
    const double width = brush_->nudge_size(step);
    canvas_->brush_width_mm = width;
    canvas_->update();
    flash(QStringLiteral("ペンの太さ %1 mm").arg(QString::fromStdString(core::py_format_g(width))), 2000);
}

bool MainWindow::paint_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer) {
    // 透明色 (the pen takes away where it passes), ゆがみ and 色混ぜ: not lines of their own
    Json flat = Json::array();
    for (const core::PenPoint& p : stroke.points) flat.push_back(Json::array({p.x, p.y}));
    if (stroke.tool == QLatin1String("pen") && colours_ != nullptr && colours_->transparent->isChecked()) {
        canvas_->stroke_dropped();
        apply_ops(Json::array({Json{{"op", "erase"}, {"page", page.index.json()}, {"layer_id", layer.id}, {"points", flat},
                                    {"width_mm", std::max(0.3, brush_->size->value())}}}));
        return true;
    }
    if (stroke.tool == QLatin1String("liquify")) {
        canvas_->stroke_dropped();
        apply_ops(Json::array({Json{{"op", "liquify"}, {"page", page.index.json()}, {"layer_id", layer.id}, {"points", flat},
                                    {"width_mm", canvas_->blend_mm}, {"strength", liquify_strength_->value() / 100.0},
                                    {"mode", liquify_mode_->currentData().toString().toStdString()}}}));
        return true;
    }
    if (stroke.tool == QLatin1String("blend")) {
        Json points = Json::array();
        for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({p.x, p.y, p.p.value_or(0.7)}));
        canvas_->stroke_dropped();
        apply_ops(Json::array({Json{{"op", "smudge"}, {"page", page.index.json()}, {"layer_id", layer.id}, {"points", points},
                                    {"width_mm", canvas_->blend_mm}, {"strength", blend_strength_->value() / 100.0},
                                    {"mode", blend_mode_->currentData().toString().toStdString()}}}));
        return true;
    }
    return false;
}

}  // namespace genko::app
