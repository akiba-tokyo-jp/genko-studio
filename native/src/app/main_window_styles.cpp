// PC4 E6 (layer.layerStyle.*, style.presets.*, window.panel.styles; SPEC.md COLOR-09「レイヤースタイル」「スタイル」):
// レイヤー → レイヤースタイル ▸ in Photoshop's order and names — 混合オプション…, each effect's window, レイヤースタイルを
// コピー・ペースト・消去, 包括光源…, レイヤーを作成, すべての効果を隠す・表示, 効果を拡大・縮小… —, the スタイル panel's
// dock (in E-0's frame) and the presets applied and made. Every change is ops through the window (set_layer_style and its
// kin, render/ops_layer_styles), so one Undo takes each back; the library is the machine's (render/style_presets).

#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>

#include <cmath>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/layer_style_dialog.hpp"
#include "app/main_window.hpp"
#include "app/styles_panel.hpp"
#include "core/layer_styles.hpp"
#include "core/patterns.hpp"
#include "render/pattern_library.hpp"
#include "render/preset_store.hpp"
#include "render/style_presets.hpp"

namespace genko::app {

using core::Json;
namespace store = render::preset_store;

namespace {

constexpr const char* kStyleMime = "application/x-genko-layer-style";

const core::Layer* layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& l : page.layers)
        if (l.id == id) return &l;
    return nullptr;
}

}  // namespace

std::vector<std::string> MainWindow::style_targets() const {
    const core::Page* page = current_page();
    if (page == nullptr) return {};
    std::vector<std::string> out;
    if (layer_panel_ != nullptr) {
        for (const std::string& id : layer_panel_->selected_ids()) {
            const core::Layer* l = layer_of(*page, id);
            if (l != nullptr && core::takes_layer_style(*l)) out.push_back(id);
        }
    }
    if (out.empty()) {
        const core::Layer* l = target_layer();
        if (l != nullptr && core::takes_layer_style(*l)) out.push_back(l->id);
    }
    return out;
}

bool MainWindow::edit_layer_style(const std::string& kind) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return false;
    if (!core::takes_layer_style(*layer)) {
        flash(QStringLiteral("このレイヤーにはレイヤースタイルを使えません（描画・ペン・塗りつぶし・配置・スマートオブジェクトのレイヤーに使えます）"), 5000, true);
        return false;
    }
    const auto asked = asking();
    const Json index = page->index.json();
    const std::string id = layer->id;
    const std::string blend = layer->blend;
    const double opacity = layer->opacity;
    const std::array<double, 2> light = core::global_light(book());
    LayerStyleDialog dialog(asked.snapshot, *page, *layer, kind, this);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return false;
    Json ops = Json::array();
    if (dialog.light() != light) ops.push_back(Json{{"op", "set_global_light"}, {"angle", dialog.light()[0]}, {"altitude", dialog.light()[1]}});
    Json op{{"op", "set_layer_style"}, {"page", index}, {"id", id}, {"style", dialog.style()}};
    if (dialog.blend() != blend) op["blend"] = dialog.blend();
    ops.push_back(std::move(op));
    if (std::abs(dialog.opacity() - opacity) > 1e-9) ops.push_back(Json{{"op", "set_layer"}, {"page", index}, {"id", id}, {"opacity", dialog.opacity()}});
    return apply_ops(ops);
}

void MainWindow::copy_layer_style() {
    const core::Layer* layer = target_layer();
    if (layer == nullptr) return;
    // (its patterns as their pictures: pasted into another book, they come with it)
    Json style = layer->style ? *layer->style : Json(nullptr);
    if (!style.is_null()) {
        core::change_style_patterns(style, [this](Json& fill) {
            const core::Pattern* p = core::find_pattern(book(), fill.value("id", std::string()));
            if (p == nullptr) return;
            const auto file = book().pattern_files.find(p->asset);
            if (file == book().pattern_files.end() || !file->second) return;
            Json put = render::pattern_library::add_op(*p, file->second);
            put.erase("op");
            for (const auto& [key, value] : fill.items())
                if (key != "id") put[key] = value;
            fill = std::move(put);
        });
    }
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kStyleMime), QByteArray::fromStdString(Json{{"style", style}, {"blend", layer->blend}}.dump()));
    QGuiApplication::clipboard()->setMimeData(data);
    flash(QStringLiteral("レイヤースタイルをコピーしました"), 2500);
}

bool MainWindow::paste_layer_style() {
    const core::Page* page = current_page();
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (page == nullptr || data == nullptr || !data->hasFormat(QString::fromLatin1(kStyleMime))) return false;
    Json copied;
    try {
        copied = Json::parse(data->data(QString::fromLatin1(kStyleMime)).toStdString());
    } catch (const std::exception&) {
        return false;
    }
    const std::vector<std::string> ids = style_targets();
    if (ids.empty()) return false;
    return apply_ops(Json::array({Json{{"op", "set_layer_style"}, {"page", page->index.json()}, {"ids", ids}, {"style", copied.value("style", Json(nullptr))}}}));
}

bool MainWindow::clear_layer_style() {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = style_targets();
    if (page == nullptr || ids.empty()) return false;
    return apply_ops(Json::array({Json{{"op", "set_layer_style"}, {"page", page->index.json()}, {"ids", ids}, {"style", nullptr}}}));
}

bool MainWindow::global_light_dialog() {
    const std::array<double, 2> now = core::global_light(book());
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("global_light_dialog"));
    dialog.setWindowTitle(QStringLiteral("包括光源"));
    auto* form = new QFormLayout(&dialog);
    auto* angle = new QDoubleSpinBox;
    angle->setObjectName(QStringLiteral("global_light_angle"));
    angle->setRange(-180, 180);
    angle->setDecimals(1);
    angle->setSuffix(QStringLiteral("°"));
    angle->setValue(now[0]);
    auto* altitude = new QDoubleSpinBox;
    altitude->setObjectName(QStringLiteral("global_light_altitude"));
    altitude->setRange(0, 90);
    altitude->setDecimals(1);
    altitude->setSuffix(QStringLiteral("°"));
    altitude->setValue(now[1]);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QStringLiteral("高度"), altitude);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) return false;
    if (angle->value() == now[0] && altitude->value() == now[1]) return false;
    return apply_ops(Json::array({Json{{"op", "set_global_light"}, {"angle", angle->value()}, {"altitude", altitude->value()}}}));
}

bool MainWindow::scale_layer_style_dialog(std::optional<double> percent) {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = style_targets();
    if (page == nullptr || ids.empty()) return false;
    if (!percent) {
        percent = ask::get_double(this, QStringLiteral("効果を拡大・縮小"), QStringLiteral("比率（%）"), 100, 1, 1000, 0);
        if (!percent) return false;
    }
    return apply_ops(Json::array({Json{{"op", "scale_layer_style"}, {"page", page->index.json()}, {"ids", ids}, {"scale", *percent}}}));
}

bool MainWindow::layer_style_to_layers() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr || !layer->style || !core::style_shows_effects(*layer->style)) return false;
    return apply_ops(Json::array({Json{{"op", "layer_style_to_layers"}, {"page", page->index.json()}, {"id", layer->id}}}));
}

bool MainWindow::show_all_effects(bool show) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    return apply_ops(Json::array({Json{{"op", "show_layer_effects"}, {"page", page->index.json()}, {"show", show}}}));
}

bool MainWindow::apply_style_preset(const Json& preset, bool add) {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = style_targets();
    if (page == nullptr) return false;
    if (ids.empty()) {
        flash(QStringLiteral("スタイルを当てるレイヤーを選んでください（描画・ペン・塗りつぶし・配置・スマートオブジェクトのレイヤー）"), 4000, true);
        return false;
    }
    Json op;
    try {
        const store::Library patterns = render::pattern_library::load(config_dir());
        op = render::style_presets::apply_op(preset, page->index.json(), ids, add, config_dir(), patterns.groups);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("スタイルを適用"), QString::fromUtf8(error.what()));
        return false;
    }
    return apply_ops(Json::array({op}));
}

bool MainWindow::new_style_preset(std::optional<QString> name, bool effects, bool blending) {
    const core::Layer* layer = target_layer();
    if (layer == nullptr) return false;
    if (!name) {
        // 新規スタイル: its name, レイヤー効果を含める, レイヤーの混合オプションを含める
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("new_style_dialog"));
        dialog.setWindowTitle(QStringLiteral("新規スタイル"));
        auto* form = new QFormLayout(&dialog);
        const store::Library now = store::load(config_dir(), render::style_presets::kind());
        auto* field = new QLineEdit(QString::fromStdString(store::unique_name(now.groups, "スタイル 1")));
        field->setObjectName(QStringLiteral("new_style_name"));
        auto* fx = new QCheckBox(QStringLiteral("レイヤー効果を含める"));
        fx->setObjectName(QStringLiteral("new_style_effects"));
        fx->setChecked(true);
        auto* mix = new QCheckBox(QStringLiteral("レイヤーの混合オプションを含める"));
        mix->setObjectName(QStringLiteral("new_style_blending"));
        mix->setChecked(true);
        form->addRow(QStringLiteral("名前"), field);
        form->addRow(fx);
        form->addRow(mix);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (ask::exec(&dialog) != QDialog::Accepted) return false;
        name = field->text().trimmed().isEmpty() ? QStringLiteral("スタイル") : field->text().trimmed();
        effects = fx->isChecked();
        blending = mix->isChecked();
        layer = target_layer();
        if (layer == nullptr) return false;
    }
    try {
        // (both libraries read again now: what another window or `genko styles` wrote is kept)
        store::Library lib = store::load(config_dir(), render::style_presets::kind());
        if (!lib.problem.empty()) throw core::Error("io", "the styles' file cannot be read: " + lib.problem);
        store::Library patterns = render::pattern_library::load(config_dir());
        bool pattern_added = false;
        const Json item = render::style_presets::item_of_layer(book(), *layer, effects, blending, name->toStdString(), config_dir(), patterns.groups, pattern_added);
        const Json out = store::add(lib.groups, render::style_presets::kind(), item, std::nullopt, "スタイル");
        if (pattern_added) render::pattern_library::save(config_dir(), patterns.groups);
        store::save(config_dir(), render::style_presets::kind(), lib.groups);
        flash(QStringLiteral("スタイル「%1」を作りました").arg(QString::fromStdString(out.value("name", name->toStdString()))), 4000);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("新規スタイル"), QString::fromUtf8(error.what()));
        return false;
    }
    if (styles_panel_ != nullptr) styles_panel_->refresh();
    return true;
}

void MainWindow::build_layer_style_actions() {
    make("act_layer_style_blending", QStringLiteral("混合オプション…"), [this] { edit_layer_style(); }, {},
         QStringLiteral("レイヤーの描画モード・不透明度・塗りの不透明度・ブレンド条件と、レイヤー効果を設定します"));
    for (const std::string_view kind : core::kEffectKinds) {
        const std::string k(kind);
        make(QStringLiteral("act_layer_style_") + QString::fromStdString(k), effect_title(k) + QStringLiteral("…"), [this, k] { edit_layer_style(k); }, {},
             QStringLiteral("レイヤースタイルの「%1」を設定します").arg(effect_title(k)));
    }
    make("act_layer_style_copy", QStringLiteral("レイヤースタイルをコピー"), [this] { copy_layer_style(); }, {},
         QStringLiteral("選んでいるレイヤーのレイヤースタイルをコピーします"));
    make("act_layer_style_paste", QStringLiteral("レイヤースタイルをペースト"), [this] { paste_layer_style(); }, {},
         QStringLiteral("コピーしたレイヤースタイルを、選んでいるレイヤーに当てます"));
    make("act_layer_style_clear", QStringLiteral("レイヤースタイルを消去"), [this] { clear_layer_style(); }, {},
         QStringLiteral("選んでいるレイヤーのレイヤースタイルを取り除きます"));
    make("act_global_light", QStringLiteral("包括光源…"), [this] { global_light_dialog(); }, {},
         QStringLiteral("包括光源を使う効果（シャドウ・ベベル）の光の角度と高度を、原稿全体で変えます"));
    make("act_layer_style_layers", QStringLiteral("レイヤーを作成"), [this] { layer_style_to_layers(); }, {},
         QStringLiteral("レイヤー効果をそれぞれの描画レイヤーにします"));
    make("act_layer_style_hide_all", QStringLiteral("すべての効果を隠す"), [this] { show_all_effects(false); }, {},
         QStringLiteral("このページのすべてのレイヤー効果を隠します（効果は消えません）"));
    make("act_layer_style_show_all", QStringLiteral("すべての効果を表示"), [this] { show_all_effects(true); }, {},
         QStringLiteral("このページの隠したレイヤー効果を、また表示します"));
    make("act_layer_style_scale", QStringLiteral("効果を拡大・縮小…"), [this] { scale_layer_style_dialog(); }, {},
         QStringLiteral("レイヤー効果のサイズや距離を、まとめて拡大・縮小します"));
}

void MainWindow::build_layer_style_menus(QMenu* layers) {
    // レイヤー → レイヤースタイル ▸ (Photoshop's place: before レイヤーマスク)
    layer_style_menu_ = new QMenu(QStringLiteral("レイヤースタイル"), layers);
    layer_style_menu_->addAction(action("act_layer_style_blending"));
    layer_style_menu_->addSeparator();
    for (const std::string_view kind : core::kEffectKinds) layer_style_menu_->addAction(action(QStringLiteral("act_layer_style_") + QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()))));
    layer_style_menu_->addSeparator();
    for (const char* name : {"act_layer_style_copy", "act_layer_style_paste", "act_layer_style_clear"}) layer_style_menu_->addAction(action(QString::fromLatin1(name)));
    layer_style_menu_->addSeparator();
    for (const char* name : {"act_global_light", "act_layer_style_layers", "act_layer_style_hide_all", "act_layer_style_show_all", "act_layer_style_scale"})
        layer_style_menu_->addAction(action(QString::fromLatin1(name)));
    QAction* before = layer_mask_menu_ != nullptr ? layer_mask_menu_->menuAction() : nullptr;
    if (before != nullptr && layers->actions().contains(before)) {
        layers->insertMenu(before, layer_style_menu_);
    } else {
        layers->addMenu(layer_style_menu_);
    }
}

void MainWindow::build_styles_dock(QDockWidget* frame) {
    // (PC4 E6: the スタイル panel in the dock E-0 made, app/styles_panel.cpp)
    styles_dock_ = frame;
    QWidget* old = frame->widget();
    styles_panel_ = new StylesPanel(this);
    frame->setWidget(styles_panel_);
    delete old;
    connect(frame, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) styles_panel_->refresh();
    });
}

}  // namespace genko::app
