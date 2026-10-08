// 素材・トーン・効果線, the view's extras and the layer commands (M3④-5, Python's main.py): the materials panel and its
// dock; 効果線 (a tool: a click in a panel puts the chosen kind, its centre drags) and its kinds, drawn only inside a
// selection or kept clear of it; 素材を置く (a tool: a click puts the material chosen); トーン — on the selection or the
// chosen panel, or where a click finds lines around it —; 上下反転, オニオンスキン, 網点を画面で; コマの形を元に戻す; and
// the layer commands of the レイヤー menu (Python's act_layer_*), which are the layer panel's buttons.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QScrollArea>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/layer_dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/material_panel.hpp"
#include "app/material_tabs.hpp"
#include "app/scanner.hpp"
#include "app/tiles.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/effects.hpp"
#include "render/materials.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

namespace {

const std::vector<std::pair<const char*, const char*>> kEffectKinds{
    {"focus", "集中線"}, {"speed", "流線"}, {"uni_flash", "ウニフラッシュ"}, {"beta_flash", "ベタフラッシュ"}};

}  // namespace

void MainWindow::build_material_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const char* name, const QList<QKeySequence>& keys, const QString& tip) {
        const QString key = QString::fromLatin1(name);
        QAction* act = make(QString::fromLatin1(attribute), title, [this, key] { choose_tool(key); }, keys, tip, true);
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[key] = act;
        return act;
    };
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    tool("act_effect", QStringLiteral("効果線"), "effect", key("K"),
         QStringLiteral("コマの中をクリックすると、選んだ効果線（集中線など）が入る。中心の＋をドラッグで動かす"));
    tool("act_stamp", QStringLiteral("素材を置く"), "stamp", {}, QStringLiteral("素材パネルで選んだ素材を、クリックした所に置く"));
    make("act_tone_here", QStringLiteral("選択範囲・選んだコマにトーンを貼る"), [this] { tone_here(); }, key("Ctrl+Shift+T"),
         QStringLiteral("素材パネルで選んだトーン（なければ網点 60 線 30%）を貼ります"));
    make("act_tone_click", QStringLiteral("クリックした所にトーンを貼る"), [this] { tone_click(); }, {},
         QStringLiteral("線で囲まれた所をクリックすると、そこにトーンが入ります"));
    for (const auto& [kind, label] : kEffectKinds) {
        auto* act = new QAction(QString::fromUtf8(label), this);
        act->setObjectName(QStringLiteral("cmd:%1").arg(QString::fromUtf8(label)));
        const std::string which = kind;
        connect(act, &QAction::triggered, this, [this, which] { choose_effect(which); });
        addAction(act);
        effect_actions_.push_back(act);
    }
    make("act_effect_within", QStringLiteral("選択範囲の中にだけ描く"), [this] { effect_clearing(std::string("within")); }, {},
         QStringLiteral("選んだ効果線を、投げ縄・長方形の選択範囲の中にだけ描きます"));
    make("act_effect_avoid", QStringLiteral("選択範囲を避ける"), [this] { effect_clearing(std::string("avoid")); }, {},
         QStringLiteral("選んだ効果線を、選択範囲（顔や人物を投げ縄で囲む）の手前で止めます。線は止まる所で細くなります"));
    make("act_effect_clear", QStringLiteral("避ける範囲を外す"), [this] { effect_clearing(std::nullopt); }, {},
         QStringLiteral("選んだ効果線の「中にだけ描く」「避ける」を外して、コマ全体に描きます"));
    make("act_materials", QStringLiteral("素材パネルを開く"), [this] { show_dock(QStringLiteral("素材")); });
    // the view
    QAction* flip_v = make("act_view_flip_v", QStringLiteral("上下反転して見る"), [] {}, key("Shift+H"),
                           QStringLiteral("表示だけを上下反転します。原稿は変わりません"), true);
    connect(flip_v, &QAction::triggered, this, [this](bool on) { canvas_->flip_view_vertical(on); });
    make("act_onion", QStringLiteral("前のページを透かす（オニオンスキン）"), [this] { onion(); });
    QAction* dots = make("act_screen_dots", QStringLiteral("網点を画面で見る"), [] {}, {},
                         QStringLiteral("トーンを、印刷と同じ網点で表示します（拡大すると点が見えます）。切ると平らな灰色で速く描きます"), true);
    connect(dots, &QAction::triggered, this, [this](bool on) {
        canvas_->renderer().set_screen_dots(on);
        canvas_->update();
    });
    // the paper read in
    make("act_import_scan", QStringLiteral("スキャン画像を線画にして取り込む…"), [this] { import_scan(false); }, {},
         QStringLiteral("紙に描いた絵の画像を新しいレイヤーに置き、線だけを抜き出します（強さ・下描きの青を消す・ゴミ取りを見ながら決める）"));
    make("act_scanner", QStringLiteral("スキャナーから取り込む…"), [this] { import_scan(true); }, {},
         QStringLiteral("スキャナーで読んだ紙を新しいレイヤーに置き、線だけを抜き出します（Windows・Linux）"));
    // the panels
    make("act_reset_shape", QStringLiteral("選んだコマの形を元に戻す"), [this] { set_selected_frame(Json{{"poly", nullptr}, {"curves", nullptr}}); });
    // the layers (the layer panel's buttons)
    const auto layers = [this](auto work) {
        return [this, work] {
            if (layer_panel_ != nullptr) work(*layer_panel_);
        };
    };
    make("act_layer_pen", QStringLiteral("新しいペンのレイヤー"), layers([](LayerPanel& p) { p.add("pen", QStringLiteral("ペン")); }), key("Ctrl+Shift+N"));
    make("act_layer_paint", QStringLiteral("新しいペイントのレイヤー"), layers([](LayerPanel& p) { p.add("paint", QStringLiteral("ペイント")); }));
    make("act_layer_folder", QStringLiteral("新しいフォルダ"), layers([](LayerPanel& p) { p.add("folder", QStringLiteral("フォルダ")); }));
    make("act_layer_dup", QStringLiteral("レイヤーを複製"), layers([](LayerPanel& p) { p.duplicate(); }), key("Ctrl+J"));
    make("act_layer_merge", QStringLiteral("下のレイヤーと結合"), layers([](LayerPanel& p) { p.merge_down(); }), key("Ctrl+Shift+E"));
    make("act_layer_delete", QStringLiteral("レイヤーを消す"), layers([](LayerPanel& p) { p.remove(); }));
    make("act_layer_up", QStringLiteral("レイヤーを前へ"), layers([](LayerPanel& p) { p.move(1); }), key("Ctrl+]"));
    make("act_layer_down", QStringLiteral("レイヤーを後ろへ"), layers([](LayerPanel& p) { p.move(-1); }), key("Ctrl+["));
    make("act_layer_draft", QStringLiteral("下描きにする（書き出さない）／戻す"), layers([](LayerPanel& p) {
        if (auto* draft = p.findChild<QCheckBox*>(QStringLiteral("layer_draft")); draft != nullptr && draft->isEnabled()) draft->click();
    }));
}

void MainWindow::build_material_dock() {
    materials_ = new MaterialPanel(this);
    materials_dock_ = new QDockWidget(QStringLiteral("素材"), this);
    materials_dock_->setObjectName(QStringLiteral("素材"));
    materials_dock_->setWidget(materials_);
    addDockWidget(Qt::RightDockWidgetArea, materials_dock_);
    materials_dock_->hide();  // Keep the initial canvas room; the window menu and work stages expose the panel.
    connect(materials_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) materials_->refresh();
    });
    view_menu_->addAction(materials_dock_->toggleViewAction());
}

void MainWindow::build_material_pages(ToolSettings* ts) {
    // ツールの設定: the effect kinds as pictures, how they are kept clear, and the panel; the materials for the stamp
    ts->add({QStringLiteral("effect")},
                        action_page({QStringLiteral("効果線の種類"), effect_tiles(effect_actions_),
                                     QStringLiteral("描く範囲（選択範囲で）"), action("act_effect_within"), action("act_effect_avoid"),
                                     action("act_effect_clear"), QStringLiteral("素材"), action("act_materials")}));
    ts->add({QStringLiteral("stamp")}, action_page({action("act_materials")}));
}

void MainWindow::material_activated(const QString& material_id, const QString& kind) {
    // (a double click puts it at once, in the middle of the page: as the M2 browser did)
    const core::Page* page = current_page();
    if (page == nullptr) return;
    Json op{{"op", "stamp_material"}, {"page", page->index.json()}, {"material_id", material_id.toStdString()},
            {"x_mm", page->spec.width_mm.value() / 2}, {"y_mm", page->spec.height_mm.value() / 2}};
    if (const core::Layer* layer = target_layer()) op["after"] = layer->id;
    if (!apply_ops(Json::array({std::move(op)}))) return;
    if (kind == QLatin1String("brush")) {
        const auto key = "my_" + QCryptographicHash::hash(material_id.toUtf8(), QCryptographicHash::Sha1).toHex().left(10).toStdString();
        if (!book().brush_custom.contains(key)) return;
        brush_->reload_kinds(key);  // (the brush panel chooses it: the pen follows)
        choose_tool(QStringLiteral("pen"));  // Updates the live pen after the successful registration.
        flash(QStringLiteral("選んだブラシで描けます"), 3500);
    }
}

// --- tones -----------------------------------------------------------------------------------------------------------

Json MainWindow::default_tone() const {
    if (materials_ != nullptr) {
        if (const auto item = materials_->current_material(); item && core::py_get(*item, "kind") == "tone") return *item;
    }
    return render::materials::get_material(config_dir(), "dot-60-30");
}

void MainWindow::after_tone(const std::string& layer_id) {
    set_target_layer(layer_id);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
}

void MainWindow::tone_here() { put_tone(default_tone(), true); }

void MainWindow::tone_click() {
    pending_material_ = default_tone();
    choose_tool(QStringLiteral("stamp"));
    flash(QStringLiteral("「%1」: 線で囲まれた所をクリックすると、そこにトーンが入ります").arg(QString::fromStdString(core::py_str(core::py_get(*pending_material_, "name", "")))),
          4000);
}

void MainWindow::put_tone(const Json& item, bool ask_click) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    Json op{{"op", "stamp_material"}, {"page", page->index.json()}, {"material_id", item["id"]}, {"id", core::new_id()}};
    if (const auto area = selection_area()) {
        op["area"] = *area;
    } else if (const core::Frame* frame = selected_frame()) {
        op["frame_id"] = frame->id;
    } else if (ask_click) {
        pending_material_ = item;
        choose_tool(QStringLiteral("stamp"));
        flash(QStringLiteral("選択範囲もコマも選ばれていません。トーンを貼る所をクリックします"), 4000);
        return;
    }
    if (apply_ops(Json::array({op}))) {
        after_tone(op["id"].get<std::string>());
        flash(QStringLiteral("「%1」を貼りました。ペンで足す・消しゴムで削る").arg(QString::fromStdString(core::py_str(core::py_get(item, "name", "")))), 3500);
    }
}

// --- materials -------------------------------------------------------------------------------------------------------

void MainWindow::use_material(const Json& item) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const Json kind = core::py_get(item, "kind");
    if (kind == "brush") {  // a brush material: added to the book and picked at once
        const auto key = "my_" + QCryptographicHash::hash(QByteArray::fromStdString(core::py_str(item["id"])), QCryptographicHash::Sha1).toHex().left(10).toStdString();
        if (apply_ops(Json::array({Json{{"op", "stamp_material"}, {"page", page->index.json()}, {"material_id", item["id"]}}}))) {
            brush_->reload_kinds(key);
            choose_tool(QStringLiteral("pen"));
            flash(QStringLiteral("ブラシ「%1」で描けます").arg(QString::fromStdString(core::py_str(core::py_get(item, "name", "")))), 3500);
        }
        return;
    }
    if (kind == "tone") {
        put_tone(item, true);
        return;
    }
    if (kind == "effect") {
        if (const core::Frame* frame = selected_frame()) {
            apply_ops(Json::array({Json{{"op", "stamp_material"}, {"page", page->index.json()}, {"material_id", item["id"]}, {"frame_id", frame->id}}}));
            return;
        }
    }
    pending_material_ = item;
    choose_tool(QStringLiteral("stamp"));
    const QString where = kind == "effect" ? QStringLiteral("コマの中") : QStringLiteral("置きたい所");
    flash(QStringLiteral("「%1」: %2をクリックします").arg(QString::fromStdString(core::py_str(core::py_get(item, "name", ""))), where), 3500);
}

void MainWindow::stamp_at(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    std::optional<Json> item = pending_material_;
    if (!item && materials_ != nullptr) item = materials_->current_material();
    if (page == nullptr || !item) {
        flash(QStringLiteral("素材パネルで素材を選びます"), 2500);
        return;
    }
    Json op{{"op", "stamp_material"}, {"page", page->index.json()}, {"material_id", (*item)["id"]}};
    const Json kind = core::py_get(*item, "kind");
    if (kind == "tone") {
        op["id"] = core::new_id();
        op["at"] = Json{{"x_mm", core::py_round(x_mm, 2)}, {"y_mm", core::py_round(y_mm, 2)}, {"gap_mm", brush_->gap_size->value()}};
        if (apply_ops(Json::array({op}))) after_tone(op["id"].get<std::string>());
        return;
    }
    if (kind == "effect" || kind == "lettering" || kind == "prim") {
        const core::Frame* frame = page->frame_at(core::Num(x_mm), core::Num(y_mm));
        op["frame_id"] = frame != nullptr ? Json(frame->id) : Json();
        op["x_mm"] = core::py_round(x_mm, 2);
        op["y_mm"] = core::py_round(y_mm, 2);
        if (kind == "lettering") op["id"] = core::new_id();
        apply_ops(Json::array({op}));  // (a lettering's words are typed over in the story panel: M4)
        return;
    }
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    op["layer_id"] = layer->id;
    op["x_mm"] = core::py_round(x_mm, 2);
    op["y_mm"] = core::py_round(y_mm, 2);
    apply_ops(Json::array({op}));
}

std::optional<Json> MainWindow::copy_selection_items() {
    const auto area = need_area();
    const core::Layer* layer = target_layer();
    const core::Page* page = current_page();
    if (!area || layer == nullptr || page == nullptr) return std::nullopt;
    render::selection::Items items;
    try {
        core::Layer copy = *layer;
        items = render::selection::lift(copy, *area, *page);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return std::nullopt;
    }
    if (items.strokes.empty() && items.patches.empty()) {
        flash(QStringLiteral("選んだ範囲に、描く先のレイヤーの絵がありません"), 3000);
        return std::nullopt;
    }
    return render::selection::items_to_json(items);
}

// --- effect lines ----------------------------------------------------------------------------------------------------

void MainWindow::choose_effect(const std::string& kind) {
    effect_kind_ = kind;
    choose_tool(QStringLiteral("effect"));
    flash(QStringLiteral("%1: コマの中をクリックします（集中線・フラッシュはそこが中心）").arg(effect_label(kind)), 3500);
}

void MainWindow::effect_at(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Frame* frame = page->frame_at(core::Num(x_mm), core::Num(y_mm));
    Json params = Json::object();
    if (effect_kind_ == "focus" || effect_kind_ == "uni_flash" || effect_kind_ == "beta_flash")
        params["center"] = Json::array({core::py_round(x_mm, 2), core::py_round(y_mm, 2)});
    if (const auto shape = selection_shape()) params["within"] = *shape;  // (as in other manga tools: drawn inside the selection)
    const std::string effect_id = core::new_id();
    if (apply_ops(Json::array({Json{{"op", "add_effect"}, {"page", page->index.json()}, {"kind", effect_kind_}, {"id", effect_id},
                                    {"frame_id", frame != nullptr ? Json(frame->id) : Json()}, {"params", params}}}))) {
        canvas_->selected_effect_id = effect_id;
        show_dock(QStringLiteral("素材"));
        materials_->refresh();
        materials_->select_effect(effect_id);
    }
}

std::optional<Json> MainWindow::selection_shape() const {
    // the selection as a shape for an effect (a lasso or rectangle; one by colour or inverted has no outline to follow)
    const auto area = selection_area();
    if (!area || !area->is_object()) return std::nullopt;
    const Json poly = core::py_get(*area, "poly");
    if (poly.is_array() && poly.size() >= 3) {
        Json out = Json::array();
        for (const Json& p : poly) out.push_back(Json::array({core::py_round(core::py_float(p[0]), 2), core::py_round(core::py_float(p[1]), 2)}));
        return out;
    }
    const Json rect = core::py_get(*area, "rect");
    if (core::py_truthy(rect) && rect.is_array() && rect.size() >= 4) {
        const double x = core::py_float(rect[0]), y = core::py_float(rect[1]), w = core::py_float(rect[2]), h = core::py_float(rect[3]);
        return Json::array({Json::array({x, y}), Json::array({x + w, y}), Json::array({x + w, y + h}), Json::array({x, y + h})});
    }
    return std::nullopt;
}

void MainWindow::effect_clearing(const std::optional<std::string>& how) {
    // the chosen effect drawn only inside the selection, kept clear of it, or over its whole panel again
    const core::Page* page = current_page();
    const Json* effect = nullptr;
    if (page != nullptr && canvas_->selected_effect_id) {
        for (const Json& e : page->effects) {
            if (e.is_object() && core::py_get(e, "id") == *canvas_->selected_effect_id) effect = &e;
        }
    }
    if (effect == nullptr) {
        flash(QStringLiteral("先に効果線を選びます（効果線の道具でクリック、または素材パネルの一覧で）"), 4000);
        return;
    }
    const Json effect_id = core::py_get(*effect, "id");
    if (!how) {
        apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", page->index.json()}, {"id", effect_id}, {"params", Json{{"within", nullptr}, {"avoid", nullptr}}}}}));
        flash(QStringLiteral("避ける範囲を外しました"), 2500);
        return;
    }
    const auto shape = selection_shape();
    if (!shape) {
        flash(QStringLiteral("投げ縄か長方形で選択範囲を作ってから選びます（色で選んだ範囲や反転した範囲は使えません）"), 5000);
        return;
    }
    Json change;
    if (*how == "within") {
        change = Json{{"within", *shape}};
    } else {
        const Json params = core::py_truthy(core::py_get(*effect, "params")) ? (*effect)["params"] : Json::object();
        Json kept = core::py_truthy(core::py_get(params, "avoid")) ? core::py_list(params["avoid"]) : Json::array();
        kept.push_back(Json{{"path", *shape}});
        change = Json{{"avoid", kept}};
    }
    if (apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", page->index.json()}, {"id", effect_id}, {"params", change}}}))) {
        flash(*how == "within" ? QStringLiteral("選択範囲の中にだけ描きます") : QStringLiteral("選択範囲の手前で線を止めます"), 3000);
    }
}

void MainWindow::draw_effect_shape(const std::string& effect_id, const std::string& key) {
    // the next pen line becomes a speed line's path, or a focus line's clear middle
    effect_shape_for_ = std::make_pair(effect_id, key);
    choose_tool(QStringLiteral("pen"));
    flash(QStringLiteral("ペンで 1 本引きます（効果線の形になります。描く先のレイヤーには描かれません）"), 6000);
}

bool MainWindow::effect_shape_stroke(const StrokeInput& stroke, const core::Page& page) {
    if (!effect_shape_for_) return false;
    const auto [effect_id, key] = *effect_shape_for_;
    effect_shape_for_.reset();
    canvas_->stroke_dropped();
    const std::size_t n = stroke.points.size();
    if (n >= 2) {
        const std::size_t step = std::max<std::size_t>(1, n / 60);
        Json shape = Json::array();
        for (std::size_t i = 0; i < n; i += step) shape.push_back(Json::array({core::py_round(stroke.points[i].x, 2), core::py_round(stroke.points[i].y, 2)}));
        apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", page.index.json()}, {"id", effect_id}, {"params", Json{{key, shape}}}}}));
    }
    return true;
}

// --- スキャン画像の線画抽出 ---------------------------------------------------------------------------------------------

void MainWindow::import_scan(bool from_scanner) {
    // the paper (a file, or the scanner) laid on a new layer over the page, then its lines drawn out with the settings
    // chosen while looking at the page
    const core::Page* page = current_page();
    if (page == nullptr) return;
    std::string blob;
    QString name;
    if (from_scanner) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        try {
            blob = scanner_source ? scanner_source(600) : scanner::scan(600);
        } catch (const scanner::ScanError& error) {
            QApplication::restoreOverrideCursor();
            flash(QString::fromStdString(error.what()), 7000, true);
            return;
        }
        QApplication::restoreOverrideCursor();
        name = QStringLiteral("スキャン");
    } else {
        const QString path = ask::open_path(this, QStringLiteral("スキャンした画像"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            flash(QStringLiteral("読み込めない画像です:\n%1").arg(file.errorString()), 6000, true);
            return;
        }
        blob = file.readAll().toStdString();
        name = QFileInfo(path).completeBaseName();
        if (name.isEmpty()) name = QFileInfo(path).fileName();
    }
    std::string png;
    try {
        render::Image paper = render::selection::open_picture(blob).convert("RGB");
        const render::Size size{render::mm_to_px(page->spec.width_mm.value(), render::selection::kWorkingDpi),
                                render::mm_to_px(page->spec.height_mm.value(), render::selection::kWorkingDpi)};
        if (paper.width() > size.width || paper.height() > size.height) paper.thumbnail(size, render::Resample::Lanczos);
        render::Image sheet = render::Image::create("RGB", size, render::Ink{255, 255, 255});  // (the paper fitted to the page, in the middle)
        sheet.paste(paper, render::Point{(size.width - paper.width()) / 2, (size.height - paper.height()) / 2});
        png = render::write_png(sheet);
    } catch (const std::exception& error) {  // (a file that cannot be read)
        flash(QStringLiteral("読み込めない画像です:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    const std::string layer_id = core::new_id();
    const Json index = page->index.json();
    if (!apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", index}, {"kind", "paint"}, {"id", layer_id},
                                     {"name", QStringLiteral("線画（%1）").arg(name).toStdString()}},
                                Json{{"op", "put_raster"}, {"page", index}, {"id", layer_id}, {"png_base64", core::b64encode(png)}}}))) {
        return;
    }
    const auto params = filter_params(this, "lineart", Json{{"drop_blue", 1}, {"keep_solid", 1}}, [this, index, layer_id](const std::optional<Json>& values) {
        if (!values) {
            preview_ops(std::nullopt);
            return;
        }
        Json op{{"op", "filter_raster"}, {"page", index}, {"id", layer_id}, {"kind", "lineart"}};
        op.update(*values);
        preview_ops(Json::array({op}));
    });
    if (!params) {
        flash(QStringLiteral("紙のままレイヤーに置きました（線画抽出はフィルターからもかけられます）"), 5000);
        return;
    }
    Json op{{"op", "filter_raster"}, {"page", index}, {"id", layer_id}, {"kind", "lineart"}};
    op.update(*params);
    if (apply_ops(Json::array({op}))) flash(QStringLiteral("線だけを新しいレイヤーに取り込みました"), 4000);
}

// --- the view --------------------------------------------------------------------------------------------------------

void MainWindow::onion() {
    const core::Page* page = current_page();
    if (page == nullptr || core::py_float(page->index.json()) < 2) return;
    apply_ops(Json::array({Json{{"op", "step_onion"}, {"page", page->index.json()}, {"delta", -1}}}));
}

}  // namespace genko::app
