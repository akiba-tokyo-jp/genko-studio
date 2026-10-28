// The paths in the window (vector@1, PC3 C-a, SPEC.md COLOR-07; Photoshop's names, the user's decision 5): ペンツール (P),
// 曲線ペンツール (Shift+P) and パス選択ツール (A) — what they draw becomes set_path on the target path (the Paths panel's row:
// the work path, a saved path, or the vector mask of the layer drawn on: set_vector_mask) —; the Paths panel (ウィンドウ →
// パス); 選択範囲 → 作業用パスを作成…; ページ → 台詞 → 作業用パスを作成 (the chosen line's letters); レイヤー → ベクトルマスク
// (すべての領域を表示・すべての領域を隠す・現在のパス・削除・使用しない・リンク) and ラスタライズ → ベクトルマスク; 表示 →
// ターゲットパス; ファイル → パスを Illustrator へ…. Every change of the book is an op (apply_ops): Undo, saving and the journal
// as for any edit.

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/paths_panel.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "core/vector.hpp"
#include "render/path_info.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace vec = core::vector;

namespace {

QMenu* submenu(QMenu* in, const QString& title) {
    if (in == nullptr) return nullptr;
    for (QAction* a : in->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

QMenu* top_menu(QMenuBar* bar, const QString& title) {
    for (QAction* a : bar->actions())
        if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
    return nullptr;
}

}  // namespace

void MainWindow::build_path_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const char* name, const QString& key, const QString& tip) {
        const QString which = QString::fromLatin1(name);
        QAction* act = make(QString::fromLatin1(attribute), title, [this, which] { choose_tool(which); }, {QKeySequence(key)}, tip, true);
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[which] = act;
    };
    tool("act_path_pen", QStringLiteral("ペンツール"), "pathpen", QStringLiteral("P"),
         QStringLiteral("クリックで角、ドラッグでなめらかな点を置いてパスを描きます（Alt+ドラッグでハンドルを折る、最初の点で閉じる、Enter で終える）"));
    tool("act_curvature_pen", QStringLiteral("曲線ペンツール"), "curvepen", QStringLiteral("Shift+P"),
         QStringLiteral("クリックした点をなめらかに通る曲線のパスを描きます（Alt+クリックで角、最初の点で閉じる、Enter で終える）"));
    tool("act_path_select", QStringLiteral("パス選択ツール"), "pathselect", QStringLiteral("A"),
         QStringLiteral("パスをクリックで選んでドラッグで動かします（Alt で複製、選んだパスの点・ハンドルもドラッグで動く、Delete で消す）"));
    make("act_work_path", QStringLiteral("作業用パスを作成…"),
         [this] {
             const Asked asked = asking();
             if (!selection_area()) {
                 need_area();
                 return;
             }
             if (const auto t = ask::get_double(this, QStringLiteral("作業用パスを作成"), QStringLiteral("許容値（mm）"), 0.25, 0.06, 1.27, 2); t && still(asked))
                 work_path_from_selection(*t);
         },
         {}, QStringLiteral("選択範囲の輪郭を、曲線の作業用パスにします（許容値が小さいほど細かく沿います）"));
    make("act_line_work_path", QStringLiteral("作業用パスを作成"), [this] { line_work_path(); }, {},
         QStringLiteral("選んだ台詞の文字の形を、作業用パスにします（フキダシ・白フチは入りません）"));
    QAction* target = make("act_show_target_path", QStringLiteral("ターゲットパス"), [] {}, {},
                           QStringLiteral("パス パネルで選んだパスをページの上に表示します（ペンツール・パス選択ツールではいつも表示）"), true);
    target->setChecked(canvas_->show_target_path);
    connect(target, &QAction::toggled, this, [this](bool on) {
        canvas_->show_target_path = on;
        canvas_->update();
    });
    make("act_export_paths_ai", QStringLiteral("パスを Illustrator へ…"), [this] { export_paths_ai(); }, {},
         QStringLiteral("このページのパスを Illustrator のファイル（.ai）に書き出します"));
    // レイヤー → ベクトルマスク (and ラスタライズ → ベクトルマスク), on the layer drawn on
    make("act_vmask_reveal_all", QStringLiteral("すべての領域を表示"), [this] { vector_mask_command("reveal_all"); }, {},
         QStringLiteral("描く先のレイヤーに、すべてを見せるベクトルマスクを付けます（パスを描いて形にします）"));
    make("act_vmask_hide_all", QStringLiteral("すべての領域を隠す"), [this] { vector_mask_command("hide_all"); }, {},
         QStringLiteral("描く先のレイヤーに、すべてを隠すベクトルマスクを付けます"));
    make("act_vmask_current_path", QStringLiteral("現在のパス"), [this] { vector_mask_command("current_path"); }, {},
         QStringLiteral("パス パネルで選んだパスで、描く先のレイヤーにベクトルマスクを付けます"));
    make("act_vmask_delete", QStringLiteral("削除"), [this] { vector_mask_command("delete"); }, {}, QStringLiteral("描く先のレイヤーのベクトルマスクを消します"));
    make("act_vmask_disable", QStringLiteral("使用しない"), [this] { vector_mask_command("enabled"); }, {},
         QStringLiteral("ベクトルマスクを一時的に使わずに見ます（もう一度で戻ります）"));
    make("act_vmask_link", QStringLiteral("リンク"), [this] { vector_mask_command("linked"); }, {},
         QStringLiteral("ベクトルマスクをレイヤーと一緒に動かすかを切り替えます"));
    make("act_rasterize_vmask", QStringLiteral("ベクトルマスク"), [this] { vector_mask_command("rasterize"); }, {},
         QStringLiteral("ベクトルマスクを画素のレイヤーマスクにします（今のマスクに掛け合わせます）"));
    connect(canvas_, &PageCanvas::pathDrawn, this, [this](const Json& subpath) {
        if (!pen_path_drawn(subpath)) path_drawn(subpath);  // (PC3 C-b: the pen's シェイプ and ピクセル, main_window_shapes.cpp)
    });
    connect(canvas_, &PageCanvas::pathEdited, this, &MainWindow::path_edited);
    connect(canvas_, &PageCanvas::targetPathChosen, this, [this] {
        if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    });
}

void MainWindow::build_path_menus(QMenu* file, QMenu* view, QMenu* tools, QMenu* layers) {
    // ツール: after the vector tools
    tools->addSeparator();
    for (const char* name : {"act_path_pen", "act_curvature_pen", "act_path_select"}) tools->addAction(action(QString::fromLatin1(name)));
    // (ファイル → 書き出し → パスを Illustrator へ… is put in Photoshop's place by build_file_menu, PC4 F-0)
    (void)file;
    // 表示
    view->addAction(action("act_show_target_path"));
    // 選択範囲: after 選択範囲を保存…
    if (QMenu* selection = top_menu(menuBar(), QStringLiteral("選択範囲"))) {
        const QList<QAction*> sel = selection->actions();
        const qsizetype after = sel.indexOf(action("act_sel_to_channel"));
        QAction* next = after >= 0 && after + 1 < sel.size() ? sel.at(after + 1) : nullptr;
        selection->insertAction(next, action("act_work_path"));
    }
    // ページ → 台詞
    if (QMenu* lines = submenu(top_menu(menuBar(), QStringLiteral("ページ")), QStringLiteral("台詞"))) {
        lines->addSeparator();
        lines->addAction(action("act_line_work_path"));
    }
    // レイヤー → ベクトルマスク (after マスク), ラスタライズ → ベクトルマスク
    QMenu* masks = new QMenu(QStringLiteral("ベクトルマスク"), layers);
    masks->setObjectName(QStringLiteral("menu_vector_mask"));
    for (const char* name : {"act_vmask_reveal_all", "act_vmask_hide_all", "act_vmask_current_path", "act_vmask_delete"})
        masks->addAction(action(QString::fromLatin1(name)));
    masks->addSeparator();
    for (const char* name : {"act_vmask_disable", "act_vmask_link"}) masks->addAction(action(QString::fromLatin1(name)));
    QAction* mask_menu = layer_mask_menu_ != nullptr ? layer_mask_menu_->menuAction() : nullptr;
    const QList<QAction*> layer_items = layers->actions();
    const qsizetype mask_at = mask_menu != nullptr ? layer_items.indexOf(mask_menu) : -1;
    layers->insertMenu(mask_at >= 0 && mask_at + 1 < layer_items.size() ? layer_items.at(mask_at + 1) : nullptr, masks);
    if (QMenu* rasterize = layers->findChild<QMenu*>(QStringLiteral("menu_rasterize"))) {
        const QList<QAction*> r = rasterize->actions();
        const qsizetype layer_at = r.indexOf(action("act_rasterize_layer"));
        rasterize->insertAction(layer_at >= 0 ? r.at(layer_at) : nullptr, action("act_rasterize_vmask"));
    }
}

void MainWindow::build_path_dock() {
    paths_panel_ = new PathsPanel(this);
    paths_dock_ = new QDockWidget(QStringLiteral("パス"), this);
    paths_dock_->setObjectName(QStringLiteral("パス"));
    paths_dock_->setWidget(paths_panel_);
    paths_dock_->setMinimumWidth(200);
    paths_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, paths_dock_);
    tabifyDockWidget(layers_dock_, paths_dock_);  // (beside the layers, as the channels)
    paths_dock_->hide();  // (ウィンドウ → パス, or a path tool, brings it)
    view_menu_->addAction(paths_dock_->toggleViewAction());
    connect(paths_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown && paths_panel_ != nullptr) paths_panel_->refresh();
    });
}

// --- the target path ------------------------------------------------------------------------------------------------

Json MainWindow::target_path_terms() const {
    const QString target = canvas_->target_path;
    if (target.startsWith(QLatin1String("mask:"))) return Json{{"layer", target.mid(5).toStdString()}};
    if (target.startsWith(QLatin1String("saved:"))) return Json{{"name", target.mid(6).toStdString()}};
    if (target.startsWith(QLatin1String("shape:"))) {  // (PC3 C-b: a pen layer's shape part)
        const QString rest = target.mid(6);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        return Json{{"shape", Json{{"layer", rest.left(slash).toStdString()}, {"id", rest.mid(slash + 1).toStdString()}}}};
    }
    return Json{{"name", "work"}};
}

void MainWindow::path_drawn(const Json& subpath) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const Json terms = target_path_terms();
    const Json one{{"subpaths", Json::array({subpath})}};
    if (terms.contains("layer")) {  // (the vector mask's path: the subpath added to it)
        const std::string id = terms["layer"].get<std::string>();
        const auto layer = std::find_if(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == id; });
        if (layer == page->layers.end() || !layer->vector_mask) {
            flash(QStringLiteral("そのレイヤーにはもうベクトルマスクがありません"), 3000);
            return;
        }
        Json path = vec::to_json(layer->vector_mask->path);
        Json sub = subpath;
        if (!path["subpaths"].empty()) sub["op"] = "combine";
        path["subpaths"].push_back(sub);
        apply_ops(Json::array({Json{{"op", "set_vector_mask"}, {"page", page->index.json()}, {"id", id}, {"path", path}}}));
    } else if (terms.contains("shape")) {  // (PC3 C-b: a subpath more of the shape part)
        apply_ops(Json::array({Json{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", terms["shape"]["layer"]},
                                    {"id", terms["shape"]["id"]}, {"append", Json{{"path", one}, {"op", "combine"}}}}}));
    } else {
        Json op{{"op", "set_path"}, {"page", page->index.json()}, {"name", terms["name"]}, {"path", one}};
        if (canvas_->target_path_now()) op["append"] = "combine";  // (a subpath more, as Photoshop adds to the path chosen)
        apply_ops(Json::array({op}));
    }
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
}

void MainWindow::path_edited(const Json& path) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const Json terms = target_path_terms();
    if (terms.contains("layer")) {
        apply_ops(Json::array({Json{{"op", "set_vector_mask"}, {"page", page->index.json()}, {"id", terms["layer"]}, {"path", path}}}));
    } else if (terms.contains("shape")) {  // (PC3 C-b: the shape part's path — moved whole: move_mm, its live shape kept)
        const Json& part = terms["shape"];
        const std::optional<vec::Path> before = canvas_->target_path_now();
        const vec::Path after = vec::path_from_json(path);
        Json op{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", part["layer"]}, {"id", part["id"]}};
        std::optional<vec::XY> shift;
        if (before && before->subpaths.size() == after.subpaths.size()) {
            bool same = true;
            for (std::size_t s = 0; same && s < after.subpaths.size(); ++s) {
                const auto& a = before->subpaths[s].knots;
                const auto& b = after.subpaths[s].knots;
                same = a.size() == b.size() && before->subpaths[s].closed == after.subpaths[s].closed && before->subpaths[s].op == after.subpaths[s].op;
                for (std::size_t k = 0; same && k < a.size(); ++k) {
                    const vec::XY d{b[k].anchor.x - a[k].anchor.x, b[k].anchor.y - a[k].anchor.y};
                    if (!shift) shift = d;
                    const auto near = [&](const vec::XY& p, const vec::XY& q) {
                        return std::abs(q.x - p.x - shift->x) < 1e-6 && std::abs(q.y - p.y - shift->y) < 1e-6;
                    };
                    same = near(a[k].anchor, b[k].anchor) && near(a[k].in, b[k].in) && near(a[k].out, b[k].out);
                }
            }
            if (!same) shift.reset();
        }
        if (after.subpaths.empty()) {  // (its last subpath taken out: the part goes)
            op = Json{{"op", "vector_edit"}, {"page", page->index.json()}, {"layer_id", part["layer"]}, {"action", "delete"}, {"stroke_id", part["id"]}};
            canvas_->target_path = QStringLiteral("work");
        } else if (shift) {
            op["move_mm"] = Json::array({core::py_round(shift->x, 4), core::py_round(shift->y, 4)});
        } else {
            op["path"] = path;
        }
        apply_ops(Json::array({op}));
    } else if (path.value("subpaths", Json::array()).empty() && terms["name"] == "work") {
        apply_ops(Json::array({Json{{"op", "delete_path"}, {"page", page->index.json()}, {"name", "work"}}}));  // (the last of the work path)
    } else {
        apply_ops(Json::array({Json{{"op", "set_path"}, {"page", page->index.json()}, {"name", terms["name"]}, {"path", path}}}));
    }
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
}

// The target path as an area (its own path, as it is now: what a fill, a stroke or the selection is made of).
static std::optional<Json> target_area(MainWindow* w, const PageCanvas* canvas) {
    const std::optional<vec::Path> path = canvas->target_path_now();
    if (!path || path->subpaths.empty()) {
        w->flash(QStringLiteral("パスがありません（ペンツールで描くか、パス パネルで選びます）"), 3000);
        return std::nullopt;
    }
    return Json{{"path", vec::to_json(*path)}};
}

void MainWindow::path_fill() {
    const auto area = target_area(this, canvas_);
    const core::Layer* layer = area ? paint_layer() : nullptr;
    if (layer == nullptr) return;
    Json op{{"op", "fill_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area}};
    op.update(paint_fields());
    apply_ops(Json::array({op}));
}

void MainWindow::path_stroke() {
    const std::optional<vec::Path> path = canvas_->target_path_now();
    if (!path || path->subpaths.empty()) {
        flash(QStringLiteral("パスがありません（ペンツールで描くか、パス パネルで選びます）"), 3000);
        return;
    }
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    const Json pen = pen_.stroke_fields();
    Json op{{"op", "stroke_path"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"path", vec::to_json(*path)},
            {"simulate_pressure", pen.value("taper", false)}};
    for (const char* key : {"kind", "width_mm", "rgb", "opacity"}) {
        if (pen.contains(key)) op[key] = pen[key];
    }
    apply_ops(Json::array({op}));
}

void MainWindow::path_to_selection() {
    const auto area = target_area(this, canvas_);
    const core::Page* page = current_page();
    if (!area || page == nullptr) return;
    Json resolved;
    try {
        render::use_brushes_of(book());
        resolved = render::selection::resolve(*area, *page, &book());
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    join_selection(resolved, canvas_->selection_how());
}

bool MainWindow::work_path_from_selection(std::optional<double> tolerance_mm) {
    const auto area = need_area();
    const core::Page* page = current_page();
    if (!area || page == nullptr) return false;
    Json op{{"op", "make_work_path"}, {"page", page->index.json()}, {"area", *area}};
    if (tolerance_mm) op["tolerance_mm"] = *tolerance_mm;
    if (!apply_ops(Json::array({op}))) return false;
    canvas_->target_path = QStringLiteral("work");
    if (paths_dock_ != nullptr) {
        paths_dock_->show();
        paths_dock_->raise();
    }
    if (paths_panel_ != nullptr) paths_panel_->refresh();
    return true;
}

void MainWindow::line_work_path() {
    if (!canvas_->selected_line_id) {
        flash(QStringLiteral("先に台詞を選びます（選択ツールで台詞をクリック）"), 3000);
        return;
    }
    if (!apply_ops(Json::array({Json{{"op", "text_to_path"}, {"id", *canvas_->selected_line_id}}}))) return;
    canvas_->target_path = QStringLiteral("work");
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
}

void MainWindow::vector_mask_command(const std::string& what) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    Json op{{"op", "set_vector_mask"}, {"page", page->index.json()}, {"id", layer->id}};
    if (what == "reveal_all" || what == "hide_all") {
        op[what] = true;
    } else if (what == "current_path") {
        const std::optional<vec::Path> path = canvas_->target_path_now();
        if (!path || canvas_->target_path.startsWith(QLatin1String("mask:"))) {
            flash(QStringLiteral("パス パネルで、マスクにするパスを選びます"), 3000);
            return;
        }
        op["path"] = vec::to_json(*path);
    } else if (what == "delete") {
        if (!layer->vector_mask) {
            flash(QStringLiteral("このレイヤーにはベクトルマスクがありません"), 3000);
            return;
        }
        op["delete"] = true;
    } else if (what == "enabled" || what == "linked") {
        if (!layer->vector_mask) {
            flash(QStringLiteral("このレイヤーにはベクトルマスクがありません"), 3000);
            return;
        }
        op[what] = !(what == "enabled" ? layer->vector_mask->enabled : layer->vector_mask->linked);
    } else if (what == "rasterize") {
        op = Json{{"op", "rasterize_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"vector_mask", true}};
    } else {
        return;
    }
    const std::string id = layer->id;
    if (!apply_ops(Json::array({op}))) return;
    if (what == "delete" || what == "rasterize") {
        if (canvas_->target_path == QStringLiteral("mask:") + QString::fromStdString(id)) canvas_->target_path = QStringLiteral("work");
    } else if (what != "enabled" && what != "linked") {
        canvas_->target_path = QStringLiteral("mask:") + QString::fromStdString(id);  // (the mask's path, to draw on with the pen)
    }
    if (paths_panel_ != nullptr && paths_dock_->isVisible()) paths_panel_->refresh();
    canvas_->update();
}

bool MainWindow::export_paths_ai(std::optional<QString> file) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    int count = 0;
    const std::string text = render::path_info::illustrator(book(), *page, "all", &count);
    if (count == 0) {
        flash(QStringLiteral("このページにはパスがありません"), 3000);
        return false;
    }
    const Asked asked = asking();
    const QString chosen = file ? *file
                                : ask::save_path(this, QStringLiteral("パスを Illustrator へ"), QStringLiteral("paths.ai"),
                                                 QStringLiteral("Illustrator (*.ai)"));
    if (chosen.isEmpty() || (!file && !still(asked))) return false;
    try {
        storage::write_atomic(core::path_from_utf8(chosen.toStdString()), text);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return false;
    }
    flash(QStringLiteral("パスを書き出しました（%1 個）").arg(count), 3000);
    return true;
}

}  // namespace genko::app
