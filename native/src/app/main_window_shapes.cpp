// The shape tools and the shape parts in the window (pen-shapes@1, PC3 C-b, SPEC.md COLOR-07「シェイプ」; Photoshop's
// names, the user's decision 5; the shapes held in pen layers, decision 7): 長方形ツール (O, act_shape: the 図形 of
// before), 楕円形ツール, 三角形ツール, 多角形ツール, ラインツール, カスタムシェイプツール (Shift+O goes round them) — the
// canvas's one tool "shape" in one of its figures —, their tool settings (モード: シェイプ — a shape part on the pen layer
// drawn on, or on a new one —, パス — subpaths of the target path —, ピクセル — the 図形 of before —; 塗り, 線, 不透明度,
// 線の上に置く, 合成, 星の比率, ラインの太さ, カスタムシェイプ), the pen tools' モード, the Shapes panel (ウィンドウ →
// シェイプ), 編集 → カスタムシェイプを定義…, レイヤー → シェイプを結合 ▸ and ラスタライズ → シェイプ, 選択範囲 → シェイプに変換,
// ページ → 台詞 → シェイプに変換 and 表示 → シェイプから新規ガイドを作成. Every change of the book is an op (apply_ops).

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/paths_panel.hpp"
#include "app/shapes_panel.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/config_dir.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "core/vector.hpp"
#include "render/path_raster.hpp"
#include "render/shape_presets.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace vec = core::vector;
namespace presets = render::shape_presets;
namespace pr = render::path_raster;

namespace {

struct ShapeTool {
    const char* attribute;
    const char* title;
    const char* tool;
    const char* kind;
    const char* layer_name;
    const char* tip;
};

// The shape tools in Photoshop's order (長方形ツール is act_shape, made with the drawing tools: main_window_paint.cpp).
const std::vector<ShapeTool>& shape_tools() {
    static const std::vector<ShapeTool> all{
        {"act_shape", "長方形ツール", "shape", "rect", "長方形", ""},
        {"act_shape_ellipse", "楕円形ツール", "shape_ellipse", "ellipse", "楕円形", "ドラッグで楕円のシェイプを描きます（Shift で正円、Alt で中心から）"},
        {"act_shape_triangle", "三角形ツール", "shape_triangle", "triangle", "三角形", "ドラッグで三角形のシェイプを描きます（Shift で縦横が同じ、Alt で中心から）"},
        {"act_shape_polygon", "多角形ツール", "shape_polygon", "polygon", "多角形",
         "ドラッグで多角形・星形のシェイプを描きます（辺の数と星の比率はツールの設定で。Alt で中心から）"},
        {"act_shape_line", "ラインツール", "shape_line", "line", "ライン", "ドラッグで直線のシェイプを描きます（Shift で 45° 刻み、太さはツールの設定で）"},
        {"act_shape_custom", "カスタムシェイプツール", "shape_custom", "custom", "シェイプ",
         "シェイプ パネルで選んだ形をドラッグで描きます（Shift で縦横比を保つ、Alt で中心から）"},
    };
    return all;
}

const ShapeTool* tool_named(const QString& tool) {
    for (const ShapeTool& t : shape_tools())
        if (tool == QLatin1String(t.tool)) return &t;
    return nullptr;
}

QComboBox* combo(const char* name, std::initializer_list<std::pair<const char*, const char*>> items, const char* chosen) {
    auto* box = new QComboBox;
    box->setObjectName(QString::fromLatin1(name));
    for (const auto& [label, key] : items) box->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    box->setCurrentIndex(std::max(0, box->findData(QString::fromLatin1(chosen))));
    return box;
}

Json rgb_json(const std::vector<std::int64_t>& rgb) {
    Json out = Json::array();
    for (const std::int64_t v : rgb) out.push_back(v);
    return out;
}

Json rgb_json(const Rgb& rgb) { return Json::array({rgb[0], rgb[1], rgb[2]}); }

double r3(double v) { return core::py_round(v, 3); }

vec::XY xy_of(const Json& p) { return vec::XY{p[0].get<double>(), p[1].get<double>()}; }

// The target part's layer and id ("shape:<layer>/<part>"), or none.
std::optional<std::pair<std::string, std::string>> target_part(const QString& target) {
    if (!target.startsWith(QLatin1String("shape:"))) return std::nullopt;
    const QString rest = target.mid(6);
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    if (slash < 0) return std::nullopt;
    return std::make_pair(rest.left(slash).toStdString(), rest.mid(slash + 1).toStdString());
}

const vec::ShapePart* part_named(const core::Page& page, const std::string& layer_id, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id != layer_id || !layer.shapes) continue;
        for (const vec::ShapePart& part : layer.shapes->items)
            if (part.id == id) return &part;
    }
    return nullptr;
}

QMenu* submenu(QMenu* in, const QString& title) {
    if (in == nullptr) return nullptr;
    for (QAction* a : in->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

QMenu* top_menu(QMenuBar* bar, const QString& title) {
    for (QAction* a : bar->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

// The action after `after` in the menu (none: at its end).
QAction* next_of(QMenu* menu, QAction* after) {
    const QList<QAction*> items = menu->actions();
    const qsizetype at = items.indexOf(after);
    return at >= 0 && at + 1 < items.size() ? items.at(at + 1) : nullptr;
}

}  // namespace

bool MainWindow::is_shape_tool(const QString& tool) { return tool_named(tool) != nullptr; }

QString MainWindow::shape_tool_of(const QString& kind) {
    for (const ShapeTool& t : shape_tools())
        if (kind == QLatin1String(t.kind)) return QString::fromLatin1(t.tool);
    return QString();  // (折れ線・曲線: the 図形's own figures, no tool of their own)
}

void MainWindow::build_shape_actions() {
    for (const ShapeTool& t : shape_tools()) {
        if (QLatin1String(t.tool) == QLatin1String("shape")) continue;  // (act_shape: main_window_paint.cpp)
        const QString name = QString::fromLatin1(t.tool);
        QAction* act = make(QString::fromLatin1(t.attribute), QString::fromUtf8(t.title), [this, name] { choose_tool(name); }, {},
                            QString::fromUtf8(t.tip), true);
        act->setAutoRepeat(false);
        tools_->addAction(act);
        tool_actions_[name] = act;
    }
    make("act_next_shape_tool", QStringLiteral("次のシェイプツール"),
         [this] {
             QString now = canvas_->tool() == QLatin1String("shape") ? shape_tool_of(canvas_->shape_kind) : QString();
             if (now.isEmpty() && canvas_->tool() != QLatin1String("shape")) {
                 choose_tool(QStringLiteral("shape"));
                 return;
             }
             const auto& all = shape_tools();
             std::size_t at = 0;
             for (std::size_t i = 0; i < all.size(); ++i)
                 if (now == QLatin1String(all[i].tool)) at = (i + 1) % all.size();
             choose_tool(QString::fromLatin1(all[at].tool));
         },
         {QKeySequence(QStringLiteral("Shift+O"))}, QStringLiteral("シェイプツールを順に持ち替えます（長方形・楕円形・三角形・多角形・ライン・カスタムシェイプ）"));
    make("act_define_custom_shape", QStringLiteral("カスタムシェイプを定義…"), [this] { define_custom_shape(); }, {},
         QStringLiteral("パス パネルで選んだパス（またはシェイプ）を、カスタムシェイプのプリセットに加えます"));
    for (const auto& [attribute, title, how, tip] :
         {std::tuple{"act_combine_unite", "シェイプを結合", "combine", "描く先のペンのレイヤーのシェイプを 1 つに合わせます"},
          {"act_combine_subtract", "前面シェイプを削除", "subtract", "前のシェイプの形を、いちばん後ろのシェイプから抜きます"},
          {"act_combine_intersect", "シェイプが重なる領域を交差", "intersect", "シェイプが重なる所だけを残します"},
          {"act_combine_exclude", "重なり合うシェイプ領域を中マド", "exclude", "シェイプが重なる所を抜きます"},
          {"act_merge_components", "シェイプコンポーネントを結合", "merge", "シェイプの中の重なり方（結合・削除・交差・中マド）を、1 つの輪郭にまとめます"}}) {
        const std::string what = how;
        make(QString::fromLatin1(attribute), QString::fromUtf8(title), [this, what] { combine_shapes(what); }, {}, QString::fromUtf8(tip));
    }
    make("act_rasterize_shape", QStringLiteral("シェイプ"), [this] { rasterize_shapes(); }, {},
         QStringLiteral("描く先のペンのレイヤーのシェイプを、画素の塗り（パッチ）にします"));
    make("act_sel_to_shape", QStringLiteral("シェイプに変換"), [this] { selection_to_shape(); }, {},
         QStringLiteral("選択範囲の輪郭を、シェイプ（描く先のペンのレイヤーの形）にします"));
    make("act_line_to_shape", QStringLiteral("シェイプに変換"), [this] { line_to_shape(); }, {},
         QStringLiteral("選んだ台詞の文字の形を、シェイプにします（台詞はそのまま残ります）"));
    make("act_guides_from_shape", QStringLiteral("シェイプから新規ガイドを作成"), [this] { guides_from_shape(); }, {},
         QStringLiteral("選んだシェイプの上下左右の端と中心に、ガイドを置きます"));
}

void MainWindow::build_shape_menus(QMenu* edit, QMenu* view, QMenu* tools, QMenu* layers) {
    // ツール: the other shape tools after the path tools (長方形ツール keeps the 図形's place, before テキスト)
    tools->addSeparator();
    for (const ShapeTool& t : shape_tools()) {
        if (QLatin1String(t.tool) != QLatin1String("shape")) tools->addAction(action(QString::fromLatin1(t.attribute)));
    }
    tools->addAction(action("act_next_shape_tool"));
    // 編集: before すべてを選択
    if (QAction* all = action("act_select_all"); all != nullptr && edit->actions().contains(all)) {
        edit->insertAction(all, action("act_define_custom_shape"));
        edit->insertSeparator(all);
    } else {
        edit->addAction(action("act_define_custom_shape"));
    }
    // レイヤー → シェイプを結合 (after ベクトルマスク), ラスタライズ → シェイプ
    QMenu* combine = new QMenu(QStringLiteral("シェイプを結合"), layers);
    combine->setObjectName(QStringLiteral("menu_combine_shapes"));
    for (const char* name : {"act_combine_unite", "act_combine_subtract", "act_combine_intersect", "act_combine_exclude"})
        combine->addAction(action(QString::fromLatin1(name)));
    combine->addSeparator();
    combine->addAction(action("act_merge_components"));
    QMenu* masks = layers->findChild<QMenu*>(QStringLiteral("menu_vector_mask"));
    layers->insertMenu(masks != nullptr ? next_of(layers, masks->menuAction()) : nullptr, combine);
    if (QMenu* rasterize = layers->findChild<QMenu*>(QStringLiteral("menu_rasterize"))) {
        const QList<QAction*> r = rasterize->actions();
        QAction* vmask = action("act_rasterize_vmask");
        rasterize->insertAction(r.contains(vmask) ? vmask : nullptr, action("act_rasterize_shape"));
    }
    // 選択範囲: after 作業用パスを作成…
    if (QMenu* selection = top_menu(menuBar(), QStringLiteral("選択範囲"))) selection->insertAction(next_of(selection, action("act_work_path")), action("act_sel_to_shape"));
    // ページ → 台詞: after 作業用パスを作成
    if (QMenu* lines = submenu(top_menu(menuBar(), QStringLiteral("ページ")), QStringLiteral("台詞")))
        lines->insertAction(next_of(lines, action("act_line_work_path")), action("act_line_to_shape"));
    // 表示: after ターゲットパス
    view->insertAction(next_of(view, action("act_show_target_path")), action("act_guides_from_shape"));
}

void MainWindow::build_shape_dock() {
    shapes_panel_ = new ShapesPanel(this);
    shapes_dock_ = new QDockWidget(QStringLiteral("シェイプ"), this);
    shapes_dock_->setObjectName(QStringLiteral("シェイプ"));
    shapes_dock_->setWidget(shapes_panel_);
    shapes_dock_->setMinimumWidth(200);
    shapes_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, shapes_dock_);
    tabifyDockWidget(layers_dock_, shapes_dock_);  // (beside the layers, as the paths)
    shapes_dock_->hide();  // (ウィンドウ → シェイプ brings it)
    view_menu_->addAction(shapes_dock_->toggleViewAction());
    connect(shapes_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown && shapes_panel_ != nullptr) shapes_panel_->refresh();
    });
}

// --- the tool settings ---------------------------------------------------------------------------------------------

void MainWindow::build_shape_pages(ToolSettings* ts, QFormLayout* form) {
    const auto store = settings();
    shape_mode_ = combo("shape_mode", {{"シェイプ", "shape"}, {"パス", "path"}, {"ピクセル", "pixel"}},
                        store->value(QStringLiteral("shapes/mode"), QStringLiteral("shape")).toString().toLatin1().constData());
    shape_mode_->setToolTip(QStringLiteral("シェイプ: 形をペンのレイヤーに置く（あとから直せる）／パス: 作業用パスに足す／ピクセル: 線と塗りで描く"));
    connect(shape_mode_, &QComboBox::activated, this, [this](int) { settings()->setValue(QStringLiteral("shapes/mode"), shape_mode_->currentData()); });
    form->insertRow(0, QStringLiteral("モード"), shape_mode_);
    shape_new_layer_ = new QCheckBox(QStringLiteral("新しいレイヤー"));
    shape_new_layer_->setObjectName(QStringLiteral("shape_new_layer"));
    shape_new_layer_->setToolTip(QStringLiteral("シェイプを、描く先の上に作る新しいペンのレイヤーに置きます"));
    shape_fill_ = combo("shape_fill", {{"なし", "none"}, {"メインの色", "main"}, {"サブの色", "sub"}, {"インキの色", "ink"}, {"グラデーション（メイン→サブ）", "gradient"}},
                        "main");
    shape_stroke_width_ = new QDoubleSpinBox;
    shape_stroke_width_->setObjectName(QStringLiteral("shape_stroke_width"));
    shape_stroke_width_->setRange(0, 20);
    shape_stroke_width_->setDecimals(2);
    shape_stroke_width_->setSingleStep(0.05);
    shape_stroke_width_->setSuffix(QStringLiteral(" mm"));
    shape_stroke_width_->setSpecialValueText(QStringLiteral("なし"));
    shape_stroke_colour_ = combo("shape_stroke_colour", {{"メインの色", "main"}, {"サブの色", "sub"}, {"インキの色", "ink"}}, "ink");
    shape_stroke_align_ = combo("shape_stroke_align", {{"内側", "inside"}, {"中央", "center"}, {"外側", "outside"}}, "center");
    shape_stroke_cap_ = combo("shape_stroke_cap", {{"線端なし", "butt"}, {"丸型線端", "round"}, {"四角形線端", "square"}}, "butt");
    shape_stroke_join_ = combo("shape_stroke_join", {{"マイター結合", "miter"}, {"ラウンド結合", "round"}, {"ベベル結合", "bevel"}}, "miter");
    shape_stroke_dash_ = combo("shape_stroke_dash", {{"実線", "solid"}, {"破線", "dashed"}, {"点線", "dotted"}}, "solid");
    shape_opacity_ = new QSpinBox;
    shape_opacity_->setObjectName(QStringLiteral("shape_opacity"));
    shape_opacity_->setRange(0, 100);
    shape_opacity_->setValue(100);
    shape_opacity_->setSuffix(QStringLiteral(" %"));
    shape_over_ = new QCheckBox(QStringLiteral("線の上に置く"));
    shape_over_->setObjectName(QStringLiteral("shape_over"));
    shape_over_->setToolTip(QStringLiteral("シェイプを、同じレイヤーのペンの線より上に描きます（入れないと線の下）"));
    shape_combine_ = combo("shape_combine", {{"新規", "new"}, {"シェイプを結合", "combine"}, {"前面シェイプを削除", "subtract"},
                                             {"シェイプが重なる領域を交差", "intersect"}, {"重なり合うシェイプ領域を中マド", "exclude"}},
                           "new");
    shape_combine_->setToolTip(QStringLiteral("新規のほかは、選んだシェイプ（パス選択ツールかパス パネルで）に形を足します"));
    shape_star_ = new QSpinBox;
    shape_star_->setObjectName(QStringLiteral("shape_star"));
    shape_star_->setRange(1, 100);
    shape_star_->setValue(100);
    shape_star_->setSuffix(QStringLiteral(" %"));
    shape_star_->setToolTip(QStringLiteral("100% で多角形、小さくすると内側の角がへこんだ星形"));
    connect(shape_star_, &QSpinBox::valueChanged, this, [this](int v) { canvas_->shape_star = v / 100.0; });
    shape_weight_ = new QDoubleSpinBox;
    shape_weight_->setObjectName(QStringLiteral("shape_weight"));
    shape_weight_->setRange(0.05, 50);
    shape_weight_->setDecimals(2);
    shape_weight_->setSingleStep(0.1);
    shape_weight_->setValue(0.5);
    shape_weight_->setSuffix(QStringLiteral(" mm"));
    shape_custom_ = new QComboBox;
    shape_custom_->setObjectName(QStringLiteral("shape_custom"));
    for (const presets::Group& g : presets::load(core::config_dir())) {
        for (const presets::Preset& p : g.shapes)
            shape_custom_->addItem(QStringLiteral("%1 / %2").arg(QString::fromStdString(g.name), QString::fromStdString(p.name)),
                                   QStringList{QString::fromStdString(g.name), QString::fromStdString(p.name)});
    }
    connect(shape_custom_, &QComboBox::activated, this, [this](int) {
        const QStringList chosen = shape_custom_->currentData().toStringList();
        if (chosen.size() == 2) choose_custom_shape(chosen[0], chosen[1]);
    });
    form->insertRow(1, QString(), shape_new_layer_);
    int at = form->rowCount() - 1;  // (before the 図形 page's hint)
    const auto row = [&](const QString& label, QWidget* widget) { form->insertRow(at++, label, widget); };
    row(QStringLiteral("塗り"), shape_fill_);
    row(QStringLiteral("線の幅"), shape_stroke_width_);
    row(QStringLiteral("線の色"), shape_stroke_colour_);
    row(QStringLiteral("線の位置"), shape_stroke_align_);
    row(QStringLiteral("線端"), shape_stroke_cap_);
    row(QStringLiteral("角"), shape_stroke_join_);
    row(QStringLiteral("線の種類"), shape_stroke_dash_);
    row(QStringLiteral("不透明度"), shape_opacity_);
    row(QString(), shape_over_);
    row(QStringLiteral("合成"), shape_combine_);
    row(QStringLiteral("星の比率"), shape_star_);
    row(QStringLiteral("ラインの太さ"), shape_weight_);
    row(QStringLiteral("カスタムシェイプ"), shape_custom_);
    // the pen tools' モード
    auto* pen_page = new QWidget;
    auto* pen_form = new QFormLayout(pen_page);
    pen_form->setContentsMargins(0, 0, 0, 0);
    pen_mode_ = combo("pen_mode", {{"シェイプ", "shape"}, {"パス", "path"}, {"ピクセル", "pixel"}},
                      store->value(QStringLiteral("shapes/pen_mode"), QStringLiteral("path")).toString().toLatin1().constData());
    connect(pen_mode_, &QComboBox::activated, this, [this](int) { settings()->setValue(QStringLiteral("shapes/pen_mode"), pen_mode_->currentData()); });
    pen_form->addRow(QStringLiteral("モード"), pen_mode_);
    auto* hint = new QLabel(QStringLiteral("シェイプ: 描いたパスを形にしてペンのレイヤーに置きます（塗り・線はシェイプツールの設定）。"
                                           "パス: 作業用パス（パス パネルで選んだパス）に足します。ピクセル: パスに沿ってペンの線を引きます（閉じたパスは図形の描き方で塗りも）。"));
    hint->setWordWrap(true);
    theme::role(hint, "hint");
    pen_form->addRow(hint);
    ts->add({QStringLiteral("pathpen"), QStringLiteral("curvepen")}, pen_page);
}

void MainWindow::choose_shape_tool(const QString& tool) {
    const ShapeTool* t = tool_named(tool);
    if (t == nullptr) return;
    const QString kind = QString::fromLatin1(t->kind);
    canvas_->shape_kind = kind;
    canvas_->shape_star = shape_star_ != nullptr ? shape_star_->value() / 100.0 : 1.0;
    if (kind == QLatin1String("custom") && !canvas_->shape_custom && shape_custom_ != nullptr && shape_custom_->count() > 0) {
        const QStringList chosen = shape_custom_->currentData().toStringList();
        const auto groups = presets::load(core::config_dir());
        if (chosen.size() == 2) {
            if (const presets::Preset* p = presets::find(groups, chosen[1].toStdString(), chosen[0].toStdString())) canvas_->shape_custom = p->path;
        }
    }
    canvas_->set_tool(QStringLiteral("shape"));
    if (shape_kind_ != nullptr) shape_kind_->setCurrentIndex(std::max(0, shape_kind_->findData(kind)));
}

void MainWindow::choose_custom_shape(const QString& group, const QString& name) {
    const auto groups = presets::load(core::config_dir());
    const presets::Preset* p = presets::find(groups, name.toStdString(), group.toStdString());
    if (p == nullptr) return;
    canvas_->shape_custom = p->path;
    if (shape_custom_ != nullptr) {
        const int at = shape_custom_->findData(QStringList{group, name});
        if (at >= 0) {
            shape_custom_->setCurrentIndex(at);
        } else {  // (one defined after the settings were made)
            shape_custom_->addItem(QStringLiteral("%1 / %2").arg(group, name), QStringList{group, name});
            shape_custom_->setCurrentIndex(shape_custom_->count() - 1);
        }
    }
    choose_tool(QStringLiteral("shape_custom"));
}

// --- what the tools make ----------------------------------------------------------------------------------------------

Json MainWindow::shape_fill_now() const {
    const QString how = shape_fill_ != nullptr ? shape_fill_->currentData().toString() : QStringLiteral("main");
    const Json sub = colours_ != nullptr ? rgb_json(colours_->sub_rgb()) : Json::array({255, 255, 255});
    if (how == QLatin1String("none")) return nullptr;
    if (how == QLatin1String("sub")) return Json{{"rgb", sub}};
    if (how == QLatin1String("ink")) return Json{{"rgb", nullptr}};
    if (how == QLatin1String("gradient")) return Json{{"gradient", Json{{"shape", "linear"}, {"rgb_from", rgb_json(pen_.rgb)}, {"rgb_to", sub}}}};
    return Json{{"rgb", rgb_json(pen_.rgb)}};
}

Json MainWindow::shape_stroke_now() const {
    if (shape_stroke_width_ == nullptr || shape_stroke_width_->value() <= 0) return nullptr;
    const QString colour = shape_stroke_colour_->currentData().toString();
    Json fill = colour == QLatin1String("ink")   ? Json{{"rgb", nullptr}}
                : colour == QLatin1String("sub") ? Json{{"rgb", colours_ != nullptr ? rgb_json(colours_->sub_rgb()) : Json::array({255, 255, 255})}}
                                                 : Json{{"rgb", rgb_json(pen_.rgb)}};
    Json stroke{{"width_mm", r3(shape_stroke_width_->value())},
                {"fill", fill},
                {"align", shape_stroke_align_->currentData().toString().toStdString()},
                {"cap", shape_stroke_cap_->currentData().toString().toStdString()},
                {"join", shape_stroke_join_->currentData().toString().toStdString()}};
    const QString dash = shape_stroke_dash_->currentData().toString();
    if (dash == QLatin1String("dashed")) stroke["dashes"] = Json::array({4, 2});
    if (dash == QLatin1String("dotted")) {  // (dots: dashes of no length with round ends)
        stroke["dashes"] = Json::array({0, 2});
        stroke["cap"] = "round";
    }
    return stroke;
}

std::optional<vec::Path> MainWindow::figure_path(const Json& shape) const {
    const std::string kind = shape.value("shape", std::string());
    try {
        if (kind == "line") {
            const vec::XY a = xy_of(shape["points"][0]), b = xy_of(shape["points"][1]);
            return pr::line_path(a, b, shape_weight_ != nullptr ? shape_weight_->value() : 0.5);
        }
        if (kind == "polyline" || kind == "curve") {
            const Json& points = shape["points"];
            const bool closed = shape.value("closed", false);
            std::vector<vec::Knot> knots;
            for (const Json& p : points) knots.push_back(vec::Knot::corner(p[0].get<double>(), p[1].get<double>()));
            if (kind == "curve" && knots.size() >= 3) {  // (Catmull-Rom through the points, as Bézier handles)
                const std::size_t n = knots.size();
                std::vector<vec::Knot> curved = knots;
                for (std::size_t i = 0; i < n; ++i) {
                    const vec::XY p = knots[i].anchor;
                    const vec::XY prev = i > 0 ? knots[i - 1].anchor : closed ? knots[n - 1].anchor : p;
                    const vec::XY next = i + 1 < n ? knots[i + 1].anchor : closed ? knots[0].anchor : p;
                    const double tx = (next.x - prev.x) / 6.0, ty = (next.y - prev.y) / 6.0;
                    curved[i].in = vec::XY{r3(p.x - tx), r3(p.y - ty)};
                    curved[i].out = vec::XY{r3(p.x + tx), r3(p.y + ty)};
                    curved[i].smooth = true;
                }
                knots = std::move(curved);
            }
            vec::Path path;
            path.subpaths.push_back(vec::Subpath{closed, vec::PathOp::Combine, std::move(knots)});
            return path;
        }
        const Json& b = shape["box"];
        const double x = b[0].get<double>(), y = b[1].get<double>(), w = b[2].get<double>(), h = b[3].get<double>();
        if (kind == "rect") {
            const double r = shape_radius_ != nullptr ? shape_radius_->value() : 0.0;
            return r > 0 ? pr::rounded_rect(x, y, w, h, {r, r, r, r}) : pr::rect_path(x, y, w, h);
        }
        if (kind == "ellipse") return pr::ellipse_path(x, y, w, h);
        if (kind == "triangle") return pr::polygon_path(x, y, w, h, 3, 1.0);
        if (kind == "polygon") return pr::polygon_path(x, y, w, h, shape.value("sides", 5), shape_star_ != nullptr ? shape_star_->value() / 100.0 : 1.0);
        if (kind == "custom" && canvas_->shape_custom) return presets::fit(*canvas_->shape_custom, {x, y, w, h}, shape.value("keep_aspect", false));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

bool MainWindow::add_shape_part_op(Json what, bool closed, const std::optional<vec::Path>& more) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    // 合成 (not 新規): the path added to the chosen part — the target path's, else the last part of the layer drawn on
    const QString how = shape_combine_ != nullptr ? shape_combine_->currentData().toString() : QStringLiteral("new");
    if (more && how != QLatin1String("new")) {
        std::optional<std::pair<std::string, std::string>> into = target_part(canvas_->target_path);
        if (into && part_named(*page, into->first, into->second) == nullptr) into.reset();
        const core::Layer* drawn_on = target_layer();
        if (!into && drawn_on != nullptr && drawn_on->shapes && !drawn_on->shapes->items.empty())
            into = std::make_pair(drawn_on->id, drawn_on->shapes->items.back().id);
        if (into) {
            const Json op{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", into->first}, {"id", into->second},
                          {"append", Json{{"path", vec::to_json(*more)}, {"op", how.toStdString()}}}};
            if (!apply_ops(Json::array({op}))) return false;
            canvas_->target_path = QStringLiteral("shape:%1/%2").arg(QString::fromStdString(into->first), QString::fromStdString(into->second));
            if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
            return true;
        }
    }
    Json op{{"op", "add_shape_part"}, {"page", page->index.json()}};
    for (const auto& [key, value] : what.items()) op[key] = value;
    const core::Layer* layer = target_layer();
    const bool fresh = (shape_new_layer_ != nullptr && shape_new_layer_->isChecked()) || layer == nullptr || layer->kind != core::LayerKind::Strokes ||
                       layer->locked;
    std::string layer_id;
    if (fresh) {  // (a new pen layer above the one drawn on: Photoshop's shape layer of its own)
        layer_id = core::new_id();
        const ShapeTool* t = tool_named(shape_tool_of(canvas_->shape_kind));
        Json made{{"id", layer_id}, {"name", t != nullptr ? t->layer_name : "シェイプ"}};
        if (layer != nullptr) made["after"] = layer->id;
        op["new_layer"] = made;
    } else {
        layer_id = layer->id;
        op["layer_id"] = layer_id;
    }
    const std::string part_id = core::new_id();
    op["id"] = part_id;
    if (closed) {
        op["fill"] = shape_fill_now();
        op["stroke"] = shape_stroke_now();
    } else {  // (an open path holds no fill: its stroke, the pen's width when the tool has none)
        op["fill"] = nullptr;
        Json stroke = shape_stroke_now();
        if (stroke.is_null()) stroke = Json{{"width_mm", r3(canvas_->brush_width_mm)}, {"fill", Json{{"rgb", rgb_json(pen_.rgb)}}}};
        op["stroke"] = stroke;
    }
    if (shape_opacity_ != nullptr && shape_opacity_->value() < 100) op["opacity"] = shape_opacity_->value() / 100.0;
    if (shape_over_ != nullptr && shape_over_->isChecked()) op["over"] = true;
    if (!apply_ops(Json::array({op}))) return false;
    canvas_->target_path = QStringLiteral("shape:%1/%2").arg(QString::fromStdString(layer_id), QString::fromStdString(part_id));
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    return true;
}

void MainWindow::shape_part_drawn(const Json& shape, const QString& mode) {
    const std::optional<vec::Path> path = figure_path(shape);
    if (!path || path->empty()) {
        if (shape.value("shape", std::string()) == "custom") flash(QStringLiteral("カスタムシェイプをシェイプ パネルかツールの設定で選びます"), 3000);
        return;
    }
    const std::string kind = shape.value("shape", std::string());
    const bool closed = kind == "polyline" || kind == "curve" ? shape.value("closed", false) : true;
    if (mode == QLatin1String("path")) {  // (subpaths of the work path, or of the path chosen in the Paths panel)
        if (target_part(canvas_->target_path)) canvas_->target_path = QStringLiteral("work");
        const QString how = shape_combine_ != nullptr ? shape_combine_->currentData().toString() : QStringLiteral("new");
        const std::string op_name = how == QLatin1String("new") ? "combine" : how.toStdString();
        Json one = vec::to_json(*path);
        for (Json& s : one["subpaths"]) {
            if (s.value("op", std::string("combine")) == "combine") s["op"] = op_name;
            path_drawn(s);
        }
        return;
    }
    // シェイプ: the live shape the figure is (its values kept: Photoshop's live shape), else its path
    Json spec;
    if (shape.contains("box") && kind != "custom") {
        const Json& box = shape["box"];
        if (kind == "rect") {
            const double r = shape_radius_ != nullptr ? shape_radius_->value() : 0.0;
            spec = Json{{"kind", r > 0 ? "rounded_rect" : "rect"}, {"rect", box}};
            if (r > 0) spec["radii"] = r;
        } else if (kind == "ellipse" || kind == "triangle") {
            spec = Json{{"kind", kind}, {"rect", box}};
        } else if (kind == "polygon") {
            const int star = shape_star_ != nullptr ? shape_star_->value() : 100;
            spec = Json{{"kind", star < 100 ? "star" : "polygon"}, {"rect", box}, {"sides", shape.value("sides", 5)}};
            if (star < 100) spec["star_ratio"] = star / 100.0;
        }
    } else if (kind == "line") {
        spec = Json{{"kind", "line"}, {"from", shape["points"][0]}, {"to", shape["points"][1]}, {"weight_mm", r3(shape_weight_->value())}};
    }
    if (spec.is_null()) spec = Json{{"kind", "path"}, {"path", vec::to_json(*path)}};
    Json what{{"shape", spec}};
    if (kind == "custom" && shape_custom_ != nullptr) {
        const QStringList chosen = shape_custom_->currentData().toStringList();
        if (chosen.size() == 2) what["name"] = chosen[1].toStdString();
    }
    add_shape_part_op(what, closed, path);
}

bool MainWindow::pen_path_drawn(const Json& subpath) {
    const QString mode = pen_mode_ != nullptr ? pen_mode_->currentData().toString() : QStringLiteral("path");
    if (mode == QLatin1String("path")) {
        if (target_part(canvas_->target_path)) canvas_->target_path = QStringLiteral("work");  // (a path: the work path, as Photoshop)
        return false;
    }
    const core::Page* page = current_page();
    if (page == nullptr) return true;
    const vec::Path path = vec::path_from_json(Json{{"subpaths", Json::array({subpath})}});
    const bool closed = subpath.value("closed", false);
    if (mode == QLatin1String("shape")) {
        add_shape_part_op(Json{{"shape", Json{{"kind", "path"}, {"path", vec::to_json(path)}}}}, closed, path);
        return true;
    }
    // ピクセル: the pen's line along the path, and a closed one filled as 図形's 描き方 says
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return true;
    const QString style = shape_style_ != nullptr ? shape_style_->currentData().toString() : QStringLiteral("line");
    Json ops = Json::array();
    if (closed && (style == QLatin1String("fill") || style == QLatin1String("both"))) {
        Json fill{{"op", "fill_area"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", Json{{"path", vec::to_json(path)}}}};
        fill.update(paint_fields());
        ops.push_back(fill);
    }
    if (!closed || style != QLatin1String("fill")) {
        const Json pen = pen_.stroke_fields();
        Json line{{"op", "stroke_path"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"path", vec::to_json(path)},
                  {"simulate_pressure", pen.value("taper", false)}};
        for (const char* key : {"kind", "width_mm", "rgb", "opacity"}) {
            if (pen.contains(key)) line[key] = pen[key];
        }
        ops.push_back(line);
    }
    apply_ops(ops);
    return true;
}

// --- the commands ---------------------------------------------------------------------------------------------------

bool MainWindow::place_shape_preset(const QString& group, const QString& name, std::optional<std::array<double, 4>> rect) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    const auto groups = presets::load(core::config_dir());
    const presets::Preset* p = presets::find(groups, name.toStdString(), group.toStdString());
    if (p == nullptr) {
        flash(QStringLiteral("そのシェイプはプリセットにありません"), 3000);
        return false;
    }
    if (!rect) {  // (a quarter of the page's shorter side, in its middle)
        const double w = page->spec.width_mm.value(), h = page->spec.height_mm.value();
        const double side = r3(std::min(w, h) / 4);
        rect = std::array<double, 4>{r3((w - side) / 2), r3((h - side) / 2), side, side};
    }
    const vec::Path path = presets::fit(p->path, *rect, true);
    return add_shape_part_op(Json{{"shape", Json{{"kind", "path"}, {"path", vec::to_json(path)}}}, {"name", name.toStdString()}}, true);
}

bool MainWindow::define_custom_shape(std::optional<QString> name) {
    const std::optional<vec::Path> path = canvas_->target_path_now();
    if (!path || path->empty()) {
        flash(QStringLiteral("先にパスかシェイプを選びます（パス パネル、パス選択ツール）"), 3000);
        return false;
    }
    std::vector<presets::Group> groups = presets::load(core::config_dir());
    QString chosen;
    if (name) {
        chosen = *name;
    } else {
        const Asked asked = asking();
        const auto answer = ask::get_text(this, QStringLiteral("カスタムシェイプを定義"), QStringLiteral("シェイプ名"), QStringLiteral("シェイプ 1"));
        if (!answer || !still(asked)) return false;
        chosen = *answer;
    }
    if (chosen.trimmed().isEmpty()) return false;
    Json made;
    try {
        made = presets::add(groups, chosen.trimmed().toStdString(), std::nullopt, *path);
        presets::save(core::config_dir(), groups);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return false;
    }
    if (shapes_panel_ != nullptr) shapes_panel_->refresh();
    const QString kept = QString::fromStdString(made.value("name", chosen.toStdString()));
    const QString group = QString::fromStdString(made.value("group", std::string(presets::kCustomGroup)));
    if (shape_custom_ != nullptr && shape_custom_->findData(QStringList{group, kept}) < 0)
        shape_custom_->addItem(QStringLiteral("%1 / %2").arg(group, kept), QStringList{group, kept});
    flash(QStringLiteral("「%1」をカスタムシェイプに加えました").arg(kept), 3000);
    return true;
}

bool MainWindow::combine_shapes(const std::string& how) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr) return false;
    std::optional<std::pair<std::string, std::string>> chosen = target_part(canvas_->target_path);
    if (chosen && (layer == nullptr || chosen->first != layer->id)) {  // (the part chosen on the page: its layer)
        for (const core::Layer& l : page->layers)
            if (l.id == chosen->first) layer = &l;
    }
    if (layer == nullptr || layer->kind != core::LayerKind::Strokes || !layer->shapes || layer->shapes->items.empty()) {
        flash(QStringLiteral("描く先のペンのレイヤーにシェイプがありません"), 3000);
        return false;
    }
    const auto& parts = layer->shapes->items;
    if (chosen && part_named(*page, layer->id, chosen->second) == nullptr) chosen.reset();
    Json ops = Json::array();
    std::string first = parts.front().id;
    if (how == "merge") {  // (the chosen part's components, else every part's)
        for (const vec::ShapePart& part : parts) {
            if (chosen && part.id != chosen->second) continue;
            ops.push_back(Json{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"id", part.id}, {"merge_components", true}});
        }
        if (chosen) first = chosen->second;
    } else if (parts.size() >= 2) {
        Json ids = Json::array();
        for (const vec::ShapePart& part : parts) ids.push_back(part.id);
        ops.push_back(Json{{"op", "combine_shape_parts"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"ids", ids}, {"operation", how}});
    } else if (parts.front().path.subpaths.size() >= 2) {  // (one part: its subpaths' way — the chosen one's, else all but the first)
        Json op{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"id", parts.front().id}, {"operation", how}};
        if (canvas_->path_subpath && *canvas_->path_subpath > 0 && chosen) op["subpath"] = static_cast<std::int64_t>(*canvas_->path_subpath);
        ops.push_back(op);
    } else {
        flash(QStringLiteral("合わせるシェイプが 1 つしかありません"), 3000);
        return false;
    }
    if (!apply_ops(ops)) return false;
    canvas_->target_path = QStringLiteral("shape:%1/%2").arg(QString::fromStdString(layer->id), QString::fromStdString(first));
    canvas_->path_subpath.reset();
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    canvas_->update();
    return true;
}

bool MainWindow::rasterize_shapes() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return false;
    if (!layer->shapes || layer->shapes->items.empty()) {
        flash(QStringLiteral("このレイヤーにはシェイプがありません"), 3000);
        return false;
    }
    if (!apply_ops(Json::array({Json{{"op", "rasterize_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"shape", true}}}))) return false;
    if (target_part(canvas_->target_path)) canvas_->target_path = QStringLiteral("work");
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    return true;
}

bool MainWindow::selection_to_shape() {
    const auto area = need_area();
    if (!area) return false;
    return add_shape_part_op(Json{{"area", *area}}, true);
}

bool MainWindow::line_to_shape() {
    if (!canvas_->selected_line_id) {
        flash(QStringLiteral("先に台詞を選びます（選択ツールで台詞をクリック）"), 3000);
        return false;
    }
    Json op{{"op", "text_to_path"}, {"id", *canvas_->selected_line_id}, {"to", "shape"}};
    const core::Layer* layer = target_layer();
    if (layer != nullptr && layer->kind == core::LayerKind::Strokes && !layer->locked && !(shape_new_layer_ != nullptr && shape_new_layer_->isChecked()))
        op["layer_id"] = layer->id;
    if (!apply_ops(Json::array({op}))) return false;
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    return true;
}

bool MainWindow::guides_from_shape() {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    // (the shape chosen, else every shape of the layer drawn on)
    std::optional<std::array<double, 4>> box;
    const auto take = [&](const vec::Path& path) {
        const auto b = path.control_bounds();
        if (!b) return;
        if (!box) {
            box = *b;
        } else {
            box = std::array<double, 4>{std::min((*box)[0], (*b)[0]), std::min((*box)[1], (*b)[1]), std::max((*box)[2], (*b)[2]), std::max((*box)[3], (*b)[3])};
        }
    };
    if (const auto chosen = target_part(canvas_->target_path)) {
        if (const vec::ShapePart* part = part_named(*page, chosen->first, chosen->second)) take(part->path);
    }
    if (!box) {
        if (const core::Layer* layer = target_layer(); layer != nullptr && layer->shapes) {
            for (const vec::ShapePart& part : layer->shapes->items) take(part.path);
        }
    }
    if (!box) {
        flash(QStringLiteral("シェイプを選びます（パス選択ツール、またはシェイプのあるペンのレイヤー）"), 3000);
        return false;
    }
    const auto [x0, y0, x1, y1] = *box;
    Json ops = Json::array();
    for (const double x : {x0, (x0 + x1) / 2, x1})
        ops.push_back(Json{{"op", "add_ruler"}, {"page", page->index.json()}, {"kind", "guide"}, {"axis", "v"}, {"at", r3(x)}});
    for (const double y : {y0, (y0 + y1) / 2, y1})
        ops.push_back(Json{{"op", "add_ruler"}, {"page", page->index.json()}, {"kind", "guide"}, {"axis", "h"}, {"at", r3(y)}});
    if (!apply_ops(ops)) return false;
    flash(QStringLiteral("ガイドを 6 本置きました"), 3000);
    return true;
}

}  // namespace genko::app
