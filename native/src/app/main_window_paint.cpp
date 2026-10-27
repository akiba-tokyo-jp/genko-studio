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
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/brush_panel.hpp"
#include "app/colours.hpp"
#include "app/fields.hpp"
#include "app/gradients_panel.hpp"
#include "app/patterns_panel.hpp"
#include "app/layer_dialogs.hpp"
#include "app/config.hpp"
#include "app/ask.hpp"
#include "app/preferences.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "core/pynum.hpp"
#include "service/wording.hpp"
#include "render/color_canvas.hpp"
#include "render/gradient_presets.hpp"
#include "render/image_ops.hpp"
#include "render/ink.hpp"
#include "render/ink_edit.hpp"
#include "render/page.hpp"
#include "render/path_raster.hpp"
#include "render/raster.hpp"
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

// (PC4 E4) the blends as Photoshop's モード lists them (wording.BLEND's words), the one kept in the settings chosen
QComboBox* blend_choice(const char* name, const QString& key) {
    auto* box = new QComboBox;
    box->setObjectName(QString::fromLatin1(name));
    for (const auto& [mode, label] : blend_modes()) box->addItem(label, QString::fromStdString(mode));
    box->setCurrentIndex(std::max(0, box->findData(settings()->value(key, QStringLiteral("normal")).toString())));
    QObject::connect(box, &QComboBox::activated, box, [box, key](int) { settings()->setValue(key, box->currentData()); });
    return box;
}

QSpinBox* percent_box(const char* name, const QString& key, int lo, int value) {
    auto* box = new QSpinBox;
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, 100);
    box->setSuffix(QStringLiteral(" %"));
    box->setValue(std::clamp(settings()->value(key, value).toInt(), lo, 100));
    QObject::connect(box, &QSpinBox::valueChanged, box, [key](int v) { settings()->setValue(key, v); });
    return box;
}

QCheckBox* kept_check(const char* name, const QString& text, const QString& key, bool value) {
    auto* box = new QCheckBox(text);
    box->setObjectName(QString::fromLatin1(name));
    box->setChecked(settings()->value(key, value ? QStringLiteral("true") : QStringLiteral("false")).toString() == QLatin1String("true"));
    QObject::connect(box, &QCheckBox::toggled, box, [key](bool on) { settings()->setValue(key, on ? QStringLiteral("true") : QStringLiteral("false")); });
    return box;
}

QComboBox* kept_choice(const char* name, const std::vector<std::pair<QString, QVariant>>& items, const QString& key, const QVariant& value) {
    auto* box = new QComboBox;
    box->setObjectName(QString::fromLatin1(name));
    for (const auto& [label, data] : items) box->addItem(label, data);
    const QString kept = settings()->value(key, value).toString();  // (the settings give back text)
    for (int i = 0; i < box->count(); ++i)
        if (box->itemData(i).toString() == kept) box->setCurrentIndex(i);
    QObject::connect(box, &QComboBox::activated, box, [box, key](int) { settings()->setValue(key, box->currentData()); });
    return box;
}

std::array<int, 3> eight(const std::array<double, 3>& c) {
    std::array<int, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) out[k] = static_cast<int>(std::lround(std::clamp(std::isfinite(c[k]) ? c[k] : 0.0, 0.0, 1.0) * 255.0));
    return out;
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
    // (PC4 E-0, the user's decision 2: スポイトツール, 塗りつぶしツール, グラデーションツール, 描画色と背景色 by Photoshop's
    // names — their ids and keys kept, preferences::renamed())
    tool("act_picker", QStringLiteral("スポイトツール"), "picker", key("I"), QStringLiteral("クリックした所の色を描画色にします（Alt＋クリックで背景色）"));
    // (PC4 E1: Photoshop's 鉛筆ツール — a hard line without anti-aliasing, drawn with its own dab brush; in the brush tools'
    // group, its key that of the group: none of its own)
    tool("act_pencil", QStringLiteral("鉛筆ツール"), "pencil", {},
         QStringLiteral("縁のくっきりした（アンチエイリアスのない）線を描きます。自動消去で、描画色の所から描き始めると背景色で描きます"));
    // (PC4 E1, view.show.brushPreview: the brush's outline on the page as 環境設定 › ペンのカーソル says, or the cross alone)
    QAction* preview = make("act_brush_preview", QStringLiteral("ブラシプレビュー"), [] {}, {},
                            QStringLiteral("描く道具のポインターに、ブラシの大きさ・形を見せます（オフ: 十字だけ）"), true);
    preview->setChecked(preferences::brush_preview());
    connect(preview, &QAction::toggled, this, [this](bool on) {
        preferences::set_brush_preview(on);
        canvas_->brush_preview = on;
        canvas_->refresh_cursor();
        canvas_->update();
    });
    canvas_->brush_preview = preview->isChecked();
    tool("act_fill", QStringLiteral("塗りつぶしツール"), "fill", key("G"),
         QStringLiteral("クリックした所を塗ります（基準は 線で囲まれた所・色の近さ。ツールの設定で）"));
    tool("act_lassofill", QStringLiteral("囲って塗る"), "lassofill", key("Shift+G"), QStringLiteral("ドラッグで囲んだ所を塗ります"));
    tool("act_gradient", QStringLiteral("グラデーションツール"), "gradient", key("U"), QStringLiteral("ドラッグの向きに色をなめらかに変えて塗る（選択範囲があればその中だけ）"));
    // (PC3 C-b, the user's decision 5: the 図形 of before is the shape tools' 長方形ツール — the others: main_window_shapes.cpp)
    tool("act_shape", QStringLiteral("長方形ツール"), "shape", key("O"),
         QStringLiteral("ドラッグで長方形のシェイプを描きます（Shift で正方形、Alt で中心から。モードがピクセルなら直線・折れ線・曲線なども）"));
    tool("act_blend", QStringLiteral("色混ぜ"), "blend", key("Shift+B"),
         QStringLiteral("ペイントのレイヤーの色をぼかす・指先でのばす・なじませる（Photoshop のぼかしツール・指先ツールは別の道具）"));
    tool("act_liquify", QStringLiteral("前方ワープツール"), "liquify", key("Shift+L"), QStringLiteral("なぞった所の絵と線を押し流す・縮める・ふくらませる・渦を巻く"));
    make("act_point_thinner", QStringLiteral("選んだ点を細く"), [this] { point_width(0.8); }, key("Ctrl+Alt+["),
         QStringLiteral("部分選択ツールで選んだ制御点のところだけ、線を細くします"));
    make("act_fill_gaps", QStringLiteral("塗り残しを塗る"), [this] { fill_gaps(); }, {},
         QStringLiteral("塗った色の間に残った小さなすき間を、同じ色で塗ります。選択範囲があればその中を、無ければなぞった所を"));
    make("act_swap_colour", QStringLiteral("描画色と背景色を入れ替え"), [this] { colours_->swap(); }, key("X"));
    make("act_default_colours", QStringLiteral("描画色と背景色を初期設定に戻す"), [this] { colours_->reset_defaults(); }, key("D"),
         QStringLiteral("描画色を黒に、背景色を白にします"));
    QAction* transparent = make("act_transparent", QStringLiteral("透明色で描く"), [] {}, {}, QStringLiteral("ブラシで描いた所が消える（もう一度で戻る）"), true);
    connect(transparent, &QAction::toggled, this, [this](bool on) {
        if (colours_ != nullptr) colours_->transparent->setChecked(on);
    });
    make("act_thicker", QStringLiteral("ブラシサイズを大きく"), [this] { nudge_brush(1); }, key("]"));
    make("act_thinner", QStringLiteral("ブラシサイズを小さく"), [this] { nudge_brush(-1); }, key("["));
}

void MainWindow::build_paint_pages(ToolSettings* ts) {
    // 図形
    shape_kind_ = new QComboBox;
    shape_kind_->setObjectName(QStringLiteral("shape_kind"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("直線"), QStringLiteral("line")}, {QStringLiteral("折れ線"), QStringLiteral("polyline")},
                                     {QStringLiteral("曲線"), QStringLiteral("curve")}, {QStringLiteral("長方形"), QStringLiteral("rect")},
                                     {QStringLiteral("楕円"), QStringLiteral("ellipse")}, {QStringLiteral("多角形"), QStringLiteral("polygon")},
                                     {QStringLiteral("三角形"), QStringLiteral("triangle")}, {QStringLiteral("カスタムシェイプ"), QStringLiteral("custom")}})
        shape_kind_->addItem(label, key);
    connect(shape_kind_, &QComboBox::activated, this, [this](int) {
        // (a figure of a shape tool takes that tool: PC3 C-b; 折れ線・曲線 stay the 図形's own)
        const QString kind = shape_kind_->currentData().toString();
        if (const QString tool = shape_tool_of(kind); !tool.isEmpty()) {
            choose_tool(tool);
        } else {
            canvas_->shape_kind = kind;
        }
    });
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
    shl->addRow(QStringLiteral("描き方（ピクセル）"), shape_style_);
    shl->addRow(QStringLiteral("多角形の角"), shape_sides_);
    shl->addRow(QStringLiteral("長方形の角の丸み"), shape_radius_);
    shl->addRow(hint(QStringLiteral("ピクセルでは線の太さと色はブラシツールと同じ。Shift で 45° と正方形、Alt で中心から。折れ線・曲線はクリックで点を置き、"
                                    "ダブルクリックか Enter で終わり（Shift+Enter で閉じる）。")));
    build_shape_pages(ts, shl);  // (PC3 C-b: モード, 塗り, 線…, and the pen tools' モード: main_window_shapes.cpp)
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
    // (PC4 E4: Photoshop's options bars of the グラデーションツール, the 塗りつぶしツール and the スポイトツール)
    build_gradient_page(ts);
    build_fill_page(ts);
    build_picker_page(ts);
}

void MainWindow::build_gradient_page(ToolSettings* ts) {
    if (gradients_ == nullptr) gradients_ = new GradientLibrary(this);
    gradient_preset_ = new QComboBox;
    gradient_preset_->setObjectName(QStringLiteral("gradient_preset"));
    gradient_preset_->setIconSize(QSize(64, 12));
    gradient_preset_->setToolTip(QStringLiteral("グラデーションツールで塗るグラデーション（グラデーション パネルのプリセットから）"));
    connect(gradient_preset_, &QComboBox::activated, this, [this](int) {
        const QPoint at = gradient_preset_->currentData().toPoint();
        const auto& groups = gradients_->groups();
        if (at.x() < 0 || static_cast<std::size_t>(at.x()) >= groups.size() || at.y() < 0 ||
            static_cast<std::size_t>(at.y()) >= groups[static_cast<std::size_t>(at.x())].items.size())
            return;
        gradients_->set_current(groups[static_cast<std::size_t>(at.x())].items[static_cast<std::size_t>(at.y())]);
    });
    connect(gradients_, &GradientLibrary::changed, this, [this] { fill_gradient_presets(); });
    connect(gradients_, &GradientLibrary::current_changed, this, [this] { fill_gradient_presets(); });
    auto* edit = new QPushButton(QStringLiteral("編集…"));
    edit->setObjectName(QStringLiteral("gradient_edit"));
    edit->setToolTip(QStringLiteral("今のグラデーションをグラデーションエディターで直します"));
    connect(edit, &QPushButton::clicked, this, [this] {
        const auto fg = brush_->rgb();
        const auto bg = colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255};
        GradientEditor editor(gradients_->current(), fg, bg, this);
        if (ask::exec(&editor) != QDialog::Accepted) return;
        try {
            gradients_->set_current(editor.item());
        } catch (const std::exception& error) {
            flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        }
    });
    gradient_shape_ = kept_choice("gradient_shape",
                                  {{QStringLiteral("線形"), QStringLiteral("linear")}, {QStringLiteral("円形"), QStringLiteral("radial")},
                                   {QStringLiteral("円錐形"), QStringLiteral("angle")}, {QStringLiteral("反射形"), QStringLiteral("reflected")},
                                   {QStringLiteral("菱形"), QStringLiteral("diamond")}, {QStringLiteral("楕円"), QStringLiteral("ellipse")}},
                                  QStringLiteral("gradient/shape"), QStringLiteral("linear"));
    gradient_blend_ = blend_choice("gradient_blend", QStringLiteral("gradient/blend"));
    connect(gradient_blend_, &QComboBox::activated, this, [this](int) { pen_changed(); });  // (its badge: review P3-2)
    gradient_opacity_ = percent_box("gradient_opacity", QStringLiteral("gradient/opacity"), 1, 100);
    gradient_reverse_ = kept_check("gradient_reverse", QStringLiteral("逆方向"), QStringLiteral("gradient/reverse"), false);
    gradient_dither_ = kept_check("gradient_dither", QStringLiteral("ディザ"), QStringLiteral("gradient/dither"), true);
    gradient_dither_->setToolTip(QStringLiteral("8 bit のレイヤーで、色の段（バンディング）が見えないように細かいノイズを混ぜます"));
    gradient_transparency_ = kept_check("gradient_transparency", QStringLiteral("透明部分"), QStringLiteral("gradient/transparency"), true);
    gradient_transparency_->setToolTip(QStringLiteral("グラデーションの不透明度の分岐点を使います（オフ: 不透明に塗る）"));
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("gradient_settings"));
    QFormLayout* form = form_on(page);
    auto* chooser = new QHBoxLayout;
    chooser->addWidget(gradient_preset_, 1);
    chooser->addWidget(edit);
    form->addRow(QStringLiteral("グラデーション"), chooser);
    form->addRow(QStringLiteral("形"), gradient_shape_);
    form->addRow(QStringLiteral("モード"), gradient_blend_);
    form->addRow(QStringLiteral("不透明度"), gradient_opacity_);
    form->addRow(gradient_reverse_);
    form->addRow(gradient_dither_);
    form->addRow(gradient_transparency_);
    form->addRow(hint(QStringLiteral("線は線のまま、その下に塗ります（8 bit のペイントのレイヤーとペンのレイヤーには絵として置き、高精度・インキのレイヤーは画素に）。"
                                     "モードを通常のほかにすると、ペイントのレイヤーの線は画素になります。描画色・背景色の分岐点は今の色で塗ります。"
                                     "16・32 bit のページでも、コマで切るレイヤー（新しいペイントのレイヤーの既定）は 8 bit の値で塗ります"
                                     "（高精度にするには、レイヤーの「コマの外にもはみ出す」を入れる）")));
    fill_gradient_presets();
    ts->add({QStringLiteral("gradient")}, page);
}

void MainWindow::fill_gradient_presets() {
    if (gradient_preset_ == nullptr || gradients_ == nullptr) return;
    const QSignalBlocker quiet(gradient_preset_);
    gradient_preset_->clear();
    const auto fg = brush_ != nullptr ? brush_->rgb() : std::array<int, 3>{20, 20, 20};
    const auto bg = colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255};
    const core::Json& now = gradients_->current();
    // (the first row: the gradient the tool paints with; then the library's presets, by their groups)
    gradient_preset_->addItem(QIcon(gradient_strip(now, QSize(64, 12), fg, bg)), QString::fromStdString(now.value("name", std::string())), QPoint(-1, -1));
    gradient_preset_->insertSeparator(1);
    const auto& groups = gradients_->groups();
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (std::size_t i = 0; i < groups[g].items.size(); ++i) {
            const core::Json& item = groups[g].items[i];
            gradient_preset_->addItem(QIcon(gradient_strip(item, QSize(64, 12), fg, bg)),
                                      QStringLiteral("%1 / %2").arg(QString::fromStdString(groups[g].name), QString::fromStdString(item.value("name", std::string()))),
                                      QPoint(static_cast<int>(g), static_cast<int>(i)));
        }
    }
    gradient_preset_->setCurrentIndex(0);
}

void MainWindow::refresh_gradient_strips() {
    if (gradient_preset_ == nullptr || gradients_ == nullptr) return;
    const auto fg = brush_ != nullptr ? brush_->rgb() : std::array<int, 3>{20, 20, 20};
    const auto bg = colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255};
    const auto& groups = gradients_->groups();
    for (int i = 0; i < gradient_preset_->count(); ++i) {
        const QVariant data = gradient_preset_->itemData(i);
        if (!data.isValid()) continue;  // (the separator)
        const QPoint at = data.toPoint();
        const core::Json* item = nullptr;
        if (at.x() < 0) {
            item = &gradients_->current();
        } else if (static_cast<std::size_t>(at.x()) < groups.size() && at.y() >= 0 &&
                   static_cast<std::size_t>(at.y()) < groups[static_cast<std::size_t>(at.x())].items.size()) {
            item = &groups[static_cast<std::size_t>(at.x())].items[static_cast<std::size_t>(at.y())];
        }
        if (item != nullptr && render::gradient_presets::uses_brush_colours(*item)) gradient_preset_->setItemIcon(i, QIcon(gradient_strip(*item, QSize(64, 12), fg, bg)));
    }
}

void MainWindow::build_fill_page(ToolSettings* ts) {
    fill_basis_ = kept_choice("fill_basis", {{QStringLiteral("線で囲まれた所"), QStringLiteral("lines")}, {QStringLiteral("色の近さ"), QStringLiteral("color")}},
                              QStringLiteral("fill/basis"), QStringLiteral("lines"));
    fill_basis_->setToolTip(QStringLiteral("線で囲まれた所: 線を壁にして、その中を塗る（隙間は閉じる）。色の近さ: クリックした所の色に近い所を塗る（Photoshop の塗りつぶしツール）"));
    fill_source_ = kept_choice("fill_source", {{QStringLiteral("描画色"), QStringLiteral("foreground")}, {QStringLiteral("パターン"), QStringLiteral("pattern")}},
                               QStringLiteral("fill/source"), QStringLiteral("foreground"));
    // (パターン, since E3's merge: the chosen pattern — the パターン panel's, Photoshop's 今のパターン — laid where it fills, its
    // fill op's `pattern`; a library pattern the book does not keep put in first by the same step)
    fill_source_->setToolTip(QStringLiteral("描画色: 描画色で塗る。パターン: 選んだパターン（パターン パネルと同じ）を敷き詰めて塗る"));
    fill_pattern_row_ = new QWidget;
    fill_pattern_row_->setObjectName(QStringLiteral("fill_pattern_row"));
    auto* pattern_row = new QHBoxLayout(fill_pattern_row_);
    pattern_row->setContentsMargins(0, 0, 0, 0);
    connect(fill_source_, &QComboBox::activated, this, [this](int) { refresh_fill_pattern(); });
    fill_blend_ = blend_choice("fill_blend", QStringLiteral("fill/blend"));
    connect(fill_blend_, &QComboBox::activated, this, [this](int) { pen_changed(); });  // (its badge: review P3-2)
    fill_blend_->setToolTip(QStringLiteral("描画モード: 通常のほかは、ペイントのレイヤーの画素に塗ります"));
    fill_opacity_ = percent_box("fill_opacity", QStringLiteral("fill/opacity"), 1, 100);
    fill_tolerance_ = new QSpinBox;
    fill_tolerance_->setObjectName(QStringLiteral("fill_tolerance"));
    fill_tolerance_->setRange(0, 255);
    fill_tolerance_->setValue(std::clamp(settings()->value(QStringLiteral("fill/color_tolerance"), 32).toInt(), 0, 255));
    fill_tolerance_->setToolTip(QStringLiteral("許容値: クリックした所の色からどれだけ違う色まで塗るか（0〜255）"));
    connect(fill_tolerance_, &QSpinBox::valueChanged, this, [](int v) { settings()->setValue(QStringLiteral("fill/color_tolerance"), v); });
    fill_antialias_ = kept_check("fill_antialias", QStringLiteral("アンチエイリアス"), QStringLiteral("fill/antialias"), true);
    fill_contiguous_ = kept_check("fill_contiguous", QStringLiteral("隣接"), QStringLiteral("fill/contiguous"), true);
    fill_contiguous_->setToolTip(QStringLiteral("オン: クリックした所につながった所だけ。オフ: 離れた所の近い色も"));
    fill_all_layers_ = kept_check("fill_all_layers", QStringLiteral("全レイヤー"), QStringLiteral("fill/all_layers"), false);
    fill_all_layers_->setToolTip(QStringLiteral("オン: 見えている全部の色を見る。オフ: 描く先のレイヤーの色だけを見る"));
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("fill_settings"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* top = new QWidget;
    QFormLayout* form = form_on(top);
    form->addRow(QStringLiteral("基準"), fill_basis_);
    form->addRow(QStringLiteral("塗りつぶし"), fill_source_);
    form->addRow(QStringLiteral("パターン"), fill_pattern_row_);
    form->addRow(QStringLiteral("モード"), fill_blend_);
    form->addRow(QStringLiteral("不透明度"), fill_opacity_);
    auto* by_colour = new QWidget;  // ("near" is a macro on Windows)
    by_colour->setObjectName(QStringLiteral("fill_color_section"));
    QFormLayout* near_form = form_on(by_colour);
    near_form->addRow(QStringLiteral("許容値"), fill_tolerance_);
    near_form->addRow(fill_antialias_);
    near_form->addRow(fill_contiguous_);
    near_form->addRow(fill_all_layers_);
    layout->addWidget(top);
    layout->addWidget(by_colour);
    if (brush_ != nullptr && brush_->fill_section != nullptr) layout->addWidget(brush_->fill_section);  // (隙間を閉じる … 囲って塗る)
    layout->addWidget(hint(QStringLiteral("色の近さで塗るときは、許容値・隣接・全レイヤーを見ます。線で囲まれた所で塗るときは、隙間を閉じる・見る範囲・色の誤差を見ます")));
    layout->addStretch(1);
    const auto follow = [this, by_colour] { by_colour->setEnabled(fill_basis_->currentData().toString() == QLatin1String("color")); };
    connect(fill_basis_, &QComboBox::activated, this, follow);
    follow();
    refresh_fill_pattern();
    ts->add({QStringLiteral("fill"), QStringLiteral("lassofill")}, page);
}

void MainWindow::refresh_fill_pattern() {
    if (fill_pattern_row_ == nullptr || fill_source_ == nullptr) return;
    const bool on = fill_source_->currentData().toString() == QLatin1String("pattern");
    fill_pattern_row_->setEnabled(on);
    // (the list of the book's and the library's patterns as they are now, the chosen one shown)
    // (the one before taken out at once — never from its own signal: the 塗りつぶし box's and choose_tool call this)
    delete fill_pattern_;
    fill_pattern_ = nullptr;
    fill_pattern_ = pattern_combo(book(), current_pattern());
    fill_pattern_->setObjectName(QStringLiteral("fill_pattern"));
    // (as narrow as the page: its names cut short in the box, whole in its list)
    fill_pattern_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fill_pattern_->setMinimumContentsLength(6);
    fill_pattern_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    fill_pattern_->setToolTip(QStringLiteral("塗るパターン（パターン パネルで選んだものと同じ）"));
    connect(fill_pattern_, &QComboBox::activated, this, [this](int) {
        if (fill_pattern_ != nullptr) set_current_pattern(fill_pattern_->currentData().toString());
    });
    fill_pattern_row_->layout()->addWidget(fill_pattern_);
}

std::optional<Json> MainWindow::fill_pattern_ops(Json& before) {
    before = Json::array();
    if (fill_source_ == nullptr || fill_source_->currentData().toString() != QLatin1String("pattern")) return std::nullopt;
    // (PC4 E3's pattern, as 塗りつぶし… lays it: 100 %, 0°, from the page's corner)
    auto spec = pattern_fill_of(current_pattern(), 100.0, 0.0, true, before);
    if (!spec) flash(QStringLiteral("塗るパターンがありません（パターン パネルで選びます）"), 4000, true);
    return spec;
}

void MainWindow::build_picker_page(ToolSettings* ts) {
    pick_size_ = kept_choice("picker_size",
                             {{QStringLiteral("指定したピクセル"), 1}, {QStringLiteral("3×3 ピクセル 平均"), 3}, {QStringLiteral("5×5 ピクセル 平均"), 5},
                              {QStringLiteral("11×11 ピクセル 平均"), 11}, {QStringLiteral("31×31 ピクセル 平均"), 31},
                              {QStringLiteral("51×51 ピクセル 平均"), 51}, {QStringLiteral("101×101 ピクセル 平均"), 101}},
                             QStringLiteral("picker/size"), 1);
    pick_size_->setToolTip(QStringLiteral("サンプル範囲: クリックした所の 1 ピクセルか、まわりの平均の色を拾います（200 dpi の画素で）"));
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("picker_settings"));
    pick_samples_ = new QVBoxLayout(page);
    pick_samples_->setContentsMargins(0, 0, 0, 0);
    auto* size_label = new QLabel(QStringLiteral("サンプル範囲"));
    pick_samples_->addWidget(size_label);
    pick_samples_->addWidget(pick_size_);
    pick_samples_->addWidget(new QLabel(QStringLiteral("サンプル")));  // (the カラー panel's choice comes here: build_colour_dock)
    pick_samples_->addWidget(hint(QStringLiteral("Alt＋クリックで背景色に。16・32 bit のページでは、8 bit より細かい値で拾います")));
    pick_samples_->addStretch(1);
    ts->add({QStringLiteral("picker")}, page);
}

void MainWindow::build_colour_dock() {
    colours_ = new ColourPanel(brush_);
    connect(colours_->transparent, &QCheckBox::toggled, this, [this](bool on) {
        if (QAction* act = action("act_transparent"); act->isChecked() != on) act->setChecked(on);
    });
    connect(colours_, &ColourPanel::pick_source_chosen, this, [this](const QString& source) { canvas_->pick_source = source; });
    connect(colours_, &ColourPanel::inks_chosen, this, [this](const std::vector<double>& ink) { choose_inks(ink, false); });
    // (PC4 E4: スポイトツール's サンプル on its page, after its サンプル範囲; the gradients' strips follow the colours)
    if (pick_samples_ != nullptr) pick_samples_->insertWidget(3, colours_->pick_source);
    const auto colours_now = [this] {
        if (gradients_panel_ != nullptr) gradients_panel_->set_colours(brush_->rgb(), colours_->sub_rgb());
        refresh_gradient_strips();  // (review P3-5: those of the 描画色 and 背景色 only)
    };
    connect(brush_, &BrushPanel::colour_set, this, colours_now);
    connect(colours_, &ColourPanel::background_set, this, colours_now);
    connect(colours_, &ColourPanel::background_set, this, [this] { pen_changed(); });  // (PC4 E2: a dab brush's 背景色 as it is drawn)
    auto* dock = new QDockWidget(QStringLiteral("カラー"), this);
    dock->setObjectName(QStringLiteral("カラー"));
    auto* scroll = new QScrollArea;  // (a tall panel scrolls on a small screen instead of making the window taller)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    dock->setWidget(scroll);  // (in the dock first, then the panel in it: styled once — build_tool_settings)
    scroll->setWidget(colours_);
    dock->setMinimumWidth(200);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(pages_dock_, dock);
    QAction* shown = dock->toggleViewAction();
    shown->setShortcut(QKeySequence(QStringLiteral("F6")));  // (PC4 E-0: Photoshop's カラー)
    view_menu_->addAction(shown);
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

void MainWindow::colour_picked(const std::array<int, 3>& seen) {
    const core::Page* page = current_page();
    // PC4 E4 (tool:Eyedropper): サンプル範囲 (an average), サンプル 現在のレイヤー以下, a precise page's floats, Alt+click the 背景色
    const int size = pick_size_ != nullptr ? std::max(1, pick_size_->currentData().toInt()) : 1;
    const QString source = canvas_->pick_source;
    const bool background = canvas_->last_pick_background;
    const bool precise = page != nullptr && !page->color && page->rgb && page->rgb->depth != 8;
    std::array<int, 3> rgb = seen;
    std::optional<std::array<double, 3>> floats;
    if (page != nullptr && canvas_->last_pick && (size > 1 || source == QLatin1String("below") || precise)) {
        floats = sampled_colour(*page, canvas_->last_pick->first, source, size);
        if (!floats) return;  // (nothing shows there)
        rgb = eight(*floats);
    }
    if (background) {
        if (precise && floats) colours_->set_background_precise(*floats);
        else colours_->set_background(rgb);
        flash(QStringLiteral("背景色を拾いました (%1, %2, %3)").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]), 2000);
        return;
    }
    if (precise && floats) {
        colours_->choose_precise(*floats);
        flash(QStringLiteral("色を拾いました (%1, %2, %3)").arg((*floats)[0], 0, 'f', 4).arg((*floats)[1], 0, 'f', 4).arg((*floats)[2], 0, 'f', 4), 2000);
        return;
    }
    // color-mode@1: on a CMYK or Lab page the eyedropper takes the inks there; when the page's rules would put other
    // inks on the colour it saw, the colour goes into the page's table with them (set_inks)
    if (page != nullptr && page->color && canvas_->last_pick && size == 1 && source != QLatin1String("below")) {
        const auto [mm, from_layer] = *canvas_->last_pick;
        try {
            std::optional<std::vector<double>> ink;
            if (from_layer) {
                if (const core::Layer* layer = target_layer()) ink = render::ink_edit::layer_inks_at(*page, *layer, mm.x(), mm.y());
            } else {
                ink = render::ink_edit::inks_at(*page, &book(), mm.x(), mm.y());
            }
            if (ink) {
                choose_inks(*ink, true);
                return;
            }
        } catch (const std::exception& error) {
            flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
            return;
        }
    }
    brush_->set_colour(rgb);
    flash(QStringLiteral("色を拾いました (%1, %2, %3)").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]), 2000);
}

std::optional<std::array<double, 3>> MainWindow::sampled_colour(const core::Page& page, const QPointF& mm, const QString& source, int size) const {
    namespace io = render::image_ops;
    try {
        const core::Layer* layer = target_layer();
        const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
        const int r = size / 2;
        // (the box of `size` pixels around the place, on a grid of width × height over the page; none off it)
        const auto box_on = [&](int width, int height) -> std::optional<render::Box> {
            const int cx = static_cast<int>(std::floor(mm.x() / w_mm * width)), cy = static_cast<int>(std::floor(mm.y() / h_mm * height));
            if (cx < 0 || cy < 0 || cx >= width || cy >= height) return std::nullopt;
            return render::Box{std::max(0, cx - r), std::max(0, cy - r), std::min(width, cx + r + 1), std::min(height, cy + r + 1)};
        };
        std::vector<io::Rgba> px;  // (straight colours 0..1 and their alphas)
        bool page_values = false;  // (px in the page's values: an RGB page of a profile's, turned to sRGB after)
        // (pixels of the box as straight colours: an ink part's as the colours its inks mean)
        const auto take = [&](const io::Pixels& part) {
            if (part.kind() == io::Pixels::Kind::Inks) {
                px = io::rgba_of(part);
                return;
            }
            page_values = true;
            for (std::size_t i = 0; i < part.size(); ++i) {
                const float* v = part.at(i);
                px.push_back(io::Rgba{v[0], v[1], v[2], part.colours() == 3 ? v[3] : 1.0f});
            }
        };
        if (source == QLatin1String("layer")) {
            if (layer == nullptr) return std::nullopt;
            const bool lines = layer->stroke_count() > 0 || !layer->patches.empty() || (layer->shapes && !layer->shapes->items.empty());
            const bool precise_layer = layer->color_raster && !page.color && !lines;
            const bool ink_layer = page.color && !layer->raster_png && !layer->color_raster && !lines;
            if (ink_layer && !layer->ink_raster) return std::nullopt;  // (nothing on it)
            if (precise_layer || ink_layer) {
                // (review P2-3: its pixels in the box read alone — a 600 dpi precise layer never copied whole)
                int width = 0, height = 0;
                if (precise_layer) {
                    const core::ColorRasterView view(*layer->color_raster);
                    width = static_cast<int>(view.width());
                    height = static_cast<int>(view.height());
                } else {
                    const core::InkRasterHeader head = core::ink_raster_header(*layer->ink_raster);
                    width = static_cast<int>(head.width);
                    height = static_cast<int>(head.height);
                }
                const auto box = box_on(width, height);
                if (!box) return std::nullopt;
                std::vector<std::size_t> wanted;
                for (int y = box->y0; y < box->y1; ++y)
                    for (int x = box->x0; x < box->x1; ++x) wanted.push_back(std::size_t(y) * std::size_t(width) + std::size_t(x));
                const auto got = render::raster::layer_colours_at(page, *layer, wanted);  // (sRGB 0..255 and the alpha)
                for (const std::size_t i : wanted) {
                    const auto& c = got.at(i);
                    px.push_back(io::Rgba{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3]});
                }
            } else {
                // (an 8-bit layer, or one with lines: its picture at the working 200 dpi, as the layer shows alone)
                const io::Pixels p = io::shown_layer_pixels(book(), page, *layer);
                const auto box = box_on(static_cast<int>(p.width()), static_cast<int>(p.height()));
                if (!box) return std::nullopt;
                io::Pixels part(p.kind(), static_cast<std::uint32_t>(box->width()), static_cast<std::uint32_t>(box->height()), p.colours());
                part.floating = p.floating;
                part.ink = p.ink;
                for (int y = box->y0; y < box->y1; ++y)
                    for (int x = box->x0; x < box->x1; ++x)
                        std::copy_n(p.at(std::size_t(y) * p.width() + std::size_t(x)), p.all(),
                                    part.at(std::size_t(y - box->y0) * part.width() + std::size_t(x - box->x0)));
                take(part);
            }
        } else {
            // (review P2-3: the page — or the layers up to the one drawn on — drawn in the box alone)
            const core::Page* drawn = &page;
            core::Page below;
            if (source == QLatin1String("below") && layer != nullptr) {
                below = page;  // (the layers up to the one drawn on: those above it left out)
                for (std::size_t i = 0; i < below.layers.size(); ++i) {
                    if (below.layers[i].id == layer->id) {
                        below.layers.resize(i + 1);
                        break;
                    }
                }
                drawn = &below;
            }
            const auto box = box_on(render::mm_to_px(w_mm, render::raster::kWorkingDpi), render::mm_to_px(h_mm, render::raster::kWorkingDpi));
            if (!box) return std::nullopt;
            take(io::page_pixels(book(), *drawn, *box));
        }
        double sum[3] = {0, 0, 0}, weight = 0;
        for (const io::Rgba& c : px) {
            if (!(c[3] > 0)) continue;
            for (std::size_t k = 0; k < 3; ++k) sum[k] += static_cast<double>(c[k]) * c[3];
            weight += c[3];
        }
        if (!(weight > 0.08 * static_cast<double>(px.size()))) return std::nullopt;  // (all but clear there)
        std::array<double, 3> out{sum[0] / weight, sum[1] / weight, sum[2] / weight};
        if (page_values) {
            const render::RgbSpace space = render::RgbSpace::of(page);
            if (!space.is_srgb()) space.to_srgb(out.data(), 1);  // (the page's values as sRGB)
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void MainWindow::choose_inks(const std::vector<double>& ink, bool picked) {
    const core::Page* page = current_page();
    if (page == nullptr || !page->color) return;
    const core::PageColor& color = *page->color;
    // (the pages of this mode and profile, whose tables take the pair: a colour none of them holds for other inks)
    std::vector<const core::Page*> alike;
    core::PageColor tables = color;
    for (const auto& other : book().pages) {
        if (!other->color || other->color->space != color.space || other->color->profile != color.profile) continue;
        alike.push_back(other.get());
        if (other.get() != page) tables.inks.insert(tables.inks.end(), other->color->inks.begin(), other->color->inks.end());
    }
    std::array<int, 3> rgb{};
    try {
        rgb = render::ink::rgb_for_inks(tables, ink);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return;
    }
    if (!render::ink::same_inks(color, render::ink::ink_of(color, rgb), ink)) {
        // (the pair in the table of every page of this mode and profile, so the colour prints so on each of them)
        Json values = Json::array();
        for (const double v : ink) values.push_back(core::py_round(v, color.space == core::InkSpace::Lab ? 3 : 5));
        const Json pair = Json::array({Json::array({Json::array({rgb[0], rgb[1], rgb[2]}), values})});
        Json ops = Json::array();
        for (const core::Page* other : alike) ops.push_back(Json{{"op", "set_inks"}, {"page", other->index.json()}, {"inks", pair}});
        if (!apply_ops(ops)) {  // (a book or page that cannot be changed: the colour still, its inks by the rules)
            brush_->set_colour(rgb);
            return;
        }
    }
    brush_->set_colour(rgb);
    QStringList said;
    const bool percent = color.space == core::InkSpace::Cmyk || color.space == core::InkSpace::Gray;  // (grey: K, color-model@1)
    const char* names[] = {"C", "M", "Y", "K"};
    const char* lab[] = {"L", "a", "b"};
    for (std::size_t i = 0; i < ink.size(); ++i) {
        const char* name = color.space == core::InkSpace::Gray ? "K" : percent ? names[i] : lab[i];
        said << (percent ? QStringLiteral("%1 %2%").arg(QString::fromLatin1(name)).arg(ink[i] * 100.0, 0, 'f', 1)
                         : QStringLiteral("%1 %2").arg(QString::fromLatin1(name)).arg(ink[i], 0, 'f', 1));
    }
    flash((picked ? QStringLiteral("色を拾いました（%1）") : QStringLiteral("色を選びました（%1）")).arg(said.join(QStringLiteral(" "))), 2000);
}

Json MainWindow::fill_tool_fields(bool blend) const {
    Json out{{"rgb", pen_.rgb}};
    if (fill_opacity_ != nullptr && fill_opacity_->value() < 100) out["opacity"] = fill_opacity_->value() / 100.0;
    if (blend && fill_blend_ != nullptr && fill_blend_->currentData().toString() != QLatin1String("normal")) {
        out["blend"] = fill_blend_->currentData().toString().toStdString();
    }
    return out;
}

void MainWindow::fill_at(double x_mm, double y_mm) {
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    const core::Page* page = current_page();
    // (パターン: the chosen pattern, a library one put into the book first by the same step)
    Json ops = Json::array();
    const bool by_pattern = fill_source_ != nullptr && fill_source_->currentData().toString() == QLatin1String("pattern");
    const std::optional<Json> pattern = fill_pattern_ops(ops);
    if (by_pattern && !pattern) return;
    Json op;
    if (fill_basis_ != nullptr && fill_basis_->currentData().toString() == QLatin1String("color")) {
        // (PC4 E4, 色の近さ: Photoshop's paint bucket — the area of colours near the one clicked, PC3 B1's {"color"})
        Json nearness{{"x_mm", core::py_round(x_mm, 2)}, {"y_mm", core::py_round(y_mm, 2)}, {"tolerance", fill_tolerance_->value()},
                      {"contiguous", fill_contiguous_->isChecked()}, {"antialias", fill_antialias_->isChecked()}};
        if (!fill_all_layers_->isChecked()) nearness["layer"] = layer->id;
        op = Json{{"op", "fill_area"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", Json{{"color", nearness}}}};
    } else {
        op = Json{{"op", "fill"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"x_mm", core::py_round(x_mm, 2)}, {"y_mm", core::py_round(y_mm, 2)}};
        op.update(brush_->fill_fields());
        // (a 描画色 of more than 8 bits on a page of 16 or 32 bits: its floats)
        if (colours_ != nullptr && colours_->is_precise() && !page->color && page->rgb && page->rgb->depth != 8) {
            op["color_rgb"] = colours_->foreground_json(page->rgb->depth == 32 ? "f32" : "u16");
        }
    }
    op.update(fill_tool_fields(layer->kind == core::LayerKind::Raster));  // (review P3-2: a pen layer's fill in 通常)
    if (pattern) {  // (the pattern instead of a colour)
        op.erase("rgb");
        op.erase("color_rgb");
        op["pattern"] = *pattern;
    }
    if (op.contains("blend") && layer->kind == core::LayerKind::Raster && channel_view().target.empty() &&
        !blend_bakes_ok(*page, *layer, QStringLiteral("塗りつぶしツール")))
        return;
    ops.push_back(std::move(op));
    apply_ops(ops);
}

void MainWindow::lasso_filled(const QVector<QPointF>& points) {
    // 囲って塗る, as the brush panel says: the shape, only the closed areas inside it, or the gaps along it
    Json pts = Json::array();
    for (const QPointF& p : points) pts.push_back(Json::array({p.x(), p.y()}));
    const QString mode = brush_->lasso_mode->currentData().toString();
    if (mode == QLatin1String("shape")) {
        const core::Layer* layer = paint_layer();
        if (layer == nullptr) return;
        Json ops = Json::array();
        const bool by_pattern = fill_source_ != nullptr && fill_source_->currentData().toString() == QLatin1String("pattern");
        const std::optional<Json> pattern = fill_pattern_ops(ops);
        if (by_pattern && !pattern) return;
        Json op{{"op", "fill_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", Json{{"poly", pts}}}};
        op.update(fill_tool_fields(layer->kind == core::LayerKind::Raster));  // (PC4 E4: the page's モード and 不透明度)
        if (pattern) {
            op.erase("rgb");
            op["pattern"] = *pattern;
        }
        if (op.contains("blend") && layer->kind == core::LayerKind::Raster && channel_view().target.empty() &&
            !blend_bakes_ok(*current_page(), *layer, QStringLiteral("囲って塗る")))
            return;
        ops.push_back(std::move(op));
        apply_ops(ops);
        return;
    }
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    const core::Page* page = current_page();
    if (mode == QLatin1String("enclosed")) {
        Json op{{"op", "fill_enclosed"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"poly", pts}};
        op.update(brush_->fill_fields());
        op.update(fill_tool_fields(false));
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
    // (PC3 C-b: the shape tools' シェイプ and パス make a shape part or subpaths — main_window_shapes.cpp —, ピクセル the
    // 図形 of before)
    const QString mode = shape_mode_ != nullptr ? shape_mode_->currentData().toString() : QStringLiteral("pixel");
    if (mode != QLatin1String("pixel")) {
        shape_part_drawn(shape, mode);
        return;
    }
    const core::Layer* layer = paint_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr) return;
    const QString how = shape_style_->currentData().toString();
    const std::string kind = shape.value("shape", std::string());
    Json figures = Json::array({shape});
    if (kind == "triangle") {
        figures[0]["shape"] = "polygon";
        figures[0]["sides"] = 3;
    } else if (kind == "custom" || (kind == "polygon" && shape_star_ != nullptr && shape_star_->value() < 100)) {
        // (the custom shape and the star: their outlines as closed polylines, one a subpath)
        const auto path = figure_path(shape);
        if (!path) return;
        figures = Json::array();
        for (const auto& line : render::path_raster::flatten_path(*path, 0.05)) {
            Json points = Json::array();
            for (const auto& [x, y] : line.pts) points.push_back(Json::array({core::py_round(x, 3), core::py_round(y, 3)}));
            if (points.size() >= 3) figures.push_back(Json{{"shape", "polyline"}, {"points", points}, {"closed", true}});
        }
    }
    Json ops = Json::array();
    for (const Json& figure : figures) {
        Json op{{"op", "add_shape"}, {"page", page->index.json()}, {"layer_id", layer->id}};
        for (const auto& [key, value] : figure.items()) {
            if (key != "keep_aspect") op[key] = value;
        }
        op["line"] = how == QLatin1String("line") || how == QLatin1String("both");
        op["fill"] = how == QLatin1String("fill") || how == QLatin1String("both");
        op["width_mm"] = canvas_->brush_width_mm;
        op["rgb"] = pen_.rgb;
        if (figure.value("shape", std::string()) == "rect" && shape_radius_->value() != 0.0) op["radius_mm"] = shape_radius_->value();
        ops.push_back(op);
    }
    if (!ops.empty()) apply_ops(ops);
}

void MainWindow::gradient(const QPointF& from, const QPointF& to) {
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? paint_layer() : nullptr;
    if (page == nullptr || layer == nullptr || gradients_ == nullptr) return;
    // (PC4 E4, tool:Gradient: the current gradient — its 描画色 and 背景色 now, a precise page's floats —, the 形, モード,
    // 不透明度, 逆方向, ディザ and 透明部分; a paint layer's pixels take it, a pen layer a picture)
    const bool precise = colours_ != nullptr && !page->color && page->rgb && page->rgb->depth != 8;
    const std::string precision = page->rgb && page->rgb->depth == 32 ? "f32" : "u16";
    const auto rgb_json = [](const std::array<int, 3>& c) { return Json::array({c[0], c[1], c[2]}); };
    const Json fg = precise ? colours_->foreground_json(precision) : rgb_json(brush_->rgb());
    const Json bg = precise ? colours_->background_json(precision)
                            : rgb_json(colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255});
    const Json stops = render::gradient_presets::stops_for(gradients_->current(), fg, bg, gradient_transparency_->isChecked());
    Json op{{"op", "gradient_fill"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"from", Json::array({from.x(), from.y()})},
            {"to", Json::array({to.x(), to.y()})}, {"stops", stops["stops"]}, {"shape", gradient_shape_->currentData().toString().toStdString()},
            {"dither", gradient_dither_->isChecked()}};
    if (stops.contains("transparency")) op["transparency"] = stops["transparency"];
    if (gradient_reverse_->isChecked()) op["reverse"] = true;
    if (layer->kind == core::LayerKind::Raster && gradient_blend_->currentData().toString() != QLatin1String("normal"))
        op["blend"] = gradient_blend_->currentData().toString().toStdString();  // (review P3-2: a pen layer's picture in 通常)
    if (gradient_opacity_->value() < 100) op["opacity"] = gradient_opacity_->value() / 100.0;
    if (const auto area = selection_area()) op["area"] = *area;
    // (a paint layer's lines and pictures become its pixels: asked once)
    // (a channel, the Quick Mask or a layer mask drawn on: the layer's own lines are not touched)
    // (review P2-1 and the coordinator's decision after it: a paint layer's lines stay lines in 通常 — an 8-bit layer's
    // gradient a picture laid under them, an ink layer's in its inks under them —; another mode makes them its pixels)
    if (op.contains("blend") && layer->kind == core::LayerKind::Raster && channel_view().target.empty() &&
        !blend_bakes_ok(*page, *layer, QStringLiteral("グラデーションツール")))
        return;
    apply_ops(Json::array({op}));
}

void MainWindow::build_gradients_dock() {
    if (gradients_ == nullptr) gradients_ = new GradientLibrary(this);
    gradients_panel_ = new GradientsPanel(gradients_);
    gradients_panel_->set_colours(brush_->rgb(), colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255});
    connect(gradients_panel_, &GradientsPanel::preset_chosen, this, [this](const Json& item) { gradient_chosen(item); });
    connect(gradients_panel_, &GradientsPanel::apply_requested, this, [this](const Json& item) { gradient_layer(item); });
    connect(gradients_panel_, &GradientsPanel::said, this, [this](const QString& words) { flash(words, 5000); });
    auto* dock = new QDockWidget(QStringLiteral("グラデーション"), this);
    dock->setObjectName(QStringLiteral("グラデーション"));
    dock->setWidget(gradients_panel_);
    dock->setMinimumWidth(200);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(layers_dock_, dock);
    dock->hide();  // (brought by ウィンドウ)
    view_menu_->addAction(dock->toggleViewAction());
}

void MainWindow::gradient_chosen(const Json& item) {
    if (gradients_ == nullptr) return;
    gradients_->set_current(item);
    // (Photoshop's: a gradient fill layer chosen takes the preset's colours, its direction and shape kept)
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? target_layer() : nullptr;
    if (layer == nullptr || layer->kind != core::LayerKind::Fill || !layer->fill || !layer->fill->contains("gradient") || layer->locked) return;
    Json g = layer->fill->at("gradient");
    const auto rgb_json = [](const std::array<int, 3>& c) { return Json::array({c[0], c[1], c[2]}); };
    const Json stops = render::gradient_presets::stops_for(item, rgb_json(brush_->rgb()),
                                                           rgb_json(colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255}));
    g["stops"] = stops["stops"];
    if (stops.contains("transparency")) g["transparency"] = stops["transparency"];
    else g.erase("transparency");
    apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"fill", Json{{"gradient", g}}}}}));
}

void MainWindow::gradient_layer(const Json& item) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto rgb_json = [](const std::array<int, 3>& c) { return Json::array({c[0], c[1], c[2]}); };
    const Json stops = render::gradient_presets::stops_for(item, rgb_json(brush_->rgb()),
                                                           rgb_json(colours_ != nullptr ? colours_->sub_rgb() : std::array<int, 3>{255, 255, 255}));
    // (Photoshop's 90°: from the bottom of the page to its top, the first colour at the bottom)
    const double w = page->spec.width_mm.value(), h = page->spec.height_mm.value();
    Json g{{"from", Json::array({core::py_round(w / 2, 3), core::py_round(h, 3)})}, {"to", Json::array({core::py_round(w / 2, 3), 0.0})},
           {"shape", "linear"}, {"stops", stops["stops"]}};
    if (stops.contains("transparency")) g["transparency"] = stops["transparency"];
    apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", page->index.json()}, {"kind", "gradient"}, {"name", item.value("name", std::string("グラデーション"))},
                                {"gradient", g}}}));
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
                             "レイヤー全体なら、先にすべてを選択（Ctrl+A）してから"),
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
        flash(QStringLiteral("ブラシサイズ（消しゴムツール） %1 mm").arg(QString::fromStdString(core::py_format_g(eraser_mm_))), 2000);
        canvas_->update();
        return;
    }
    const double width = brush_->nudge_size(step);
    canvas_->brush_width_mm = width;
    canvas_->update();
    flash(QStringLiteral("ブラシサイズ %1 mm").arg(QString::fromStdString(core::py_format_g(width))), 2000);
}

bool MainWindow::paint_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer) {
    // 透明色 (the pen takes away where it passes), ゆがみ and 色混ぜ: not lines of their own
    Json flat = Json::array();
    for (const core::PenPoint& p : stroke.points) flat.push_back(Json::array({p.x, p.y}));
    if (PageCanvas::draws_line(stroke.tool) && colours_ != nullptr && colours_->transparent->isChecked()) {  // (PC4 E1: the pencil too)
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
