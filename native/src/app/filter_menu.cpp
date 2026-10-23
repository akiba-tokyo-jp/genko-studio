// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5.1-5.3; the user's decision 1): the フィルター menu after レイヤー —
// Photoshop's commands in its order and words (前回のフィルター, スマートフィルター用に変換, フィルターギャラリー…, 広角補正…,
// Camera Raw フィルター…, レンズ補正…, ゆがみ…, 消点…, then シャープ・ノイズ・ピクセレート・ビデオ・ぼかし・ぼかしギャラリー・
// 表現手法・描画・変形・その他), 漫画 (Genko's own five: 線画抽出・ゴミ取り・雨・光彩・白黒にする, their old windows) and
// プラグイン. Each command works the layer drawn on (on a smart object it adds a smart filter; inside the selection; a
// channel targeted, on it), its window made from its kind's table (app/filter_forms) or its group's own (filter_windows),
// the result shown while its settings change; a heavy one (render::fx::work) previewed at the screen's resolution on a
// thread and, when sent, worked on a thread with its progress and 中止 (the user's decision 4: never refused for it; the
// CLI and MCP wait for it as for any op). 前回のフィルター (Ctrl+F) sends the last one again with its settings on the
// layer drawn on now. Python's 22 filters are no longer offered as new (the layer panel's かける… opens this menu); their
// smart filters and books work as before.

#include "app/filter_menu.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QTimer>
#include <QtConcurrent>

#include <QRandomGenerator>

#include <algorithm>
#include <atomic>
#include <memory>
#include <stop_token>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/colours.hpp"
#include "app/filter_forms.hpp"
#include "app/blur_gallery_tool.hpp"
#include "app/filter_gallery_window.hpp"
#include "app/lens_window.hpp"
#include "app/lighting_window.hpp"
#include "app/render_dialogs.hpp"
#include "app/vanishing_window.hpp"
#include "app/layer_dialogs.hpp"
#include "app/main_window.hpp"
#include "core/adjust_kinds.hpp"
#include "core/command_bus.hpp"
#include "core/filter_kinds.hpp"
#include "core/ids.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/plugins.hpp"
#include "render/raster_ops.hpp"
#include "render/segment/segment.hpp"
#include "render/shape_parts.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

struct MainWindow::FilterJob {
    std::stop_source stop;
    bool done = false;
};

namespace {

QString name_of(const QString& title) {
    QString name = title;
    if (name.endsWith(QStringLiteral("…"))) name.chop(1);
    return name;
}

FilterItem kind_item(const char* name, const char* title, const char* kind, QList<QKeySequence> keys = {}) {
    FilterItem item;
    item.name = QString::fromLatin1(name);
    item.title = QString::fromUtf8(title);
    item.kind = kind;
    item.keys = std::move(keys);
    return item;
}

// A preset (filters.rs:45-50): its kind and settings from core's table.
FilterItem preset_item(const char* name, std::string_view upstream) {
    FilterItem item;
    item.name = QString::fromLatin1(name);
    for (const core::FilterPreset& preset : core::filter_presets()) {
        if (preset.upstream != upstream) continue;
        item.title = QString::fromUtf8(preset.name.data(), static_cast<qsizetype>(preset.name.size()));
        item.kind = std::string(preset.kind);
        item.fixed = preset.params;
    }
    return item;
}

// A command the window has already (ゆがみ…, スマートフィルター用に変換, the plugins'): put in the menu by its name.
FilterItem existing(const char* name) {
    FilterItem item;
    item.name = QString::fromLatin1(name);
    item.op.clear();
    return item;
}

FilterItem separator() { return FilterItem{}; }

// 前回のフィルター's name as a command (its words but while the フィルター menu is open, when they are the last one's).
const QString kLastFilterWords = QStringLiteral("フィルターの再実行");

FilterItem op_item(const char* name, const char* title, const char* op, const char* kind, QList<QKeySequence> keys = {}) {
    FilterItem item = kind_item(name, title, kind, std::move(keys));
    item.op = op;
    return item;
}

FilterItem manga_item(const char* name, const char* title, const char* kind) {
    FilterItem item = kind_item(name, title, kind);
    item.python = true;
    return item;
}

}  // namespace

const std::vector<FilterSubmenu>& filter_menu_table() {
    static const std::vector<FilterSubmenu> table{
        {QString(), QString(),
         {existing("act_smart_filters_convert"), separator(), kind_item("act_filter_gallery", "フィルターギャラリー…", "filter_gallery"),
          kind_item("act_filter_adaptive_wide_angle", "広角補正…", "adaptive_wide_angle", {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+A"))}),
          kind_item("act_filter_camera_raw", "Camera Raw フィルター…", "camera_raw", {QKeySequence(QStringLiteral("Ctrl+Shift+A"))}),
          kind_item("act_filter_lens_correction", "レンズ補正…", "lens_correction"),  // (no key: Ctrl+Shift+R is 定規を表示)
          existing("act_liquify_filter"),
          op_item("act_filter_vanishing_point", "消点…", "vanishing_point", "vanishing_point", {QKeySequence(QStringLiteral("Ctrl+Alt+V"))})}},
        {QStringLiteral("sharpen"), QStringLiteral("シャープ"),
         {kind_item("act_filter_sharpen_unsharp_mask", "アンシャープマスク…", "sharpen_unsharp_mask"),
          preset_item("act_filter_sharpen", "filter.sharpen.sharpen"), preset_item("act_filter_sharpen_more", "filter.sharpen.sharpenMore"),
          preset_item("act_filter_sharpen_edges", "filter.sharpen.sharpenEdges"), kind_item("act_filter_sharpen_smart", "スマートシャープ…", "sharpen_smart")}},
        {QStringLiteral("noise"), QStringLiteral("ノイズ"),
         {kind_item("act_filter_noise_dust_and_scratches", "ダスト＆スクラッチ…", "noise_dust_and_scratches"),
          kind_item("act_filter_noise_add", "ノイズを加える…", "noise_add"), kind_item("act_filter_noise_reduce", "ノイズを軽減…", "noise_reduce"),
          kind_item("act_filter_noise_median", "明るさの中間値…", "noise_median"), preset_item("act_filter_despeckle", "filter.noise.despeckle")}},
        {QStringLiteral("pixelate"), QStringLiteral("ピクセレート"),
         {kind_item("act_filter_pixelate_color_halftone", "カラーハーフトーン…", "pixelate_color_halftone"),
          kind_item("act_filter_pixelate_fragment", "ぶれ", "pixelate_fragment"),
          kind_item("act_filter_pixelate_mezzotint", "メゾティント…", "pixelate_mezzotint"),
          kind_item("act_filter_pixelate_mosaic", "モザイク…", "pixelate_mosaic"), kind_item("act_filter_pixelate_crystallize", "水晶…", "pixelate_crystallize"),
          kind_item("act_filter_pixelate_pointillize", "点描…", "pixelate_pointillize"), kind_item("act_filter_pixelate_facet", "ファセット", "pixelate_facet")}},
        {QStringLiteral("video"), QStringLiteral("ビデオ"),
         {kind_item("act_filter_video_ntsc_colors", "NTSC カラー", "video_ntsc_colors"),
          kind_item("act_filter_video_de_interlace", "インターレース解除…", "video_de_interlace")}},
        {QStringLiteral("blur"), QStringLiteral("ぼかし"),
         {preset_item("act_filter_blur", "filter.blur.blur"), preset_item("act_filter_blur_more", "filter.blur.blurMore"),
          kind_item("act_filter_blur_gaussian", "ぼかし（ガウス）…", "blur_gaussian"), kind_item("act_filter_blur_shape", "ぼかし（シェイプ）…", "blur_shape"),
          kind_item("act_filter_blur_box", "ぼかし（ボックス）…", "blur_box"), kind_item("act_filter_blur_lens", "ぼかし（レンズ）…", "blur_lens"),
          kind_item("act_filter_blur_motion", "ぼかし（移動）…", "blur_motion"), kind_item("act_filter_blur_smart", "ぼかし（詳細）…", "blur_smart"),
          kind_item("act_filter_blur_surface", "ぼかし（表面）…", "blur_surface"), kind_item("act_filter_blur_radial", "ぼかし（放射状）…", "blur_radial"),
          kind_item("act_filter_blur_average", "平均", "blur_average")}},
        {QStringLiteral("blur_gallery"), QStringLiteral("ぼかしギャラリー"),
         {kind_item("act_filter_blur_field", "フィールドぼかし…", "blur_field"), kind_item("act_filter_blur_iris", "虹彩絞りぼかし…", "blur_iris"),
          kind_item("act_filter_blur_tilt_shift", "チルトシフト…", "blur_tilt_shift"), kind_item("act_filter_blur_path", "パスぼかし…", "blur_path"),
          kind_item("act_filter_blur_spin", "スピンぼかし…", "blur_spin")}},
        {QStringLiteral("stylize"), QStringLiteral("表現手法"),
         {kind_item("act_filter_stylize_emboss", "エンボス…", "stylize_emboss"), kind_item("act_filter_stylize_solarize", "ソラリゼーション", "stylize_solarize"),
          kind_item("act_filter_stylize_tiles", "タイル…", "stylize_tiles"), kind_item("act_filter_stylize_extrude", "押し出し…", "stylize_extrude"),
          kind_item("act_filter_stylize_diffuse", "拡散…", "stylize_diffuse"), kind_item("act_filter_stylize_wind", "風…", "stylize_wind"),
          kind_item("act_filter_stylize_find_edges", "輪郭検出", "stylize_find_edges"),
          kind_item("act_filter_stylize_trace_contour", "輪郭のトレース…", "stylize_trace_contour"),
          kind_item("act_filter_stylize_oil_paint", "油彩…", "stylize_oil_paint")}},
        {QStringLiteral("render"), QStringLiteral("描画"),
         {op_item("act_filter_render_flame", "炎…", "render_picture", "flame"), op_item("act_filter_render_picture_frame", "額縁…", "render_picture", "picture_frame"),
          op_item("act_filter_render_tree", "木…", "render_picture", "tree"), separator(), kind_item("act_filter_render_fibers", "ファイバー…", "render_fibers"),
          kind_item("act_filter_render_clouds", "雲模様 1", "render_clouds"),
          kind_item("act_filter_render_difference_clouds", "雲模様 2", "render_difference_clouds"),
          kind_item("act_filter_render_lighting_effects", "照明効果…", "render_lighting_effects"),
          kind_item("act_filter_render_lens_flare", "逆光…", "render_lens_flare")}},
        {QStringLiteral("distort"), QStringLiteral("変形"),
         {kind_item("act_filter_distort_shear", "シアー…", "distort_shear"), kind_item("act_filter_distort_zigzag", "ジグザグ…", "distort_zigzag"),
          kind_item("act_filter_distort_pinch", "つまむ…", "distort_pinch"), kind_item("act_filter_distort_wave", "波形…", "distort_wave"),
          kind_item("act_filter_distort_ripple", "波紋…", "distort_ripple"), kind_item("act_filter_distort_spherize", "球面…", "distort_spherize"),
          kind_item("act_filter_distort_polar_coordinates", "極座標…", "distort_polar_coordinates"),
          kind_item("act_filter_distort_displace", "置き換え…", "distort_displace"), kind_item("act_filter_distort_twirl", "渦巻き…", "distort_twirl")}},
        {QStringLiteral("other"), QStringLiteral("その他"),
         {kind_item("act_filter_other_hsb_hsl", "HSB/HSL", "other_hsb_hsl"), kind_item("act_filter_other_custom", "カスタム…", "other_custom"),
          kind_item("act_filter_other_high_pass", "ハイパス…", "other_high_pass"), kind_item("act_filter_other_minimum", "明るさの最小値…", "other_minimum"),
          kind_item("act_filter_other_maximum", "明るさの最大値…", "other_maximum"), kind_item("act_filter_other_offset", "スクロール…", "other_offset")}},
        // (Genko's own, decision 1: Python's kinds in their old windows; 光彩 — 光彩拡散 before, the gallery's name)
        {QStringLiteral("manga"), QStringLiteral("漫画"),
         {manga_item("act_filter_manga_lineart", "線画抽出…", "lineart"), manga_item("act_filter_manga_despeckle", "ゴミ取り…", "despeckle"),
          manga_item("act_filter_manga_rain", "雨…", "rain"), manga_item("act_filter_manga_glow", "光彩…", "glow"),
          manga_item("act_filter_manga_bitonal", "白黒にする…", "bitonal")}},
        // (the filters a person installed and chose to run come first when it opens: their contents are PC5's)
        {QStringLiteral("plugins"), QStringLiteral("プラグイン"), {existing("act_plugins"), existing("act_plugin_settings")}},
    };
    return table;
}

const FilterItem* filter_item(const QString& name) {
    for (const FilterSubmenu& menu : filter_menu_table()) {
        for (const FilterItem& item : menu.items) {
            if (item.name == name) return &item;
        }
    }
    return nullptr;
}

const std::vector<std::pair<std::string, FilterWindow>>& filter_windows() {
    // (D3 filter_gallery, D4 blur_field … blur_spin, render_lighting_effects, flame, picture_frame, tree, D5 camera_raw, D6
    // lens_correction, adaptive_wide_angle, vanishing_point: each its group's)
    static const std::vector<std::pair<std::string, FilterWindow>> all{
        {"filter_gallery", [](const FilterWindowCall& call) { filter_gallery::open(call); }},  // (D3: app/filter_gallery_window.cpp)
        {"lens_correction", [](const FilterWindowCall& call) { lens_window::open_lens(call); }},  // (D6: app/lens_window.cpp)
        {"adaptive_wide_angle", [](const FilterWindowCall& call) { lens_window::open_wide_angle(call); }},
        {"vanishing_point", [](const FilterWindowCall& call) { vanishing_window::open(call); }},  // (D6: app/vanishing_window.cpp)
        // (D4: 照明効果's window, 炎・額縁・木's windows, ぼかしギャラリー's work over the canvas)
        {"render_lighting_effects", [](const FilterWindowCall& call) { lighting_window::open(call); }},  // (app/lighting_window.cpp)
        {"flame", [](const FilterWindowCall& call) { render_dialogs::open(call); }},  // (app/render_dialogs.cpp)
        {"picture_frame", [](const FilterWindowCall& call) { render_dialogs::open(call); }},
        {"tree", [](const FilterWindowCall& call) { render_dialogs::open(call); }},
        {"blur_field", [](const FilterWindowCall& call) { blur_gallery::open(call); }},  // (app/blur_gallery_tool.cpp)
        {"blur_iris", [](const FilterWindowCall& call) { blur_gallery::open(call); }},
        {"blur_tilt_shift", [](const FilterWindowCall& call) { blur_gallery::open(call); }},
        {"blur_path", [](const FilterWindowCall& call) { blur_gallery::open(call); }},
        {"blur_spin", [](const FilterWindowCall& call) { blur_gallery::open(call); }},
    };
    return all;
}

const FilterWindow* filter_window(std::string_view kind) {
    for (const auto& [name, window] : filter_windows()) {
        if (name == kind) return &window;
    }
    return nullptr;
}

bool filter_available(const FilterItem& item) {
    if (item.op.empty() || item.python) return true;
    if (filter_window(item.kind) != nullptr) return true;
    if (item.op != "filter_raster") return false;  // (render_picture and vanishing_point: their groups' windows)
    return core::pc4_filter(item.kind) && render::fx::kernel(item.kind) != nullptr;
}

// --- the window's commands ---------------------------------------------------------------------------------------------

void MainWindow::build_filter_actions() {
    for (const FilterSubmenu& menu : filter_menu_table()) {
        for (const FilterItem& item : menu.items) {
            if (item.name.isEmpty() || item.op.empty()) continue;  // (a separator, a command of its own)
            const QString name = item.name;
            QAction* act = make(name, item.title, [this, name] {
                if (const FilterItem* found = filter_item(name)) filter_command(*found);
            }, item.keys);
            act->setEnabled(filter_available(item));
        }
    }
    make("act_filter_last", kLastFilterWords, [this] { repeat_filter(); }, {QKeySequence(QStringLiteral("Ctrl+F"))},
         QStringLiteral("最後にかけたフィルターを、同じ設定で今のレイヤーにかけます"))
        ->setEnabled(false);
}

void MainWindow::build_filter_menus(QMenu* filter) {
    filter_menu_ = filter;
    filter->setObjectName(QStringLiteral("menu_filter"));
    filter->addAction(action("act_filter_last"));
    for (const FilterSubmenu& table : filter_menu_table()) {
        QMenu* menu = filter;
        if (!table.key.isEmpty()) {
            if (table.key == QLatin1String("manga") || table.key == QLatin1String("sharpen")) filter->addSeparator();
            menu = filter->addMenu(table.title);
            menu->setObjectName(QStringLiteral("menu_filter_%1").arg(table.key));
            if (table.key == QLatin1String("plugins")) {
                filter_plugins_menu_ = menu;
                menu->setProperty("remade", true);
            }
        }
        for (const FilterItem& item : table.items) {
            if (item.name.isEmpty()) {
                menu->addSeparator();
            } else if (QAction* act = action(item.name)) {
                menu->addAction(act);
            }
        }
    }
    connect(filter, &QMenu::aboutToShow, this, [this] {
        follow_filter_menu();
        // (前回のフィルター's words while the menu is open: the last one's name, as Photoshop's; its name as a command — the
        // keys, クイックアクセス, コマンドを探す — stays フィルターの再実行, back when the menu closes)
        if (QAction* last = action("act_filter_last"); last != nullptr && last_filter_)
            last->setText(QString::fromStdString(last_filter_->value("title", std::string())));
    });
    // (while the menu is not open its commands are on as far as this build has them: a key — Ctrl+F — or コマンドを探す
    // reaches them, and the command itself says what it cannot work on)
    connect(filter, &QMenu::aboutToHide, this, [this] {
        for (const FilterSubmenu& table : filter_menu_table()) {
            for (const FilterItem& item : table.items) {
                if (item.name.isEmpty() || item.op.empty()) continue;
                if (QAction* act = action(item.name)) act->setEnabled(filter_available(item));
            }
        }
        if (QAction* last = action("act_filter_last")) {
            last->setEnabled(last_filter_.has_value());
            last->setText(kLastFilterWords);
        }
    });
    follow_filter_menu();
    emit filter->aboutToHide();
}

void MainWindow::follow_filter_menu() {
    const core::Layer* layer = target_layer();
    const bool paint = layer != nullptr && (layer->kind == core::LayerKind::Raster || layer->kind == core::LayerKind::Strokes);
    const bool smart = layer != nullptr && layer->kind == core::LayerKind::Smart;
    for (const FilterSubmenu& table : filter_menu_table()) {
        for (const FilterItem& item : table.items) {
            if (item.name.isEmpty() || item.op.empty()) continue;
            QAction* act = action(item.name);
            if (act == nullptr) continue;
            // (grey: not in this build yet; the layer drawn on not a paint, pen or smart object layer; a smart object and a
            // command that cannot be a smart filter — 消点 is not: ラスタライズしてから. 炎・額縁・木 draw on any page: on a
            // layer that is not a paint layer, or none, a new one — PC4 D4, review P3-7)
            const bool on = item.op == "render_picture" ? current_page() != nullptr
                                                         : paint || (smart && item.op == "filter_raster" && (item.python || core::smart::filter_kind(item.kind)));
            act->setEnabled(filter_available(item) && on);
        }
    }
    if (QAction* last = action("act_filter_last")) last->setEnabled(last_filter_.has_value() && (paint || smart));
    // (the plugins' filters, each a person installed and chose to run, before the folder and the settings)
    if (filter_plugins_menu_ != nullptr) {
        for (QAction* act : filter_plugins_menu_->actions()) {
            if (act->property("filter_plugin").isValid()) {
                filter_plugins_menu_->removeAction(act);
                act->deleteLater();
            }
        }
        QAction* before = filter_plugins_menu_->actions().isEmpty() ? nullptr : filter_plugins_menu_->actions().front();
        std::vector<QAction*> made;
        try {
            for (const auto& plugin : render::plugins::available()) {
                auto* act = new QAction(QString::fromStdString(plugin.name), filter_plugins_menu_);
                const std::string kind = std::string(render::plugins::kPrefix) + plugin.key;
                act->setData(QString::fromStdString(kind));
                act->setProperty("filter_plugin", true);
                act->setEnabled(paint);
                FilterItem item;
                item.title = act->text() + QStringLiteral("…");
                item.kind = kind;
                item.python = true;
                connect(act, &QAction::triggered, this, [this, item] { filter_command(item); });
                made.push_back(act);
            }
        } catch (const std::exception&) {
        }
        filter_plugins_menu_->insertActions(before, QList<QAction*>(made.begin(), made.end()));
        if (!made.empty()) filter_plugins_menu_->insertSeparator(before)->setProperty("filter_plugin", true);
    }
}

void MainWindow::filter_command(const FilterItem& item) {
    const QString name = name_of(item.title);
    if (!filter_available(item)) {
        flash(QStringLiteral("「%1」は、この版ではまだ使えません").arg(name), 4000, true);
        return;
    }
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    const bool paint = layer != nullptr && (layer->kind == core::LayerKind::Raster || layer->kind == core::LayerKind::Strokes);
    const bool smart = layer != nullptr && layer->kind == core::LayerKind::Smart;
    // (PC4 D4, review P3-7: 炎・額縁・木 draw on any page — on the paint or pen layer chosen, else on a new one above what is
    // chosen, or on top —; their own questions after their window)
    const bool drawing = item.op == "render_picture";
    if (page == nullptr || (!drawing && !paint && !smart)) {
        flash(QStringLiteral("「%1」は、ペン・ペイントのレイヤーかスマートオブジェクトを選んでから使います").arg(name), 4000, true);
        return;
    }
    if (!drawing && smart && (item.op != "filter_raster" || item.kind.starts_with(render::plugins::kPrefix) ||
                              (!item.python && !core::smart::filter_kind(item.kind)))) {
        flash(QStringLiteral("「%1」はスマートオブジェクトには使えません（ラスタライズしてから使います）").arg(name), 5000, true);
        return;
    }
    if ((!drawing || paint) && layer->locked) {  // (炎・額縁・木 on a locked layer that is not a paint layer: a new one)
        flash(QStringLiteral("レイヤーがロックされています"), 4000, true);
        return;
    }
    const auto asked = asking();
    const std::optional<Json> area = selection_area();
    // (a pen layer's lines, its fills and shapes become pixels — as filter_raster bakes them —: asked first, as the layer
    // panel's かける… did; Photoshop asks to rasterize a type layer before a filter. A channel drawn on leaves them as they are.
    // 炎・額縁・木: asked after their window, unless they go on a new layer)
    const bool lines = layer != nullptr && layer->stroke_count() > 0;
    const bool vectors = paint && (lines || !layer->patches.empty() || render::shape_parts::any(*layer)) && channel_view().target.empty();
    const QString vectors_words = lines ? QStringLiteral("ペンの線は画像になり、あとから線として消せなくなります")
                                        : QStringLiteral("図形・塗りは画像になり、あとから図形として直せなくなります");
    if (!drawing && vectors) {
        if (!ask::question(this, name, QStringLiteral("レイヤー「%1」の%2。「%3」をかけますか\n（元に戻す で取り消せます）").arg(wording::layer_label(*layer), vectors_words, name)) ||
            !still(asked))
            return;
    }
    // (on a smart object: a smart filter, as Photoshop adds it, not asked — but when Photoshop's picture of it goes, its
    // filters drawn again by Genko, or a selection is not used for a filter mask it has already: the layer panel's words)
    if (!drawing && smart && layer->smart && layer->smart->is_object()) {
        const Json* psd = core::get(*layer->smart, "psd");
        const bool photoshops = psd != nullptr && psd->is_object() && psd->contains("shown") && !(*psd)["shown"].is_null();
        const Json* mask = core::get(*layer->smart, "filter_mask");
        const bool masked = area && mask != nullptr && mask->is_object();
        if (photoshops || masked) {
            QString text = QStringLiteral("スマートオブジェクト「%1」に「%2」をスマートフィルターとして足します（あとから直せます）。").arg(wording::layer_label(*layer), name);
            if (photoshops) text += QStringLiteral("\nPhotoshop で付けたフィルターの見た目は、Genko が描き直した絵に替わります。");
            if (masked) text += QStringLiteral("\nフィルターマスクが既にあるので、選択範囲は使いません（フィルターマスクはそのままです）。");
            text += QStringLiteral("\n（元に戻す で取り消せます）");
            if (!ask::question(this, name, text) || !still(asked)) return;
        }
    }
    const std::string layer_id = layer != nullptr ? layer->id : std::string(), kind = item.kind;
    const Json index = page->index.json();
    const std::string op_name = smart && !drawing ? "smart_filter_add" : item.op;
    // (a kind with a seed — ノイズを加える, ぼかし（レンズ）'s noise —: a new one each time it is applied, as Photoshop's grains
    // differ each time — 前回のフィルター too —; the window's preview and its かける the same one; the op holds it)
    const bool python = item.python;
    const core::AdjustKind* table = core::adjust_kind(kind);
    const bool seeded = !python && table != nullptr &&
                        std::any_of(table->params.begin(), table->params.end(), [](const core::AdjustParam& p) { return p.key == "seed"; });
    const std::optional<std::uint32_t> seed = seeded ? std::optional<std::uint32_t>(QRandomGenerator::global()->generate()) : std::nullopt;
    const auto op_of = [op_name, index, layer_id, kind, area, seed](const Json& params) {
        Json op{{"op", op_name}, {"page", index}, {"kind", kind}};
        if (!layer_id.empty()) op["id"] = layer_id;  // (炎・額縁・木 with no layer chosen: a new one on top)
        op.update(params);
        if (seed && !op.contains("seed")) op["seed"] = *seed;
        if (area) op["area"] = *area;
        return op;
    };
    // (heavy: its work on the layer's grid past what a moment takes — render::fx; Python's and the plugins' as before)
    const render::fx::Grid grid = layer != nullptr ? render::fx::layer_grid(*page, *layer) : render::fx::Grid{};
    const auto heavy = [grid, kind](const Json& params) {
        // (PC4 D6: 消点 — a kind without a kernel — on a thread whenever it pastes or stamps: its pictures over the page's
        // grid, its strokes' footprints; the planes alone at once)
        if (kind == "vanishing_point") {
            const auto given = [&params](const char* key) {
                const Json* list = core::get(params, key);
                return list != nullptr && list->is_array() && !list->empty();
            };
            return given("paste") || given("clone");
        }
        // (PC4 D4: 炎・額縁・木 — shapes laid over the layer's grid, a flame's thousands of them — always on a thread)
        if (core::picture_kind(kind) != nullptr) return true;
        return render::fx::work(kind, params, std::uint64_t(grid.width) * grid.height, grid.s, grid.levels) >= render::fx::kHeavyWork;
    };
    const std::string op = item.op;
    // (炎・額縁・木 on the pen layer itself, not a new one: its lines asked about now, after the window — PC4 D4, review P3-7)
    const QString lines_question =
        drawing && vectors ? QStringLiteral("レイヤー「%1」の%2。そのレイヤーに「%3」を描きますか（「新しいレイヤーに描く」なら線はそのままです）\n（元に戻す で取り消せます）")
                                 .arg(wording::layer_label(*layer), vectors_words, name)
                           : QString();
    const auto send = [this, op_of, heavy, name, kind, op, python, asked, lines_question](const Json& params) {
        if (!still(asked)) return;
        if (!lines_question.isEmpty() && !(params.contains("new_layer") && params["new_layer"] == Json(true)) &&
            (!ask::question(this, name, lines_question) || !still(asked)))
            return;
        if (!send_filter(Json::array({op_of(params)}), name, heavy(params))) return;  // (refused, stopped: not remembered)
        if (op != "vanishing_point") {  // (消点 is not 前回のフィルター: PhotoCraft's lastFilter neither)
            last_filter_ = Json{{"title", name.toStdString()}, {"kind", kind}, {"op", op}, {"python", python}, {"params", params}};
            if (QAction* last = action("act_filter_last")) {
                last->setEnabled(true);
                last->setStatusTip(QStringLiteral("最後にかけたフィルター（%1）を、同じ設定で今のレイヤーにかけます").arg(name));
            }
        }
    };
    if (item.fixed) {
        send(*item.fixed);
        return;
    }
    const FilterPreview show = [this, op_of, heavy](const std::optional<Json>& values) {
        if (!values) {
            preview_filter(std::nullopt, false);
            return;
        }
        preview_filter(Json::array({op_of(adjust_forms::without_nulls(*values))}), heavy(*values));
    };
    if (const FilterWindow* own = filter_window(kind)) {
        FilterWindowCall call;
        call.window = this;
        call.item = &item;
        call.send = send;
        call.page = page;
        call.layer = layer;
        call.preview = show;  // (D4)
        call.previewing = [this] { return filter_previewing(); };
        (*own)(call);
        return;
    }
    // (D4: a kind whose settings are its seed and the main and sub colours alone — 雲模様 1・2 — has no window, as Photoshop's:
    // the colours of now)
    const core::AdjustKind* found = python ? nullptr : core::adjust_kind(kind);
    const core::FilterInfo* info = python ? nullptr : core::filter_info(kind);
    if (found != nullptr && info != nullptr && !info->colours.empty() &&
        std::all_of(found->params.begin(), found->params.end(), [info](const core::AdjustParam& p) {
            return p.key == "seed" || std::find(info->colours.begin(), info->colours.end(), p.key) != info->colours.end();
        })) {
        const auto& rgb = pen().rgb;
        const Json main_colour = Json::array({static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])});
        Json sub_colour = Json::array({255, 255, 255});
        if (colours() != nullptr) {
            const auto sub = colours()->sub_rgb();
            sub_colour = Json::array({sub[0], sub[1], sub[2]});
        }
        Json params = Json::object();
        for (const std::string_view key : info->colours) params[std::string(key)] = key == "background" ? sub_colour : main_colour;
        send(params);
        return;
    }
    std::optional<Json> params;
    if (python) {  // (漫画's five and the plugins: their old windows)
        params = filter_params(this, kind, Json::object(), show);
    } else if (found != nullptr && !found->params.empty()) {
        adjust_forms::FormContext context;
        context.page = page;
        context.smart = smart;
        if (!smart && area) {  // (D4: the selection's box, its places' frame)
            const auto box = canvas()->selection_box();
            if (box[2] > box[0] && box[3] > box[1]) context.base_aspect = (box[2] - box[0]) / (box[3] - box[1]);
        }
        const auto& rgb = pen().rgb;
        context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
        if (colours() != nullptr) context.sub = colours()->sub_rgb();
        params = filter_forms::dialog(this, kind, Json::object(), context, filter_forms::Preview{show, [this] { return filter_previewing(); }});
    } else {
        params = Json::object();  // (no settings: no window — ぶれ, ファセット, 平均 …)
    }
    if (!params) return;
    send(adjust_forms::without_nulls(*params));
}

void MainWindow::repeat_filter() {
    if (!last_filter_) {
        flash(QStringLiteral("まだフィルターをかけていません"), 3000);
        return;
    }
    FilterItem item;
    item.title = QString::fromStdString(last_filter_->value("title", std::string()));
    item.kind = last_filter_->value("kind", std::string());
    item.op = last_filter_->value("op", std::string("filter_raster"));
    item.python = last_filter_->value("python", false);
    item.fixed = last_filter_->contains("params") ? (*last_filter_)["params"] : Json::object();
    filter_command(item);
}

namespace {

struct Worked {
    std::optional<core::ApplyResult> result;
    std::vector<std::string> taken;
    std::string error;
    bool cancelled = false;
};

// The window of a heavy filter at work (over every window of the app: nothing else changes the books meanwhile, and none
// is closed under it): 中止 — its button, Esc, closing it — asks the work to stop and says so, the window staying until the
// thread is back (QProgressDialog's own cancel would hide it at once).
class FilterProgress : public QProgressDialog {
public:
    using QProgressDialog::QProgressDialog;
    std::function<void()> stopping;

protected:
    void reject() override {
        if (stopping) stopping();
    }
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        if (stopping) stopping();
    }
};

}  // namespace

bool MainWindow::send_filter(const Json& ops, const QString& name, bool heavy) {
    if (!heavy) return apply_ops(ops);
    // (a heavy one on a thread on a copy of the book, the window waiting with its progress and 中止 — nothing changed when
    // stopped —; the book changed meanwhile — another window of it, an agent's change taken in —: worked again on it, on
    // the thread; another book here by then: nothing)
    const QPointer<MainWindow> alive(this);
    const std::shared_ptr<Session> origin = session_;
    auto stop = std::make_shared<std::stop_source>();
    auto done = std::make_shared<std::atomic<double>>(0.0);
    const QPointer<FilterProgress> progress = new FilterProgress(QStringLiteral("「%1」をかけています…").arg(name), QStringLiteral("中止"), 0, 1000, this);
    progress->setObjectName(QStringLiteral("filter_progress"));
    progress->setWindowTitle(name);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    QObject::disconnect(progress, SIGNAL(canceled()), progress, SLOT(cancel()));  // (kept up while it stops: `stopping`)
    progress->stopping = [stop, p = progress.data(), words = QStringLiteral("「%1」を中止しています…").arg(name)] {
        stop->request_stop();
        p->setLabelText(words);
    };
    QObject::connect(progress, &QProgressDialog::canceled, progress, [p = progress.data()] {
        if (p->stopping) p->stopping();
    });
    filter_working_ = stop;
    progress->show();
    QTimer tick;
    tick.setInterval(100);
    QObject::connect(&tick, &QTimer::timeout, progress, [p = progress.data(), done] { p->setValue(static_cast<int>(done->load() * 1000)); });
    tick.start();
    bool applied = false;
    QString said;
    bool trouble = false;
    for (;;) {
        Json targeted;
        try {
            render::use_brushes_of(book());
            targeted = with_contents_target(with_channel_target(ops));
        } catch (const std::exception& error) {  // (as apply_ops says it: the book as it was)
            said = wording::error(QString::fromUtf8(error.what()));
            trouble = true;
            break;
        }
        const DocPtr base = session_->snapshot();
        done->store(0.0);
        QFutureWatcher<Worked> watcher;
        QEventLoop loop;
        QObject::connect(&watcher, &QFutureWatcher<Worked>::finished, &loop, &QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run([base, targeted, actor = session_->actor(), token = stop->get_token(), done]() -> Worked {
            const render::segment::StopScope scope(token);
            const render::fx::ProgressScope progress_scope(*done);
            core::ScopedIdScript script;
            try {
                core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(*base, targeted, core::Actor(actor));
                return Worked{std::move(result), script.taken(), {}, false};
            } catch (const render::Cancelled&) {
                return Worked{std::nullopt, {}, {}, true};
            } catch (const std::exception& error) {
                return Worked{std::nullopt, {}, error.what(), false};
            }
        }));
        if (!watcher.isFinished()) loop.exec();
        if (!watcher.isFinished()) stop->request_stop();  // (the loop left early — the app quitting —: stopped, waited for)
        Worked worked = watcher.result();
        if (!alive) return false;  // (the window gone meanwhile: nothing of it touched)
        if (session_ != origin) {
            said = QStringLiteral("別の原稿に替わったので、「%1」はかけませんでした").arg(name);
            break;
        }
        if (worked.cancelled || stop->stop_requested()) {  // (stopped: not laid on, even when the thread had finished)
            said = QStringLiteral("「%1」を中止しました（原稿は変わっていません）").arg(name);
            break;
        }
        if (!worked.result) {
            said = wording::error(QString::fromStdString(worked.error));
            trouble = true;
            break;
        }
        if (session_->snapshot() != base) {  // (the book changed while it was worked: again on the book as it is now)
            if (progress) progress->setLabelText(QStringLiteral("原稿が変わったので、「%1」をもう一度かけています…").arg(name));
            continue;
        }
        applied = apply_worked(ops, targeted, base, std::move(*worked.result), worked.taken);
        break;
    }
    tick.stop();
    filter_working_.reset();
    if (progress) {
        progress->stopping = nullptr;
        progress->hide();
        progress->deleteLater();
    }
    if (!said.isEmpty()) flash(said, trouble ? 6000 : 4000, trouble);
    if (close_after_filter_) {  // (asked to close while it was worked: closed now that it is back)
        close_after_filter_ = false;
        QTimer::singleShot(0, this, [this] { close(); });
    }
    return applied;
}

void MainWindow::stop_filter_work() {
    if (filter_job_) filter_job_->stop.request_stop();
    if (filter_working_) filter_working_->request_stop();
}

void MainWindow::preview_filter(const std::optional<Json>& ops, bool heavy) {
    if (filter_job_) {
        filter_job_->stop.request_stop();
        filter_job_.reset();
    }
    if (!ops || !heavy) {
        preview_ops(ops);
        return;
    }
    // (a heavy one: worked out on a thread on a copy of the book at the screen's resolution, its layer's pixels averaged
    // down — its lengths with them —, shown when it is done and still wanted)
    auto job = std::make_shared<FilterJob>();
    filter_job_ = job;
    Json targeted;
    try {
        render::use_brushes_of(book());
        targeted = with_contents_target(with_channel_target(*ops));
    } catch (const std::exception&) {
        show_preview(std::nullopt);
        return;
    }
    const DocPtr base = session_->snapshot();
    const std::size_t at = static_cast<std::size_t>(std::max(0, page_index_));
    const std::string id = (*ops)[0].value("id", std::string());
    int most = 2400;
    if (const core::Page* page = current_page()) {  // (at most the page at the working 200 dpi)
        most = std::min(most, static_cast<int>(std::lround(std::max(page->spec.width_mm.value(), page->spec.height_mm.value()) / 25.4 * 200)));
    }
    auto* watcher = new QFutureWatcher<std::optional<core::Document>>(this);
    connect(watcher, &QFutureWatcher<std::optional<core::Document>>::finished, this, [this, watcher, job] {
        std::optional<core::Document> doc = watcher->result();
        watcher->deleteLater();
        if (filter_job_ != job || job->stop.stop_requested()) return;  // (not wanted any more)
        job->done = true;
        show_preview(std::move(doc));
    });
    watcher->setFuture(QtConcurrent::run([base, targeted, at, id, most, actor = session_->actor(), token = job->stop.get_token()]() -> std::optional<core::Document> {
        const render::segment::StopScope scope(token);
        core::ScopedIdScript script;
        try {
            const core::Document reduced = render::fx::preview_book(*base, at, id, most);
            return core::CommandBus(render::ops_registry()).apply(reduced, targeted, core::Actor(actor)).doc;
        } catch (const std::exception&) {  // (stopped, or refused: no preview)
            return std::nullopt;
        }
    }));
}

bool MainWindow::filter_previewing() const { return filter_job_ && !filter_job_->done; }

}  // namespace genko::app
