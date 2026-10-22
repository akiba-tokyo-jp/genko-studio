// smart-objects@1, PC3 A2 (design section A 5; Photoshop's names, the user's decision 8 of PC2): a smart object's smart
// filters in the layer panel — under its row a row スマートフィルター (its eye: all of them on or off; its marks: the
// filter mask, on or off) and a row for each filter, the top one first as Photoshop lists them (its eye: shown; its marks:
// its blend mode and opacity when they are not normal at 100 %; a double-click: スマートフィルターを編集…, the filter's own
// window with its settings now; the right button: スマートフィルターを編集…, 描画オプション…, moving it up or down, deleting
// it; on the header: スマートフィルターを無効にする, フィルターマスクを無効にする, フィルターマスクを削除, スマートフィルターをクリア).
// Every change is an op (render/ops_smart_filters.cpp): Undo, saving and the journal as for any edit.

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QListWidget>
#include <QMenu>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/filter_forms.hpp"
#include "app/filter_menu.hpp"
#include "app/layer_dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/smart_filters.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kRowRole = Qt::UserRole + 4;  // (what a row is: "smart_filters", or "smart_filter:<index>")

const Json* filters_of(const core::Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return nullptr;
    const Json* filters = core::get(*layer.smart, "filters");
    return filters != nullptr && filters->is_array() ? filters : nullptr;
}

const Json* filter_mask_of(const core::Layer& layer) {
    if (!layer.smart || !layer.smart->is_object()) return nullptr;
    const Json* mask = core::get(*layer.smart, "filter_mask");
    return mask != nullptr && mask->is_object() ? mask : nullptr;
}

bool flag(const Json& object, const char* key) {
    const Json* v = core::get(object, key);
    return !(v != nullptr && v->is_boolean() && !v->get<bool>());
}

}  // namespace

// The filter's name in the panel: the layer panel's filter list's (filters.KINDS), or its window's (PC2's kinds).
QString smart_filter_title(const std::string& kind) {
    // (PC3 A5: the filters that bend the picture, by the commands' names)
    if (kind == "liquify") return QStringLiteral("ゆがみ");
    if (kind == "perspective_warp") return QStringLiteral("遠近法ワープ");
    if (kind == "puppet_warp") return QStringLiteral("パペットワープ");
    for (const auto& [key, label] : filter_kinds())
        if (key == kind) return label;
    if (core::pc4_filter(kind)) return filter_forms::title(kind);  // (PC4 節 D: フィルター's, by Photoshop's name)
    QString title = adjust_forms::command_title(kind);
    if (title.endsWith(QStringLiteral("…"))) title.chop(1);
    return title.isEmpty() ? QString::fromStdString(kind) : title;
}

int LayerPanel::row_kind(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return kLayerRow;
    return rows_[static_cast<std::size_t>(row)];
}

void LayerPanel::add_smart_filter_rows(const core::Layer& layer, const QString& indent) {
    const Json* filters = filters_of(layer);
    const Json* mask = filter_mask_of(layer);
    if ((filters == nullptr || filters->empty()) && mask == nullptr) return;
    const QString under = indent + QStringLiteral("　　");
    QStringList marks;
    if (mask != nullptr) marks << (flag(*mask, "enabled") ? QStringLiteral("フィルターマスク") : QStringLiteral("フィルターマスク（無効）"));
    auto* head = new QListWidgetItem(under + QStringLiteral("スマートフィルター") + (marks.isEmpty() ? QString() : QStringLiteral("　· ") + marks.join(QStringLiteral(" · "))));
    head->setData(Qt::UserRole + 2, marks.join(QStringLiteral(" · ")));
    head->setData(Qt::UserRole + 3, under + QStringLiteral("スマートフィルター"));
    head->setData(kRowRole, QStringLiteral("smart_filters"));
    head->setToolTip(QStringLiteral("スマートフィルター（目: すべてのスマートフィルターを有効・無効に。右クリック: フィルターマスク・クリア）"));
    head->setFlags(head->flags() | Qt::ItemIsUserCheckable);
    head->setCheckState(flag(*layer.smart, "filters_on") ? Qt::Checked : Qt::Unchecked);
    list_->addItem(head);
    ids_.push_back(layer.id);
    rows_.push_back(kFiltersRow);
    if (filters == nullptr) return;
    for (int k = static_cast<int>(filters->size()) - 1; k >= 0; --k) {  // (the top one first, as Photoshop lists them)
        const Json& f = (*filters)[static_cast<std::size_t>(k)];
        if (!f.is_object()) continue;
        const QString name = smart_filter_title(core::py_str(core::py_get(f, "kind", Json(""))));
        QStringList meta;
        const std::string blend = core::py_str(core::py_get(f, "blend", Json("normal")));
        for (const auto& [key, label] : blend_modes())
            if (key == blend && key != "normal") meta << label;
        const double opacity = core::py_get(f, "opacity", Json(1.0)).is_number() ? core::py_get(f, "opacity", Json(1.0)).get<double>() : 1.0;
        if (opacity < 0.995) meta << QStringLiteral("%1%").arg(std::lround(opacity * 100));
        auto* item = new QListWidgetItem(under + QStringLiteral("　") + name + (meta.isEmpty() ? QString() : QStringLiteral("　· ") + meta.join(QStringLiteral(" · "))));
        item->setData(Qt::UserRole + 2, meta.join(QStringLiteral(" · ")));
        item->setData(Qt::UserRole + 3, under + QStringLiteral("　") + name);
        item->setData(kRowRole, QStringLiteral("smart_filter:%1").arg(k));
        item->setToolTip(QStringLiteral("スマートフィルター（ダブルクリック: 設定を直す。右クリック: 描画オプション・並べ替え・削除）"));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(flag(f, "visible") ? Qt::Checked : Qt::Unchecked);
        list_->addItem(item);
        ids_.push_back(layer.id);
        rows_.push_back(k);
    }
}

bool LayerPanel::smart_filter_visibility(int row, bool shown) {
    const int kind = row_kind(row);
    if (kind == kLayerRow) return false;
    const std::string id = ids_[static_cast<std::size_t>(row)];  // (the row's own smart object: not always the one chosen)
    if (kind == kFiltersRow) smart_filter_op(Json{{"filters_on", shown}}, "smart_filter_state", id);
    else smart_filter_op(Json{{"index", kind}, {"visible", shown}}, "smart_filter_set", id);
    return true;
}

void LayerPanel::smart_filter_op(const Json& fields, const std::string& op, const std::string& id) {
    const auto [page, layer] = this->layer();
    if (page == nullptr || (id.empty() && (layer == nullptr || layer->kind != core::LayerKind::Smart))) return;
    Json sent{{"op", op}, {"page", page->index.json()}, {"id", id.empty() ? layer->id : id}};
    sent.update(fields);
    window_->apply_ops(Json::array({sent}));
}

void LayerPanel::edit_smart_filter(int index) {
    const auto [page, layer] = this->layer();
    if (page == nullptr || layer == nullptr) return;
    const Json* filters = filters_of(*layer);
    if (filters == nullptr || index < 0 || index >= static_cast<int>(filters->size())) return;
    const Json f = (*filters)[static_cast<std::size_t>(index)];
    const std::string kind = core::py_str(core::py_get(f, "kind", Json("")));
    if (core::smart::bend_filter(kind)) {  // (PC3 A5: the transform box or Liquify's window, from its settings)
        window_->edit_bend_filter(layer->id, index);
        return;
    }
    // (its settings as the page has them: a blur's centre on the page — independent review A P2-1)
    const Json now = render::smart_filters::params_on_page(window_->book(), *page, *layer, f);
    const auto asked = window_->asking();
    const std::string layer_id = layer->id;
    const Json page_index = page->index.json();
    if (const FilterWindow* own = filter_window(kind)) {  // (PC4 節 D: a group's own window — フィルターギャラリー… —, from its settings)
        const render::fx::Grid grid = render::fx::layer_grid(*page, *layer);
        FilterWindowCall call;
        call.window = window_;
        call.now = now;
        call.page = page;
        call.layer = layer;
        call.filter = index;
        call.send = [this, asked, page_index, layer_id, index, kind, grid](const Json& values) {
            if (!window_->still(asked)) return;
            const bool heavy = render::fx::work(kind, values, std::uint64_t(grid.width) * grid.height, grid.s) >= render::fx::kHeavyWork;
            window_->send_filter(Json::array({Json{{"op", "smart_filter_set"}, {"page", page_index}, {"id", layer_id}, {"index", index}, {"params", values}}}),
                                 filter_forms::title(kind), heavy);
        };
        (*own)(call);
        return;
    }
    // (shown on the page while the numbers change: the filter set again on a copy of the book)
    const FilterPreview preview = [this, page_index, layer_id, index](const std::optional<Json>& values) {
        if (!values) {
            window_->preview_ops(std::nullopt);
            return;
        }
        window_->preview_ops(Json::array(
            {Json{{"op", "smart_filter_set"}, {"page", page_index}, {"id", layer_id}, {"index", index}, {"params", adjust_forms::without_nulls(*values)}}}));
    };
    std::optional<Json> params;
    if (core::pc4_filter(kind)) {  // (PC4 節 D: フィルター's kinds, their windows of the table; what the person changed laid over them)
        adjust_forms::FormContext context;
        context.page = page;
        const auto& rgb = window_->pen().rgb;
        context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
        // (a heavy one — render::fx::work on the smart object's grid —: previewed and set on a thread, as フィルター's commands)
        const render::fx::Grid grid = render::fx::layer_grid(*page, *layer);
        const auto heavy = [grid, kind](const Json& values) {
            return render::fx::work(kind, values, std::uint64_t(grid.width) * grid.height, grid.s) >= render::fx::kHeavyWork;
        };
        const FilterPreview shown = [this, page_index, layer_id, index, heavy](const std::optional<Json>& values) {
            if (!values) {
                window_->preview_filter(std::nullopt, false);
                return;
            }
            const Json set{{"op", "smart_filter_set"}, {"page", page_index}, {"id", layer_id}, {"index", index}, {"params", adjust_forms::without_nulls(*values)}};
            window_->preview_filter(Json::array({set}), heavy(*values));
        };
        Json opened;
        const auto values = filter_forms::dialog(this, kind, now, context, filter_forms::Preview{shown, [this] { return window_->filter_previewing(); }}, &opened);
        if (!values || !window_->still(asked)) return;
        params = opened.is_object() ? adjust_forms::changes(opened, *values) : *values;
        if (params->is_object() && params->empty()) return;  // (nothing changed)
        window_->send_filter(Json::array({Json{{"op", "smart_filter_set"}, {"page", page_index}, {"id", layer_id}, {"index", index}, {"params", *params}}}),
                             filter_forms::title(kind), heavy(adjust_forms::without_nulls(*values)));
        return;
    } else if (adjust_forms::form_kind(kind) != nullptr) {  // (PC2's kinds: their own window; what the person changed laid over them)
        adjust_forms::FormContext context;
        context.page = page;
        const auto& rgb = window_->pen().rgb;
        context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
        Json opened;
        const auto values = adjust_forms::adjust_dialog(this, kind, now, context, preview, &opened);
        if (values) params = opened.is_object() ? adjust_forms::changes(opened, *values) : *values;
    } else if (kind == "gradient_map") {
        const auto& rgb = window_->pen().rgb;
        params = gradient_map_dialog(this, now, {{static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])}, {255, 255, 255}});
    } else {
        params = filter_params(this, kind, now, preview, kind == "levels" || kind == "curve" ? histogram(*page, *layer) : std::nullopt);
    }
    if (!params || !window_->still(asked)) return;
    if (params->is_object() && params->empty()) return;  // (nothing changed)
    smart_filter_op(Json{{"index", index}, {"params", *params}});
}

void LayerPanel::smart_filter_blending(int index) {
    const auto [page, layer] = this->layer();
    if (page == nullptr || layer == nullptr) return;
    const Json* filters = filters_of(*layer);
    if (filters == nullptr || index < 0 || index >= static_cast<int>(filters->size())) return;
    const Json& f = (*filters)[static_cast<std::size_t>(index)];
    const auto asked = window_->asking();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("smart_filter_blending"));
    dialog.setWindowTitle(QStringLiteral("描画オプション（%1）").arg(smart_filter_title(core::py_str(core::py_get(f, "kind", Json(""))))));
    auto* column = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* mode = new QComboBox;
    mode->setObjectName(QStringLiteral("smart_filter_blend"));
    for (const auto& [key, label] : blend_modes()) mode->addItem(label, QString::fromStdString(key));
    mode->setCurrentIndex(std::max(0, mode->findData(QString::fromStdString(core::py_str(core::py_get(f, "blend", Json("normal")))))));
    auto* opacity = new QSpinBox;
    opacity->setObjectName(QStringLiteral("smart_filter_opacity"));
    opacity->setRange(0, 100);
    opacity->setSuffix(QStringLiteral(" %"));
    const Json given = core::py_get(f, "opacity", Json(1.0));
    opacity->setValue(static_cast<int>(std::lround((given.is_number() ? given.get<double>() : 1.0) * 100)));
    form->addRow(QStringLiteral("描画モード"), mode);
    form->addRow(QStringLiteral("不透明度"), opacity);
    column->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    column->addWidget(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted || !window_->still(asked)) return;
    smart_filter_op(Json{{"index", index}, {"blend", mode->currentData().toString().toStdString()}, {"opacity", opacity->value() / 100.0}});
}

void LayerPanel::smart_filter_menu(const QPoint& at) {
    QListWidgetItem* item = list_->itemAt(at);
    if (item == nullptr) return;
    const int row = list_->row(item);
    const int kind = row_kind(row);
    if (kind == kLayerRow) return;
    list_->setCurrentRow(row);
    const auto [page, layer] = this->layer();
    if (page == nullptr || layer == nullptr) return;
    QMenu menu(this);
    if (kind >= 0) {
        const Json* filters = filters_of(*layer);
        const int count = filters != nullptr ? static_cast<int>(filters->size()) : 0;
        menu.addAction(QStringLiteral("スマートフィルターを編集…"), this, [this, kind] { edit_smart_filter(kind); });
        menu.addAction(QStringLiteral("描画オプション…"), this, [this, kind] { smart_filter_blending(kind); });
        menu.addSeparator();
        QAction* up = menu.addAction(QStringLiteral("上へ"), this, [this, kind] { smart_filter_op(Json{{"index", kind}, {"to", kind + 1}}, "smart_filter_move"); });
        up->setEnabled(kind + 1 < count);
        QAction* down = menu.addAction(QStringLiteral("下へ"), this, [this, kind] { smart_filter_op(Json{{"index", kind}, {"to", kind - 1}}, "smart_filter_move"); });
        down->setEnabled(kind > 0);
        menu.addSeparator();
        menu.addAction(QStringLiteral("スマートフィルターを削除"), this, [this, kind] { smart_filter_op(Json{{"index", kind}}, "smart_filter_delete"); });
    } else {
        for (const char* name : {"act_smart_filters_off", "act_smart_filter_mask_off", "act_smart_filter_mask_delete", "act_smart_filters_clear"})
            if (QAction* act = window_->action(QString::fromLatin1(name))) menu.addAction(act);
    }
    menu.exec(list_->viewport()->mapToGlobal(at));
}

}  // namespace genko::app
