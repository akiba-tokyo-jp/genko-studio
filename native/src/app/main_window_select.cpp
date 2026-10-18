// The window's selection commands (Python's genko/app/main.py: _select_all, _change_selection, _wand, _select_colour,
// _selection_painted, _keep_selection, _quick_mask, _copy, _cut, _paste, _delete_area, _flip, _transform_selection,
// _warp_selection, _transform_numbers, _fill_area, _line_width): the selection menu and what the marquee tool's areas
// do. The selection is the canvas's; each change to the book is an op on the layer drawn on.

#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMenu>
#include <QSpinBox>

#include <cmath>
#include <map>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "service/wording.hpp"
#include "core/base64.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "render/color_edit.hpp"
#include "render/fill_patches.hpp"
#include "render/raster_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

namespace {

using Std = QKeySequence::StandardKey;

// The marquee tool's ways, by the window's tool names (Python's _tool).
const std::map<QString, QString>& marquee_ways() {
    static const std::map<QString, QString> ways{{QStringLiteral("rect"), QStringLiteral("rect")},         {QStringLiteral("lasso"), QStringLiteral("lasso")},
                                                 {QStringLiteral("wand"), QStringLiteral("wand")},         {QStringLiteral("ellipse"), QStringLiteral("ellipse")},
                                                 {QStringLiteral("polyline"), QStringLiteral("polyline")}, {QStringLiteral("colour"), QStringLiteral("color")},
                                                 {QStringLiteral("selpen"), QStringLiteral("pen")},        {QStringLiteral("selerase"), QStringLiteral("erase")}};
    return ways;
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
         QStringLiteral("クリックした所の、線で囲まれた範囲を選びます"));
    tool("act_sel_ellipse", QStringLiteral("楕円形選択ツール"), QStringLiteral("ellipse"), {}, QStringLiteral("ドラッグで楕円に選ぶ（Shift で足す、Alt で引く）"));
    tool("act_sel_polyline", QStringLiteral("多角形選択ツール"), QStringLiteral("polyline"), {}, QStringLiteral("クリックで角を置き、ダブルクリックか Enter で閉じる"));
    tool("act_sel_colour", QStringLiteral("色域選択"), QStringLiteral("colour"), {}, QStringLiteral("クリックした所と同じ色の所をページ中から選ぶ"));
    tool("act_sel_pen", QStringLiteral("選択ペン"), QStringLiteral("selpen"), {}, QStringLiteral("なぞった所を選択範囲に足す"));
    tool("act_sel_erase", QStringLiteral("選択消し"), QStringLiteral("selerase"), {}, QStringLiteral("なぞった所を選択範囲から外す"));

    make("act_select_all", QStringLiteral("すべてを選択"), [this] { select_all(); }, {QKeySequence(Std::SelectAll)});
    make("act_deselect", QStringLiteral("選択を解除"), [this] { canvas_->set_selection(std::nullopt); }, {QKeySequence(QStringLiteral("Ctrl+D"))});
    make("act_sel_invert", QStringLiteral("選択範囲を反転"), [this] { change_selection(Json{{"invert", true}}); }, {QKeySequence(QStringLiteral("Ctrl+Alt+I"))});
    make("act_sel_grow", QStringLiteral("拡張…"), [this] { change_selection_by("grow_mm", 1); }, {}, QStringLiteral("選択範囲の縁を外へ広げる"));
    make("act_sel_shrink", QStringLiteral("縮小…"), [this] { change_selection_by("grow_mm", -1); }, {}, QStringLiteral("選択範囲の縁を内へ狭める"));
    make("act_sel_feather", QStringLiteral("境界をぼかす…"), [this] { change_selection_by("feather_mm", 1); },
         {QKeySequence(QStringLiteral("Shift+F6"))}, QStringLiteral("選択範囲の縁をなめらかにぼかす"));
    make("act_sel_layer", QStringLiteral("ピクセルを選択"), [this] { select_drawn(); }, {}, QStringLiteral("描く先のレイヤーで描いてある所を選ぶ"));
    make("act_sel_keep", QStringLiteral("選択範囲をストック…"), [this] { keep_selection(); }, {},
         QStringLiteral("名前を付けてページに残す（後で「ストックから選ぶ」）"));
    QAction* quick = make("act_quick_mask", QStringLiteral("クイックマスクモードで編集"), [] {}, {}, QStringLiteral("選択範囲を赤で見せ、選択ペン・選択消しで直す"), true);
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
         {QKeySequence(QStringLiteral("Ctrl+Shift+P"))}, QStringLiteral("選択範囲の 4 隅を好きな所へ引っぱる。Enter で確定、Esc でやめる"));
    make("act_warp_mesh", QStringLiteral("ワープ"), [this] { start_warp(QStringLiteral("mesh")); },
         {QKeySequence(QStringLiteral("Ctrl+Shift+W"))}, QStringLiteral("選択範囲の 3×3 の点を引っぱって曲げる。Enter で確定、Esc でやめる"));
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
    static const std::vector<const char*> names{"act_marquee", "act_sel_ellipse", "act_lasso", "act_sel_polyline", "act_wand",
                                                "act_sel_colour", "act_sel_pen", "act_sel_erase"};
    return names;
}

const std::vector<const char*>& MainWindow::retouch_tools() {
    static const std::vector<const char*> names;  // (B4: スポット修復ブラシツール 修復ブラシツール コピースタンプツール ヒストリーブラシツール)
    return names;
}

void MainWindow::build_tool_groups(QMenu* tools) {
    for (const std::vector<const char*>* group : {&selection_tools(), &retouch_tools()}) {
        if (group->empty()) continue;
        for (const char* name : *group) tools->addAction(action(QString::fromLatin1(name)));
        tools->addSeparator();
    }
}

void MainWindow::build_selection_menu(QMenu* menu) {
    for (const char* name : {"act_select_all", "act_deselect", "act_sel_invert"}) menu->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    QMenu* modify = menu->addMenu(QStringLiteral("選択範囲を変更"));
    for (const char* name : {"act_sel_grow", "act_sel_shrink", "act_sel_feather"}) modify->addAction(action(QString::fromLatin1(name)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("act_quick_mask")));
    menu->addSeparator();
    // (channels@1: the selection made from a channel, and kept as an alpha channel)
    channel_load_menu_ = menu->addMenu(QStringLiteral("選択範囲を読み込む"));
    channel_load_menu_->setProperty("remade", true);
    connect(channel_load_menu_, &QMenu::aboutToShow, this, [this] { fill_channel_menu(); });
    menu->addAction(action(QStringLiteral("act_sel_to_channel")));
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
    for (const char* name : {"act_warp_perspective", "act_warp_mesh", "act_warp_mesh_grid"}) menu->addAction(action(QString::fromLatin1(name)));
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
    const Json spec{{"x_mm", x_mm}, {"y_mm", y_mm}, {"tolerance", colour_tolerance}, {"contiguous", colour_contiguous}};
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
    if (!need_area()) return;
    const Asked asked = asking();
    const QString title = QString::fromLatin1(key) == QLatin1String("grow_mm") ? (sign > 0 ? QStringLiteral("広げる") : QStringLiteral("狭める"))
                                                                                 : QStringLiteral("ぼかす");
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
    // the selection shown in red, to be painted with the selection pen and eraser
    canvas_->quick_mask = on;
    if (on) {
        choose_tool(QStringLiteral("selpen"));
        flash(QStringLiteral("クイックマスクモードで編集: 選択ペンで足し、選択消しで外します。終わったらもう一度「クイックマスクモードで編集」"), 5000);
    }
    canvas_->update();
}

// --- moving and changing what is in it ---------------------------------------------------------------------------------

Json MainWindow::moved_area(const Json& area, const std::array<double, 6>& m) {
    if (area.contains("poly") && core::py_truthy(area["poly"])) {
        Json poly = Json::array();
        for (const Json& p : area["poly"]) {
            const double x = p[0].get<double>(), y = p[1].get<double>();
            poly.push_back(Json::array({core::py_round(m[0] * x + m[2] * y + m[4], 3), core::py_round(m[1] * x + m[3] * y + m[5], 3)}));
        }
        return Json{{"poly", poly}};
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
    const core::Layer* layer = area ? paint_layer() : nullptr;
    if (!area || layer == nullptr) return;
    std::array<double, 6> m{};
    for (int i = 0; i < 6; ++i) m[static_cast<std::size_t>(i)] = core::py_round(values.value(i), 5);
    const Json matrix = Json::array({m[0], m[1], m[2], m[3], m[4], m[5]});
    const std::vector<QPointF> outline = canvas_->selection()->outline;
    if (apply_ops(Json::array({Json{{"op", "transform_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area},
                                    {"matrix", matrix}, {"interp", transform_interp_}}}))) {
        std::vector<QPointF> moved;
        for (const QPointF& p : outline) moved.emplace_back(m[0] * p.x() + m[2] * p.y() + m[4], m[1] * p.x() + m[3] * p.y() + m[5]);
        canvas_->set_selection(moved_area(*area, m), moved);
    }
}

void MainWindow::start_warp(const QString& kind, int columns, int rows) {
    if (!need_area()) return;
    if (canvas_->tool() != QLatin1String("marquee")) canvas_->set_tool(QStringLiteral("marquee"));
    canvas_->start_warp(kind, columns, rows);
    flash(QStringLiteral("点を引っぱって形を決め、Enter（または「変形を確定」）で確定します。Esc でやめます"), 6000);
}

void MainWindow::start_mesh_grid() {
    if (!need_area()) return;
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
    const auto area = selection_area();
    const core::Layer* layer = area ? paint_layer() : nullptr;
    if (!area || layer == nullptr) return;
    if (apply_ops(Json::array({Json{{"op", "transform_area"}, {"page", current_page()->index.json()}, {"layer_id", layer->id}, {"area", *area},
                                    {"warp", warp}, {"interp", transform_interp_}}})))
        canvas_->set_selection(std::nullopt);
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
    if (!need_area()) return;
    const auto [x0, y0, x1, y1] = canvas_->selection_box();
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    transform_selection(QVector<double>{double(sx), 0, 0, double(sy), cx - sx * cx, cy - sy * cy});
}

void MainWindow::delete_area() {
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

}  // namespace genko::app
