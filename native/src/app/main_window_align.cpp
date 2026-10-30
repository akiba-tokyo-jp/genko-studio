// 移動・整列・分布・重ね順・リンク・吸着 in the window (PC3 C-d, SPEC.md COLOR-07「移動・整列」, the user's decision 5:
// Photoshop's names): レイヤー → 重ね順 ▸ (最前面へ Ctrl+Shift+], 前面へ Ctrl+], 背面へ Ctrl+[, 最背面へ Ctrl+Shift+[, 逆順),
// 整列 ▸ (上端・垂直方向中央・下端・左端・水平方向中央・右端, and Genko's own bases: 仕上がり・基本枠・選んだコマ), 分布 ▸ (上端・
// 垂直方向中央・下端・左端・水平方向中央・右端・水平方向・垂直方向), レイヤーをリンク, リンクしたレイヤーを選択, レイヤーをロック…;
// 表示 → スナップ (Ctrl+Shift+;) and スナップ先 ▸ (ガイド・グリッド・レイヤー・ドキュメントの境界・すべて・なし); the move tool's
// page (自動選択 and its target, バウンディングボックスを表示), its drag (transform_layers' move by whole working pixels: the
// layers chosen, their folders' and linked layers with them) and Ctrl+click (the layer showing there). What they act on:
// the layers chosen in the layer panel (else the layer drawn on) and, for 整列 and 分布, the line chosen too (the user's
// 調整役の判断: layers and balloons by one op).

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>
#include <set>
#include <tuple>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/pynum.hpp"
#include "render/layer_align.hpp"
#include "render/layer_bounds.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace lb = render::layer_bounds;
namespace la = render::layer_align;

namespace {

// 整列 ▸ and 分布 ▸: each command's key in the op and its words (Photoshop's).
const std::vector<std::pair<const char*, const char*>>& edges() {
    static const std::vector<std::pair<const char*, const char*>> all{{"top", "上端"},  {"vcenter", "垂直方向中央"}, {"bottom", "下端"},
                                                                       {"left", "左端"}, {"hcenter", "水平方向中央"}, {"right", "右端"}};
    return all;
}

// 整列 ▸'s bases (the align op's `to`): the chosen layers and lines together (one alone: the paper, or the selection),
// and Genko's own.
const std::vector<std::tuple<const char*, const char*, const char*>>& bases() {
    static const std::vector<std::tuple<const char*, const char*, const char*>> all{
        {"auto", "act_align_to_auto", "選んだもの・選択範囲・用紙に"},
        {"trim", "act_align_to_trim", "仕上がりに"},
        {"frame", "act_align_to_frame", "基本枠に"},
        {"panel", "act_align_to_panel", "選んだコマに"}};
    return all;
}

}  // namespace

// --- the commands -----------------------------------------------------------------------------------------------------

void MainWindow::build_align_actions() {
    // レイヤー → 重ね順 (前面へ・背面へ are the layer panel's: main_window_materials.cpp, arrange_layers now)
    make("act_layer_front", QStringLiteral("最前面へ"), [this] { arrange_layers("front"); }, {QKeySequence(QStringLiteral("Ctrl+Shift+]"))},
         QStringLiteral("選んだレイヤーを、同じフォルダーの中でいちばん上に重ねます"));
    make("act_layer_back", QStringLiteral("最背面へ"), [this] { arrange_layers("back"); }, {QKeySequence(QStringLiteral("Ctrl+Shift+["))},
         QStringLiteral("選んだレイヤーを、同じフォルダーの中でいちばん下に重ねます"));
    make("act_layer_reverse", QStringLiteral("逆順"), [this] { arrange_layers("reverse"); }, {},
         QStringLiteral("選んだ 2 つ以上のレイヤーの重なりの順を逆にします"));
    // 整列 ▸ and its bases, 分布 ▸
    for (const auto& [key, words] : edges()) {
        const std::string edge = key;
        make(QStringLiteral("act_align_%1").arg(QString::fromLatin1(key)), QString::fromUtf8(words), [this, edge] { align_layers(edge.c_str()); }, {},
             QStringLiteral("選んだレイヤー・台詞の%1をそろえます（1 つだけなら用紙か選択範囲に）").arg(QString::fromUtf8(words)));
    }
    auto* base_group = new QActionGroup(this);
    base_group->setExclusive(true);
    for (const auto& [key, attribute, words] : bases()) {
        QAction* act = make(QString::fromLatin1(attribute), QString::fromUtf8(words), [this] {
            const auto store = settings();
            for (const auto& [k, a, w] : bases())
                if (action(QString::fromLatin1(a))->isChecked()) store->setValue(QStringLiteral("align/to"), QString::fromLatin1(k));
        }, {}, QStringLiteral("整列でそろえる先"), true);
        base_group->addAction(act);
    }
    {
        const QString chosen = settings()->value(QStringLiteral("align/to"), QStringLiteral("auto")).toString();
        for (const auto& [key, attribute, words] : bases()) {
            QAction* act = action(QString::fromLatin1(attribute));
            const QSignalBlocker quiet(act);
            act->setChecked(chosen == QLatin1String(key));
        }
        if (base_group->checkedAction() == nullptr) action(QStringLiteral("act_align_to_auto"))->setChecked(true);
    }
    for (const auto& [key, words] : edges()) {
        const std::string by = key;
        make(QStringLiteral("act_distribute_%1").arg(QString::fromLatin1(key)), QString::fromUtf8(words), [this, by] { distribute_layers(by.c_str()); }, {},
             QStringLiteral("選んだ 3 つ以上のレイヤー・台詞の%1を等しい間隔にします（両端の 2 つは動きません）").arg(QString::fromUtf8(words)));
    }
    make("act_distribute_horizontal", QStringLiteral("水平方向"), [this] { distribute_layers("horizontal"); }, {},
         QStringLiteral("選んだ 3 つ以上のレイヤー・台詞の左右のすき間を等しくします（両端の 2 つは動きません）"));
    make("act_distribute_vertical", QStringLiteral("垂直方向"), [this] { distribute_layers("vertical"); }, {},
         QStringLiteral("選んだ 3 つ以上のレイヤー・台詞の上下のすき間を等しくします（両端の 2 つは動きません）"));
    // リンク・ロック
    make("act_link_layers", QStringLiteral("レイヤーをリンク"), [this] { link_layers(); }, {},
         QStringLiteral("選んだレイヤーをリンクします（移動・変形で一緒に動きます）。リンクしたものを選んでもう一度で外します"));
    make("act_select_linked", QStringLiteral("リンクしたレイヤーを選択"), [this] { select_linked_layers(); }, {},
         QStringLiteral("描く先のレイヤーとリンクしたレイヤーをすべて選びます"));
    make("act_lock_layers", QStringLiteral("レイヤーをロック…"), [this] { lock_layers_dialog(); }, {},
         QStringLiteral("選んだレイヤーの透明部分・画像・位置をロックします"));
    // 表示 → スナップ, スナップ先 ▸ (the grid's is ツール → 定規's グリッドにスナップ too: the same switch)
    make("act_snap_on", QStringLiteral("スナップ"), [this] { snap_toggles(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+;"))},
         QStringLiteral("移動・自由変形・切り抜き・シェイプ・ペンツールの点を、ガイドやレイヤーの端に吸い付けます（Ctrl を押している間は吸い付きません）"), true);
    make("act_snap_guides", QStringLiteral("ガイド"), [this] { snap_toggles(); }, {}, QStringLiteral("ガイド線に吸い付けます"), true);
    make("act_snap_grid", QStringLiteral("グリッド"), [this] {
        QAction* grid = action(QStringLiteral("act_grid_snap"));
        grid->setChecked(action(QStringLiteral("act_snap_grid"))->isChecked());
        guide_toggles();
    }, {}, QStringLiteral("グリッドに吸い付けます（定規 → グリッドにスナップ と同じ）"), true);
    make("act_snap_layers", QStringLiteral("レイヤー"), [this] { snap_toggles(); }, {}, QStringLiteral("ほかのレイヤーの端と中央に吸い付けます"), true);
    // (PC5 J-b: スライス — the user and layer-based slices' edges)
    make("act_snap_slices", QStringLiteral("スライス"), [this] { snap_toggles(); }, {}, QStringLiteral("スライスの端に吸い付けます"), true);
    make("act_snap_bounds", QStringLiteral("ドキュメントの境界"), [this] { snap_toggles(); }, {}, QStringLiteral("用紙の端と中央に吸い付けます"), true);
    const auto every = [this](bool on) {
        for (const char* name : {"act_snap_guides", "act_snap_layers", "act_snap_slices", "act_snap_bounds", "act_snap_grid", "act_grid_snap"}) {
            QAction* act = action(QString::fromLatin1(name));
            const QSignalBlocker quiet(act);
            act->setChecked(on);
        }
        guide_toggles();
        snap_toggles();
    };
    make("act_snap_all", QStringLiteral("すべて"), [every] { every(true); }, {}, QStringLiteral("スナップ先をすべて入れます"));
    make("act_snap_none", QStringLiteral("なし"), [every] { every(false); }, {}, QStringLiteral("スナップ先をすべて切ります"));
    {
        const auto store = settings();
        for (const auto& [name, key] : {std::pair{"act_snap_on", "on"}, {"act_snap_guides", "guides"}, {"act_snap_layers", "layers"}, {"act_snap_slices", "slices"}, {"act_snap_bounds", "bounds"}}) {
            QAction* act = action(QString::fromLatin1(name));
            const QSignalBlocker quiet(act);
            act->setChecked(store->value(QStringLiteral("snap/%1").arg(QString::fromLatin1(key)), QStringLiteral("true")).toString().toLower() == QLatin1String("true"));
        }
        QAction* grid = action(QStringLiteral("act_snap_grid"));
        const QSignalBlocker quiet(grid);
        grid->setChecked(action(QStringLiteral("act_grid_snap"))->isChecked());
    }
    connect(action(QStringLiteral("act_grid_snap")), &QAction::toggled, this, [this](bool on) {  // (the one switch, two places)
        QAction* grid = action(QStringLiteral("act_snap_grid"));
        const QSignalBlocker quiet(grid);
        grid->setChecked(on);
    });
    // the move tool's options (its page: build_move_page)
    make("act_move_auto_select", QStringLiteral("自動選択"), [this] {
        canvas_->auto_select = action(QStringLiteral("act_move_auto_select"))->isChecked();
        settings()->setValue(QStringLiteral("move/auto_select"), canvas_->auto_select);
    }, {}, QStringLiteral("クリックした所に描いてあるレイヤーを選んで動かします（切っているときは Ctrl+クリックで）"), true);
    make("act_move_show_box", QStringLiteral("バウンディングボックスを表示"), [this] {
        canvas_->show_move_box = action(QStringLiteral("act_move_show_box"))->isChecked();
        settings()->setValue(QStringLiteral("move/show_box"), canvas_->show_move_box);
        canvas_->update();
    }, {}, QStringLiteral("動かすレイヤーを囲む枠を出します。枠の □ をドラッグすると自由変形になります"), true);
    {
        const auto store = settings();
        for (const auto& [name, key] : {std::pair{"act_move_auto_select", "auto_select"}, {"act_move_show_box", "show_box"}}) {
            QAction* act = action(QString::fromLatin1(name));
            const QSignalBlocker quiet(act);
            act->setChecked(store->value(QStringLiteral("move/%1").arg(QString::fromLatin1(key)), QStringLiteral("false")).toString().toLower() == QLatin1String("true"));
        }
        canvas_->auto_select = action(QStringLiteral("act_move_auto_select"))->isChecked();
        canvas_->auto_select_group = store->value(QStringLiteral("move/auto_group"), QStringLiteral("false")).toString().toLower() == QLatin1String("true");
        canvas_->show_move_box = action(QStringLiteral("act_move_show_box"))->isChecked();
    }
    // the canvas's side: the boxes it snaps to, the move tool's box, 自動選択, the box's □
    canvas_->snap_boxes = [this] { return layer_snap_boxes(); };
    canvas_->moving_box = [this] { return move_box(); };
    connect(canvas_, &PageCanvas::layerPicked, this, [this](double x, double y, bool group) { pick_layer_at(x, y, group); });
    connect(canvas_, &PageCanvas::moveBoxGrabbed, this, [this] { start_layer_transform(QStringLiteral("free")); });
    connect(tools_, &QActionGroup::triggered, this, [this](QAction* act) {
        // (the tools that snap: their layers' boxes drawn now, ready for the first drag)
        for (const char* name : {"act_move", "act_crop_tool", "act_shape", "act_path_pen", "act_curvature_pen"})
            if (act == action(QString::fromLatin1(name))) (void)layer_snap_boxes();
    });
    snap_toggles();
}

void MainWindow::build_align_menus(QMenu* layers, QMenu* view) {
    QMenu* align = layers->addMenu(QStringLiteral("整列"));
    for (const auto& [key, words] : edges()) align->addAction(action(QStringLiteral("act_align_%1").arg(QString::fromLatin1(key))));
    align->addSeparator();
    for (const auto& [key, attribute, words] : bases()) align->addAction(action(QString::fromLatin1(attribute)));
    QMenu* spread = layers->addMenu(QStringLiteral("分布"));
    for (const auto& [key, words] : edges()) spread->addAction(action(QStringLiteral("act_distribute_%1").arg(QString::fromLatin1(key))));
    spread->addSeparator();
    for (const char* name : {"act_distribute_horizontal", "act_distribute_vertical"}) spread->addAction(action(QString::fromLatin1(name)));
    for (const char* name : {"act_link_layers", "act_select_linked", "act_lock_layers"}) layers->addAction(action(QString::fromLatin1(name)));
    view->addSeparator();
    view->addAction(action("act_snap_on"));
    QMenu* to = view->addMenu(QStringLiteral("スナップ先"));
    for (const char* name : {"act_snap_guides", "act_snap_grid", "act_snap_layers", "act_snap_slices", "act_snap_bounds"}) to->addAction(action(QString::fromLatin1(name)));
    to->addSeparator();
    for (const char* name : {"act_snap_all", "act_snap_none"}) to->addAction(action(QString::fromLatin1(name)));
}

void MainWindow::build_move_page(ToolSettings* ts) {
    auto* target = new QComboBox;
    target->setObjectName(QStringLiteral("move_auto_target"));
    target->addItem(QStringLiteral("レイヤー"), false);
    target->addItem(QStringLiteral("グループ（いちばん外のフォルダー）"), true);
    target->setCurrentIndex(canvas_->auto_select_group ? 1 : 0);
    target->setToolTip(QStringLiteral("自動選択で選ぶもの"));
    connect(target, &QComboBox::activated, this, [this, target](int) {
        canvas_->auto_select_group = target->currentData().toBool();
        settings()->setValue(QStringLiteral("move/auto_group"), canvas_->auto_select_group);
    });
    std::vector<QAction*> aligns, distributes;  // (整列 ▸ and 分布 ▸ by one button each)
    for (const auto& [key, words] : edges()) {
        aligns.push_back(action(QStringLiteral("act_align_%1").arg(QString::fromLatin1(key))));
        distributes.push_back(action(QStringLiteral("act_distribute_%1").arg(QString::fromLatin1(key))));
    }
    std::vector<QAction*> base_actions;
    for (const auto& [key, attribute, words] : bases()) base_actions.push_back(action(QString::fromLatin1(attribute)));
    ts->add({QStringLiteral("move")},
            action_page({QStringLiteral("移動"), action("act_move_auto_select"), static_cast<QWidget*>(target), action("act_move_show_box"),
                         action("act_snap_on"), QStringLiteral("整列・分布"), static_cast<QWidget*>(menu_button(QStringLiteral("整列"), {aligns, base_actions})),
                         static_cast<QWidget*>(menu_button(QStringLiteral("分布"), {distributes, {action("act_distribute_horizontal"), action("act_distribute_vertical")}})),
                         QStringLiteral("レイヤー"), action("act_layer_dup"), action("act_select_all"), action("act_link_layers")}));
}

void MainWindow::snap_toggles() {
    canvas_->snap_on = action(QStringLiteral("act_snap_on"))->isChecked();
    canvas_->snap_guides = action(QStringLiteral("act_snap_guides"))->isChecked();
    canvas_->snap_layers = action(QStringLiteral("act_snap_layers"))->isChecked();
    canvas_->snap_bounds = action(QStringLiteral("act_snap_bounds"))->isChecked();
    canvas_->snap_slices = action(QStringLiteral("act_snap_slices"))->isChecked();  // (PC5 J-b)
    const auto store = settings();
    for (const auto& [name, key] : {std::pair{"act_snap_on", "on"}, {"act_snap_guides", "guides"}, {"act_snap_layers", "layers"}, {"act_snap_slices", "slices"}, {"act_snap_bounds", "bounds"}})
        store->setValue(QStringLiteral("snap/%1").arg(QString::fromLatin1(key)), action(QString::fromLatin1(name))->isChecked());
    canvas_->update();
}

// --- what they act on ---------------------------------------------------------------------------------------------------

std::vector<std::string> MainWindow::move_targets() const {
    const core::Page* page = current_page();
    if (page == nullptr) return {};
    std::vector<std::string> out;
    try {
        for (const std::string& id : la::with_links(*page, la::top_level(*page, transform_targets()))) {
            if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
            for (const std::string& inner : lb::inside(*page, id))
                if (std::find(out.begin(), out.end(), inner) == out.end()) out.push_back(inner);
        }
    } catch (const std::exception&) {
    }
    return out;
}

std::vector<std::pair<std::string, std::array<double, 4>>> MainWindow::layer_snap_boxes() {
    const DocPtr& doc = canvas_->doc();
    const std::size_t index = canvas_->page_index();
    if (!doc || index >= doc->pages.size() || doc->is_deferred(index)) return {};
    const core::PagePtr& page = doc->pages[index];
    if (snap_boxes_.page.lock() == page) return snap_boxes_.ready ? snap_boxes_.boxes : std::vector<std::pair<std::string, std::array<double, 4>>>{};
    // (the page as it is now: its boxes drawn on a worker; until they come, none)
    snap_boxes_ = SnapBoxes{page, {}, false};
    const std::uint64_t ticket = ++snap_ticket_;
    using Boxes = std::vector<std::pair<std::string, std::array<double, 4>>>;
    auto* watcher = new QFutureWatcher<Boxes>(this);
    connect(watcher, &QFutureWatcher<Boxes>::finished, this, [this, watcher, ticket] {
        Boxes boxes = watcher->result();
        watcher->deleteLater();
        if (ticket != snap_ticket_) return;
        snap_boxes_.boxes = std::move(boxes);
        snap_boxes_.ready = true;
        canvas_->update();
    });
    watcher->setFuture(QtConcurrent::run([doc, index]() -> Boxes {
        Boxes out;
        try {
            const core::Page& p = doc->page(index);
            std::map<std::string, const core::Layer*> by_id;
            for (const core::Layer& layer : p.layers) by_id[layer.id] = &layer;
            for (const core::Layer& layer : p.layers) {
                if (layer.kind == core::LayerKind::Folder) continue;  // (a folder's layers have their own)
                bool shown = true;  // (the layer and every folder it is in)
                std::set<std::string> seen;
                for (const core::Layer* up = &layer; up != nullptr && seen.insert(up->id).second;) {
                    shown = shown && up->visible;
                    const auto parent = up->parent_id.is_string() ? by_id.find(up->parent_id.get<std::string>()) : by_id.end();
                    up = parent != by_id.end() ? parent->second : nullptr;
                }
                if (!shown) continue;
                if (const auto box = lb::of_layer(*doc, p, layer.id)) out.emplace_back(layer.id, *box);
            }
        } catch (const std::exception&) {
        }
        return out;
    }));
    return {};
}

std::optional<std::array<double, 4>> MainWindow::move_box() {
    const std::vector<std::string> moved = move_targets();
    if (moved.empty()) return std::nullopt;
    std::vector<std::array<double, 4>> boxes;
    for (const auto& [id, box] : layer_snap_boxes())
        if (std::find(moved.begin(), moved.end(), id) != moved.end()) boxes.push_back(box);
    return core::snap::united(boxes);
}

// --- 重ね順・整列・分布 ------------------------------------------------------------------------------------------------

bool MainWindow::arrange_layers(const char* to) {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (page == nullptr || ids.empty()) return false;
    if (std::string(to) == "reverse" && ids.size() < 2) {
        flash(QStringLiteral("逆順にするレイヤーを、レイヤー パネルで 2 つ以上選びます"), 3000);
        return false;
    }
    std::vector<core::Layer> layers = page->layers;  // (nothing moves: nothing sent)
    try {
        if (!la::arrange(layers, la::top_level(*page, ids), to)) {
            flash(QStringLiteral("これ以上は動かせません"), 2500);
            return false;
        }
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return false;
    }
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    return apply_ops(Json::array({Json{{"op", "arrange_layers"}, {"page", page->index.json()}, {"ids", list}, {"to", to}}}));
}

bool MainWindow::align_layers(const char* edge) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    Json ids = Json::array();
    for (const std::string& id : transform_targets()) ids.push_back(id);
    Json op{{"op", "align_layers"}, {"page", page->index.json()}, {"ids", ids}, {"edge", edge}};
    if (canvas_->selected_line_id) op["lines"] = Json::array({*canvas_->selected_line_id});
    if (ids.empty() && !op.contains("lines")) {
        flash(QStringLiteral("先にそろえるレイヤーか台詞を選びます"), 3000);
        return false;
    }
    std::string to = "auto";
    for (const auto& [key, attribute, words] : bases())
        if (action(QString::fromLatin1(attribute))->isChecked()) to = key;
    op["to"] = to;
    if (to == "auto") {
        if (const auto area = selection_area()) op["area"] = *area;
    } else if (to == "panel") {
        const core::Frame* frame = selected_frame();
        if (frame == nullptr) {
            flash(QStringLiteral("先にそろえる先のコマを選びます（選択ツールでコマをクリック）"), 3500);
            return false;
        }
        op["frame_id"] = frame->id;
    }
    return apply_ops(Json::array({op}));
}

bool MainWindow::distribute_layers(const char* by) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    Json ids = Json::array();
    for (const std::string& id : transform_targets()) ids.push_back(id);
    Json op{{"op", "distribute_layers"}, {"page", page->index.json()}, {"ids", ids}, {"by", by}};
    if (canvas_->selected_line_id) op["lines"] = Json::array({*canvas_->selected_line_id});
    if (ids.size() + (op.contains("lines") ? 1 : 0) < 3) {
        flash(QStringLiteral("分布するレイヤー・台詞を、レイヤー パネルで 3 つ以上選びます"), 3000);
        return false;
    }
    return apply_ops(Json::array({op}));
}

// --- リンク・ロック -----------------------------------------------------------------------------------------------------

bool MainWindow::link_layers() {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (page == nullptr || ids.empty()) return false;
    bool unlink = false;
    try {
        unlink = la::unlinks(*page, ids);
    } catch (const std::exception&) {
        return false;
    }
    if (!unlink && ids.size() < 2) {
        flash(QStringLiteral("リンクするレイヤーを、レイヤー パネルで 2 つ以上選びます（Ctrl+クリック）"), 3500);
        return false;
    }
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    if (!apply_ops(Json::array({Json{{"op", "link_layers"}, {"page", page->index.json()}, {"ids", list}}}))) return false;
    flash(unlink ? QStringLiteral("リンクを外しました") : QStringLiteral("%1 つのレイヤーをリンクしました（一緒に動きます）").arg(ids.size()), 3000);
    return true;
}

bool MainWindow::select_linked_layers() {
    const core::Page* page = current_page();
    const core::Layer* target = target_layer();
    if (page == nullptr || target == nullptr || layer_panel_ == nullptr) return false;
    if (!target->link) {
        flash(QStringLiteral("描く先のレイヤーは、どのレイヤーともリンクしていません"), 3000);
        return false;
    }
    std::vector<std::string> ids;
    for (const core::Layer& layer : page->layers)
        if (layer.link == target->link) ids.push_back(layer.id);
    layer_panel_->select_ids(ids);
    return true;
}

void MainWindow::lock_layers_dialog() {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (page == nullptr || ids.empty()) return;
    std::vector<const core::Layer*> chosen;
    for (const core::Layer& layer : page->layers)
        if (std::find(ids.begin(), ids.end(), layer.id) != ids.end()) chosen.push_back(&layer);
    const auto all = [&](auto has) { return !chosen.empty() && std::all_of(chosen.begin(), chosen.end(), has); };
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("lock_layers"));
    dialog.setWindowTitle(QStringLiteral("レイヤーをロック"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("選んだ %1 つのレイヤーのロック").arg(chosen.size())));
    auto* every = new QCheckBox(QStringLiteral("すべて"));
    every->setObjectName(QStringLiteral("lock_all"));
    auto* alpha = new QCheckBox(QStringLiteral("透明部分"));
    alpha->setObjectName(QStringLiteral("lock_alpha"));
    alpha->setToolTip(QStringLiteral("描いてない所に描けなくします（透明部分の保護）"));
    auto* pixels = new QCheckBox(QStringLiteral("画像"));
    pixels->setObjectName(QStringLiteral("lock_pixels"));
    pixels->setToolTip(QStringLiteral("描く・消すをできなくします（レイヤーのロック）"));
    auto* position = new QCheckBox(QStringLiteral("位置"));
    position->setObjectName(QStringLiteral("lock_position"));
    position->setToolTip(QStringLiteral("移動・変形・整列で動かないようにします"));
    // (PC5 I-a, layer-locks@1: Photoshop's アートボードの内外への自動ネストを防ぐ — kept for PSD and .pcraft: Genko's artboards are
    // pages — and すべて its own lock: the others shown on and not to be changed while it is on)
    auto* nest = new QCheckBox(QStringLiteral("アートボードの内外への自動ネストを防ぐ"));
    nest->setObjectName(QStringLiteral("lock_nest"));
    nest->setToolTip(QStringLiteral("Photoshop で開いたときのための設定です（Genko のアートボードはページなので、ここでは何も変わりません）"));
    every->setToolTip(QStringLiteral("描く・動かす・不透明度や描画モードを変える・削除をできなくします（表示・名前・重ね順は変えられます）"));
    alpha->setChecked(all([](const core::Layer* l) { return l->lock_alpha; }));
    pixels->setChecked(all([](const core::Layer* l) { return l->locked; }));
    position->setChecked(all([](const core::Layer* l) { return l->lock_position; }));
    nest->setChecked(all([](const core::Layer* l) { return l->lock_nest; }));
    every->setChecked(all([](const core::Layer* l) { return l->lock_all; }));
    const auto shown = [=] {
        for (QCheckBox* box : {alpha, pixels, position, nest}) box->setEnabled(!every->isChecked());
    };
    connect(every, &QCheckBox::clicked, &dialog, [=](bool on) {
        for (QCheckBox* box : {alpha, pixels, position}) box->setChecked(on);
        shown();
    });
    shown();
    for (QCheckBox* box : {every, alpha, pixels, position, nest}) layout->addWidget(box);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    apply_ops(Json::array({Json{{"op", "set_layers"}, {"page", page->index.json()}, {"ids", list}, {"lock_alpha", alpha->isChecked()},
                                {"locked", pixels->isChecked()}, {"lock_position", position->isChecked()}, {"lock_nest", nest->isChecked()},
                                {"lock_all", every->isChecked()}}}));
}

// --- the move tool ------------------------------------------------------------------------------------------------------

bool MainWindow::pick_layer_at(double x_mm, double y_mm, bool group) {
    const core::Page* page = canvas_->page();  // (the page as the canvas draws it, of the book it draws)
    const DocPtr& doc = canvas_->doc();
    if (page == nullptr || !doc) return false;
    // (the layers whose box holds the point, if the boxes are there; then the topmost showing something at the point)
    std::set<std::string> under;
    const auto boxes = layer_snap_boxes();
    for (const auto& [id, b] : boxes)
        if (x_mm >= b[0] && x_mm <= b[2] && y_mm >= b[1] && y_mm <= b[3]) under.insert(id);
    std::optional<std::string> hit;
    try {
        std::map<std::string, const core::Layer*> by_id;
        for (const core::Layer& layer : page->layers) by_id[layer.id] = &layer;
        for (auto it = page->layers.rbegin(); it != page->layers.rend() && !hit; ++it) {
            if (!boxes.empty() && !under.contains(it->id)) continue;
            bool shown = true;
            std::set<std::string> seen;
            for (const core::Layer* up = &*it; up != nullptr && seen.insert(up->id).second;) {
                shown = shown && up->visible;
                const auto parent = up->parent_id.is_string() ? by_id.find(up->parent_id.get<std::string>()) : by_id.end();
                up = parent != by_id.end() ? parent->second : nullptr;
            }
            if (shown && lb::shows_at(*doc, *page, *it, x_mm, y_mm)) hit = it->id;
        }
    } catch (const std::exception&) {
        return false;
    }
    if (!hit) return false;
    const std::string id = group ? lb::outermost(*page, *hit) : *hit;
    set_target_layer(id);
    if (layer_panel_ != nullptr) layer_panel_->select_ids({id});
    return true;
}

}  // namespace genko::app
