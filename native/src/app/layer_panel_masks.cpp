// PC3 B2 (masks@1, SPEC.md COLOR-06): the layer panel's part of レイヤー → レイヤーマスク past the plain set_layer_mask —
// 選択範囲をマスク, 透明部分から, 適用, リンク — and the layers chosen in the list: 選択範囲 → すべてのレイヤー,
// レイヤーの選択を解除, レイヤーを検索, レイヤーを分離. Genko always draws on one layer (the list's current row): the
// choice of several is the list's selection around it, and the layer drawn on stays chosen.

#include <QAction>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>

#include <algorithm>

#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

void LayerPanel::hide_selection() {
    const auto area = window_->selection_area();
    if (!area) {
        window_->flash(QStringLiteral("先に選択範囲ツール（M・L・W）で隠したい所を選びます"), 5000);
        return;
    }
    mask(Json{{"area", *area}, {"invert", true}});
    window_->canvas()->set_selection(std::nullopt);  // (made: the selection let go, as Photoshop does)
}

void LayerPanel::mask_from_transparency() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr || loading_) return;
    Json op{{"op", "mask_from_transparency"}, {"page", page->index.json()}, {"id", layer->id}};
    if (!layer->mask) op["linked"] = true;  // (a new mask moves with its layer)
    window_->apply_ops(Json::array({op}));
    selected(false);
}

void LayerPanel::apply_mask() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr || loading_) return;
    if (!layer->mask) {
        window_->flash(QStringLiteral("このレイヤーにはマスクがありません"), 3000);
        return;
    }
    window_->apply_ops(Json::array({Json{{"op", "apply_layer_mask"}, {"page", page->index.json()}, {"id", layer->id}}}));
    selected(false);
}

// (PC5 I-a: through select_ids, the panel's one place of its choice)
std::vector<std::string> LayerPanel::shown_ids() const {
    std::vector<std::string> out;
    for (int row = 0; row < list_->count() && row < static_cast<int>(ids_.size()); ++row) {
        const QListWidgetItem* item = list_->item(row);
        if (item != nullptr && !item->isHidden() && row_kind(row) == kLayerRow) out.push_back(ids_[static_cast<std::size_t>(row)]);
    }
    return out;
}

void LayerPanel::select_all_layers() {
    if (list_->count() == 0) return;
    select_ids(shown_ids(), SelectMode::Add);
}

void LayerPanel::deselect_layers() {
    const int row = list_->currentRow();  // (the layer drawn on stays chosen)
    select_ids(row >= 0 && row < static_cast<int>(ids_.size()) ? std::vector<std::string>{ids_[static_cast<std::size_t>(row)]} : std::vector<std::string>{});
}

void LayerPanel::find_layers() {
    search_->setVisible(true);
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
    window_->flash(QStringLiteral("レイヤーの名前を入れます。Enter で、見つかったレイヤーを選びます"), 4000);
}

void LayerPanel::choose_found() {
    if (search_->text().trimmed().isEmpty()) return;
    std::optional<int> first;
    const std::vector<std::string> found = shown_ids();
    select_ids(found);
    for (int row = 0; row < list_->count() && !found.empty(); ++row) {
        if (row_kind(row) == kLayerRow && !list_->item(row)->isHidden()) {
            first = row;
            break;
        }
    }
    if (!first) {
        window_->flash(QStringLiteral("その名前のレイヤーはありません"), 3000);
        return;
    }
    // (the layer drawn on: the first found, unless it is one of them)
    const QListWidgetItem* current = list_->currentItem();
    if (current == nullptr || current->isHidden() || !current->isSelected()) {
        list_->setCurrentRow(*first, QItemSelectionModel::NoUpdate);
        selected();
    }
}

void LayerPanel::isolate_layers(bool on) {
    const core::Page* page = window_->current_page();
    isolated_.clear();
    isolated_page_.clear();
    if (on && page != nullptr) {
        isolated_ = selected_ids();
        if (const core::Layer* target = window_->target_layer();
            target != nullptr && std::find(isolated_.begin(), isolated_.end(), target->id) == isolated_.end())
            isolated_.push_back(target->id);
        isolated_page_ = page->id;
    }
    if (QAction* act = window_->action(QStringLiteral("act_isolate_layers")); act != nullptr && act->isChecked() != isolated()) {
        act->blockSignals(true);
        act->setChecked(isolated());
        act->blockSignals(false);
    }
    refresh();
}

}  // namespace genko::app
