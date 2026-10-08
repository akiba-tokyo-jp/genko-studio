// Rulers, the grid and 3D in the window (M3④-4, Python's main.py: act_ruler, act_3d, the ruler kinds, the ruler
// commands — snap, show, grid, delete, a layer's own, 定規ペン, a selection or a panel from a ruler, the perspective
// grid, the perspective from the 3D and the camera from the ruler, fixed, the eye level — and the 3D ones: a figure, a
// stick figure, a head, a hand, a model, boxes, props, backgrounds, poses, traced as lines; the 定規・3D panel (its own
// file); and what the canvas's guides become: _place_ruler, the edit_ruler / edit_prim of a drag, _prim_posed).

#include <QAction>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>

#include <cmath>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/config.hpp"
#include "app/guide_panel.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/mesh3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pynum.hpp"
#include "core/rulers.hpp"

namespace genko::app {

using core::Json;

namespace {

struct RulerKind {
    const char* title;
    const char* kind;
    int vps;
    int copies;
    bool ask;
    const char* tip;
};

const std::vector<RulerKind>& ruler_kinds() {
    static const std::vector<RulerKind> kinds = {
        {"直線定規", "line", 1, 2, false, "ドラッグで置く。近くで描いた線がまっすぐ沿う"},
        {"曲線定規", "curve", 1, 2, false, "クリックで点を打ち、ダブルクリック（Enter）で終わる"},
        {"平行線定規", "parallel", 1, 2, false, "ドラッグで角度を決める。どこで描いてもその角度の直線になる"},
        {"同心円定規", "concentric", 1, 2, false, "中心からドラッグ（Alt で楕円）。描いた線が円に沿う"},
        {"放射線定規（集中線）", "radial", 1, 2, false, "中心をクリック。描いた線が中心へ向かう"},
        {"パース定規（1 点）", "perspective", 1, 2, false, "消失点をクリック"},
        {"パース定規（2 点）", "perspective", 2, 2, false, "消失点を 2 つクリック（アイレベルが引かれる）"},
        {"パース定規（3 点）", "perspective", 3, 2, false, "消失点を 3 つクリック"},
        {"対称定規（左右）", "symmetry", 1, 2, false, "対称の軸をドラッグ。描いた線が反対側にも描かれる"},
        {"対称定規（回転）…", "symmetry", 1, 2, true, "中心から軸をドラッグ。描いた線が中心の周りに写される"},
        {"平行曲線定規", "parallel_curve", 1, 2, false, "クリックで曲線を置き、Enter で終わる。どこで描いてもその曲線の形に沿う"},
        {"多重曲線定規", "multi_curve", 1, 2, false, "1 本目の曲線をクリックで置いて Enter、2 本目も置いて Enter。描いた線は 2 本の間の形に沿う"},
        {"放射曲線定規", "radial_curve", 1, 2, false, "中心をクリックしてから曲線をクリックで置き、Enter。描いた線は中心から広がる同じ形に沿う"},
        {"図形定規（長方形）", "rect", 1, 2, false, "対角をドラッグ。近くで描いた線が長方形の辺に沿って回る（角はそのまま）"},
        {"図形定規（楕円）", "ellipse", 1, 2, false, "外側の四角をドラッグ。近くで描いた線が楕円に沿う"},
        {"図形定規（多角形）", "polygon", 1, 2, false, "角をクリックで打ち、Enter で閉じる。近くで描いた線が辺に沿う"},
    };
    return kinds;
}

const std::vector<std::pair<const char*, const char*>> kProps = {{"chair", "椅子"}, {"desk", "机"}, {"table", "テーブル"}, {"bed", "ベッド"},
                                                                 {"door", "ドア"},   {"window", "窓"}, {"shelf", "棚"},       {"car", "車"}};
const std::vector<std::pair<const char*, const char*>> kScenes = {{"room", "部屋"}, {"classroom", "教室"}, {"corridor", "廊下"}, {"street", "街並み"}};
// the stick figure's poses (mannequin.PRESETS), then the 3D figure's own (guide_panel.FIGURE_PRESETS)
const std::vector<std::pair<const char*, const char*>> kPoses = {{"stand", "立つ"},       {"walk", "歩く"},   {"run", "走る"},     {"sit", "座る"},
                                                                 {"point", "指さす"},    {"look_back", "振り返る"}, {"arms_up", "両手を上げる"},
                                                                 {"think", "考える"},    {"kneel", "片ひざ"},  {"peace", "ピース"}};

double r2(double v) { return core::py_round(v, 2); }

const Json* find_by_id(const Json& list, const std::string& id) {
    if (!list.is_array()) return nullptr;
    for (const Json& item : list)
        if (item.is_object() && item.value("id", Json()) == Json(id)) return &item;
    return nullptr;
}

}  // namespace

void MainWindow::build_guide_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const char* name, const QString& key, const QString& tip) {
        const QString which = QString::fromLatin1(name);
        QAction* act = make(QString::fromLatin1(attribute), title, [this, which] { choose_tool(which); },
                            key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{QKeySequence(key)}, tip, true);
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[which] = act;
    };
    tool("act_ruler", QStringLiteral("定規"), "ruler", QStringLiteral("R"),
         QStringLiteral("「定規」メニューで選んだ定規を置く（ドラッグ・クリック）。置いた定規の□をドラッグで動かす"));
    tool("act_3d", QStringLiteral("3D 操作"), "3d", QStringLiteral("J"), QStringLiteral("デッサン人形の関節（○）や箱をドラッグして動かす。箱の上の○で回す"));
    for (const RulerKind& k : ruler_kinds()) {
        auto* act = new QAction(QString::fromUtf8(k.title), this);
        act->setStatusTip(QString::fromUtf8(k.tip));
        act->setToolTip(QString::fromUtf8(k.title) + QStringLiteral("\n") + QString::fromUtf8(k.tip));
        const RulerKind chosen = k;
        connect(act, &QAction::triggered, this, [this, chosen] { choose_ruler(chosen.kind, chosen.vps, chosen.copies, chosen.ask); });
        ruler_actions_.push_back(act);
    }
    const auto toggle = [this](const char* attribute, const QString& title, const QString& key, const QString& tip) {
        make(QString::fromLatin1(attribute), title, [this] { guide_toggles(); }, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{QKeySequence(key)},
             tip, true);
    };
    toggle("act_snap", QStringLiteral("定規にスナップ"), QStringLiteral("Ctrl+2"), QStringLiteral("ペンの線を定規に沿わせる（切ると自由に描ける）"));
    toggle("act_show_rulers", QStringLiteral("定規を表示"), QStringLiteral("Ctrl+Shift+R"), QString());
    toggle("act_grid", QStringLiteral("グリッドを表示"), QStringLiteral("Ctrl+'"), QString());
    toggle("act_grid_snap", QStringLiteral("グリッドにスナップ"), QString(), QStringLiteral("Shift で引く直線と定規の点がグリッドに吸い付く"));
    {
        const auto store = settings();
        for (const auto& [name, key, fallback] : {std::tuple{"act_snap", "snap", true}, {"act_show_rulers", "show", true}, {"act_grid", "grid", false},
                                                  {"act_grid_snap", "grid_snap", false}}) {
            QAction* act = action(QString::fromLatin1(name));
            const QSignalBlocker quiet(act);
            act->setChecked(store->value(QStringLiteral("guides/%1").arg(QString::fromLatin1(key)), fallback).toString().toLower() == QLatin1String("true"));
        }
        bool ok = false;
        const double spacing = store->value(QStringLiteral("guides/grid_mm")).toDouble(&ok);
        if (ok && spacing > 0) canvas_->grid_mm = spacing;
    }
    make("act_grid_mm", QStringLiteral("グリッドの間隔…"), [this] { grid_spacing(); });
    make("act_del_ruler", QStringLiteral("選んだ定規を消す"), [this] { guides_->delete_ruler(); });
    make("act_ruler_layer", QStringLiteral("選んだ定規をこのレイヤー専用にする／戻す"), [this] { ruler_to_target_layer(); }, {},
         QStringLiteral("描く先のレイヤーを描いている時だけ、その定規が見えて効きます"));
    make("act_ruler_pen", QStringLiteral("選んだ定規の線を描く（定規ペン）"), [this] { ruler_pen(); }, {},
         QStringLiteral("定規そのものを、描く先のレイヤーにペンの線で描きます"));
    make("act_ruler_selection", QStringLiteral("選んだ定規から選択範囲を作る"), [this] { ruler_selection(); }, {},
         QStringLiteral("閉じた形の定規（図形定規・円・閉じた曲線）の中を選択範囲にします"));
    make("act_persp_grid", QStringLiteral("パース定規のグリッド…"), [this] { perspective_grid(); }, {},
         QStringLiteral("選んだパース定規に、地面のグリッドを出す（線の数。0 で消す）"));
    make("act_ruler_from_3d", QStringLiteral("3D に合わせてパース定規を作る"), [this] { ruler_from_3d(); }, {},
         QStringLiteral("ページの 3D（選んだもの、なければ最初のもの）の消失点にパース定規を置きます"));
    make("act_camera_from_ruler", QStringLiteral("カメラを選んだパース定規に合わせる"), [this] { camera_from_ruler(); }, {},
         QStringLiteral("3D のカメラを回して、3D の消失点が選んだパース定規の消失点に来るようにします"));
    make("act_ruler_frame", QStringLiteral("選んだ定規でコマを割る・作る"), [this] { ruler_frame(); }, {},
         QStringLiteral("直線の定規: その線でコマを割ります。円・閉じた曲線の定規: その形のコマを作ります"));
    make("act_ruler_fix", QStringLiteral("選んだ定規を固定する／外す"), [this] { ruler_flag("fixed"); }, {}, QStringLiteral("点を動かせないようにします"));
    make("act_ruler_horizon", QStringLiteral("パースの目の高さを固定する／外す"), [this] { ruler_flag("lock_horizon"); }, {},
         QStringLiteral("消失点を動かしても、アイレベル（目の高さ）の上を滑るだけにします"));
    make("act_clear_rulers", QStringLiteral("このページの定規をすべて消す"), [this] { clear_rulers(); });
    make("act_add_figure", QStringLiteral("デッサン人形を置く"), [this] { add_prim("figure"); }, {},
         QStringLiteral("体型を変えられ、関節をドラッグでポーズを付けられる人形を、選んだコマ（なければページ）の真ん中に置きます"));
    make("act_add_stick", QStringLiteral("棒人形（手早いポーズ用）を置く"), [this] { add_prim("mannequin"); });
    make("act_add_head", QStringLiteral("頭部（顔の向きの目安）を置く"), [this] { add_prim("head"); });
    make("act_add_hand", QStringLiteral("手（指のポーズ）を置く"), [this] { add_prim("hand"); });
    make("act_import_obj", QStringLiteral("3D モデルを読み込む（OBJ・glTF・VRM）…"), [this] { import_model(); });
    make("act_add_box", QStringLiteral("3D の箱を置く"), [this] { add_prim("box"); });
    make("act_add_cylinder", QStringLiteral("3D の円柱を置く"), [this] { add_prim("cylinder"); });
    make("act_add_stairs", QStringLiteral("3D の階段を置く"), [this] { add_prim("stairs"); });
    make("act_add_floor", QStringLiteral("床（パースの格子）を置く"), [this] { add_prim("floor"); }, {}, QStringLiteral("地面の格子で、背景のパースの目安にします"));
    make("act_add_sphere", QStringLiteral("3D の球を置く"), [this] { add_prim("sphere"); });
    make("act_add_cone", QStringLiteral("3D の円錐を置く"), [this] { add_prim("cone"); });
    for (const auto& [key, label] : kProps) {
        auto* act = new QAction(QStringLiteral("小物: %1").arg(QString::fromUtf8(label)), this);
        act->setStatusTip(QStringLiteral("1 つずつ置ける 3D の小物（箱の組み合わせ）"));
        const std::string prop = key;
        connect(act, &QAction::triggered, this, [this, prop] { add_prim("prop", prop); });
        prop_actions_.push_back(act);
    }
    for (const auto& [key, label] : kScenes) {
        auto* act = new QAction(QStringLiteral("背景: %1").arg(QString::fromUtf8(label)), this);
        act->setStatusTip(QStringLiteral("壁・床・窓・机などの 3D の下描きをまとめて置きます。まとめて動かし・回し・線にできます"));
        const std::string scene = key;
        connect(act, &QAction::triggered, this, [this, scene] { add_scene(scene); });
        scene_actions_.push_back(act);
    }
    make("act_trace", QStringLiteral("3D を線にする（描く先のレイヤーへ）"), [this] { trace_prims(false); }, {},
         QStringLiteral("このページの 3D を鉛筆の線にして下描きにします"));
    make("act_del_prim", QStringLiteral("選んだ 3D を消す"), [this] { guides_->delete_prim(); });
    for (const auto& [key, label] : kPoses) {
        auto* act = new QAction(QStringLiteral("ポーズ: %1").arg(QString::fromUtf8(label)), this);
        const std::string preset = key;
        connect(act, &QAction::triggered, this, [this, preset] { pose(preset); });
        pose_actions_.push_back(act);
    }
}

void MainWindow::build_guide_menus(QMenu* tools) {
    QMenu* ruler = tools->addMenu(QStringLiteral("定規"));
    for (QAction* act : ruler_actions_) ruler->addAction(act);
    ruler->addSeparator();
    for (const char* name : {"act_snap", "act_show_rulers", "act_del_ruler", "act_clear_rulers", "act_ruler_layer", "act_ruler_pen", "act_ruler_frame",
                             "act_ruler_selection", "act_ruler_fix", "act_ruler_horizon", "act_persp_grid", "act_ruler_from_3d", "act_camera_from_ruler"})
        ruler->addAction(action(name));
    ruler->addSeparator();
    for (const char* name : {"act_grid", "act_grid_snap", "act_grid_mm"}) ruler->addAction(action(name));
    QMenu* three = tools->addMenu(QStringLiteral("3D"));
    for (const char* name : {"act_add_figure", "act_add_stick", "act_add_head", "act_add_hand", "act_add_box", "act_add_cylinder", "act_add_sphere",
                             "act_add_cone", "act_add_stairs", "act_add_floor"})
        three->addAction(action(name));
    QMenu* props = three->addMenu(QStringLiteral("小物"));
    for (QAction* act : prop_actions_) props->addAction(act);
    three->addSeparator();
    for (QAction* act : scene_actions_) three->addAction(act);
    three->addAction(action("act_import_obj"));
    three->addSeparator();
    for (QAction* act : pose_actions_) three->addAction(act);
    three->addSeparator();
    three->addAction(action("act_trace"));
    three->addAction(action("act_del_prim"));
}

void MainWindow::build_guide_pages(ToolSettings* ts) {
    const auto group = [this](std::size_t from, std::size_t to) {
        return std::vector<QAction*>(ruler_actions_.begin() + static_cast<std::ptrdiff_t>(from), ruler_actions_.begin() + static_cast<std::ptrdiff_t>(to));
    };
    ts->add({QStringLiteral("ruler")},
            action_page({QStringLiteral("定規"),
                         static_cast<QWidget*>(menu_button(QStringLiteral("定規の種類"), {group(0, 5), group(5, 8), group(8, 10), group(10, 13), group(13, 16)})),
                         static_cast<QWidget*>(menu_button(QStringLiteral("選んだ定規"),
                                                           {{action("act_ruler_layer"), action("act_ruler_pen"), action("act_ruler_frame"), action("act_ruler_selection")},
                                                            {action("act_ruler_fix"), action("act_ruler_horizon"), action("act_persp_grid")},
                                                            {action("act_ruler_from_3d"), action("act_camera_from_ruler")}})),
                         action("act_del_ruler"), action("act_clear_rulers"), QStringLiteral("吸着と表示"), action("act_snap"), action("act_show_rulers"),
                         QStringLiteral("グリッド"), action("act_grid"), action("act_grid_snap"), action("act_grid_mm")}));
    ts->add({QStringLiteral("3d")},
            action_page({QStringLiteral("置く"),
                         static_cast<QWidget*>(menu_button(QStringLiteral("置く"), {{action("act_add_figure"), action("act_add_stick"), action("act_add_head"),
                                                                                      action("act_add_hand")},
                                                                                     {action("act_add_box"), action("act_add_cylinder"), action("act_add_stairs"),
                                                                                      action("act_add_floor")},
                                                                                     scene_actions_,
                                                                                     {action("act_import_obj")}})),
                         QStringLiteral("動かす・線にする"), static_cast<QWidget*>(menu_button(QStringLiteral("人形のポーズ"), {pose_actions_})),
                         action("act_trace"), action("act_del_prim")}));
}

void MainWindow::build_guide_dock() {
    guides_ = new GuidePanel(this);
    auto* dock = new QDockWidget(QStringLiteral("定規・3D"), this);
    dock->setObjectName(QStringLiteral("定規・3D"));
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(guides_);
    dock->setWidget(scroll);
    dock->setMinimumWidth(200);
    // (for occasional work: its tab has a close button and it joins the row of tabs when opened)
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(pages_dock_, dock);
    dock->hide();
    connect(dock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) guides_->refresh();
    });
    view_menu_->addAction(dock->toggleViewAction());
    guide_toggles();
}

// --- the commands ---------------------------------------------------------------------------------------------------

void MainWindow::guide_toggles() {
    canvas_->snap_rulers = action("act_snap")->isChecked();
    canvas_->rulers_visible = action("act_show_rulers")->isChecked();
    canvas_->grid_visible = action("act_grid")->isChecked();
    canvas_->grid_snap = action("act_grid_snap")->isChecked();
    const auto store = settings();
    for (const auto& [name, key] : {std::pair{"act_snap", "snap"}, {"act_show_rulers", "show"}, {"act_grid", "grid"}, {"act_grid_snap", "grid_snap"}})
        store->setValue(QStringLiteral("guides/%1").arg(QString::fromLatin1(key)), action(QString::fromLatin1(name))->isChecked());
    canvas_->update();
}

void MainWindow::grid_spacing() {
    const auto value = ask::get_double(this, QStringLiteral("グリッドの間隔"), QStringLiteral("間隔（mm）"), canvas_->grid_mm, 0.5, 100, 1);
    if (!value) return;
    canvas_->grid_mm = *value;
    settings()->setValue(QStringLiteral("guides/grid_mm"), *value);
    if (!action("act_grid")->isChecked()) {
        action("act_grid")->setChecked(true);
        guide_toggles();
    }
    canvas_->update();
}

void MainWindow::choose_ruler(const std::string& kind, int vps, int copies, bool ask_copies) {
    if (ask_copies) {
        const auto asked = ask::get_int(this, QStringLiteral("対称定規"), QStringLiteral("写しの数（中心の周りに）"), 6, 3, 32);
        if (!asked) return;
        copies = *asked;
    }
    canvas_->ruler_kind = QString::fromStdString(kind);
    canvas_->ruler_vps = vps;
    canvas_->ruler_copies = copies;
    choose_tool(QStringLiteral("ruler"));
    for (const RulerKind& k : ruler_kinds()) {
        if (k.kind == kind && (kind != "perspective" || k.vps == vps) && (kind != "symmetry" || k.ask == ask_copies)) {
            QString title = QString::fromUtf8(k.title);
            if (title.endsWith(QStringLiteral("…"))) title.chop(1);
            flash(QStringLiteral("%1: %2").arg(title, QString::fromUtf8(k.tip)), 5000);
            break;
        }
    }
}

void MainWindow::place_ruler(const Json& ruler) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const std::string id = core::new_id();
    Json op{{"op", "add_ruler"}, {"page", page->index.json()}, {"id", id}};
    for (const auto& [key, value] : ruler.items()) op[key] = value;
    if (!apply_ops(Json::array({op}))) return;
    if (ruler.value("kind", std::string()) == "guide") {
        flash(QStringLiteral("ガイド線を引きました。近くで描き始めた線が沿います（定規 → 選んだ定規を消す で消す）"), 3500);
        return;
    }
    canvas_->selected_ruler_id = id;
    if (!action("act_snap")->isChecked()) {
        action("act_snap")->setChecked(true);
        guide_toggles();
    }
    flash(QStringLiteral("定規を置きました。ペン（B）で描くと沿います（Ctrl+2 で切り替え）"), 3500);
    if (guides_ != nullptr) guides_->refresh();
}

const Json* MainWindow::selected_ruler() {
    const core::Page* page = current_page();
    const Json* found = page != nullptr && canvas_->selected_ruler_id ? find_by_id(page->rulers, *canvas_->selected_ruler_id) : nullptr;
    if (found == nullptr) flash(QStringLiteral("先に定規の道具で、定規の点をクリックして選びます"), 5000);
    return found;
}

void MainWindow::ruler_to_target_layer() {
    const Json* ruler = selected_ruler();
    const core::Layer* layer = target_layer();
    if (ruler == nullptr || layer == nullptr) return;
    const bool kept = ruler->contains("layer_id") && core::py_truthy((*ruler)["layer_id"]);
    const Json now = kept ? Json() : Json(layer->id);
    const QString label = wording::layer_label(*layer);
    if (apply_ops(Json::array({Json{{"op", "edit_ruler"}, {"page", current_page()->index.json()}, {"id", ruler->value("id", Json())}, {"layer_id", now}}}))) {
        flash(kept ? QStringLiteral("定規をどのレイヤーでも使えるように戻しました") : QStringLiteral("定規を「%1」専用にしました").arg(label), 4000);
    }
}

void MainWindow::ruler_pen() {
    const Json* ruler = selected_ruler();
    const core::Layer* layer = ruler != nullptr ? paint_layer() : nullptr;
    if (ruler == nullptr || layer == nullptr) return;
    apply_ops(Json::array({Json{{"op", "ruler_to_layer"}, {"page", current_page()->index.json()}, {"id", ruler->value("id", Json())}, {"layer_id", layer->id},
                                {"width_mm", std::max(0.1, brush_->size->value())}, {"rgb", pen_.rgb}}}));
}

std::optional<Json> MainWindow::closed_ruler_outline(const Json& ruler) const {
    const core::Page* page = current_page();
    const auto lines = core::rulers::outline(ruler, core::rulers::PageSize{page->spec.width_mm, page->spec.height_mm});
    if (lines.empty() || lines.front().size() <= 3) return std::nullopt;
    const auto& line = lines.front();
    if (std::hypot(line.front().x.value() - line.back().x.value(), line.front().y.value() - line.back().y.value()) >= 1.0) return std::nullopt;
    Json poly = Json::array();
    for (std::size_t i = 0; i + 1 < line.size(); ++i) poly.push_back(Json::array({r2(line[i].x.value()), r2(line[i].y.value())}));
    return poly;
}

void MainWindow::ruler_selection() {
    // 定規から選択範囲: the inside of a closed ruler (a shape, a circle, a closed curve)
    const Json* ruler = selected_ruler();
    if (ruler == nullptr) return;
    std::optional<Json> poly;
    try {
        poly = closed_ruler_outline(*ruler);
    } catch (const std::exception&) {
    }
    if (!poly) {
        flash(QStringLiteral("選択範囲にできるのは、閉じた形の定規（図形定規・円・閉じた曲線）です"), 6000);
        return;
    }
    choose_tool(QStringLiteral("rect"));
    canvas_->set_selection(Json{{"poly", *poly}});
}

void MainWindow::perspective_grid() {
    const Json* ruler = selected_ruler();
    if (ruler == nullptr) return;
    if (ruler->value("kind", std::string()) != "perspective") {
        flash(QStringLiteral("パース定規を選んでください"), 5000);
        return;
    }
    const std::string id = ruler->value("id", std::string());
    const int now = ruler->contains("grid") && core::py_truthy((*ruler)["grid"]) ? static_cast<int>(core::to_int((*ruler)["grid"])) : 12;
    const Asked asked = asking();
    const auto lines = ask::get_int(this, QStringLiteral("パースのグリッド"), QStringLiteral("地面のグリッドの線の数（0 で消す）"), now, 0, 60);
    if (lines && still(asked))
        apply_ops(Json::array({Json{{"op", "edit_ruler"}, {"page", current_page()->index.json()}, {"id", id}, {"grid", *lines}}}));
}

void MainWindow::ruler_from_3d() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    bool any = false;
    if (page->prims.is_array())
        for (const Json& p : page->prims) any = any || p.value("kind", std::string()) != "mannequin";
    if (!any) {
        flash(QStringLiteral("このページに 3D がありません（3D の道具で置いてから）"), 5000);
        return;
    }
    Json op{{"op", "ruler_from_3d"}, {"page", page->index.json()}};
    if (canvas_->selected_prim_id) op["prim_id"] = *canvas_->selected_prim_id;
    apply_ops(Json::array({op}));
}

void MainWindow::camera_from_ruler() {
    const Json* ruler = selected_ruler();
    if (ruler == nullptr) return;
    Json op{{"op", "camera_from_ruler"}, {"page", current_page()->index.json()}, {"id", ruler->value("id", Json())}};
    if (canvas_->selected_prim_id) op["prim_id"] = *canvas_->selected_prim_id;
    apply_ops(Json::array({op}));
}

void MainWindow::ruler_frame() {
    // a straight ruler cuts the panel it crosses; a circle or a closed curve becomes a panel of its shape
    const Json* ruler = selected_ruler();
    if (ruler == nullptr) return;
    const core::Page* page = current_page();
    std::vector<std::vector<core::rulers::OutlinePoint>> lines;
    try {
        lines = core::rulers::outline(*ruler, core::rulers::PageSize{page->spec.width_mm, page->spec.height_mm});
    } catch (const std::exception&) {
    }
    if (ruler->value("kind", std::string()) == "line" && !lines.empty()) {
        const double x0 = lines[0].front().x.value(), y0 = lines[0].front().y.value(), x1 = lines[0].back().x.value(), y1 = lines[0].back().y.value();
        const core::Frame* target = page->frame_at(core::Num((x0 + x1) / 2), core::Num((y0 + y1) / 2));
        if (target == nullptr) target = page->frame_at(core::Num(x0), core::Num(y0));
        if (target == nullptr) target = page->frame_at(core::Num(x1), core::Num(y1));
        if (target == nullptr) {
            flash(QStringLiteral("定規の線がどのコマにもかかっていません"), 5000);
            return;
        }
        apply_ops(Json::array({Json{{"op", "cut_frame"}, {"page", page->index.json()}, {"frame_id", target->id}, {"p0", Json::array({r2(x0), r2(y0)})},
                                    {"p1", Json::array({r2(x1), r2(y1)})}}}));
        return;
    }
    const bool closed = !lines.empty() && lines[0].size() > 3 &&
                        std::hypot(lines[0].front().x.value() - lines[0].back().x.value(), lines[0].front().y.value() - lines[0].back().y.value()) < 1.0;
    if (!closed) {
        flash(QStringLiteral("コマにできるのは、直線（割る）か、円・閉じた曲線（その形のコマ）の定規です"), 6000);
        return;
    }
    Json points = Json::array();
    for (std::size_t i = 0; i + 1 < lines[0].size(); ++i) points.push_back(Json::array({r2(lines[0][i].x.value()), r2(lines[0][i].y.value())}));
    apply_ops(Json::array({Json{{"op", "add_frame"}, {"page", page->index.json()}, {"points", points}, {"tolerance_mm", 0.15}}}));
}

void MainWindow::ruler_flag(const std::string& key) {
    const Json* ruler = selected_ruler();
    if (ruler == nullptr) return;
    const bool now = ruler->contains(key) && core::py_truthy((*ruler)[key]);
    apply_ops(Json::array({Json{{"op", "edit_ruler"}, {"page", current_page()->index.json()}, {"id", ruler->value("id", Json())}, {key, !now}}}));
}

void MainWindow::clear_rulers() {
    const core::Page* page = current_page();
    if (page != nullptr && page->rulers.is_array() && !page->rulers.empty()) {
        apply_ops(Json::array({Json{{"op", "delete_ruler"}, {"page", page->index.json()}}}));
        canvas_->selected_ruler_id.reset();
    }
}

void MainWindow::after_prim(const std::string& id) {
    canvas_->selected_prim_id = id;
    choose_tool(QStringLiteral("3d"));
    show_dock(QStringLiteral("定規・3D"));
    guides_->refresh();
}

void MainWindow::add_prim(const std::string& kind, const std::string& prop) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Frame* frame = selected_frame();
    const core::Rect r = frame != nullptr ? frame->rect : page->inner_rect_mm();
    const double rx = r.x.value(), ry = r.y.value(), rw = r.width.value(), rh = r.height.value();
    double cx = rx + rw / 2, cy = ry + rh / 2;
    int count = 0;  // (the next one beside the last, not on top of it)
    if (page->prims.is_array()) {
        for (const Json& p : page->prims) {
            if (frame == nullptr) {
                ++count;
                continue;
            }
            double px = 0, py = 0;
            try {
                if (p.contains("pos") && p["pos"].is_array() && p["pos"].size() >= 2) {
                    px = core::to_float(p["pos"][0]);
                    py = core::to_float(p["pos"][1]);
                }
            } catch (const std::exception&) {
            }
            if (core::contains(*frame, core::Num(px), core::Num(py))) ++count;
        }
    }
    const double shift = 12.0 * count;
    cx += shift;
    cy += shift * 0.5;
    const std::string id = core::new_id();
    Json op;
    if (kind == "mannequin" || kind == "figure") {
        const double height = core::py_round(std::max(30.0, std::min(140.0, rh * 0.8)), 1);
        op = Json{{"op", kind == "mannequin" ? "add_mannequin" : "add_figure"}, {"page", page->index.json()}, {"id", id},
                  {"pos", Json::array({cx, cy + height * 0.05, 0})}, {"height_mm", height}};
    } else if (kind == "head" || kind == "hand") {
        const double side = core::py_round(std::max(15.0, std::min(60.0, std::min(rw, rh) * 0.35)), 1);
        op = Json{{"op", "add_" + kind}, {"page", page->index.json()}, {"id", id}, {"pos", Json::array({cx, cy, 0})}, {"size_mm", side}};
    } else {
        const double side = core::py_round(std::max(15.0, std::min(80.0, std::min(rw, rh) * 0.4)), 1);
        Json size = Json::array({side, side, side});
        if (kind == "floor") size = Json::array({std::min(rw, 200.0), 1, std::min(rw, 200.0)});
        if (kind == "stairs") size = Json::array({side, side, side * 1.4});
        if (kind == "cylinder") size = Json::array({side * 0.8, side * 1.3, side * 0.8});
        if (kind == "cone") size = Json::array({side * 0.8, side * 1.2, side * 0.8});
        if (kind == "prop") {
            size = Json::array({side * 0.8, side * 0.8, side * 0.8});
            if (prop == "door") size = Json::array({side * 0.5, side * 1.1, side * 0.1});
            if (prop == "window") size = Json::array({side, side * 0.8, side * 0.1});
            if (prop == "bed") size = Json::array({side * 0.9, side * 0.45, side * 1.6});
            if (prop == "car") size = Json::array({side * 1.8, side * 0.7, side * 0.9});
            if (prop == "shelf") size = Json::array({side * 0.8, side * 1.1, side * 0.35});
        }
        op = Json{{"op", "add_prim3d"}, {"page", page->index.json()}, {"kind", kind}, {"id", id}, {"pos", Json::array({cx, cy, 0})}, {"size", size}};
        if (!prop.empty()) op["prop"] = prop;
    }
    if (apply_ops(Json::array({op}))) after_prim(id);
}

void MainWindow::import_model() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const QString path = ask::open_path(this, QStringLiteral("3D モデルを読み込む"), QStringLiteral("3D モデル (*.obj *.glb *.gltf *.vrm)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        flash(QStringLiteral("読み込めませんでした（%1）").arg(file.errorString()), 6000, true);
        return;
    }
    const QByteArray raw = file.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();
    page = current_page();  // (the dialog ran the event loop: the page may be another)
    if (page == nullptr) return;
    const core::Frame* frame = selected_frame();
    const core::Rect r = frame != nullptr ? frame->rect : page->inner_rect_mm();
    const std::string id = core::new_id();
    Json op{{"op", "import_model"}, {"page", page->index.json()}};
    if (suffix == QLatin1String("glb") || suffix == QLatin1String("vrm")) {
        op["glb"] = core::b64encode(std::string_view(raw.constData(), static_cast<std::size_t>(raw.size())));
    } else if (suffix == QLatin1String("gltf")) {
        op["gltf"] = QString::fromUtf8(raw).toStdString();
    } else {
        op["obj"] = QString::fromUtf8(raw).toStdString();
    }
    op["id"] = id;
    op["name"] = QFileInfo(path).completeBaseName().toStdString();
    op["pos"] = Json::array({r.x.value() + r.width.value() / 2, r.y.value() + r.height.value() / 2, 0});
    op["size_mm"] = core::py_round(std::min(r.width.value(), r.height.value()) * 0.6, 1);
    if (apply_ops(Json::array({op}))) after_prim(id);
}

void MainWindow::add_scene(const std::string& kind) {
    // a whole background (room, classroom, corridor, street) as one 3D guide, filling the chosen panel
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Frame* frame = selected_frame();
    const core::Rect r = frame != nullptr ? frame->rect : page->inner_rect_mm();
    const Json base = core::prim3d::scene_size(kind);
    const double scale = std::max(0.3, std::min(2.0, r.width.value() / core::to_float(base[0]) * 1.1));
    Json size = Json::array();
    for (const Json& v : base) size.push_back(core::py_round(core::to_float(v) * scale, 1));
    const double depth = core::to_float(size[2]) / 2 + core::prim3d::scene_view(kind).second * 220;
    const std::string id = core::new_id();
    Json op{{"op", "add_scene"}, {"page", page->index.json()}, {"kind", kind}, {"id", id},
            {"pos", Json::array({r.x.value() + r.width.value() / 2, r.y.value() + r.height.value() * 0.5, core::py_round(depth, 1)})}, {"size", size}};
    if (frame != nullptr) op["frame_id"] = frame->id;
    if (apply_ops(Json::array({op}))) after_prim(id);
}

void MainWindow::pose(const std::string& preset) {
    const core::Page* page = current_page();
    if (page == nullptr || !page->prims.is_array()) return;
    const auto figure = [](const Json& p) {
        const std::string k = p.value("kind", std::string());
        return k == "mannequin" || k == "figure";
    };
    const Json* prim = nullptr;
    if (canvas_->selected_prim_id)
        for (const Json& p : page->prims)
            if (figure(p) && p.value("id", std::string()) == *canvas_->selected_prim_id) prim = &p;
    if (prim == nullptr)
        for (const Json& p : page->prims)
            if (prim == nullptr && figure(p)) prim = &p;
    if (prim == nullptr) {
        flash(QStringLiteral("先にデッサン人形を置きます（3D → デッサン人形を置く）"), 3000);
        return;
    }
    if (prim->value("kind", std::string()) == "figure") {
        if (!core::mesh3d::figure_presets().contains(preset)) {
            flash(QStringLiteral("このポーズは棒人形だけのものです"), 3000);
            return;
        }
        apply_ops(Json::array({Json{{"op", "pose_figure"}, {"page", page->index.json()}, {"id", prim->value("id", Json())}, {"preset", preset}}}));
        return;
    }
    apply_ops(Json::array({Json{{"op", "pose_mannequin"}, {"page", page->index.json()}, {"id", prim->value("id", Json())}, {"preset", preset}}}));
}

void MainWindow::trace_prims(bool selected_only) {
    const core::Page* page = current_page();
    const core::Layer* layer = paint_layer();
    if (page == nullptr || layer == nullptr) return;
    if (!page->prims.is_array() || page->prims.empty()) {
        flash(QStringLiteral("このページに 3D がありません"), 2500);
        return;
    }
    Json op{{"op", "trace_prims"}, {"page", page->index.json()}, {"layer_id", layer->id}};
    if (selected_only && canvas_->selected_prim_id) op["ids"] = Json::array({*canvas_->selected_prim_id});
    const QString label = wording::layer_label(*layer);
    if (apply_ops(Json::array({op}))) flash(QStringLiteral("「%1」に線で写しました").arg(label), 3000);
}

void MainWindow::prim_posed(const QString& prim_id, const QString& handle, const Json& to) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const Json* prim = find_by_id(page->prims, prim_id.toStdString());
    const bool figure = prim != nullptr && prim->value("kind", std::string()) == "figure";
    Json drag{{"handle", handle.toStdString()}, {"to", to}};
    static const std::vector<QString> ik_chains{QStringLiteral("l_wrist"), QStringLiteral("l_hand"), QStringLiteral("r_wrist"), QStringLiteral("r_hand"),
                                                QStringLiteral("l_ankle"), QStringLiteral("l_toe"),  QStringLiteral("r_ankle"), QStringLiteral("r_toe")};
    if (figure && std::find(ik_chains.begin(), ik_chains.end(), handle) != ik_chains.end() && guides_->ik->isChecked())
        drag["ik"] = true;  // (IK: the hand or foot pulled, the arm or leg follows)
    apply_ops(Json::array({Json{{"op", figure ? "pose_figure" : "pose_mannequin"}, {"page", page->index.json()}, {"id", prim_id.toStdString()}, {"drag", drag}}}));
}

}  // namespace genko::app
