// The window's selection commands (Python's genko/app/main.py: _select_all, _change_selection, _wand, _select_colour,
// _selection_painted, _keep_selection, _quick_mask, _copy, _cut, _paste, _delete_area, _flip, _transform_selection,
// _warp_selection, _transform_numbers, _fill_area, _line_width): the selection menu and what the marquee tool's areas
// do. The selection is the canvas's; each change to the book is an op on the layer drawn on. PC3 B1 (SPEC.md COLOR-06,
// the user's decisions 4 and 5): 自動選択ツール's two criteria, 選択範囲を変更 by the distance transform (拡張・縮小・境界線・
// 滑らかに), 選択範囲を拡大・近似色を選択, 再選択, 選択範囲を変形 (the outline only), 選択範囲の境界線, the Quick Mask as the page's
// channel (op quick_mask, painted by every pixel tool), マジック消しゴムツール, the selection's right-click menu and the
// selection tools' settings (選択方法・アンチエイリアス・ぼかし・スタイル); the windows are select_dialogs.cpp.

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMenu>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <cmath>
#include <map>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "service/wording.hpp"
#include "core/channels.hpp"
#include "core/masks.hpp"
#include "core/base64.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "render/color_edit.hpp"
#include "render/fill_patches.hpp"
#include "render/raster_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_pc3.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

namespace {

using Std = QKeySequence::StandardKey;

// The marquee tool's ways, by the window's tool names (Python's _tool; PC3 B1: 色域選択 is 自動選択ツール's 色の近さ now).
const std::map<QString, QString>& marquee_ways() {
    static const std::map<QString, QString> ways{{QStringLiteral("rect"), QStringLiteral("rect")},         {QStringLiteral("lasso"), QStringLiteral("lasso")},
                                                 {QStringLiteral("wand"), QStringLiteral("wand")},         {QStringLiteral("ellipse"), QStringLiteral("ellipse")},
                                                 {QStringLiteral("polyline"), QStringLiteral("polyline")}, {QStringLiteral("selpen"), QStringLiteral("pen")},
                                                 {QStringLiteral("selerase"), QStringLiteral("erase")},
                                                 // (PC3 B3: クイック選択ツール, オブジェクト選択ツール — main_window_smart_select.cpp)
                                                 {QStringLiteral("quick"), QStringLiteral("quick")},       {QStringLiteral("object"), QStringLiteral("object")}};
    return ways;
}

// The tools' groups past the selection's (PC3 B1: the eraser's マジック消しゴムツール).
const std::vector<const char*>& eraser_tools() {
    static const std::vector<const char*> names{"act_magic_eraser"};
    return names;
}

// The selection's right-click menu (Photoshop's, over a selection with a selection tool), by the commands' ids: each
// group's command joins it when it is made (B2 選択範囲をコピーしたレイヤー・カットしたレイヤー・塗りつぶし…・境界線を描く…,
// B3 選択とマスク…, B4 コンテンツに応じた塗りつぶし…); a null is a line between.
const std::vector<const char*>& selection_context_items() {
    static const std::vector<const char*> names{"act_deselect",      "act_sel_reselect",      "act_sel_invert", nullptr,          "act_select_mask",
                                                "act_sel_feather",   nullptr,                 "act_sel_to_channel", "act_layer_via_copy", "act_layer_via_cut",
                                                nullptr,             "act_sel_transform",     nullptr,          "act_fill_dialog",  "act_fill_selection",
                                                "act_stroke_dialog", "act_content_aware_fill"};
    return names;
}

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::hint(label);
    return label;
}

}  // namespace

// transform_matrix: scale, then turn (degrees, clockwise on the page), both about `pivot`, then move by (dx, dy) mm.
std::array<double, 6> transform_matrix(QPointF pivot, double dx, double dy, double sx, double sy, double angle) {
    const double px = pivot.x(), py = pivot.y();
    const double t = angle * 3.14159265358979323846 / 180;
    const double c = std::cos(t), s = std::sin(t);
    const double a = c * sx, b = s * sx, cc = -s * sy, d = c * sy;
    return {a, b, cc, d, px - a * px - cc * py + dx, py - b * px - d * py + dy};
}

bool MainWindow::is_marquee_tool(const QString& tool) { return marquee_ways().contains(tool); }

void MainWindow::choose_marquee(const QString& tool) {
    canvas_->marquee = marquee_ways().at(tool);
    canvas_->set_tool(QStringLiteral("marquee"));
}

QString MainWindow::marquee_tool_of(const QString& way) {
    for (const auto& [tool, kind] : marquee_ways())
        if (kind == way) return tool;
    return QStringLiteral("rect");
}

void MainWindow::build_selection_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const QString& name, const QList<QKeySequence>& keys, const QString& tip) {
        QAction* act = make(QString::fromLatin1(attribute), title, [this, name] { choose_tool(name); }, keys, tip, true);
        act->setAutoRepeat(false);
        tools_->addAction(act);
        tool_actions_[name] = act;
    };
    tool("act_marquee", QStringLiteral("長方形選択ツール"), QStringLiteral("rect"), {QKeySequence(QStringLiteral("M"))},
         QStringLiteral("ドラッグで選ぶ。中をドラッグで移動、□で拡大縮小、○で回転（Shift で 15° 刻み・縦横比を保つ）"));
    tool("act_lasso", QStringLiteral("なげなわツール"), QStringLiteral("lasso"), {QKeySequence(QStringLiteral("L"))}, QStringLiteral("ドラッグで囲んで選びます"));
    tool("act_wand", QStringLiteral("自動選択ツール"), QStringLiteral("wand"), {QKeySequence(QStringLiteral("W"))},
         QStringLiteral("クリックした所を選びます（基準はツールの設定: 線で囲まれた所・色の近さ）"));
    tool("act_sel_ellipse", QStringLiteral("楕円形選択ツール"), QStringLiteral("ellipse"), {}, QStringLiteral("ドラッグで楕円に選ぶ（Shift で足す、Alt で引く）"));
    tool("act_sel_polyline", QStringLiteral("多角形選択ツール"), QStringLiteral("polyline"), {}, QStringLiteral("クリックで角を置き、ダブルクリックか Enter で閉じる"));
    tool("act_sel_pen", QStringLiteral("選択ペン"), QStringLiteral("selpen"), {}, QStringLiteral("なぞった所を選択範囲に足す"));
    tool("act_sel_erase", QStringLiteral("選択消し"), QStringLiteral("selerase"), {}, QStringLiteral("なぞった所を選択範囲から外す"));
    tool("act_magic_eraser", QStringLiteral("マジック消しゴムツール"), QStringLiteral("magiceraser"), {},
         QStringLiteral("クリックした所に近い色の所を透明にします（描く先のペイントのレイヤー）"));

    make("act_select_all", QStringLiteral("すべてを選択"), [this] { select_all(); }, {QKeySequence(Std::SelectAll)});
    make("act_deselect", QStringLiteral("選択を解除"), [this] { canvas_->set_selection(std::nullopt); }, {QKeySequence(QStringLiteral("Ctrl+D"))});
    make("act_sel_invert", QStringLiteral("選択範囲を反転"), [this] { change_selection(Json{{"invert", true}}); }, {QKeySequence(QStringLiteral("Ctrl+Alt+I"))});
    make("act_sel_reselect", QStringLiteral("再選択"), [this] { reselect(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+D"))},
         QStringLiteral("最後に解除した選択範囲を戻す"));
    make("act_color_range", QStringLiteral("色域指定…"), [this] { color_range_dialog(); }, {},
         QStringLiteral("色（スポイトで拾う）・色の系統・明るさ・色域外で選ぶ"));
    make("act_sel_border", QStringLiteral("境界線…"), [this] { change_selection_by("border_mm", 1); }, {}, QStringLiteral("選択範囲の縁を、その幅の帯にする"));
    make("act_sel_smooth", QStringLiteral("滑らかに…"), [this] { change_selection_by("smooth_mm", 1); }, {}, QStringLiteral("選択範囲の縁のでこぼこをならす"));
    make("act_sel_grow", QStringLiteral("拡張…"), [this] { change_selection_by("expand_mm", 1); }, {}, QStringLiteral("選択範囲の縁を外へ広げる"));
    make("act_sel_shrink", QStringLiteral("縮小…"), [this] { change_selection_by("contract_mm", 1); }, {}, QStringLiteral("選択範囲の縁を内へ狭める"));
    make("act_sel_grow_colour", QStringLiteral("選択範囲を拡大"), [this] { grow_selection(true); }, {},
         QStringLiteral("選択範囲の色に近い、つながった所まで広げる（許容値は自動選択ツールの設定）"));
    make("act_sel_similar", QStringLiteral("近似色を選択"), [this] { grow_selection(false); }, {},
         QStringLiteral("選択範囲の色に近い所を、ページ中から選ぶ（許容値は自動選択ツールの設定）"));
    make("act_sel_transform", QStringLiteral("選択範囲を変形"), [this] { transform_selection_outline(); }, {},
         QStringLiteral("枠のつまみで選択範囲の形だけを動かす・拡大縮小・回転する（中身は動かない）。Enter で終わる"));
    make("act_sel_load", QStringLiteral("選択範囲を読み込む…"), [this] { load_selection_dialog(); }, {},
         QStringLiteral("チャンネル・色の版・透明部分・マスクから選択範囲を作る（反転・追加・削除・共通範囲も）"));
    QAction* edges = make("act_sel_edges", QStringLiteral("選択範囲の境界線"), [] {}, {}, QStringLiteral("選択範囲の点線を表示する・隠す"), true);
    edges->setChecked(true);
    connect(edges, &QAction::toggled, this, [this](bool on) {
        canvas_->show_selection_edges = on;
        canvas_->update();
    });
    make("act_sel_feather", QStringLiteral("境界をぼかす…"), [this] { change_selection_by("feather_mm", 1); },
         {QKeySequence(QStringLiteral("Shift+F6"))}, QStringLiteral("選択範囲の縁をなめらかにぼかす"));
    make("act_sel_layer", QStringLiteral("ピクセルを選択"), [this] { select_drawn(); }, {}, QStringLiteral("描く先のレイヤーで描いてある所を選ぶ"));
    make("act_sel_keep", QStringLiteral("選択範囲をストック…"), [this] { keep_selection(); }, {},
         QStringLiteral("名前を付けてページに残す（後で「ストックから選ぶ」）"));
    QAction* quick = make("act_quick_mask", QStringLiteral("クイックマスクモードで編集"), [] {}, {}, QStringLiteral("選択範囲を赤で見せ、ブラシなどの道具で直す"), true);
    connect(quick, &QAction::toggled, this, [this](bool on) { quick_mask(on); });
    make("act_copy", QStringLiteral("コピー"), [this] { copy(); }, {QKeySequence(Std::Copy)});
    make("act_cut", QStringLiteral("カット"), [this] { cut(); }, {QKeySequence(Std::Cut)});
    make("act_paste", QStringLiteral("ペースト"), [this] { paste(); }, {QKeySequence(Std::Paste)}, QStringLiteral("新しいレイヤーにペーストします（そのまま動かせます）"));
    // (Python's: the platform's first key of Delete, and Backspace — not all of Delete's keys: on X11 they hold Ctrl+D, which
    // is 選択を解除's, and a key two commands share does neither)
    QList<QKeySequence> delete_keys = {QKeySequence(Std::Delete)};
    if (!delete_keys.contains(QKeySequence(QStringLiteral("Backspace")))) delete_keys << QKeySequence(QStringLiteral("Backspace"));
    make("act_delete_area", QStringLiteral("消去"), [this] { delete_area(); }, delete_keys);
    make("act_flip_h", QStringLiteral("水平方向に反転"), [this] { flip(-1, 1); });
    make("act_flip_v", QStringLiteral("垂直方向に反転"), [this] { flip(1, -1); });
    make("act_fill_selection", QStringLiteral("描画色で塗りつぶし"), [this] { fill_selection(); }, {QKeySequence(QStringLiteral("Alt+Backspace"))});
    make("act_line_width", QStringLiteral("選択範囲の線の太さ…"), [this] { line_width(); });
    make("act_warp_perspective", QStringLiteral("自由な形に"), [this] { start_warp(QStringLiteral("perspective")); },
         {QKeySequence(QStringLiteral("Ctrl+Shift+P"))},
         QStringLiteral("選択範囲（無ければ選んだレイヤーの枠）の 4 隅を好きな所へ引っぱる。Enter で確定、Esc でやめる"));
    make("act_warp_mesh", QStringLiteral("ワープ"), [this] { start_warp(QStringLiteral("mesh")); }, {QKeySequence(QStringLiteral("Ctrl+Shift+W"))},
         QStringLiteral("選択範囲の 3×3 の点（無ければ選んだレイヤーのワープの枠）を引っぱって曲げる。Enter で確定、Esc でやめる"));
    make("act_warp_mesh_grid", QStringLiteral("ワープ（格子の数を決める）…"), [this] { start_mesh_grid(); }, {},
         QStringLiteral("横と縦の格子の数（1〜8）を決めてから、点を引っぱって曲げる"));
    make("act_move_pivot", QStringLiteral("基準位置を動かす"), [this] { move_pivot(); }, {},
         QStringLiteral("次にクリックした所を、選択範囲を回す・数で変形するときの中心にします（置いた＋はドラッグで動かせる）"));
    make("act_transform_numbers", QStringLiteral("変形を数で決める…"), [this] { transform_numbers(); }, {},
         QStringLiteral("選択範囲を、移動（mm）・拡大率（%）・回転（°）の数で変形します。基準位置（選択範囲の中の＋）を中心に"));
    make("act_warp_apply", QStringLiteral("変形を確定"), [this] { canvas_->finish_warp(); });
}

// The menus (decision 5: Photoshop's names and places). Each group adds its new items to its own list here: 選択範囲
// (B1 色域指定… 被写体を選択…, B3 選択とマスク…), 選択範囲を変更 (B1 境界線… 滑らかに…), the tools' groups (B1 クイック選択ツール
// オブジェクト選択ツール, B4 the retouching tools) and 編集 → 変形 (C-c).

const std::vector<const char*>& MainWindow::selection_tools() {
    static const std::vector<const char*> names{"act_marquee", "act_sel_ellipse", "act_lasso", "act_sel_polyline", "act_object_select",
                                                "act_quick_select", "act_wand", "act_sel_pen", "act_sel_erase"};
    return names;
}

const std::vector<const char*>& MainWindow::retouch_tools() {
    static const std::vector<const char*> names{"act_spot_healing", "act_healing", "act_clone_stamp", "act_history_brush"};  // (B4: retouch_tools.cpp)
    return names;
}

void MainWindow::build_tool_groups(QMenu* tools) {
    for (const std::vector<const char*>* group : {&selection_tools(), &eraser_tools(), &retouch_tools()}) {
        if (group->empty()) continue;
        for (const char* name : *group) tools->addAction(action(QString::fromLatin1(name)));
        tools->addSeparator();
    }
}

void MainWindow::build_selection_menu(QMenu* menu) {
    for (const char* name : {"act_select_all", "act_deselect", "act_sel_reselect", "act_sel_invert"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    // (PC3 B2: the layers chosen, main_window_masks.cpp)
    for (const char* name : {"act_select_all_layers", "act_deselect_layers", "act_find_layers", "act_isolate_layers"})
        menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("act_color_range")));
    // (PC3 B3: フォーカス領域…, 被写体を選択, 空を選択; 選択とマスク… — main_window_smart_select.cpp, select_mask_window.cpp)
    for (const char* name : {"act_focus_area", "act_select_subject", "act_select_sky"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("act_select_mask")));
    menu->addSeparator();
    QMenu* modify = menu->addMenu(QStringLiteral("選択範囲を変更"));
    for (const char* name : {"act_sel_border", "act_sel_smooth", "act_sel_grow", "act_sel_shrink", "act_sel_feather"})
        modify->addAction(action(QString::fromLatin1(name)));
    for (const char* name : {"act_sel_grow_colour", "act_sel_similar"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("act_sel_transform")));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("act_quick_mask")));
    menu->addSeparator();
    // (channels@1: the selection made from a channel, and kept as an alpha channel)
    for (const char* name : {"act_sel_load", "act_sel_to_channel"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    // Genko's own: its layer's drawing, the kept areas (saved_areas), the lines' width
    menu->addAction(action(QStringLiteral("act_sel_layer")));
    menu->addAction(action(QStringLiteral("act_sel_keep")));
    stock_menu_ = menu->addMenu(QStringLiteral("ストックから選ぶ"));
    stock_menu_->setProperty("remade", true);
    connect(stock_menu_, &QMenu::aboutToShow, this, [this] { fill_stock(); });
    menu->addAction(action(QStringLiteral("act_line_width")));
}

void MainWindow::build_transform_menu(QMenu* menu) {
    // (C-c: Photoshop's order — 再実行; 拡大・縮小 … ワープ; the warp's splits; the turns and flips)
    menu->addAction(action(QStringLiteral("act_transform_again")));
    menu->addSeparator();
    for (const char* name : {"act_transform_scale", "act_transform_rotate", "act_transform_skew", "act_warp_perspective", "act_transform_perspective",
                             "act_warp_mesh", "act_warp_mesh_grid", "act_split_warp_h", "act_split_warp_v", "act_split_warp_cross", "act_remove_warp_split"})
        menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    for (const char* name : {"act_transform_rotate_180", "act_transform_rotate_90cw", "act_transform_rotate_90ccw"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    for (const char* name : {"act_flip_h", "act_flip_v"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    for (const char* name : {"act_transform_numbers", "act_move_pivot", "act_warp_apply"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    QMenu* interp = menu->addMenu(QStringLiteral("補間方法"));
    auto* group = new QActionGroup(interp);
    for (const auto& [key, label] : {std::pair{"bilinear", "なめらか（バイリニア）"}, {"bicubic", "よりなめらか（バイキュービック）"},
                                     {"nearest", "ハード（ニアレストネイバー・ドット絵やトーンに）"}}) {
        const std::string chosen = key;
        QAction* item = interp->addAction(QString::fromUtf8(label), this, [this, chosen] { transform_interp_ = chosen; });
        item->setCheckable(true);
        item->setChecked(chosen == transform_interp_);
        group->addAction(item);
    }
}

// --- the area ---------------------------------------------------------------------------------------------------------

std::optional<Json> MainWindow::need_area() {
    auto area = selection_area();
    if (!area) flash(QStringLiteral("先に選択範囲を作ります（長方形選択ツール M・なげなわツール L・自動選択ツール W）"), 3000);
    return area;
}

void MainWindow::join_selection(const std::optional<Json>& area, const QString& how) {
    const core::Page* page = current_page();
    if (!area || page == nullptr) return;
    std::optional<Json> joined = area;
    if (how != QLatin1String("replace")) {
        try {
            render::use_brushes_of(book());
            joined = render::selection::combine(selection_area(), *area, how.toStdString(), *page, &book());
        } catch (const std::exception& error) {
            flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
            return;
        }
    }
    canvas_->set_selection(joined);
    if (!joined) flash(QStringLiteral("選択範囲がなくなりました"), 2000);
}

void MainWindow::selection_drawn(const Json& area, const QString& how) {
    if (how != QLatin1String("replace")) join_selection(area, how);
}

void MainWindow::selection_painted(const QVector<QPointF>& points, bool add) {
    std::vector<std::array<double, 2>> line;
    for (const QPointF& p : points) line.push_back({p.x(), p.y()});
    std::optional<Json> area;
    try {
        area = render::selection::stroke_area(line, canvas_->selection_pen_mm);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    if (!area) return;
    if (!add && !canvas_->selection()) return;
    join_selection(area, add ? QStringLiteral("add") : QStringLiteral("subtract"));
}

void MainWindow::wand(double x_mm, double y_mm) {
    if (wand_criterion == QLatin1String("colour")) {  // (色の近さ: Photoshop's magic wand)
        select_colour(x_mm, y_mm);
        return;
    }
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    namespace fills = render::tone_fills;
    const int dpi = fills::kFillDpi;
    std::optional<Json> area;
    try {
        const auto prefs = settings();
        const std::string reference = prefs->value(QStringLiteral("fill/reference"), QStringLiteral("page")).toString().toStdString();
        const double gap = prefs->value(QStringLiteral("fill/gap"), 0.3).toDouble();
        const render::Image seen = render::fill_reference_of(book(), *page, layer->id, reference, dpi);
        std::optional<render::Box> window;
        if (const core::Frame* panel = page->frame_at(core::Num(x_mm), core::Num(y_mm))) {
            const auto& r = panel->rect;
            window = render::Box{std::max(0, fills::px(r.x.value() - 2, dpi)), std::max(0, fills::px(r.y.value() - 2, dpi)),
                                 std::min(seen.width(), fills::px(r.x.value() + r.width.value() + 2, dpi)),
                                 std::min(seen.height(), fills::px(r.y.value() + r.height.value() + 2, dpi))};
        }
        const auto mask = fills::region_mask(seen, render::Point{fills::px(x_mm, dpi), fills::px(y_mm, dpi)}, fills::px(gap, dpi), 160, 1, window);
        if (mask) {  // (selection.wand_area)
            if (const auto patch = fills::mask_patch(*mask, dpi, {0, 0, 0}))
                area = Json{{"mask", Json{{"box", patch->attrs["box"]}, {"png", core::b64encode(*patch->png)}}}};
        }
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    if (!area) {
        flash(QStringLiteral("そこは線の上です。線で囲まれた中をクリックします"), 3000);
        return;
    }
    join_selection(area, canvas_->selection_how());
}

void MainWindow::select_colour(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    Json spec{{"x_mm", x_mm}, {"y_mm", y_mm}, {"tolerance", colour_tolerance}, {"contiguous", colour_contiguous}};
    if (canvas_->select_antialias) spec["antialias"] = true;  // (PC3 B1: the edge grey, as Photoshop's wand by default)
    if (!wand_all_layers) {  // (the drawing layer's own colours)
        if (const core::Layer* layer = target_layer()) spec["layer"] = layer->id;
    }
    Json area;
    try {
        render::use_brushes_of(book());
        area = render::selection::resolve(Json{{"color", spec}}, *page, &book());
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    join_selection(area, canvas_->selection_how());
}

void MainWindow::change_selection(const Json& change) {
    // invert, grow, shrink or soften the selection (the page as a whole when inverting nothing)
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto current = selection_area();
    if (!current && !change.value("invert", false)) {
        need_area();
        return;
    }
    Json spec{{"union", Json::array({current ? *current : Json{{"rect", Json::array({0, 0, 0.01, 0.01})}}})}};
    spec.update(change);
    Json area;
    try {
        render::use_brushes_of(book());
        area = render::selection::resolve(spec, *page, &book());
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    canvas_->set_selection(area);
}

void MainWindow::change_selection_by(const char* key, int sign) {
    // (選択範囲を変更: 拡張… expand_mm and 縮小… contract_mm by the distance transform, 境界線… border_mm, 滑らかに… smooth_mm,
    // 境界をぼかす… feather_mm)
    if (!need_area()) return;
    const Asked asked = asking();
    static const std::map<std::string, QString> words{{"expand_mm", QStringLiteral("拡張する")},   {"contract_mm", QStringLiteral("縮小する")},
                                                      {"border_mm", QStringLiteral("境界線の")},   {"smooth_mm", QStringLiteral("滑らかにする")},
                                                      {"feather_mm", QStringLiteral("ぼかす")},     {"grow_mm", QStringLiteral("広げる")}};
    const auto word = words.find(key);
    const QString title = word != words.end() ? word->second : QString();
    if (const auto amount = ask::get_double(this, QStringLiteral("選択範囲"), title + QStringLiteral("幅（mm）"), 1.0, 0.1, 50.0, 1);
        amount && still(asked))
        change_selection(Json{{key, sign * *amount}});
}

void MainWindow::select_all() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const double w = page->spec.width_mm.value(), h = page->spec.height_mm.value();
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    canvas_->set_selection(Json{{"poly", Json::array({Json::array({0, 0}), Json::array({w, 0}), Json::array({w, h}), Json::array({0, h})})}});
}

void MainWindow::select_drawn() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    Json area;
    try {
        render::use_brushes_of(book());
        area = render::selection::resolve(Json{{"transparency", layer->id}}, *page, &book());  // (its alpha, grey as it is)
    } catch (const core::OpError&) {
        flash(QStringLiteral("描く先のレイヤーには、まだ何も描いてありません"), 3000);
        return;
    }
    join_selection(area, QStringLiteral("replace"));
}

bool MainWindow::keep_selection(std::optional<QString> name) {
    const auto area = need_area();
    const core::Page* page = current_page();
    if (!area || page == nullptr) return false;
    if (!name) {
        const Asked asked = asking();
        name = ask::get_text(this, QStringLiteral("選択範囲をストック"), QStringLiteral("名前（例: 空、髪、背景）"));
        if (!name || name->trimmed().isEmpty() || !still(asked)) return false;
    }
    return apply_ops(Json::array({Json{{"op", "store_area"}, {"page", page->index.json()}, {"name", name->trimmed().toStdString()}, {"area", *area}}}));
}

void MainWindow::fill_stock() {
    QMenu* menu = stock_menu_;
    for (QMenu* sub : menu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly)) sub->deleteLater();  // (clear() leaves them)
    menu->clear();
    const core::Page* page = current_page();
    Json saved = Json::object();
    if (page != nullptr && page->extra.is_object() && page->extra.contains("saved_areas") && page->extra["saved_areas"].is_object())
        saved = page->extra["saved_areas"];
    if (saved.empty()) {
        menu->addAction(QStringLiteral("（このページにストックはありません）"))->setEnabled(false);
        return;
    }
    for (const auto& [name, area] : saved.items()) {
        const std::string key = name;
        menu->addAction(QString::fromStdString(name), this, [this, key] { use_stock(key); });
    }
    QMenu* forget = menu->addMenu(QStringLiteral("ストックを消す"));
    const Json index = page->index.json();
    for (const auto& [name, area] : saved.items()) {
        const std::string key = name;
        forget->addAction(QString::fromStdString(name), this,
                          [this, key, index] { apply_ops(Json::array({Json{{"op", "forget_area"}, {"page", index}, {"name", key}}})); });
    }
}

void MainWindow::use_stock(const std::string& name) {
    const core::Page* page = current_page();
    if (page == nullptr || !page->extra.is_object() || !page->extra.contains("saved_areas")) return;
    const Json& saved = page->extra["saved_areas"];
    if (saved.is_object() && saved.contains(name)) join_selection(saved[name], QStringLiteral("replace"));
}

void MainWindow::quick_mask(bool on) {
    // (PC3 B1, masks@1: the page's Quick Mask channel, white where the selection was, which every pixel tool paints — white
    // selects, black masks —; out of it, the selection it says. Each a step of the book's history: 取り消し goes back.)
    const core::Page* page = current_page();
    if (page == nullptr || (core::quick_mask_of(*page) != nullptr) == on) {
        quick_mask_follow();
        return;
    }
    Json op{{"op", "quick_mask"}, {"page", page->index.json()}, {"on", on}};
    const auto area = on ? selection_area() : std::nullopt;
    if (area) op["area"] = *area;
    std::optional<Json> back;  // (out of it: the selection the channel says, as the op says it)
    if (!on) {
        try {
            back = render::quick_mask_selection(*page);
        } catch (const std::exception& error) {
            flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
            quick_mask_follow();
            return;
        }
    }
    if (!apply_ops(Json::array({op}))) {
        quick_mask_follow();
        return;
    }
    if (on) {
        canvas_->set_selection(std::nullopt);
        if (canvas_->tool() == QLatin1String("marquee")) choose_tool(QStringLiteral("pen"));
        flash(QStringLiteral("クイックマスクモードで編集: ブラシ・消しゴム・塗りなどで、白は選択範囲に、黒はマスク（赤）になります。終わったらもう一度「クイックマスクモードで編集」"),
              6000);
    } else {
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        canvas_->set_selection(back);
    }
    quick_mask_follow();
}

void MainWindow::quick_mask_follow() {
    const core::Page* page = current_page();
    const core::Channel* quick = page != nullptr ? core::quick_mask_of(*page) : nullptr;
    canvas_->quick_mask = quick != nullptr;
    if (QAction* act = action(QStringLiteral("act_quick_mask"))) {
        const QSignalBlocker quiet(act);
        act->setChecked(quick != nullptr);
    }
    // (in Quick Mask the tools paint its channel: targeted unless another channel is)
    if (quick != nullptr && channel_view().target.empty()) target_quick_mask(quick->id);
}

void MainWindow::target_quick_mask(const std::string& id) {
    const core::Page* page = current_page();
    if (page == nullptr || core::find_channel(*page, id) == nullptr) return;
    ChannelView view = channel_view();
    view.target = id;
    view.hidden.assign(core::plate_names(*page).size(), false);  // (the page's colours shown, the mask laid over in its colour)
    view.shown.insert(id);
    channel_views_[page->id] = view;
    channel_view_changed();
}

// --- moving and changing what is in it ---------------------------------------------------------------------------------

Json MainWindow::moved_area(const Json& area, const std::array<double, 6>& m) {
    if (area.contains("poly") && core::py_truthy(area["poly"])) {
        Json poly = Json::array();
        for (const Json& p : area["poly"]) {
            const double x = p[0].get<double>(), y = p[1].get<double>();
            poly.push_back(Json::array({core::py_round(m[0] * x + m[2] * y + m[4], 3), core::py_round(m[1] * x + m[3] * y + m[5], 3)}));
        }
        Json out = area;  // (its anti-aliasing and feather kept)
        out["poly"] = poly;
        return out;
    }
    core::Patch patch;
    patch.attrs = Json{{"box", area["mask"]["box"]}, {"mode", "mask"}};
    patch.png = std::make_shared<const std::string>(render::selection::b64decode(area["mask"]["png"]));
    const auto moved = render::selection::transform_patch(patch, m);
    if (!moved) return area;
    return Json{{"mask", Json{{"box", moved->attrs["box"]}, {"png", core::b64encode(*moved->png)}}}};
}

void MainWindow::transform_selection(const QVector<double>& values) {
    const auto area = selection_area();
    std::array<double, 6> m{};
    for (int i = 0; i < 6; ++i) m[static_cast<std::size_t>(i)] = core::py_round(values.value(i), 5);
    if (area && canvas_->transform_outline) {  // (選択範囲を変形: the outline alone, the book as it is)
        std::vector<QPointF> moved;
        for (const QPointF& p : canvas_->selection()->outline) moved.emplace_back(m[0] * p.x() + m[2] * p.y() + m[4], m[1] * p.x() + m[3] * p.y() + m[5]);
        canvas_->set_selection(moved_area(*area, m), moved);
        canvas_->transform_outline = true;  // (still transforming until Enter)
        return;
    }
    if (!area) return;
    // (C-c: the chosen layers within the selection, by transform_layers; the selection goes with them)
    transform_within_selection(Json{{"matrix", Json::array({m[0], m[1], m[2], m[3], m[4], m[5]})}});
}

void MainWindow::start_warp(const QString& kind, int columns, int rows) {
    if (!selection_area()) {  // (no selection: the chosen layers' box, 自由変形's way)
        start_layer_transform(kind == QLatin1String("perspective") ? QStringLiteral("distort") : QStringLiteral("warp"));
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) canvas_->set_tool(QStringLiteral("marquee"));
    canvas_->start_warp(kind, columns, rows);
    flash(QStringLiteral("点を引っぱって形を決め、Enter（または「変形を確定」）で確定します。Esc でやめます"), 6000);
}

void MainWindow::start_mesh_grid() {
    if (!selection_area()) {  // (no selection: the warp's box; its splits make the grid)
        start_layer_transform(QStringLiteral("warp"));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("mesh_grid_dialog"));
    dialog.setWindowTitle(QStringLiteral("メッシュの格子"));
    auto* form = new QFormLayout(&dialog);
    auto* across = new QSpinBox;
    auto* down = new QSpinBox;
    across->setObjectName(QStringLiteral("mesh_across"));
    down->setObjectName(QStringLiteral("mesh_down"));
    const int now = settings()->value(QStringLiteral("warp/mesh"), 3).toInt();
    for (QSpinBox* box : {across, down}) {
        box->setRange(1, 8);
        box->setValue(now);
    }
    form->addRow(QStringLiteral("横の格子の数"), across);
    form->addRow(QStringLiteral("縦の格子の数"), down);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    settings()->setValue(QStringLiteral("warp/mesh"), across->value());
    start_warp(QStringLiteral("mesh"), across->value(), down->value());
}

void MainWindow::warp_selection(const Json& warp) {
    // (C-c: the selection's corners as a true projection, its grid of points as a mesh: transform_layers within it)
    if (!selection_area()) return;
    if (warp.contains("perspective")) {
        transform_within_selection(Json{{"quad", warp["perspective"]}});
    } else if (warp.contains("mesh")) {
        transform_within_selection(Json{{"grid_warp", warp}});
    }
}

void MainWindow::move_pivot() {
    if (!need_area() || !canvas_->selection()) return;
    if (canvas_->tool() != QLatin1String("marquee")) canvas_->set_tool(QStringLiteral("marquee"));
    canvas_->pivot_mode = true;
    flash(QStringLiteral("基準位置にする所をクリックします"), 4000);
}

void MainWindow::transform_numbers() {
    // 変形の数値入力: move, scale and turn the selection by numbers, about its 基準位置
    if (!need_area() || !canvas_->selection()) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("transform_numbers_dialog"));
    dialog.setWindowTitle(QStringLiteral("変形を数で決める"));
    auto* form = new QFormLayout(&dialog);
    std::map<std::string, QDoubleSpinBox*> boxes;
    for (const auto& [key, label, lo, hi, value, suffix] :
         {std::tuple{"dx", "右へ", -500.0, 500.0, 0.0, " mm"}, {"dy", "下へ", -500.0, 500.0, 0.0, " mm"}, {"sx", "横の大きさ", 1.0, 1000.0, 100.0, " %"},
          {"sy", "縦の大きさ", 1.0, 1000.0, 100.0, " %"}, {"angle", "回転（右回り）", -360.0, 360.0, 0.0, "°"}}) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(QStringLiteral("transform_%1").arg(QString::fromLatin1(key)));
        box->setRange(lo, hi);
        box->setDecimals(2);
        box->setValue(value);
        box->setSuffix(QString::fromUtf8(suffix));
        form->addRow(QString::fromUtf8(label), box);
        boxes[key] = box;
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const auto m = transform_matrix(canvas_->selection_pivot(), boxes["dx"]->value(), boxes["dy"]->value(), boxes["sx"]->value() / 100,
                                    boxes["sy"]->value() / 100, boxes["angle"]->value());
    const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
    bool changed = false;
    for (std::size_t i = 0; i < 6; ++i) changed = changed || std::abs(m[i] - identity[i]) > 1e-6;
    if (changed) transform_selection(QVector<double>(m.begin(), m.end()));
}

void MainWindow::flip(int sx, int sy) {
    (void)sy;
    turn_layers(sx < 0 ? "flip_h" : "flip_v");  // (C-c: the chosen layers, within the selection when there is one)
}

void MainWindow::delete_area() {
    // (vector@1, PC3 C-a: the pen's last anchor while it draws, the path selection tool's chosen subpath: canvas_pen.cpp)
    if (canvas_->pen_drawing() && canvas_->pen_back()) return;
    if (canvas_->work_remove()) return;  // (PC4 D4: ぼかしギャラリー's chosen pin or point while its work is on the page)
    if (canvas_->tool() == QLatin1String("pathselect") && canvas_->path_delete()) return;
    if (!selection_area() && canvas_->tool() == QLatin1String("select") && canvas_->selected_line_id) {
        apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", *canvas_->selected_line_id}}}));  // Delete on a picked balloon
        canvas_->selected_line_id.reset();
        return;
    }
    const auto area = need_area();
    if (!area) return;
    if (const core::Layer* layer = paint_layer())
        apply_ops(Json::array({Json{{"op", "delete_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area}}}));
}

void MainWindow::fill_selection() {
    const auto area = selection_area();
    const core::Layer* layer = paint_layer();
    if (layer == nullptr || !area) {
        if (!area) need_area();
        return;
    }
    Json op{{"op", "fill_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area}, {"rgb", pen_.rgb}};
    if (pen_.opacity < 1) op["opacity"] = core::py_round(pen_.opacity, 3);
    apply_ops(Json::array({op}));
}

void MainWindow::line_width() {
    const auto area = need_area();
    const core::Layer* layer = area ? paint_layer() : nullptr;
    if (!area || layer == nullptr) return;
    const Asked asked = asking();
    if (const auto value = ask::get_double(this, QStringLiteral("線の太さ"), QStringLiteral("選んだ範囲の線の太さ（mm）"), pen_.width_mm, 0.05, 50, 2);
        value && still(asked))
        apply_ops(Json::array({Json{{"op", "set_stroke_width"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area},
                                    {"width_mm", *value}}}));
}


// --- PC3 B1: 選択範囲を拡大・近似色を選択, 再選択, 選択範囲を変形, マジック消しゴムツール ----------------------------------------

void MainWindow::grow_selection(bool contiguous) {
    // (the selection's colours widened by 自動選択ツール's 許容値, on the page as it shows or the drawing layer alone)
    const auto current = need_area();
    const core::Page* page = current_page();
    if (!current || page == nullptr) return;
    Json spec{{"area", *current}, {"tolerance", colour_tolerance}};
    if (!wand_all_layers) {
        if (const core::Layer* layer = target_layer()) spec["layer"] = layer->id;
    }
    Json area;
    try {
        render::use_brushes_of(book());
        area = render::selection::resolve(Json{{contiguous ? "grow" : "similar", spec}}, *page, &book());
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    canvas_->set_selection(area);
}

void MainWindow::reselect() {
    const auto previous = canvas_->previous_selection();
    if (!previous) {
        flash(QStringLiteral("このページで戻せる選択範囲はありません"), 3000);
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    canvas_->set_selection(previous->area, previous->outline);
}

void MainWindow::transform_selection_outline() {
    if (!need_area()) return;
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    canvas_->transform_outline = true;
    canvas_->update();
    flash(QStringLiteral("選択範囲を変形: 枠のつまみで選択範囲の形だけを変えます（中身は動きません）。Enter で終わります"), 5000);
}

void MainWindow::magic_erase(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? paint_layer() : nullptr;
    if (layer == nullptr) return;
    const MagicEraserSettings& s = magic_eraser_settings;
    Json op{{"op", "magic_erase"},
            {"page", page->index.json()},
            {"layer_id", layer->id},
            {"x_mm", core::py_round(x_mm, 3)},
            {"y_mm", core::py_round(y_mm, 3)},
            {"tolerance", s.tolerance},
            {"contiguous", s.contiguous},
            {"antialias", s.antialias},
            {"sample", s.all_layers ? "page" : "layer"},
            {"opacity", core::py_round(s.opacity / 100.0, 3)}};
    if (const auto area = selection_area()) op["area"] = *area;
    // (a layer whose transparency is protected takes the background colour: Genko's paper, white)
    apply_ops(Json::array({op}));
}

// --- the selection's right-click menu --------------------------------------------------------------------------------

bool MainWindow::selection_context_menu(const QPoint& global) {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("selection_context_menu"));
    if (!fill_selection_menu(menu, canvas_->to_mm(QPointF(canvas_->mapFromGlobal(global))))) return false;
    menu.exec(global);
    return true;
}

bool MainWindow::fill_selection_menu(QMenu& menu, const QPointF& mm) {
    // (Photoshop's: with a selection tool, over the selection)
    const auto& selection = canvas_->selection();
    if (canvas_->tool() != QLatin1String("marquee") || !selection || selection->outline.empty()) return false;
    Json outline = Json::array();
    for (const QPointF& p : selection->outline) outline.push_back(Json::array({p.x(), p.y()}));
    if (!render::selection::AreaTest(Json{{"poly", outline}}).contains(mm.x(), mm.y())) return false;
    bool gap = false;
    for (const char* name : selection_context_items()) {
        if (name == nullptr) {
            gap = !menu.actions().isEmpty();
            continue;
        }
        QAction* act = action(QString::fromLatin1(name));
        if (act == nullptr) continue;  // (another group's command, not in this build yet)
        if (gap) menu.addSeparator();
        gap = false;
        menu.addAction(act);
    }
    return true;
}

// --- the tools' settings (ツールの設定) --------------------------------------------------------------------------------

void MainWindow::build_selection_pages(ToolSettings* ts) {
    marquee_mode_ = new QComboBox;
    marquee_mode_->setObjectName(QStringLiteral("marquee_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("長方形選択"), QStringLiteral("rect")}, {QStringLiteral("楕円形選択"), QStringLiteral("ellipse")},
                                     {QStringLiteral("なげなわ"), QStringLiteral("lasso")}, {QStringLiteral("多角形選択"), QStringLiteral("polyline")},
                                     {QStringLiteral("オブジェクト選択"), QStringLiteral("object")}, {QStringLiteral("クイック選択"), QStringLiteral("quick")},
                                     {QStringLiteral("自動選択"), QStringLiteral("wand")}, {QStringLiteral("選択ペン"), QStringLiteral("selpen")},
                                     {QStringLiteral("選択消し"), QStringLiteral("selerase")}})
        marquee_mode_->addItem(label, key);
    connect(marquee_mode_, &QComboBox::activated, this, [this](int) { choose_tool(marquee_mode_->currentData().toString()); });
    // 選択方法: how a new area joins the selection when no key is held
    auto* method = new QWidget;
    method->setObjectName(QStringLiteral("select_method"));
    auto* method_row = new QHBoxLayout(method);
    method_row->setContentsMargins(0, 0, 0, 0);
    auto* methods = new QButtonGroup(method);
    for (const auto& [key, label, word] : {std::tuple{"replace", "新規選択", "新規"}, {"add", "選択範囲に追加", "追加"},
                                           {"subtract", "現在の選択範囲から一部削除", "削除"}, {"intersect", "現在の選択範囲との共通範囲", "共通"}}) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("select_method_%1").arg(QString::fromLatin1(key)));
        button->setText(QString::fromUtf8(word));
        button->setToolTip(QString::fromUtf8(label));
        button->setCheckable(true);
        button->setChecked(QString::fromLatin1(key) == canvas_->select_method);
        methods->addButton(button);
        method_row->addWidget(button);
        const QString chosen = QString::fromLatin1(key);
        connect(button, &QToolButton::toggled, this, [this, chosen](bool on) {
            if (on) canvas_->select_method = chosen;
        });
    }
    method_row->addStretch();
    auto* antialias = new QCheckBox(QStringLiteral("アンチエイリアス"));
    antialias->setObjectName(QStringLiteral("select_antialias"));
    antialias->setToolTip(QStringLiteral("楕円形選択・なげなわ・多角形選択と、自動選択（色の近さ）の縁をなめらかに（長方形選択は付かない）"));
    antialias->setChecked(canvas_->select_antialias);
    connect(antialias, &QCheckBox::toggled, this, [this](bool on) { canvas_->select_antialias = on; });
    auto* feather = new QDoubleSpinBox;
    feather->setObjectName(QStringLiteral("select_feather"));
    feather->setRange(0, 50);
    feather->setDecimals(1);
    feather->setSuffix(QStringLiteral(" mm"));
    feather->setToolTip(QStringLiteral("作る選択範囲の縁をぼかす幅（0: ぼかさない）"));
    connect(feather, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->select_feather_mm = v; });
    auto* style = new QComboBox;
    style->setObjectName(QStringLiteral("marquee_style"));
    for (const auto& [label, key] : {std::pair{"標準", "normal"}, {"縦横比を固定", "ratio"}, {"固定", "fixed"}})
        style->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    auto* width = new QDoubleSpinBox;
    auto* height = new QDoubleSpinBox;
    width->setObjectName(QStringLiteral("marquee_width"));
    height->setObjectName(QStringLiteral("marquee_height"));
    for (QDoubleSpinBox* box : {width, height}) {
        box->setRange(0.1, 1000);
        box->setDecimals(1);
        box->setValue(1);
        box->setEnabled(false);
    }
    connect(style, &QComboBox::currentIndexChanged, this, [this, style, width, height](int) {
        canvas_->marquee_style = style->currentData().toString();
        const bool fixed = canvas_->marquee_style == QLatin1String("fixed");
        for (QDoubleSpinBox* box : {width, height}) {
            box->setEnabled(canvas_->marquee_style != QLatin1String("normal"));
            box->setSuffix(fixed ? QStringLiteral(" mm") : QString());
        }
        if (fixed && width->value() < 5 && height->value() < 5) {  // (a size one sees)
            width->setValue(30);
            height->setValue(20);
        }
    });
    connect(width, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->style_width = v; });
    connect(height, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->style_height = v; });
    auto* criterion = new QComboBox;
    criterion->setObjectName(QStringLiteral("wand_criterion"));
    criterion->addItem(QStringLiteral("線で囲まれた所"), QStringLiteral("lines"));
    criterion->addItem(QStringLiteral("色の近さ"), QStringLiteral("colour"));
    criterion->setToolTip(QStringLiteral("自動選択ツールが見るもの: 線で囲まれた所（塗りつぶしと同じ）か、クリックした所の色に近い所か"));
    connect(criterion, &QComboBox::currentIndexChanged, this, [this, criterion](int) { wand_criterion = criterion->currentData().toString(); });
    selection_pen_ = new QDoubleSpinBox;
    selection_pen_->setObjectName(QStringLiteral("selection_pen"));
    selection_pen_->setRange(0.2, 60);
    selection_pen_->setSuffix(QStringLiteral(" mm"));
    selection_pen_->setValue(canvas_->selection_pen_mm);
    connect(selection_pen_, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->selection_pen_mm = v; });
    colour_tolerance_box_ = new QSpinBox;
    colour_tolerance_box_->setObjectName(QStringLiteral("colour_tolerance"));
    colour_tolerance_box_->setRange(0, 255);
    colour_tolerance_box_->setValue(static_cast<int>(colour_tolerance));
    colour_tolerance_box_->setToolTip(QStringLiteral("自動選択ツール（色の近さ）: どれだけ違う色まで同じとみなすか"));
    connect(colour_tolerance_box_, &QSpinBox::valueChanged, this, [this](int v) { colour_tolerance = v; });
    colour_contiguous_box_ = new QCheckBox(QStringLiteral("隣接"));
    colour_contiguous_box_->setObjectName(QStringLiteral("colour_contiguous"));
    connect(colour_contiguous_box_, &QCheckBox::toggled, this, [this](bool on) { colour_contiguous = on; });
    auto* all_layers = new QCheckBox(QStringLiteral("全レイヤーを対象"));
    all_layers->setObjectName(QStringLiteral("wand_all_layers"));
    all_layers->setToolTip(QStringLiteral("見えているページの色で選ぶ（外すと描く先のレイヤーの色だけ）"));
    all_layers->setChecked(wand_all_layers);
    connect(all_layers, &QCheckBox::toggled, this, [this](bool on) { wand_all_layers = on; });
    auto* sel_form = new QWidget;
    auto* sfl = new QFormLayout(sel_form);
    sfl->setContentsMargins(0, 0, 0, 0);
    sfl->setRowWrapPolicy(QFormLayout::WrapLongRows);
    sfl->addRow(QStringLiteral("選び方"), marquee_mode_);
    sfl->addRow(QStringLiteral("選択方法"), method);
    sfl->addRow(QString(), antialias);
    sfl->addRow(QStringLiteral("ぼかし"), feather);
    sfl->addRow(QStringLiteral("スタイル"), style);
    sfl->addRow(QStringLiteral("幅"), width);
    sfl->addRow(QStringLiteral("高さ"), height);
    sfl->addRow(QStringLiteral("自動選択の基準"), criterion);
    sfl->addRow(QStringLiteral("許容値"), colour_tolerance_box_);
    sfl->addRow(QString(), colour_contiguous_box_);
    sfl->addRow(QString(), all_layers);
    sfl->addRow(QStringLiteral("選択ペンの太さ"), selection_pen_);
    sfl->addRow(smart_select_settings_widget());  // (PC3 B3: クイック選択・オブジェクト選択)
    ts->add({QStringLiteral("marquee")},
            action_page({sel_form, static_cast<QWidget*>(hint(QStringLiteral("Shift で選択範囲に追加・Alt で一部削除・両方で共通範囲（その時だけ、選択方法より先に）"))),
                         QStringLiteral("選択範囲と中身"),
                         static_cast<QWidget*>(menu_button(QStringLiteral("選択範囲"),
                                                           {{action("act_select_all"), action("act_deselect"), action("act_sel_reselect"), action("act_sel_invert")},
                                                            {action("act_color_range"), action("act_sel_grow_colour"), action("act_sel_similar"), action("act_sel_layer")},
                                                            {action("act_select_subject"), action("act_select_sky"), action("act_focus_area"), action("act_select_mask")},
                                                            {action("act_sel_border"), action("act_sel_smooth"), action("act_sel_grow"), action("act_sel_shrink"),
                                                             action("act_sel_feather"), action("act_sel_transform")},
                                                            {action("act_sel_keep"), action("act_quick_mask"), action("act_sel_load"), action("act_sel_to_channel")}})),
                         static_cast<QWidget*>(menu_button(QStringLiteral("中身"),
                                                           {{action("act_copy"), action("act_cut"), action("act_paste"), action("act_delete_area")},
                                                            {action("act_flip_h"), action("act_flip_v"), action("act_warp_perspective"), action("act_warp_mesh"),
                                                             action("act_warp_apply")},
                                                            {action("act_fill_selection"), action("act_line_width"), action("act_tone_here")}}))}));
    // マジック消しゴムツール
    auto* tolerance = new QSpinBox;
    tolerance->setObjectName(QStringLiteral("magic_tolerance"));
    tolerance->setRange(0, 255);
    tolerance->setValue(static_cast<int>(magic_eraser_settings.tolerance));
    connect(tolerance, &QSpinBox::valueChanged, this, [this](int v) { magic_eraser_settings.tolerance = v; });
    auto* magic_antialias = new QCheckBox(QStringLiteral("アンチエイリアス"));
    magic_antialias->setObjectName(QStringLiteral("magic_antialias"));
    magic_antialias->setChecked(magic_eraser_settings.antialias);
    connect(magic_antialias, &QCheckBox::toggled, this, [this](bool on) { magic_eraser_settings.antialias = on; });
    auto* magic_contiguous = new QCheckBox(QStringLiteral("隣接"));
    magic_contiguous->setObjectName(QStringLiteral("magic_contiguous"));
    magic_contiguous->setChecked(magic_eraser_settings.contiguous);
    connect(magic_contiguous, &QCheckBox::toggled, this, [this](bool on) { magic_eraser_settings.contiguous = on; });
    auto* magic_all = new QCheckBox(QStringLiteral("全レイヤーを対象"));
    magic_all->setObjectName(QStringLiteral("magic_all_layers"));
    magic_all->setChecked(magic_eraser_settings.all_layers);
    connect(magic_all, &QCheckBox::toggled, this, [this](bool on) { magic_eraser_settings.all_layers = on; });
    auto* opacity = new QSpinBox;
    opacity->setObjectName(QStringLiteral("magic_opacity"));
    opacity->setRange(1, 100);
    opacity->setSuffix(QStringLiteral(" %"));
    opacity->setValue(static_cast<int>(magic_eraser_settings.opacity));
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { magic_eraser_settings.opacity = v; });
    auto* magic_form = new QWidget;
    auto* mfl = new QFormLayout(magic_form);
    mfl->setContentsMargins(0, 0, 0, 0);
    mfl->setRowWrapPolicy(QFormLayout::WrapLongRows);
    mfl->addRow(QStringLiteral("許容値"), tolerance);
    mfl->addRow(QString(), magic_antialias);
    mfl->addRow(QString(), magic_contiguous);
    mfl->addRow(QString(), magic_all);
    mfl->addRow(QStringLiteral("不透明度"), opacity);
    ts->add({QStringLiteral("magiceraser")},
            action_page({magic_form, static_cast<QWidget*>(hint(QStringLiteral("透明部分を保護したレイヤーでは、消す代わりに白で塗ります。選択範囲があればその中だけ")))}));
}

}  // namespace genko::app
