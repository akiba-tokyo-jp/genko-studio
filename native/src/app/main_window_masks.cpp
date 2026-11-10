// PC3 B2 (masks@1, SPEC.md COLOR-06): the window's commands of the layers' masks and of the layers chosen — ファイル →
// スクリプト → すべてのマスクを統合 (apply_layer_mask on every pixel layer with a mask, one Undo), and 選択範囲 →
// すべてのレイヤー・レイヤーの選択を解除・レイヤーを検索・レイヤーを分離 (the layer panel's, app/layer_panel_masks.cpp).
// レイヤー → レイヤーマスク is the layer panel's mask menu (app/layer_panel.cpp, layer_panel_masks.cpp).

#include <QAction>

#include "app/layer_panel.hpp"
#include "app/main_window.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_mask_actions() {
    make("act_select_all_layers", QStringLiteral("すべてのレイヤー"), [this] { if (layer_panel_ != nullptr) layer_panel_->select_all_layers(); },
         {QKeySequence(QStringLiteral("Ctrl+Alt+A"))}, QStringLiteral("このページのレイヤーをすべて選びます（描く先はそのまま）"));
    make("act_deselect_layers", QStringLiteral("レイヤーの選択を解除"), [this] { if (layer_panel_ != nullptr) layer_panel_->deselect_layers(); }, {},
         QStringLiteral("選んだレイヤーを外し、描く先のレイヤーだけを選んだままにします"));
    make("act_find_layers", QStringLiteral("レイヤーを検索"), [this] { if (layer_panel_ != nullptr) layer_panel_->find_layers(); },
         {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F"))}, QStringLiteral("レイヤーを名前で探します（Enter で、見つかったレイヤーを選びます）"));
    QAction* isolate = make("act_isolate_layers", QStringLiteral("レイヤーを分離"), [] {}, {},
                            QStringLiteral("選んだレイヤーだけをレイヤー パネルに出します（もう一度で全部に戻す）"), true);
    connect(isolate, &QAction::toggled, this, [this](bool on) {
        if (layer_panel_ != nullptr && layer_panel_->isolated() != on) layer_panel_->isolate_layers(on);
    });
    make("act_flatten_masks", QStringLiteral("すべてのマスクを統合"), [this] { flatten_all_masks(); }, {},
         QStringLiteral("このページの、マスクのあるペン・ペイントのレイヤーすべてで、マスクを適用します"));
}

void MainWindow::flatten_all_masks() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    Json ops = Json::array();
    int skipped = 0;
    for (const core::Layer& layer : page->layers) {
        // (a mask in use: one not used stays, as PhotoCraft's script leaves it)
        if (!layer.mask || !layer.mask->png || layer.mask->png->empty() || !layer.mask->enabled) continue;
        if ((layer.kind != core::LayerKind::Raster && layer.kind != core::LayerKind::Strokes) || layer.locked) {
            ++skipped;  // (a layer of another kind, or locked: its mask stays)
            continue;
        }
        ops.push_back(Json{{"op", "apply_layer_mask"}, {"page", page->index.json()}, {"id", layer.id}});
    }
    if (ops.empty()) {
        flash(skipped > 0 ? QStringLiteral("マスクを適用できるペン・ペイントのレイヤーがありません（ロックされているか、ほかの種類のレイヤーです）")
                          : QStringLiteral("このページに、マスクのあるレイヤーはありません"),
              3500);
        return;
    }
    if (apply_ops(ops)) {
        flash(skipped > 0 ? QStringLiteral("%1 枚のレイヤーでマスクを適用しました（%2 枚はそのまま）").arg(ops.size()).arg(skipped)
                          : QStringLiteral("%1 枚のレイヤーでマスクを適用しました").arg(ops.size()),
              3500);
    }
}

}  // namespace genko::app
