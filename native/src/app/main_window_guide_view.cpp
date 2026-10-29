// 表示's guides, grid and extras (PC5 H-b, 節 H 4.4 and 5.1, SPEC.md VIEW-01「ガイド・グリッド・エクストラ」「単位・定規」):
// エクストラ (the master switch of what is drawn over the page to help, Photoshop's Ctrl+H), 表示 → 表示・非表示 ▸ (レイヤーエッジ,
// 選択範囲のエッジ, ターゲットパス, グリッド, ガイド, カンバスガイド, スマートガイド, ピクセルグリッド, ブラシプレビュー, メッシュ,
// 編集ピン — the production's slices, notes and count join it from 節 J —, すべて, なし, エクストラオプションを表示…), ガイドをロック,
// ガイドを消去, カンバスガイドを消去, 新規ガイド…, 新規ガイドレイアウトを作成…; the canvas's guides grabbed and dragged off; the
// preferences of 単位・定規 and ガイド・グリッド・スライス on the canvas. The guides are the page's guide rulers (ops: add_ruler,
// edit_ruler, delete_ruler with its kind, add_guide_layout); the rest is this computer's view, never the book's (Photoshop
// keeps a guide's lock in the file — Genko keeps it here, the coordinator's decision).

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/guide_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/view_units.hpp"
#include "core/prefs_table.hpp"
#include "core/pyops.hpp"

namespace genko::app {

using core::Json;

namespace {

// 表示・非表示 ▸'s own switches of 節 H (kept: view/show_…)
struct ShowItem {
    const char* attribute;
    const char* title;
    const char* key;
    bool fallback;
    bool PageCanvas::*flag;
    const char* tip;
};

const ShowItem kShowItems[] = {
    {"act_show_layer_edges", "レイヤーエッジ", "view/show_layer_edges", false, &PageCanvas::show_layer_edges,
     "選んだレイヤーの描かれた範囲を細い線で囲みます"},
    {"act_show_guides", "ガイド", "view/show_guides", true, &PageCanvas::show_guide_lines, "置いたガイドを表示します"},
    {"act_show_canvas_guides", "カンバスガイド", "view/show_canvas_guides", true, &PageCanvas::show_canvas_guides, "ページに置いたガイドを表示します"},
    {"act_show_smart_guides", "スマートガイド", "view/show_smart_guides", true, &PageCanvas::show_smart_guides,
     "ドラッグ中に、ほかのレイヤーや用紙の中央にそろった線と、両隣と同じになった間隔を表示します"},
    {"act_show_mesh", "メッシュ", "view/show_mesh", true, &PageCanvas::show_mesh, "ワープ・パペットワープ・遠近法ワープの格子と枠を表示します"},
    {"act_show_edit_pins", "編集ピン", "view/show_edit_pins", true, &PageCanvas::show_edit_pins,
     "ワープの点とハンドル、パペットワープのピン、遠近法ワープの角を表示します"},
};

bool truthy(const QVariant& value, bool fallback) {
    if (!value.isValid()) return fallback;
    const QString text = value.toString().toLower();
    return text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("yes") || text == QLatin1String("on");
}

QString words(bool on) { return on ? QStringLiteral("true") : QStringLiteral("false"); }

Qt::PenStyle pen_style(const std::string& style) {
    if (style == "dashedLines") return Qt::DashLine;
    if (style == "dots") return Qt::DotLine;
    return Qt::SolidLine;
}

bool has_guides(const core::Page& page) {
    return page.rulers.is_array() && std::any_of(page.rulers.begin(), page.rulers.end(), [](const Json& r) {
               return r.is_object() && core::py_equals(core::py_get(r, "kind"), Json("guide"));
           });
}

}  // namespace

void MainWindow::build_guide_view_actions() {
    const auto store = settings();
    // エクストラ (Photoshop's Ctrl+H: none in Genko's keys, where it is ヒストリー…)
    QAction* extras = make("act_extras", QStringLiteral("エクストラ"), [] {}, {},
                           QStringLiteral("ガイド・グリッド・選択範囲のエッジなど、表示・非表示 ▸ で入れた補助の表示をまとめて隠す・出す"), true);
    extras->setChecked(truthy(store->value(QStringLiteral("view/extras")), true));
    connect(extras, &QAction::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("view/extras"), words(on));
        apply_extras();
    });
    for (const ShowItem& item : kShowItems) {
        QAction* act = make(QString::fromLatin1(item.attribute), QString::fromUtf8(item.title), [] {}, {}, QString::fromUtf8(item.tip), true);
        act->setChecked(truthy(store->value(QString::fromLatin1(item.key)), item.fallback));
        const QString key = QString::fromLatin1(item.key);
        connect(act, &QAction::toggled, this, [this, key](bool on) {
            settings()->setValue(key, words(on));
            apply_extras();
        });
    }
    make("act_show_all", QStringLiteral("すべて"), [this] {
        for (QAction* act : findChild<QMenu*>(QStringLiteral("menu_show"))->actions()) {
            if (act->isCheckable() && act != action("act_brush_preview") && !act->isChecked()) act->trigger();
        }
        action("act_extras")->setChecked(true);
    }, {}, QStringLiteral("表示・非表示 ▸ の項目をすべて入れます"));
    make("act_show_none", QStringLiteral("なし"), [this] {
        for (QAction* act : findChild<QMenu*>(QStringLiteral("menu_show"))->actions()) {
            if (act->isCheckable() && act != action("act_brush_preview") && act->isChecked()) act->trigger();
        }
    }, {}, QStringLiteral("表示・非表示 ▸ の項目をすべて切ります"));
    make("act_extras_options", QStringLiteral("エクストラオプションを表示…"), [this] { ask_extras_options(); }, {},
         QStringLiteral("エクストラで出す・隠す補助の表示を、まとめて選びます"));
    // the guides
    QAction* lock = make("act_lock_guides", QStringLiteral("ガイドをロック"), [] {}, {}, QStringLiteral("置いたガイドを動かせないようにします（このパソコンの設定）"), true);
    lock->setChecked(truthy(store->value(QStringLiteral("guides/lock")), false));
    connect(lock, &QAction::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("guides/lock"), words(on));
        canvas_->lock_guides = on;
        canvas_->update();
    });
    make("act_clear_guides", QStringLiteral("ガイドを消去"), [this] { clear_guides(true); }, {},
         QStringLiteral("原稿のすべてのページのガイドを消します（描画定規はそのまま）"));
    make("act_clear_canvas_guides", QStringLiteral("カンバスガイドを消去"), [this] { clear_guides(false); }, {},
         QStringLiteral("このページのガイドを消します（描画定規はそのまま）"));
    make("act_new_guide", QStringLiteral("新規ガイド…"), [this] { ask_new_guide(); }, {}, QStringLiteral("向きと位置を数で決めてガイドを置きます"));
    make("act_new_guide_layout", QStringLiteral("新規ガイドレイアウトを作成…"), [this] { ask_guide_layout(); }, {},
         QStringLiteral("列・行・マージンのガイドをまとめて置きます"));
    // the canvas: a guide dragged off, double-clicked; the chosen layers' boxes (レイヤーエッジ: the snap's boxes, drawn on a
    // worker — none until they come)
    connect(canvas_, &PageCanvas::guideRemoved, this, [this](const QString& id) {
        if (const core::Page* page = current_page()) {
            apply_ops(Json::array({Json{{"op", "delete_ruler"}, {"page", page->index.json()}, {"id", id.toStdString()}}}));
        }
    });
    connect(canvas_, &PageCanvas::guideAsked, this, [this](const QString& id) { ask_new_guide(id); });
    canvas_->layer_edges = [this] {
        std::vector<std::array<double, 4>> out;
        const std::vector<std::string> chosen = transform_targets();
        for (const auto& [id, box] : layer_snap_boxes()) {
            if (std::find(chosen.begin(), chosen.end(), id) != chosen.end()) out.push_back(box);
        }
        return out;
    };
    canvas_->lock_guides = lock->isChecked();
    apply_extras();
    apply_guide_prefs();
}

void MainWindow::build_guide_view_menus(QMenu* view) {
    // (ターゲットパス, ブラシプレビュー and 選択範囲のエッジ move from 表示 into 表示・非表示 ▸; グリッド is there too — and in
    // ツール → 定規, as before)
    QMenu* shown = show_menu(view);
    for (const char* name : {"act_show_target_path", "act_brush_preview", "act_sel_edges"}) {
        QAction* act = action(QString::fromLatin1(name));
        view->removeAction(act);
        shown->addAction(act);
    }
    shown->addAction(action("act_grid"));
    for (const ShowItem& item : kShowItems) shown->addAction(action(QString::fromLatin1(item.attribute)));
    for (const char* name : {"act_show_all", "act_show_none", "act_extras_options"}) shown->addAction(action(QString::fromLatin1(name)));
    for (const char* name : {"act_extras", "act_lock_guides", "act_clear_guides", "act_clear_canvas_guides", "act_new_guide", "act_new_guide_layout"})
        view->addAction(action(QString::fromLatin1(name)));
    // (Photoshop's: a switch of 表示・非表示 ▸ turned on while エクストラ is off turns エクストラ on — every part's, 節 J's too,
    // being in it by now)
    for (QAction* act : shown->actions()) {
        if (!act->isCheckable() || act == action("act_brush_preview")) continue;
        connect(act, &QAction::toggled, this, [this](bool on) {
            if (on && !action("act_extras")->isChecked()) action("act_extras")->setChecked(true);
        });
    }
}

void MainWindow::apply_extras() {
    canvas_->extras_shown = action("act_extras")->isChecked();
    for (const ShowItem& item : kShowItems) canvas_->*item.flag = action(QString::fromLatin1(item.attribute))->isChecked();
    if (canvas_->show_layer_edges) (void)layer_snap_boxes();  // (the boxes asked for now: drawn when they come)
    canvas_->update();
}

void MainWindow::apply_guide_prefs() {
    const auto store = settings();
    const auto pref = [&store](const char* path) { return core::prefs::value(*store, path); };
    PageCanvas* c = canvas_;
    c->guide_colour = QColor(QString::fromStdString(pref("guidesGridAndSlices.guideColor").get<std::string>()));
    c->guide_style = pen_style(pref("guidesGridAndSlices.guideStyle").get<std::string>());
    c->smart_colour = QColor(QString::fromStdString(pref("guidesGridAndSlices.smartGuideColor").get<std::string>()));
    c->grid_colour = QColor(QString::fromStdString(pref("guidesGridAndSlices.gridColor").get<std::string>()));
    c->grid_style = pen_style(pref("guidesGridAndSlices.gridStyle").get<std::string>());
    c->grid_subdivisions = pref("guidesGridAndSlices.subdivisions").get<int>();
    if (store->contains(QStringLiteral("guides/grid_mm"))) c->grid_mm = pref("guidesGridAndSlices.gridlineEvery").get<double>();
    c->ruler_unit = view_units::ruler();
    c->points_per_inch = view_units::points_per_inch();
    c->column_pt = view_units::column_pt();
    c->gutter_pt = view_units::gutter_pt();
    c->print_dpi = view_units::screen_dpi();
    c->update();
}

void MainWindow::clear_guides(bool every_page) {
    Json ops = Json::array();
    const auto clear = [&ops](const core::Page& page) {
        if (has_guides(page)) ops.push_back(Json{{"op", "delete_ruler"}, {"page", page.index.json()}, {"kind", "guide"}});
    };
    if (every_page) {
        for (const auto& page : book().pages) clear(*page);
    } else if (const core::Page* page = current_page()) {
        clear(*page);
    }
    if (ops.empty()) {
        flash(every_page ? QStringLiteral("この原稿にガイドはありません") : QStringLiteral("このページにガイドはありません"), 3000);
        return;
    }
    if (apply_ops(ops)) flash(every_page ? QStringLiteral("すべてのページのガイドを消しました") : QStringLiteral("このページのガイドを消しました"), 3000);
}

void MainWindow::ask_new_guide(const QString& moved) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    QString axis;
    std::optional<double> at;
    if (!moved.isEmpty()) {
        for (const Json& r : page->rulers) {
            if (!r.is_object() || r.value("id", Json()) != Json(moved.toStdString())) continue;
            axis = QString::fromStdString(r.value("axis", std::string("h")));
            if (r.value("at", Json()).is_number()) at = r["at"].get<double>();
        }
        if (!at) return;  // (gone meanwhile)
    }
    NewGuideDialog dialog(this, *page, view_units::ruler(), view_units::basis(page, true), view_units::basis(page, false), axis, at);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    page = current_page();
    if (page == nullptr) return;
    if (!moved.isEmpty()) {
        apply_ops(Json::array({Json{{"op", "edit_ruler"}, {"page", page->index.json()}, {"id", moved.toStdString()},
                                    {"axis", dialog.axis().toStdString()}, {"at", dialog.at_mm()}}}));
        return;
    }
    place_ruler(Json{{"kind", "guide"}, {"axis", dialog.axis().toStdString()}, {"at", dialog.at_mm()}});
}

void MainWindow::ask_guide_layout() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    GuideLayoutDialog dialog(this, *page, view_units::ruler(), view_units::basis(page, true), view_units::basis(page, false),
                             [this](std::optional<core::GuideLayout> preview) {
                                 canvas_->guide_preview = std::move(preview);
                                 canvas_->update();
                             });
    const bool chosen = ask::exec(&dialog) == QDialog::Accepted;
    canvas_->guide_preview.reset();
    canvas_->update();
    if (!chosen || current_page() == nullptr) return;
    Json op = dialog.op();
    op["page"] = current_page()->index.json();
    if (apply_ops(Json::array({op}))) flash(QStringLiteral("ガイドレイアウトを置きました"), 3000);
}

void MainWindow::ask_extras_options() {
    // (Photoshop's エクストラオプションを表示: エクストラ and what it shows, chosen together)
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("extras_options"));
    dialog.setWindowTitle(QStringLiteral("エクストラオプション"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* master = new QCheckBox(QStringLiteral("エクストラを表示"));
    master->setObjectName(QStringLiteral("extras_on"));
    master->setChecked(action("act_extras")->isChecked());
    layout->addWidget(master);
    std::vector<std::pair<QAction*, QCheckBox*>> boxes;
    for (QAction* act : findChild<QMenu*>(QStringLiteral("menu_show"))->actions()) {
        if (!act->isCheckable() || act == action("act_brush_preview")) continue;
        auto* box = new QCheckBox(act->text());
        box->setObjectName(QStringLiteral("extras_") + act->property("genko_attribute").toString());
        box->setChecked(act->isChecked());
        layout->addWidget(box);
        boxes.emplace_back(act, box);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    for (const auto& [act, box] : boxes) {
        if (act->isChecked() != box->isChecked()) act->trigger();
    }
    action("act_extras")->setChecked(master->isChecked());
}

}  // namespace genko::app
