// PC2a (window.panel.adjustments・window.panel.properties): the 色調補正 and 属性 panels (app/adjust_panels.hpp).

#include "app/adjust_panels.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/adjust_forms.hpp"
#include "app/layer_dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/smart_panel.hpp"
#include "app/session.hpp"
#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/ids.hpp"

namespace genko::app::adjust_forms {

using core::Json;

namespace {

QDockWidget* properties_dock(MainWindow* window) { return window->findChild<QDockWidget*>(QStringLiteral("属性")); }

// The layer chosen (the one drawn on) and its page.
std::pair<const core::Page*, const core::Layer*> chosen(const MainWindow* window) {
    const core::Page* page = window->current_page();
    return {page, page != nullptr ? window->target_layer() : nullptr};
}

}  // namespace

// --- 色調補正 ---------------------------------------------------------------------------------------------------------

AdjustmentsPanel::AdjustmentsPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("adjustments_panel"));
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(QStringLiteral("押すと、描く先のレイヤーの上に色調補正のレイヤーを作ります（あとから「属性」で直せます）"));
    hint->setWordWrap(true);
    hint->setProperty("role", "hint");
    layout->addWidget(hint);
    auto* grid = new QGridLayout;
    int at = 0;
    for (const FormKind& form : form_kinds()) {
        const core::AdjustKind* kind = core::adjust_kind(form.kind);
        if (kind == nullptr || !kind->layer) continue;
        auto* button = new QPushButton(form.title);
        button->setObjectName(QStringLiteral("adjustment_%1").arg(QString::fromUtf8(form.kind.data(), static_cast<qsizetype>(form.kind.size()))));
        connect(button, &QPushButton::clicked, this, [this, kind = std::string(form.kind)] { add(kind); });
        grid->addWidget(button, at / 2, at % 2);
        ++at;
    }
    layout->addLayout(grid);
    layout->addStretch(1);
}

bool AdjustmentsPanel::add(const std::string& kind) {
    const FormKind* form = form_kind(kind);
    const auto [page, layer] = chosen(window_);
    if (form == nullptr || page == nullptr) return false;
    Json adjust{{"kind", kind}};
    adjust.update(form->fresh);
    // (a tone curve starts straight: Python's curve without points is a gamma curve)
    if (kind == "curve" && !adjust.contains("points")) adjust["points"] = Json::array({Json::array({0, 0}), Json::array({255, 255})});
    const std::string id = core::new_id();
    Json op{{"op", "add_layer"}, {"page", page->index.json()}, {"kind", "adjust"}, {"name", form->title.toStdString()}, {"id", id}, {"adjust", adjust}};
    if (layer != nullptr) op["after"] = layer->id;
    if (!window_->apply_ops(Json::array({op}))) return false;
    window_->set_target_layer(id);
    if (window_->layer_panel() != nullptr) window_->layer_panel()->refresh();
    if (QDockWidget* dock = properties_dock(window_)) {
        dock->show();
        dock->raise();
        if (auto* panel = qobject_cast<PropertiesPanel*>(dock->widget())) panel->refresh();
    }
    return true;
}

// --- 属性 ------------------------------------------------------------------------------------------------------------

PropertiesPanel::PropertiesPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("properties_panel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    content_ = new QWidget;
    body_ = new QVBoxLayout(content_);
    scroll->setWidget(content_);
    outer->addWidget(scroll);
    preview_ = new QTimer(this);
    preview_->setSingleShot(true);
    preview_->setInterval(250);
    connect(preview_, &QTimer::timeout, this, [this] {
        if (const auto ops = pending()) window_->preview_ops(*ops);
    });
    settle_ = new QTimer(this);  // (let go: what was changed applied as one op)
    settle_->setSingleShot(true);
    settle_->setInterval(900);
    connect(settle_, &QTimer::timeout, this, [this] { commit(); });
    watch_ = new QTimer(this);  // (the book or the layer chosen changed from anywhere: shown again)
    watch_->setInterval(300);
    connect(watch_, &QTimer::timeout, this, [this] {
        if (!isVisible() || editing_) return;
        const auto [page, layer] = chosen(window_);
        const void* seen = window_->session().snapshot().get();
        const std::string page_id = page != nullptr ? page->id : std::string(), layer_id = layer != nullptr ? layer->id : std::string();
        if (seen != seen_ || page_id != page_id_ || layer_id != layer_id_) refresh();
    });
    watch_->start();
    build();
}

void PropertiesPanel::refresh() {
    if (editing_) commit();
    build();
}

void PropertiesPanel::build() {
    loading_ = true;
    while (QLayoutItem* item = body_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    form_ = nullptr;
    opacity_ = nullptr;
    blend_ = nullptr;
    opened_ = Json::object();
    const auto [page, layer] = chosen(window_);
    seen_ = window_->session().snapshot().get();
    page_id_ = page != nullptr ? page->id : std::string();
    layer_id_ = layer != nullptr ? layer->id : std::string();
    kind_.clear();
    title_ = new QLabel;
    title_->setObjectName(QStringLiteral("properties_title"));
    title_->setWordWrap(true);
    body_->addWidget(title_);
    if (layer == nullptr) {
        title_->setText(QStringLiteral("レイヤーを選ぶと、ここに設定が出ます"));
        body_->addStretch(1);
        loading_ = false;
        return;
    }
    const bool correction = layer->kind == core::LayerKind::Adjust && layer->adjust && layer->adjust->is_object();
    if (correction) kind_ = layer->adjust->value("kind", std::string());
    title_->setText(QStringLiteral("%1%2").arg(QString::fromStdString(layer->title.empty() ? layer->id : layer->title),
                                               correction ? QStringLiteral("（色調補正）") : QString()));
    auto* rows = new QWidget;
    auto* form = new QFormLayout(rows);
    form->setContentsMargins(0, 0, 0, 0);
    opacity_ = new QDoubleSpinBox;
    opacity_->setObjectName(QStringLiteral("properties_opacity"));
    opacity_->setRange(0, 100);
    opacity_->setDecimals(0);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setValue(std::round(layer->opacity * 100));
    form->addRow(QStringLiteral("不透明度"), opacity_);
    blend_ = new QComboBox;
    blend_->setObjectName(QStringLiteral("properties_blend"));
    for (const auto& [key, label] : blend_modes()) blend_->addItem(label, QString::fromStdString(key));
    const std::string blend = correction ? core::native_of(*layer->adjust).blend : layer->blend;
    blend_->setCurrentIndex(std::max(0, blend_->findData(QString::fromStdString(blend))));
    form->addRow(QStringLiteral("描画モード"), blend_);
    body_->addWidget(rows);
    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    if (!correction) {
        auto* lock = new QCheckBox(QStringLiteral("ロック"));
        lock->setObjectName(QStringLiteral("properties_lock"));
        lock->setChecked(layer->locked);
        const Json index = page->index.json();
        const std::string id = layer->id;
        connect(lock, &QCheckBox::toggled, this, [this, index, id](bool on) {
            window_->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", index}, {"id", id}, {"locked", on}}}));
        });
        body_->addWidget(lock);
        if (layer->kind == core::LayerKind::Smart) body_->addWidget(smart_properties(window_, *page, *layer));  // (smart-objects@1)
        if (layer->kind == core::LayerKind::Fill) {
            auto* edit = new QPushButton(QStringLiteral("塗りを直す…"));
            connect(edit, &QPushButton::clicked, this, [this] {
                if (window_->layer_panel() != nullptr) window_->layer_panel()->edit_special();
            });
            body_->addWidget(edit);
        }
    } else if (const FormKind* kind = form_kind(kind_)) {
        form_ = new Form;
        FormContext context;
        context.page = page;
        const auto& rgb = window_->pen().rgb;
        context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
        kind->build(*form_, context);
        form_->set_values(kind->prepare ? kind->prepare(*layer->adjust) : *layer->adjust);
        opened_ = form_->values();
        connect(form_, &Form::changed, this, [this] { changed(); });
        auto* reset = new QPushButton(QStringLiteral("既定に戻す"));
        reset->setObjectName(QStringLiteral("properties_reset"));
        const Json index = page->index.json();
        const std::string id = layer->id;
        connect(reset, &QPushButton::clicked, this, [this, index, id] {
            window_->apply_ops(Json::array({Json{{"op", "set_adjustment"}, {"page", index}, {"id", id}}}));
        });
        body_->addWidget(form_);
        body_->addWidget(reset);
    } else {  // (a kind with no form here: its own window)
        auto* edit = new QPushButton(QStringLiteral("設定を開く…"));
        connect(edit, &QPushButton::clicked, this, [this] {
            if (window_->layer_panel() != nullptr) window_->layer_panel()->edit_special();
        });
        body_->addWidget(edit);
    }
    body_->addStretch(1);
    loading_ = false;
}

void PropertiesPanel::changed() {
    if (loading_) return;
    editing_ = true;
    preview_->start();
    settle_->start();
}

// The ops of what is being changed: the correction's settings the person changed laid over it (set_adjustment, its blend
// in its native: the user's decision 3 — what is not changed stays as it is) and the layer's opacity (set_layer), or
// another layer's opacity and blend.
std::optional<Json> PropertiesPanel::pending() const {
    const core::Document& book = window_->book();
    const core::Page* page = nullptr;
    for (std::size_t i = 0; i < book.pages.size(); ++i) {
        if (!book.is_deferred(i) && book.page(i).id == page_id_) page = &book.page(i);
    }
    if (page == nullptr || opacity_ == nullptr || blend_ == nullptr) return std::nullopt;
    const core::Layer* layer = nullptr;
    for (const core::Layer& l : page->layers)
        if (l.id == layer_id_) layer = &l;
    if (layer == nullptr) return std::nullopt;
    Json ops = Json::array();
    const double opacity = opacity_->value() / 100.0;
    const std::string blend = blend_->currentData().toString().toStdString();
    const Json index = page->index.json();
    if (std::abs(opacity - layer->opacity) > 1e-9) ops.push_back(Json{{"op", "set_layer"}, {"page", index}, {"id", layer_id_}, {"opacity", opacity}});
    if (layer->kind == core::LayerKind::Adjust && layer->adjust) {
        Json given = form_ != nullptr ? changes(opened_, form_->values()) : Json::object();
        if (blend != core::native_of(*layer->adjust).blend) given["native"]["blend"] = blend == "normal" ? Json() : Json(blend);
        if (!given.empty()) {
            Json op{{"op", "set_adjustment"}, {"page", index}, {"id", layer_id_}};
            for (const auto& [key, value] : given.items()) op[key] = value;
            ops.push_back(op);
        }
    } else if (blend != layer->blend) {
        ops.push_back(Json{{"op", "set_layer"}, {"page", index}, {"id", layer_id_}, {"blend", blend}});
    }
    if (ops.empty()) return std::nullopt;
    return ops;
}

void PropertiesPanel::commit() {
    preview_->stop();
    settle_->stop();
    if (!editing_) return;
    editing_ = false;
    window_->preview_ops(std::nullopt);
    if (const auto ops = pending()) window_->apply_ops(*ops);
}

// --- the docks --------------------------------------------------------------------------------------------------------

void build_tone_docks(MainWindow* window, QMenu* window_menu) {
    const auto dock = [window, window_menu](const QString& title, QWidget* widget) {
        auto* d = new QDockWidget(title, window);
        d->setObjectName(title);
        d->setWidget(widget);
        d->setMinimumWidth(220);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        window->addDockWidget(Qt::RightDockWidgetArea, d);
        d->hide();
        if (window_menu != nullptr) window_menu->addAction(d->toggleViewAction());
        return d;
    };
    dock(QStringLiteral("色調補正"), new AdjustmentsPanel(window));
    dock(QStringLiteral("属性"), new PropertiesPanel(window));
}

}  // namespace genko::app::adjust_forms
