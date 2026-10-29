// PC5 I-a (SPEC.md AUTO-01「レイヤーの基本」): the rest of Photoshop's layer basics in the window — レイヤー → 新規 → 背景から
// レイヤーへ…・レイヤーからのグループ…, レイヤーを複製… (its window), 非表示レイヤーを削除, クリッピングマスクを作成・解除, レイヤーの
// グループ解除, and ファイル → スクリプト → すべての空のレイヤーを削除・すべてのレイヤー効果を統合. Each change of the book is an
// op through apply_ops (core/ops_layers.cpp's delete_layers, duplicate_layers, ungroup_layers; render/ops_layer_styles.cpp's
// bake_layer_styles), so Undo, saving and the journal are as for any edit. The names follow Photoshop's (PhotoCraft v0.2.0's
// engine/src/extra_cmds.rs and file_cmds.rs, MIT).

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/ids.hpp"
#include "core/layer_basics.hpp"
#include "core/layer_styles.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

// `command` put in `menu` just after `anchor` (at the end when the menu does not hold it).
void put_after(QMenu* menu, QAction* anchor, QAction* command) {
    const QList<QAction*> all = menu->actions();
    const qsizetype at = all.indexOf(anchor);
    menu->insertAction(at >= 0 && at + 1 < all.size() ? all[at + 1] : nullptr, command);
}

// `command` put in `menu` just before `anchor` (at the end when the menu does not hold it).
void put_before(QMenu* menu, QAction* anchor, QAction* command) { menu->insertAction(menu->actions().contains(anchor) ? anchor : nullptr, command); }

}  // namespace

void MainWindow::build_layer_basic_actions() {
    const auto panel = [this](auto work) {
        return [this, work] {
            if (layer_panel_ != nullptr) work(*layer_panel_);
        };
    };
    make(QStringLiteral("act_layer_from_background"), QStringLiteral("背景からレイヤーへ…"), [this] { layer_from_background(); }, {},
         QStringLiteral("開いた画像の「背景」（画像を統合した層も）を、透明部分・位置のロックの無い普通のレイヤーにします"));
    make(QStringLiteral("act_layer_group_from_layers"), QStringLiteral("レイヤーからのグループ…"), [this] { group_from_layers(); }, {},
         QStringLiteral("選んだレイヤーを、名前を付けたグループにまとめます"));
    make(QStringLiteral("act_layer_ungroup"), QStringLiteral("レイヤーのグループ解除"), panel([](LayerPanel& p) { p.ungroup(); }),
         {QKeySequence(QStringLiteral("Ctrl+Shift+G"))}, QStringLiteral("グループの中のレイヤーをその場に出し、グループを消します（1 手）"));
    make(QStringLiteral("act_layer_delete_hidden"), QStringLiteral("非表示レイヤーを削除"), panel([](LayerPanel& p) { p.delete_hidden(); }), {},
         QStringLiteral("このページの非表示のレイヤーを消します（ネーム・ペン入れ・背景・仕上げ、すべてをロックしたレイヤーは残します）"));
    make(QStringLiteral("act_clip_create"), QStringLiteral("クリッピングマスクを作成"), [this] { clipping_mask(std::nullopt); },
         {QKeySequence(QStringLiteral("Ctrl+Alt+G"))}, QStringLiteral("選んだレイヤーを、すぐ下のレイヤーでクリップします（クリップしたレイヤーでは解除）"));
    make(QStringLiteral("act_clip_release"), QStringLiteral("クリッピングマスクを解除"), [this] { clipping_mask(false); }, {},
         QStringLiteral("選んだレイヤーのクリップを外します"));
    make(QStringLiteral("act_delete_empty_layers"), QStringLiteral("すべての空のレイヤーを削除"), [this] { delete_empty_layers(); }, {},
         QStringLiteral("すべてのページの、何も描いていないレイヤーと空のグループを消します"));
    make(QStringLiteral("act_bake_layer_effects"), QStringLiteral("すべてのレイヤー効果を統合"), [this] { bake_all_layer_effects(); }, {},
         QStringLiteral("すべてのページの、レイヤースタイルと境界効果を持つレイヤーを、その見えのまま画素にします"));
}

void MainWindow::build_layer_basic_menus(QMenu* layers) {
    // レイヤー → 新規 ▸: 背景からレイヤーへ…, レイヤーからのグループ… before レイヤーからのフレーム… (Photoshop's order)
    QMenu* made = nullptr;
    for (QAction* a : layers->actions())
        if (a->menu() != nullptr && a->text() == QStringLiteral("新規")) made = a->menu();
    if (made != nullptr) {
        put_before(made, action(QStringLiteral("act_layer_frame")), action(QStringLiteral("act_layer_from_background")));
        put_before(made, action(QStringLiteral("act_layer_frame")), action(QStringLiteral("act_layer_group_from_layers")));
    }
    // 削除 → 非表示レイヤー after レイヤーを消す; クリッピングマスクを作成・解除 and レイヤーのグループ解除 after まとめて ▸ (its
    // レイヤーをグループ化 Ctrl+G)
    put_after(layers, action(QStringLiteral("act_layer_delete")), action(QStringLiteral("act_layer_delete_hidden")));
    QAction* many = layer_many_menu_ != nullptr ? layer_many_menu_->menuAction() : nullptr;
    QAction* last = many;
    for (const char* name : {"act_clip_create", "act_clip_release", "act_layer_ungroup"}) {
        put_after(layers, last, action(QString::fromLatin1(name)));
        last = action(QString::fromLatin1(name));
    }
}

void MainWindow::duplicate_layer_dialog() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    std::vector<std::string> ids = layer_panel_ != nullptr ? layer_panel_->selected_ids() : std::vector<std::string>{};
    if (std::find(ids.begin(), ids.end(), layer->id) == ids.end()) ids = {layer->id};
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("duplicate_layers"));
    dialog.setWindowTitle(QStringLiteral("レイヤーを複製"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(wording::layer_label(*layer) + QStringLiteral(" のコピー"));
    name->setObjectName(QStringLiteral("duplicate_name"));
    name->setEnabled(ids.size() == 1);  // (several: each "… のコピー")
    form->addRow(ids.size() == 1 ? QStringLiteral("複製:") : QStringLiteral("複製: %1 つのレイヤー").arg(ids.size()), name);
    auto* where = new QComboBox;
    where->setObjectName(QStringLiteral("duplicate_to_page"));
    // (保存先: Photoshop's documents are Genko's pages — this book's, of the same paper and mode)
    for (const core::PagePtr& other : book().pages) {
        if (!other) continue;
        const bool same = other->spec.width_mm == page->spec.width_mm && other->spec.height_mm == page->spec.height_mm && other->color == page->color &&
                          other->rgb == page->rgb;
        if (!same) continue;
        const QString label = QStringLiteral("%1 ページ").arg(QString::fromStdString(other->index.repr())) +
                              (other->id == page->id ? QStringLiteral("（このページ）") : QString());
        where->addItem(label, QString::fromStdString(other->index.json().dump()));
        if (other->id == page->id) where->setCurrentIndex(where->count() - 1);
    }
    form->addRow(QStringLiteral("保存先"), where);
    layout->addLayout(form);
    layout->addWidget(new QLabel(QStringLiteral("グループはその中のレイヤーごと複製します。別の原稿へは、コピーしてペーストします")));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const std::string copy = core::new_id();
    Json op{{"op", "duplicate_layers"}, {"page", page->index.json()}, {"ids", ids}, {"new_ids", Json{{layer->id, copy}}}};
    const Json to = Json::parse(where->currentData().toString().toStdString());
    if (to != page->index.json()) op["to_page"] = to;
    Json ops = Json::array({op});
    const QString given = name->text().trimmed();
    if (ids.size() == 1 && !given.isEmpty() && given != wording::layer_label(*layer) + QStringLiteral(" のコピー"))
        ops.push_back(Json{{"op", "set_layer"}, {"page", to}, {"id", copy}, {"name", given.toStdString()}});
    if (apply_ops(ops) && to == page->index.json()) {
        set_target_layer(copy);
        if (layer_panel_ != nullptr) layer_panel_->refresh();
    }
}

void MainWindow::group_from_layers() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    std::vector<std::string> ids = layer_panel_ != nullptr ? layer_panel_->selected_ids() : std::vector<std::string>{};
    if (ids.empty()) ids = {layer->id};
    const auto asked = asking();
    const auto name = ask::get_text(this, QStringLiteral("レイヤーからのグループ"), QStringLiteral("名前:"), QStringLiteral("グループ"));
    if (!name || !still(asked)) return;
    const QString given = name->trimmed().isEmpty() ? QStringLiteral("グループ") : name->trimmed();
    apply_ops(Json::array({Json{{"op", "group_layers"}, {"page", page->index.json()}, {"ids", ids}, {"id", core::new_id()},
                                {"name", given.toStdString()}, {"group", true}}}));  // (PC5 I-b, the user's decision 2: a group, 通過)
}

void MainWindow::layer_from_background() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    if (core::core_layer(*layer) || layer->kind == core::LayerKind::Folder) {
        flash(QStringLiteral("「%1」は Photoshop の背景ではありません（背景にあたるのは、開いた画像の「背景」と画像を統合した層です）")
                  .arg(wording::layer_label(*layer)),
              6000);
        return;
    }
    const auto asked = asking();
    const auto name = ask::get_text(this, QStringLiteral("新規レイヤー"), QStringLiteral("レイヤー名:"), QStringLiteral("レイヤー 0"));
    if (!name || !still(asked)) return;
    const QString given = name->trimmed().isEmpty() ? QStringLiteral("レイヤー 0") : name->trimmed();
    apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"name", given.toStdString()},
                                {"lock_alpha", false}, {"lock_position", false}, {"lock_all", false}}}));
}

void MainWindow::clipping_mask(std::optional<bool> on) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    std::vector<std::string> ids = layer_panel_ != nullptr ? layer_panel_->selected_ids() : std::vector<std::string>{};
    if (std::find(ids.begin(), ids.end(), layer->id) == ids.end()) ids = {layer->id};
    // (Alt+Ctrl+G: on a clipped layer, Photoshop's same key releases it)
    const bool clip = on.value_or(!layer->clip);
    apply_ops(Json::array({Json{{"op", "set_layers"}, {"page", page->index.json()}, {"ids", ids}, {"clip", clip}}}));
}

void MainWindow::delete_empty_layers() {
    if (current_page() == nullptr) return;
    const auto asked = asking();
    if (!ask::question(this, QStringLiteral("すべての空のレイヤーを削除"),
                       QStringLiteral("すべてのページの、何も描いていないレイヤーと空のグループを消します（ネーム・ペン入れ・背景・仕上げ、すべてを"
                                      "ロックしたレイヤーは残します）。\n（元に戻す で取り消せます）")) ||
        !still(asked))
        return;
    apply_ops(Json::array({Json{{"op", "delete_layers"}, {"page", "all"}, {"empty", true}}}));
}

void MainWindow::bake_all_layer_effects() {
    if (current_page() == nullptr) return;
    const auto asked = asking();
    if (!ask::question(this, QStringLiteral("すべてのレイヤー効果を統合"),
                       QStringLiteral("すべてのページの、レイヤースタイルと境界効果を持つレイヤーを、その見えのまま画素にします（不透明度・描画モード・"
                                      "クリップはそのまま）。\n（元に戻す で取り消せます）")) ||
        !still(asked))
        return;
    apply_ops(Json::array({Json{{"op", "bake_layer_styles"}, {"page", "all"}}}));
}

}  // namespace genko::app
