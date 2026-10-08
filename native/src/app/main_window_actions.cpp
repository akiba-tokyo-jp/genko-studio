// The main window's commands: the actions with Python's words, keys and tips (MainWindow._build_actions), the menus,
// the tool palette and the command bar, the docks, and what each command does.

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDir>
#include <QDockWidget>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSizePolicy>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/dialogs.hpp"
#include "app/frame_tools.hpp"
#include "app/icons.hpp"
#include "app/material_panel.hpp"
#include "app/material_tabs.hpp"
#include "app/subview.hpp"
#include "app/main_window.hpp"
#include "app/navigator.hpp"
#include "app/pages_panel.hpp"
#include "app/perf.hpp"
#include "app/templates.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/exposure.hpp"
#include "core/pynum.hpp"
#include "render/brushes.hpp"
#include "render/image.hpp"
#include "render/page.hpp"

namespace genko::app {

using core::Json;

namespace {

using Std = QKeySequence::StandardKey;

// QKeySequence(k) for each key: a standard key is its first binding on this system (as Python's QKeySequence(std.X)).
QList<QKeySequence> keys(std::initializer_list<QKeySequence> list) { return QList<QKeySequence>(list); }
QKeySequence std_key(Std key) { return QKeySequence(key); }

struct Stage {
    const char* key;
    const char* label;
    std::vector<const char*> show;
    std::vector<const char*> front;
};

const std::vector<Stage>& stages() {
    // (comfort.STAGES: the panels each stage of the work uses most)
    static const std::vector<Stage> list = {
        {"name", "ネーム", {"ツールの設定", "クイックアクセス", "ページ", "台詞", "点検"}, {"台詞", "ページ"}},
        {"ink", "作画", {"ツールの設定", "クイックアクセス", "全体図", "レイヤー", "カラー", "素材", "履歴"}, {"レイヤー", "素材"}},
        {"finish", "仕上げ", {"ツールの設定", "クイックアクセス", "レイヤー", "素材", "定規・3D", "点検"}, {"レイヤー", "素材"}},
        {"letter", "写植", {"ツールの設定", "ページ", "台詞", "点検"}, {"台詞", "ページ"}},
        {"review", "承認", {"承認箱", "コマの詳細", "資料", "ページ", "点検"}, {"承認箱", "ページ"}},
    };
    return list;
}

Json point_json(const QPointF& p) { return Json::array({core::py_round(p.x(), 2), core::py_round(p.y(), 2)}); }

}  // namespace

QAction* MainWindow::action(const QString& attribute) const {
    const auto it = actions_.find(attribute);
    return it == actions_.end() ? nullptr : it->second;
}

QAction* MainWindow::make(const QString& attribute, const QString& title, std::function<void()> slot, const QList<QKeySequence>& shortcuts,
                          const QString& tip, bool checkable) {
    auto* act = new QAction(title, this);
    // (each key once: where the system's standard key is one of the others — Redo is Ctrl+Y on Windows — Python's list
    // binds it twice, and Qt takes a key bound twice as ambiguous and does nothing)
    QList<QKeySequence> unique;
    for (const QKeySequence& key : shortcuts) {
        if (!key.isEmpty() && !unique.contains(key)) unique << key;
    }
    if (!unique.isEmpty()) act->setShortcuts(unique);
    if (!tip.isEmpty()) {
        act->setStatusTip(tip);
        act->setToolTip((title + QStringLiteral("  ") + act->shortcut().toString(QKeySequence::NativeText) + QStringLiteral("\n") + tip).trimmed());
    }
    act->setCheckable(checkable);
    act->setObjectName(QStringLiteral("cmd:%1").arg(title));
    connect(act, &QAction::triggered, this, [slot = std::move(slot)](bool) { slot(); });
    addAction(act);  // (its keys work wherever the window has the focus)
    actions_[attribute] = act;
    return act;
}

void MainWindow::build_actions() {
    PageCanvas* c = canvas_;
    make("act_new", QStringLiteral("新しい原稿…"), [this] { new_book(); }, keys({std_key(Std::New)}));
    make("act_open", QStringLiteral("開く…"), [this] { open_book(); }, keys({std_key(Std::Open)}));
    make("act_save", QStringLiteral("保存"), [this] { save(); }, keys({std_key(Std::Save)}), QStringLiteral("変更は自動で保存されます。今すぐ書き込むときに使います"));
    make("act_save_as", QStringLiteral("別の場所に保存…"), [this] { save_as(); }, keys({std_key(Std::SaveAs)}));
    make("act_exposure", QStringLiteral("露光量の調整層…"), [this] { exposure_dialog(); });
    make("act_nombre", QStringLiteral("ノンブル（ページ番号）の設定…"), [this] { nombre_dialog(); });
    make("act_undo", QStringLiteral("元に戻す"), [this] { undo(); }, keys({std_key(Std::Undo)}));
    make("act_redo", QStringLiteral("やり直す"), [this] { redo(); }, keys({std_key(Std::Redo), QKeySequence(QStringLiteral("Ctrl+Y"))}));
    make("act_fit", QStringLiteral("全体を表示"), [c] { c->glide([c] { c->fit_page(); }); }, keys({QKeySequence(QStringLiteral("Ctrl+0"))}));
    make("act_zoom_in", QStringLiteral("拡大"), [c] { c->glide([c] { c->zoom_by(1.25); }); },
         keys({std_key(Std::ZoomIn), QKeySequence(QStringLiteral("Ctrl+="))}));
    make("act_zoom_out", QStringLiteral("縮小"), [c] { c->glide([c] { c->zoom_by(0.8); }); }, keys({std_key(Std::ZoomOut)}));
    make("act_actual", QStringLiteral("原寸（紙の大きさ）"), [c] { c->glide([c] { c->actual_size(); }); }, keys({QKeySequence(QStringLiteral("Ctrl+1"))}));
    // (- and ^ as in CLIP STUDIO PAINT, too: Intel graphics drivers take Ctrl+Alt+arrows to turn the whole screen)
    make("act_turn_left", QStringLiteral("左に回す（15°）"), [c] { c->rotate_view(-15); },
         keys({QKeySequence(QStringLiteral("-")), QKeySequence(QStringLiteral("Ctrl+Alt+Left"))}),
         QStringLiteral("表示だけを回します（原稿は回りません）。Shift＋スペースを押しながらドラッグでも回せます"));
    make("act_turn_right", QStringLiteral("右に回す（15°）"), [c] { c->rotate_view(15); },
         keys({QKeySequence(QStringLiteral("^")), QKeySequence(QStringLiteral("Ctrl+Alt+Right"))}), QStringLiteral("表示だけを回します（原稿は回りません）"));
    make("act_turn_reset", QStringLiteral("回転・反転を戻す"), [c] { c->reset_view(); }, keys({QKeySequence(QStringLiteral("Ctrl+Alt+0"))}));
    make("act_zoom_tool", QStringLiteral("虫めがね"), [this] { choose_tool(QStringLiteral("zoom")); }, keys({QKeySequence(QStringLiteral("Z"))}),
         QStringLiteral("クリックで拡大、Alt＋クリックで縮小、ドラッグで囲んだ所を画面いっぱいに"), true);
    make("act_zoom_value", QStringLiteral("表示倍率を打ち込む…"), [this] { ask_zoom(); }, {}, QStringLiteral("倍率（%）を数で決めます。ステータスバーの倍率でも"));
    QAction* mirror = make("act_mirror", QStringLiteral("左右反転して見る"), [] {}, keys({QKeySequence(QStringLiteral("H"))}),
                           QStringLiteral("表示だけを左右反転します（絵の歪みを見つける）。原稿は変わりません"), true);
    connect(mirror, &QAction::triggered, this, [c](bool on) { c->flip_view(on); });
    make("act_overview", QStringLiteral("ページを並べて見る"), [this] { page_overview(); }, keys({QKeySequence(QStringLiteral("Ctrl+Shift+O"))}),
         QStringLiteral("全ページを縮小図で並べ、ダブルクリックで開きます"));
    make("act_prev", QStringLiteral("◀ 前のページ"), [this] { jump(-1); }, keys({std_key(Std::MoveToPreviousPage), QKeySequence(QStringLiteral("Ctrl+Left"))}));
    make("act_next", QStringLiteral("次のページ ▶"), [this] { jump(1); }, keys({std_key(Std::MoveToNextPage), QKeySequence(QStringLiteral("Ctrl+Right"))}));
    QAction* guides = make("act_guides", QStringLiteral("仕上がり線・基本枠を表示"), [] {}, keys({QKeySequence(QStringLiteral("Ctrl+;"))}),
                           QStringLiteral("断ち切り（裁ち落とし）・仕上がり線・基本枠"), true);
    guides->setChecked(true);
    connect(guides, &QAction::triggered, this, [c, guides](bool) {
        c->show_guides = guides->isChecked();
        c->update();
    });
    QAction* proof = make("act_cmyk_proof", QStringLiteral("CMYK で見る（色校正）"), [] {}, {},
                          QStringLiteral("印刷したときの色の見当（CMYK の範囲に収めた色）で表示します。プロファイルは書き出しで選んだもの"), true);
    // (the CMYK profile chosen last for export, kept on this computer, while its file is there)
    const auto profile = [] {
        std::optional<std::filesystem::path> icc;
        const QString chosen = settings()->value(QStringLiteral("color/icc")).toString();
        if (!chosen.isEmpty() && QFileInfo(chosen).isFile()) icc = core::path_from_utf8(chosen.toStdString());
        return icc;
    };
    c->renderer().set_proof_profile(profile);
    connect(proof, &QAction::triggered, this, [c, profile](bool on) {
        c->renderer().set_cmyk_proof(on ? std::optional<std::optional<std::filesystem::path>>(profile()) : std::nullopt);
        c->update();
    });
    // the tools (one at a time: tool_actions)
    make("act_select", QStringLiteral("選択"), [this] { choose_tool(QStringLiteral("select")); }, keys({QKeySequence(QStringLiteral("V"))}),
         QStringLiteral("コマを選ぶ・フキダシを動かす・ドラッグで表示を動かす"), true);
    make("act_pen", QStringLiteral("ペン"), [this] { choose_tool(QStringLiteral("pen")); }, keys({QKeySequence(QStringLiteral("B"))}),
         QStringLiteral("レイヤー パネルで選んだレイヤーに描きます"), true);
    make("act_eraser", QStringLiteral("消しゴム"), [this] { choose_tool(QStringLiteral("eraser")); }, keys({QKeySequence(QStringLiteral("E"))}),
         QStringLiteral("ペンの線は触れた所で切れます"), true);
    make("act_frame", QStringLiteral("コマ割り"), [this] { choose_tool(QStringLiteral("frame")); }, keys({QKeySequence(QStringLiteral("F"))}),
         QStringLiteral("コマの中をドラッグして割る（斜めも。水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで間隔を動かす・選んだコマの角をドラッグで形を変える"), true);
    make("act_move", QStringLiteral("レイヤー移動"), [this] { choose_tool(QStringLiteral("move")); }, keys({QKeySequence(QStringLiteral("Q"))}),
         QStringLiteral("描く先のレイヤーを丸ごとドラッグで動かす（Shift で縦・横・45°）"), true);
    tools_ = new QActionGroup(this);
    for (const auto& [tool, name] : {std::pair{"select", "act_select"}, {"pen", "act_pen"}, {"eraser", "act_eraser"}, {"frame", "act_frame"},
                                     {"move", "act_move"}, {"zoom", "act_zoom_tool"}}) {
        QAction* act = actions_.at(QString::fromLatin1(name));
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[QString::fromLatin1(tool)] = act;
    }
    actions_.at("act_select")->setChecked(true);
    build_selection_actions();  // (範囲選択 and the selection's commands: main_window_select.cpp)
    build_anim_actions();       // (アニメーション, タイムラプス: main_window_anim.cpp)
    build_paint_actions();      // (スポイト … ゆがみ, 色の入れ替え, 透明色, 太く・細く: main_window_paint.cpp)
    build_vector_actions();     // (線の修正, 線の編集 and its commands: main_window_vector.cpp)
    build_guide_actions();      // (定規, 3D and their commands: main_window_guides.cpp)
    build_material_actions();   // (効果線, 素材を置く, トーン, the view's extras, the layer commands: main_window_materials.cpp)
    build_line_actions();       // (テキスト, 台詞 and the book's lines, the 台詞 panel: main_window_lines.cpp)
    make("act_point_wider", QStringLiteral("選んだ点を太く"), [this] { point_width(1.25); }, keys({QKeySequence(QStringLiteral("Ctrl+Alt+]"))}),
         QStringLiteral("線の編集で選んだ制御点のところだけ、線を太くします"));
    make("act_color", QStringLiteral("ペンの色…"), [this] { pick_colour(); }, keys({QKeySequence(QStringLiteral("C"))}));
    make("act_layer_merge_down", QStringLiteral("下のレイヤーと結合"), [this] { layer_operation("merge_down"); });
    make("act_layer_merge_layers", QStringLiteral("選んだレイヤーを結合…"), [this] { layer_operation("merge_layers"); });
    make("act_layer_merge_visible", QStringLiteral("表示レイヤーの結合コピーを作る"), [this] { layer_operation("merge_visible"); });
    make("act_layer_flatten", QStringLiteral("画像をフラット化…"), [this] { layer_operation("flatten"); });
    make("act_layer_convert_paint", QStringLiteral("ペイントレイヤーに変換"), [this] { layer_operation("convert_layer"); });
    make("act_layer_convert_pen", QStringLiteral("ペンレイヤーに変換…"), [this] { layer_operation("convert_pen"); });
    // panels
    make("act_split_h", QStringLiteral("コマを横に割る（上下に分ける）"), [this] { split(QStringLiteral("horizontal")); },
         keys({QKeySequence(QStringLiteral("Ctrl+Shift+H"))}));
    make("act_split_v", QStringLiteral("コマを縦に割る（左右に分ける）"), [this] { split(QStringLiteral("vertical")); },
         keys({QKeySequence(QStringLiteral("Ctrl+Shift+V"))}));
    make("act_merge", QStringLiteral("コマを結合（割る前に戻す）"), [this] { merge(); }, keys({QKeySequence(QStringLiteral("Ctrl+Shift+M"))}));
    make("act_delete_frame", QStringLiteral("このコマを消す（ほかのコマはそのまま）"), [this] { delete_frame(); });
    make("act_frame_selection", QStringLiteral("このコマを選択範囲にする"), [this] { frame_to_selection(); }, {},
         QStringLiteral("選んだコマの形を選択範囲にします（塗りつぶし・トーン・消去をコマの中だけに）"));
    make("act_gutters", QStringLiteral("コマ間隔の設定…"), [this] { gutter_settings(); }, {}, QStringLiteral("新しく割るときの上下・左右の間隔"));
    make("act_border", QStringLiteral("選んだコマの枠線の太さ…"), [this] { border_width(); });
    make("act_corner", QStringLiteral("選んだコマの角の丸み…"), [this] { corner_radius(); }, {}, QStringLiteral("角を丸くします（0 で角ばる）"));
    make("act_no_border", QStringLiteral("選んだコマの枠線をなくす"), [this] { set_selected_frame(Json::object({{"border_mm", 0}})); });
    make("act_bleed", QStringLiteral("選んだコマを断ち切りにする（紙の端まで）"), [this] { toggle_bleed(); });
    for (const auto& [key, label] : {std::pair{"solid", "実線"}, {"double", "二重線"}, {"dashed", "破線"}, {"dotted", "点線"}, {"rough", "手描き風"}}) {
        auto* act = new QAction(QStringLiteral("枠線: %1").arg(QString::fromUtf8(label)), this);
        act->setObjectName(QStringLiteral("cmd:枠線: %1").arg(QString::fromUtf8(label)));
        const QString kind = QString::fromLatin1(key);
        connect(act, &QAction::triggered, this, [this, kind] { border_kind(kind); });
        addAction(act);
        border_kind_actions_.push_back(act);
    }
    make("act_border_colour", QStringLiteral("選んだコマの枠線の色…"), [this] { border_colour(); });
    QAction* numbers = make("act_frame_numbers", QStringLiteral("コマ番号（読み順）を表示"), [] {}, {},
                            QStringLiteral("コマの読み順を番号で見ます（印刷には出ません）"), true);
    connect(numbers, &QAction::triggered, this, [this](bool on) {
        canvas_->show_frame_numbers = on;
        canvas_->binding = book().binding;
        canvas_->update();
    });
    make("act_template", QStringLiteral("テンプレートでコマを割る…"), [this] { templates_dialog(); }, {},
         QStringLiteral("今のページのコマと台詞を作り直します"));
    make("act_save_template", QStringLiteral("今のコマ割りをテンプレートに残す…"), [this] { save_template(); }, {},
         QStringLiteral("このページのコマ割り（形・枠線・断ち切り・角の丸み）を、自分のテンプレートとして残します"));
    // pages
    make("act_add_page", QStringLiteral("ページを追加（この後ろに）"), [this] { add_page(); });
    make("act_del_page", QStringLiteral("このページを消す…"), [this] { del_page(); });
    make("act_dup_page", QStringLiteral("このページを複製"), [this] {
        if (const core::Page* page = current_page()) duplicate_page(static_cast<int>(core::py_int(page->index.json())));
    });
    make("act_page_up", QStringLiteral("このページを前へ"), [this] {
        const core::Page* page = current_page();
        if (page == nullptr) return;
        std::vector<int> order;
        for (const auto& p : book().pages) order.push_back(static_cast<int>(core::py_int(p->index.json())));
        const int i = page_index_;
        if (i <= 0) return;
        std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(i - 1)]);
        reorder_pages(order, static_cast<int>(core::py_int(page->index.json())));
    }, keys({QKeySequence(QStringLiteral("Ctrl+Shift+Up"))}));
    make("act_page_down", QStringLiteral("このページを後ろへ"), [this] {
        const core::Page* page = current_page();
        if (page == nullptr) return;
        std::vector<int> order;
        for (const auto& p : book().pages) order.push_back(static_cast<int>(core::py_int(p->index.json())));
        const int i = page_index_;
        if (i + 1 >= static_cast<int>(order.size())) return;
        std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(i + 1)]);
        reorder_pages(order, static_cast<int>(core::py_int(page->index.json())));
    }, keys({QKeySequence(QStringLiteral("Ctrl+Shift+Down"))}));
    make("act_name_ok", QStringLiteral("ネーム完了 → 作画へ進む"), [this] { name_ok(); }, {}, QStringLiteral("承認の要らない原稿（AI を使わない原稿）で使います"));
    // books and windows
    // (every key the system has for Close, not only the first as Python's QKeySequence(std.Close) gives: Ctrl+W closes
    // a book — with its question — on Windows and Linux too, where Ctrl+F4 comes first; SPEC SAVE-01, AC-SAVE 2)
    make("act_close", QStringLiteral("閉じる"), [this] { close_document(); }, QKeySequence::keyBindings(Std::Close),
         QStringLiteral("この原稿を閉じます（最後の原稿ならウィンドウも）"));
    make("act_new_window", QStringLiteral("新しいウィンドウ（同じ原稿）"), [this] { new_window(); }, {},
         QStringLiteral("この原稿をもう 1 つのウィンドウで開きます。拡大して描きながら、別の窓で全体を見る"));
    make("act_next_doc", QStringLiteral("次の原稿"), [this] { next_document(1); }, keys({QKeySequence(QStringLiteral("Ctrl+Tab"))}));
    make("act_prev_doc", QStringLiteral("前の原稿"), [this] { next_document(-1); }, keys({QKeySequence(QStringLiteral("Ctrl+Shift+Tab"))}));
    make("act_quit", QStringLiteral("Genko を終わる"), [] { QApplication::closeAllWindows(); }, keys({std_key(Std::Quit)}));
    // the tools' pictures and their tooltips with their keys
    for (const auto& [name, attribute] : {std::pair{"select", "act_select"}, {"pen", "act_pen"}, {"eraser", "act_eraser"}, {"frame", "act_frame"},
                                          {"picker", "act_picker"}, {"fill", "act_fill"}, {"lassofill", "act_lassofill"},
                                          {"gradient", "act_gradient"}, {"shape", "act_shape"}, {"blend", "act_blend"}, {"reshape", "act_reshape"},
                                          {"rect", "act_marquee"}, {"lasso", "act_lasso"}, {"wand", "act_wand"}, {"ruler", "act_ruler"},
                                          {"text", "act_text"},
                                          {"3d", "act_3d"}, {"effect", "act_effect"}, {"stamp", "act_stamp"},
                                          {"move", "act_move"}, {"undo", "act_undo"}, {"redo", "act_redo"}, {"fit", "act_fit"},
                                          {"zoom_in", "act_zoom_in"}, {"zoom_out", "act_zoom_out"}, {"prev", "act_prev"}, {"next", "act_next"}}) {
        QAction* act = actions_.at(QString::fromLatin1(attribute));
        act->setIcon(icons::icon(name));
        const QString shortcut = act->shortcut().toString();
        if (!shortcut.isEmpty() && std::any_of(tool_actions_.begin(), tool_actions_.end(), [act](const auto& t) { return t.second == act; })) {
            act->setToolTip(QStringLiteral("%1（%2）").arg(act->text(), shortcut) + (act->statusTip().isEmpty() ? QString() : QStringLiteral("\n") + act->statusTip()));
        }
    }
}

void MainWindow::build_menus() {
    QMenuBar* bar = menuBar();
    QMenu* file = bar->addMenu(QStringLiteral("ファイル"));
    file->addAction(action("act_new"));
    file->addAction(action("act_open"));
    recent_menu_ = file->addMenu(QStringLiteral("最近使った原稿"));
    connect(recent_menu_, &QMenu::aboutToShow, this, [this] {
        recent_menu_->clear();
        const auto paths = recent_projects();
        if (paths.empty()) {
            recent_menu_->addAction(QStringLiteral("（まだありません）"))->setEnabled(false);
            return;
        }
        for (std::size_t i = 0; i < paths.size() && i < 12; ++i) {
            const auto path = paths[i];
            QAction* item = recent_menu_->addAction(QString::fromStdString(core::path_to_utf8(path.stem())), this, [this, path] { open_project(path); });
            item->setToolTip(QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path))));  // (the whole path)
        }
    });
    file->addSeparator();
    file->addAction(action("act_save"));
    file->addAction(action("act_save_as"));
    file->addSeparator();
    file->addAction(action("act_import_scan"));
    file->addAction(action("act_scanner"));
    file->addSeparator();
    file->addAction(action("act_timelapse"));
    file->addAction(action("act_timelapse_export"));
    file->addSeparator();
    file->addAction(action("act_close"));
    file->addAction(action("act_quit"));
    QMenu* edit = bar->addMenu(QStringLiteral("編集"));
    edit->addAction(action("act_undo"));
    edit->addAction(action("act_redo"));
    edit->addSeparator();
    for (const char* name : {"act_cut", "act_copy", "act_paste", "act_delete_area"}) edit->addAction(action(name));
    edit->addSeparator();
    for (const char* name : {"act_select_all", "act_deselect"}) edit->addAction(action(name));
    QMenu* view = bar->addMenu(QStringLiteral("表示"));
    for (const char* name : {"act_fit", "act_zoom_in", "act_zoom_out", "act_actual", "act_zoom_value", "act_zoom_tool"}) view->addAction(action(name));
    view->addSeparator();
    for (const char* name : {"act_turn_left", "act_turn_right", "act_mirror", "act_view_flip_v", "act_turn_reset"}) view->addAction(action(name));
    view->addSeparator();
    for (const char* name : {"act_overview", "act_prev", "act_next"}) view->addAction(action(name));
    view->addSeparator();
    view->addAction(action("act_guides"));
    view->addAction(action("act_onion"));
    view->addAction(action("act_cmyk_proof"));
    view->addAction(action("act_screen_dots"));
    QMenu* tools = bar->addMenu(QStringLiteral("ツール"));
    // (Python's order; the tools not ported yet join it as they come)
    for (const char* name : {"act_select", "act_move", "act_pen", "act_eraser", "act_blend", "act_shape", "act_text", "act_frame"})
        tools->addAction(action(name));
    tools->addSeparator();
    for (const char* name : {"act_picker", "act_fill", "act_lassofill", "act_fill_gaps", "act_gradient", "act_reshape", "act_vector", "act_liquify"})
        tools->addAction(action(name));
    tools->addSeparator();
    for (const char* name : {"act_marquee", "act_lasso", "act_wand"}) tools->addAction(action(name));
    tools->addSeparator();
    for (const char* name : {"act_ruler", "act_3d", "act_effect", "act_stamp"}) tools->addAction(action(name));
    tools->addSeparator();
    for (const char* name : {"act_thicker", "act_thinner"}) tools->addAction(action(name));
    tools->addSeparator();
    for (const char* name : {"act_swap_colour", "act_transparent"}) tools->addAction(action(name));
    tools->addSeparator();
    QMenu* tones = tools->addMenu(QStringLiteral("トーン・効果線"));
    for (const char* name : {"act_tone_here", "act_tone_click"}) tones->addAction(action(name));
    tones->addSeparator();
    for (QAction* act : effect_actions_) tones->addAction(act);
    tones->addSeparator();
    for (const char* name : {"act_effect_within", "act_effect_avoid", "act_effect_clear"}) tones->addAction(action(name));
    tones->addSeparator();
    tones->addAction(action("act_materials"));
    tools->addAction(action("act_color"));
    tools->addAction(action("act_exposure"));
    tools->addAction(action("act_point_wider"));
    tools->addAction(action("act_point_thinner"));
    tools->addSeparator();
    build_guide_menus(tools);  // (定規 and 3D: main_window_guides.cpp)
    build_selection_menu(bar->addMenu(QStringLiteral("選択")));
    QMenu* layers = bar->addMenu(QStringLiteral("レイヤー"));
    for (const char* name : {"act_layer_pen", "act_layer_paint", "act_layer_folder"}) layers->addAction(action(name));
    layers->addSeparator();
    for (const char* name : {"act_layer_dup", "act_layer_merge", "act_layer_delete"}) layers->addAction(action(name));
    layers->addSeparator();
    for (const char* name : {"act_layer_up", "act_layer_down"}) layers->addAction(action(name));
    layers->addSeparator();
    layers->addAction(action("act_layer_draft"));
    layers->addSeparator();
    for (const char* name : {"act_layer_merge_down", "act_layer_merge_layers", "act_layer_merge_visible", "act_layer_flatten", "act_layer_convert_paint", "act_layer_convert_pen"})
        layers->addAction(action(name));
    layers->addSeparator();
    layers->addAction(action("act_plugins"));
    layers->addAction(action("act_plugin_settings"));
    QMenu* pages = bar->addMenu(QStringLiteral("ページ"));
    for (const char* name : {"act_add_page", "act_dup_page", "act_del_page"}) pages->addAction(action(name));
    pages->addSeparator();
    pages->addAction(action("act_page_up"));
    pages->addAction(action("act_page_down"));
    pages->addAction(action("act_nombre"));
    pages->addAction(action("act_timeline"));
    pages->addSeparator();
    QMenu* frames = pages->addMenu(QStringLiteral("コマ"));
    for (const char* name : {"act_split_h", "act_split_v", "act_merge", "act_delete_frame", "act_frame_selection"}) frames->addAction(action(name));
    frames->addSeparator();
    frames->addAction(action("act_template"));
    frames->addAction(action("act_save_template"));
    frames->addSeparator();
    for (const char* name : {"act_gutters", "act_border", "act_no_border"}) frames->addAction(action(name));
    for (QAction* act : border_kind_actions_) frames->addAction(act);
    frames->addAction(action("act_border_colour"));
    frames->addAction(action("act_corner"));
    frames->addAction(action("act_bleed"));
    frames->addAction(action("act_reset_shape"));
    frames->addSeparator();
    frames->addAction(action("act_frame_numbers"));
    build_line_menus(pages);  // (台詞, ストーリーエディター, 台詞の検索・置換: main_window_lines.cpp)
    pages->addSeparator();
    pages->addAction(action("act_name_ok"));
    view_menu_ = bar->addMenu(QStringLiteral("ウィンドウ"));
    view_menu_->addAction(action("act_new_window"));
    view_menu_->addAction(action("act_next_doc"));
    view_menu_->addAction(action("act_prev_doc"));
    view_menu_->addSeparator();
    QMenu* stage_menu = view_menu_->addMenu(QStringLiteral("作業の段階"));
    for (const Stage& stage : stages()) {
        const QString key = QString::fromLatin1(stage.key);
        QAction* act = stage_menu->addAction(QStringLiteral("%1の並び").arg(QString::fromUtf8(stage.label)), this, [this, key] { apply_stage(key); });
        act->setStatusTip(QStringLiteral("この段階でよく使うパネルだけを出します"));
        stage_actions_[key] = act;
    }
}

void MainWindow::build_toolbars() {
    palette_ = new QToolBar(QStringLiteral("道具"));
    palette_->setObjectName(QStringLiteral("tools"));
    palette_->setMovable(false);
    palette_->setOrientation(Qt::Vertical);
    palette_->setIconSize(QSize(24, 24));
    palette_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // (Python's palette: select, move, pen, eraser, blend, shape, fill, lassofill, gradient, picker | frame | the selection)
    for (const char* name : {"act_select", "act_move", "act_pen", "act_eraser", "act_blend", "act_shape", "act_fill", "act_lassofill", "act_gradient",
                             "act_picker"})
        palette_->addAction(action(name));
    palette_->addSeparator();
    palette_->addAction(action("act_text"));
    palette_->addAction(action("act_frame"));
    palette_->addSeparator();
    for (const char* name : {"act_marquee", "act_lasso", "act_wand", "act_reshape"}) palette_->addAction(action(name));
    palette_->addSeparator();
    for (const char* name : {"act_ruler", "act_3d", "act_effect"}) palette_->addAction(action(name));
    for (const auto& [name, label] : {std::pair{"act_marquee", "長方形選択"}, {"act_lasso", "投げ縄選択"}, {"act_reshape", "線の修正"}, {"act_3d", "3D"}})
        action(QString::fromLatin1(name))->setIconText(QString::fromUtf8(label));  // (the palette's names stay short; menus keep the full name)
    addToolBar(Qt::LeftToolBarArea, palette_);
    commands_ = new QToolBar(QStringLiteral("操作"));
    commands_->setObjectName(QStringLiteral("commands"));
    commands_->setMovable(false);
    commands_->setIconSize(QSize(20, 20));
    commands_->setToolButtonStyle(Qt::ToolButtonIconOnly);  // (the names are in the tooltips)
    // the open books' tabs on the left of the bar, the commands on the right
    commands_->addWidget(doc_tabs_);
    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    commands_->addWidget(spacer);
    for (const char* name : std::initializer_list<const char*>{"act_undo", "act_redo", nullptr, "act_fit", "act_zoom_in", "act_zoom_out", nullptr,
                                                               "act_prev", "act_next"}) {
        if (name == nullptr) {
            commands_->addSeparator();
        } else {
            commands_->addAction(action(name));
        }
    }
    addToolBar(commands_);
}

void MainWindow::build_docks() {
    build_tool_settings();  // (ツールの設定, on the left: main_window_brushes.cpp)
    navigator_ = new Navigator(canvas_);
    navigator_dock_ = new QDockWidget(QStringLiteral("全体図"), this);
    navigator_dock_->setObjectName(QStringLiteral("全体図"));
    navigator_dock_->setWidget(navigator_);
    navigator_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, navigator_dock_);
    splitDockWidget(tool_settings_dock_, navigator_dock_, Qt::Vertical);
    view_menu_->addAction(navigator_dock_->toggleViewAction());
    build_anim_dock();  // (タイムライン, a tab beside the navigator: main_window_anim.cpp)
    // サブビュー: the reference pictures, a tab beside the navigator too
    subview_ = new SubView(this);
    auto* sub_dock = new QDockWidget(QStringLiteral("サブビュー"), this);
    sub_dock->setObjectName(QStringLiteral("サブビュー"));
    sub_dock->setWidget(subview_);
    sub_dock->setFeatures(navigator_dock_->features());
    addDockWidget(Qt::LeftDockWidgetArea, sub_dock);
    tabifyDockWidget(navigator_dock_, sub_dock);
    navigator_dock_->raise();
    sub_dock->hide();  // (for some work only, as Python's: ウィンドウ brings it; the tabs fit a small screen without it)
    view_menu_->addAction(sub_dock->toggleViewAction());
    pages_dock_ = new QDockWidget(QStringLiteral("ページ"), this);
    pages_dock_->setObjectName(QStringLiteral("ページ"));
    pages_dock_->setWidget(pages_);
    pages_dock_->setMinimumWidth(170);
    pages_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, pages_dock_);
    view_menu_->addAction(pages_dock_->toggleViewAction());
    layer_panel_ = new LayerPanel(this);
    auto* layers = new QDockWidget(QStringLiteral("レイヤー"), this);
    layers->setObjectName(QStringLiteral("レイヤー"));
    // (a tall panel scrolls on a small screen instead of making the window taller; a tab beside the pages)
    auto* layer_scroll = new QScrollArea;
    layer_scroll->setWidgetResizable(true);
    layer_scroll->setFrameShape(QFrame::NoFrame);
    layer_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layer_scroll->setWidget(layer_panel_);
    layers->setWidget(layer_scroll);
    layers->setMinimumWidth(200);
    layers->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, layers);
    tabifyDockWidget(pages_dock_, layers);
    view_menu_->addAction(layers->toggleViewAction());
    build_line_dock();  // (台詞, a tab beside the pages and the layers: main_window_lines.cpp)
    pages_dock_->raise();
    build_colour_dock();  // (カラー, a tab beside the pages and the layers: main_window_paint.cpp)
    build_guide_dock();   // (定規・3D, a tab when it is opened: main_window_guides.cpp)
    build_material_dock();  // (素材, トーン, 効果線: main_window_materials.cpp)
    setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
}

// --- what the commands do ---------------------------------------------------------------------------------------------

void MainWindow::choose_tool(const QString& tool) {
    if (is_marquee_tool(tool)) {  // (範囲選択: the marquee tool, in one of its ways)
        choose_marquee(tool);
        if (marquee_mode_ != nullptr) marquee_mode_->setCurrentIndex(std::max(0, marquee_mode_->findData(tool)));
    } else {
        canvas_->set_tool(tool);
    }
    if (const auto it = tool_actions_.find(tool); it != tool_actions_.end()) it->second->setChecked(true);
    if (tool == QLatin1String("effect")) set_effect_pictures(effect_actions_, {"focus", "speed", "uni_flash", "beta_flash"});  // (drawn when first needed)
    if (tool_settings_ != nullptr) tool_settings_->show_tool(canvas_->tool());
    panel_for_tool(tool);  // (the lines for the text tool, the layers for the drawing tools: main_window_lines.cpp)
    pen_changed();
}

void MainWindow::undo() {
    try {
        perf::event("undo");
        session_->undo();
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000);
    }
    watch();
}

void MainWindow::redo() {
    try {
        perf::event("redo");
        session_->redo();
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000);
    }
    watch();
}

const core::Layer* MainWindow::paint_layer() {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return nullptr;
    if (!drawable(*layer)) {
        const QString why = layer->locked ? QStringLiteral("ロックされています") : QStringLiteral("ペンかペイントのレイヤーではありません");
        flash(QStringLiteral("「%1」には描けません（%2）。レイヤー パネルで選び直します").arg(wording::layer_label(*layer), why), 4000);
        return nullptr;
    }
    return layer;
}

void MainWindow::on_stroke(const StrokeInput& stroke) {
    const core::Page* page = current_page();
    if (page == nullptr || target_layer() == nullptr) {
        canvas_->stroke_dropped();
        return;
    }
    if (effect_shape_stroke(stroke, *page)) return;  // (this stroke shapes an effect, not ink: main_window_materials.cpp)
    if (text_path_stroke(stroke)) return;             // (or is the path a line's words follow: main_window_lines.cpp)
    if (balloon_eraser_stroke(stroke, *page)) return;  // (or cuts the balloons it went over: フキダシを削る)
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) {
        canvas_->stroke_dropped();
        return;
    }
    if (paint_stroke(stroke, *page, *layer)) return;  // (透明色, ゆがみ, 色混ぜ: main_window_paint.cpp)
    const bool rulers = canvas_->snap_rulers && page->rulers.is_array() && !page->rulers.empty();
    if (stroke.tool == QLatin1String("eraser")) {
        Json points = Json::array();
        for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({p.x, p.y}));
        Json op = Json::object({{"op", "erase"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"points", points}, {"width_mm", eraser_mm_}});
        const Json eraser = eraser_fields(*layer);
        for (const auto& [key, value] : eraser.items()) op[key] = value;
        if (rulers) op["snap_ruler"] = true;
        canvas_->stroke_dropped();
        apply_ops(Json::array({op}));
        return;
    }
    Json points = Json::array();
    for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({p.x, p.y, p.p.value_or(0.7)}));
    Json op = Json::object({{"op", "add_stroke"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"points", points}});
    const Json fields = pen_.stroke_fields();
    for (const auto& [key, value] : fields.items()) op[key] = value;
    if (stroke.rotation.size() == stroke.points.size() &&
        std::any_of(stroke.rotation.begin(), stroke.rotation.end(), [](double v) { return std::abs(v) > 0.5; })) {
        Json turns = Json::array();
        for (const double v : stroke.rotation) turns.push_back(core::py_round(v, 1));  // (a pen that reports its barrel turn)
        op["rotation"] = turns;
    }
    Json ops = Json::array({op});
    const std::string kind = op.value("kind", std::string());
    if (kind.starts_with("my_") && !book().brush_custom.contains(kind)) {
        // the book keeps the brush's settings, so the line looks the same on any computer
        render::brushes::Brush own = render::brushes::brush(kind);
        if (own.key != kind) {  // (not known: from the library again, never the G pen's settings under its name)
            try {
                render::brushes::register_brushes(render::brushes::load_library(config_dir()));
            } catch (const std::exception&) {
            }
            own = render::brushes::brush(kind);
        }
        if (own.key != kind) {
            flash(QStringLiteral("このブラシ（%1）が見つからないため描けませんでした。一覧から選び直してください").arg(QString::fromStdString(kind)), 5000, true);
            canvas_->stroke_dropped();
            return;
        }
        Json define{{"op", "define_brush"}, {"key", kind}};
        const Json settings_of = core::brush_to_dict(own);
        for (const auto& [key, value] : settings_of.items()) define[key] = value;
        ops.insert(ops.begin(), define);
    }
    if (rulers) ops.back()["snap_ruler"] = true;
    if (apply_ops(ops, {stroke.id})) {
        canvas_->stroke_applied(stroke.id);
        if (colours_ != nullptr) colours_->remember(brush_->rgb());  // (a colour just used: the front of 履歴)
    } else {
        canvas_->stroke_dropped();
    }
}

void MainWindow::on_frame_selected(const QString& frame_id) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    // (choosing goes through an op like every other change: no direct edits of the model)
    if (!(page->selected_frame_id.is_string() && page->selected_frame_id.get<std::string>() == frame_id.toStdString())) {
        apply_ops(Json::array({Json::object({{"op", "select_frame"}, {"page", page->index.json()}, {"frame_id", frame_id.toStdString()}})}));
    }
}

void MainWindow::context_menu(const QString& frame_id, const QPoint& global) {
    QMenu menu(this);
    if (!frame_id.isEmpty()) {
        for (const char* name : {"act_split_h", "act_split_v", "act_merge", "act_delete_frame", "act_frame_selection"}) menu.addAction(action(name));
        menu.addSeparator();
        QAction* ref = menu.addAction(QStringLiteral("AI 用の参照をコピー"));
        ref->setToolTip(QStringLiteral("このコマを AI に伝える言葉（ページ・読み順・AI の使う名前）をコピーします"));
        connect(ref, &QAction::triggered, this, [this, frame_id] {
            const core::Page* page = current_page();
            if (page == nullptr) return;
            const auto leaves = page->leaf_frames();
            int order = 0;
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                if (leaves[i]->id == frame_id.toStdString()) order = static_cast<int>(i) + 1;
            }
            const core::Frame* frame = page->find_frame(frame_id.toStdString());
            QString slot;
            if (frame != nullptr && frame->panel && frame->panel->is_object() && frame->panel->contains("slot")) slot = QString::fromStdString(core::py_str((*frame->panel)["slot"]));
            const QString index = QString::fromStdString(page->index.repr());
            const QString names = QStringLiteral("page %1, frame_id \"%2\"").arg(index, frame_id) + (slot.isEmpty() ? QString() : QStringLiteral(", slot \"%1\"").arg(slot));
            const QString words = QStringLiteral("%1 ページ目の %2 コマ目（読み順）［%3］").arg(index).arg(order).arg(names);
            QApplication::clipboard()->setText(words);
            flash(QStringLiteral("コピーしました: %1").arg(words), 4000);
        });
        menu.addSeparator();
    }
    menu.addAction(action("act_fit"));
    menu.exec(global);
}

double MainWindow::gutter_mm(const QString& cut) const {
    const double fallback = cut == QLatin1String("horizontal") ? 6.0 : 3.0;
    bool ok = false;
    const double value = settings()->value(QStringLiteral("gutter_%1").arg(cut), fallback).toDouble(&ok);
    return ok ? value : fallback;
}

void MainWindow::split(const QString& axis) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (!core::py_truthy(page->selected_frame_id)) {
        flash(QStringLiteral("先にコマをクリックして選びます（選択ツール）"), 6000);
        return;
    }
    apply_ops(Json::array({Json::object({{"op", "split_frame"},
                                         {"page", page->index.json()},
                                         {"axis", axis.toStdString()},
                                         {"frame_id", page->selected_frame_id},
                                         {"gutter_mm", gutter_mm(axis == QLatin1String("horizontal") ? QStringLiteral("horizontal") : QStringLiteral("vertical"))}})}));
}

void MainWindow::gutter_settings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("コマ間隔"));
    auto* form = new QFormLayout(&dialog);
    std::map<QString, QDoubleSpinBox*> spins;
    for (const auto& [key, label] : {std::pair{"horizontal", "上下の間隔（段と段の間）"}, {"vertical", "左右の間隔（横に並ぶコマの間）"}}) {
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(QString::fromLatin1(key));
        spin->setRange(0, 30);
        spin->setSingleStep(0.5);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setValue(gutter_mm(QString::fromLatin1(key)));
        form->addRow(QString::fromUtf8(label), spin);
        spins[QString::fromLatin1(key)] = spin;
    }
    auto* note = new QLabel(QStringLiteral("これから割るコマに使います。今ある間隔は、コマ ツール（F）で間の白をドラッグして変えます。"));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) == QDialog::Accepted) {
        const auto store = settings();
        for (const auto& [key, spin] : spins) store->setValue(QStringLiteral("gutter_%1").arg(key), spin->value());
    }
}

const core::Frame* MainWindow::selected_frame() const {
    const core::Page* page = current_page();
    if (page == nullptr || !page->selected_frame_id.is_string()) return nullptr;
    return page->find_frame(page->selected_frame_id.get<std::string>());
}

void MainWindow::set_selected_frame(const Json& change) {
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    Json op = Json::object({{"op", "set_frame"}, {"page", current_page()->index.json()}, {"frame_id", frame->id}});
    for (const auto& [key, value] : change.items()) op[key] = value;
    apply_ops(Json::array({op}));
}

bool MainWindow::modal_target_unchanged(const std::shared_ptr<Session>& origin, const DocPtr& snapshot, int page_index) {
    if (session_ == origin && origin->snapshot() == snapshot && page_index_ == page_index) return true;
    flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
    return false;
}

void MainWindow::border_width() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    if (const auto value = ask::get_double(this, QStringLiteral("枠線の太さ"), QStringLiteral("枠線の太さ（mm）"), frame->border_mm, 0.0, 5.0, 2)) {
        if (!modal_target_unchanged(origin, snapshot, page_index)) return;
        set_selected_frame(Json::object({{"border_mm", *value}}));
    }
}

void MainWindow::corner_radius() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    if (const auto value = ask::get_double(this, QStringLiteral("角の丸み"), QStringLiteral("角の丸み（半径 mm、0 で角ばる）"), frame->corner_mm, 0.0, 50.0, 1)) {
        if (!modal_target_unchanged(origin, snapshot, page_index)) return;
        set_selected_frame(Json::object({{"corner_mm", *value}}));
    }
}

void MainWindow::border_kind(const QString& kind) {
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    Json style = frame->line && frame->line->is_object() ? *frame->line : Json::object();
    style["kind"] = kind.toStdString();
    set_selected_frame(Json::object({{"line", style == Json::object({{"kind", "solid"}}) ? Json(nullptr) : style}}));
}

void MainWindow::nombre_dialog() {
    const auto origin=session_;const auto snapshot=origin->snapshot();const int page_index=page_index_;
    if (!current_page() || !origin->read_only_reason().empty()) return;
    Json cfg={{"position","bottom_center"},{"font","gothic"},{"size_mm",3.0},{"start",1},{"hidden",false},{"hidden_size_mm",2.0},{"show",true}};
    if (snapshot->nombre.is_object()) for (const auto& [key,value]:snapshot->nombre.items()) cfg[key]=value;
    double visible_size=3,hidden_size=2;
    try { visible_size=core::py_float(cfg["size_mm"]);hidden_size=core::py_float(cfg["hidden_size_mm"]); }
    catch (const std::exception&) {flash(QStringLiteral("ノンブルの値が不正です。原稿は変更していません。"),6000,true);return;}
    if(!std::isfinite(visible_size)||!std::isfinite(hidden_size)||visible_size<1||visible_size>20||hidden_size<1||hidden_size>10){flash(QStringLiteral("ノンブルは1〜20mm、隠しノンブルは1〜10mmです。原稿は変更していません。"),6000,true);return;}
    QDialog dialog(this);dialog.setObjectName("nombre_dialog");dialog.setWindowTitle(QStringLiteral("ノンブル（ページ番号）の設定"));
    auto* layout=new QVBoxLayout(&dialog);auto* note=new QLabel(QStringLiteral("書体・位置は原稿全体に適用します。ページ番号は校正・印刷に入り、ネーム表示には入りません。"));note->setWordWrap(true);layout->addWidget(note);
    auto* form=new QFormLayout;layout->addLayout(form);
    auto* enabled=new QCheckBox(QStringLiteral("このページにノンブルを入れる"));enabled->setObjectName("nombre_numero");enabled->setChecked(snapshot->page(static_cast<std::size_t>(page_index)).numero);form->addRow(enabled);
    auto* position=new QComboBox;position->setObjectName("nombre_position");
    for(const auto& [label,key]:{std::pair{"下・中央","bottom_center"},{"下・外側","bottom_outside"},{"上・外側","top_outside"},{"横・外側","side_outside"}})position->addItem(QString::fromUtf8(label),key);
    position->setCurrentIndex(std::max(0,position->findData(QString::fromStdString(core::py_str(cfg["position"])) )));const int pos_before=position->currentIndex();form->addRow(QStringLiteral("位置"),position);
    auto* font=new QComboBox;font->setObjectName("nombre_font");
    for(const auto& [label,key]:{std::pair{"アンチック","antique"},{"ゴシック","gothic"},{"明朝","mincho"},{"丸ゴシック","maru"},{"手書き","hand"},{"効果音","sfx"},{"ポップ","sfx_pop"}})font->addItem(QString::fromUtf8(label),key);
    font->setCurrentIndex(std::max(0,font->findData(QString::fromStdString(core::py_str(cfg["font"])) )));const int font_before=font->currentIndex();form->addRow(QStringLiteral("書体"),font);
    const auto size_box=[](const char* name,double value){auto* box=new QDoubleSpinBox;box->setObjectName(name);box->setDecimals(6);box->setRange(1,20);box->setSuffix(QStringLiteral(" mm"));box->setSingleStep(.1);box->setValue(value);return box;};
    auto* size=size_box("nombre_size",visible_size);auto* hidden_size_box=size_box("nombre_hidden_size",hidden_size);hidden_size_box->setMaximum(10);const double size_before=size->value(),hidden_size_before=hidden_size_box->value();
    form->addRow(QStringLiteral("大きさ"),size);
    auto* start=new QLineEdit(QString::fromStdString(core::py_str(cfg["start"])));start->setObjectName("nombre_start");const QString start_before=start->text();form->addRow(QStringLiteral("最初のページ番号"),start);
    auto* show=new QCheckBox(QStringLiteral("見えるノンブルを入れる"));show->setObjectName("nombre_show");show->setChecked(core::py_truthy(cfg["show"]));const bool show_before=show->isChecked();form->addRow(show);
    auto* hidden=new QCheckBox(QStringLiteral("隠しノンブルも入れる"));hidden->setObjectName("nombre_hidden");hidden->setChecked(core::py_truthy(cfg["hidden"]));const bool hidden_before=hidden->isChecked();form->addRow(hidden);form->addRow(QStringLiteral("隠しノンブルの大きさ"),hidden_size_box);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);
    dialog.resize(440,dialog.sizeHint().height());
    if(ask::exec(&dialog)!=QDialog::Accepted||!modal_target_unchanged(origin,snapshot,page_index))return;
    Json op={{"op","set_nombre"},{"page",snapshot->page(static_cast<std::size_t>(page_index)).index.json()},{"numero",enabled->isChecked()}};
    if(position->currentIndex()!=pos_before)op["position"]=position->currentData().toString().toStdString();
    if(font->currentIndex()!=font_before)op["font"]=font->currentData().toString().toStdString();
    if(size->value()!=size_before)op["size_mm"]=size->value();
    if(start->text()!=start_before)op["start"]=start->text().toStdString();
    if(show->isChecked()!=show_before)op["show"]=show->isChecked();
    if(hidden->isChecked()!=hidden_before)op["hidden"]=hidden->isChecked();
    if(hidden_size_box->value()!=hidden_size_before)op["hidden_size_mm"]=hidden_size_box->value();
    apply_ops(Json::array({op}));
}

void MainWindow::layer_operation(const std::string& operation) {
    const core::Page* current = current_page();
    if (!current || !session_->read_only_reason().empty()) return;
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Page page = *current;
    const bool pen = operation == "convert_pen";
    Json op = {{"op", pen ? "convert_layer" : operation}, {"page", page.index.json()}};
    if (operation == "merge_visible") {
        op["copy"] = true;
    } else if (operation == "flatten") {
        QMessageBox confirm(QMessageBox::Warning,QStringLiteral("画像をフラット化"),
            QStringLiteral("用紙色を背景に、表示中の作画を1つの不透明レイヤーへ結合します。\n非表示の作画レイヤーは破棄します（ネーム・下描き・コマ枠は保持）。\n操作は取り消せます。続けますか？"),QMessageBox::Yes|QMessageBox::No,this);
        confirm.setObjectName(QStringLiteral("flatten_layer_confirm"));confirm.setDefaultButton(QMessageBox::No);
        confirm.button(QMessageBox::Yes)->setText(QStringLiteral("フラット化"));confirm.button(QMessageBox::No)->setText(QStringLiteral("取消"));
        if (confirm.exec()!=QMessageBox::Yes) return;
        op["op"]="merge_visible";op["copy"]=false;op["flatten"]=true;
    } else {
        const bool multi = operation == "merge_layers";
        const int minimum = multi ? 2 : 1;
        QDialog dialog(this); dialog.setObjectName("merge_layer_dialog");
        dialog.setWindowTitle(multi ? QStringLiteral("結合するレイヤーを選ぶ") : QStringLiteral("対象レイヤーを選ぶ"));
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(multi ? QStringLiteral("2つ以上選びます。元に戻す操作で結合前に戻せます。") :
                                             QStringLiteral("操作するレイヤーを1つ選びます。"), &dialog));
        auto* choices = new QListWidget(&dialog); choices->setObjectName("merge_layer_choices");
        choices->setSelectionMode(multi ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
        for (const auto& layer : page.layers) {
            auto* item = new QListWidgetItem(wording::layer_label(layer), choices);
            item->setData(Qt::UserRole, QString::fromStdString(layer.id));
            item->setToolTip(QString::fromStdString(layer.id));
            if (layer.locked) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            else if (target_layer_id_ && *target_layer_id_ == layer.id) item->setSelected(true);
        }
        layout->addWidget(choices);
        QDoubleSpinBox* minimum_length=nullptr;
        if (pen) {
            layout->addWidget(new QLabel(QStringLiteral("短い印を除く最小の線長（mm）"),&dialog));
            minimum_length=new QDoubleSpinBox(&dialog);minimum_length->setObjectName("convert_pen_min_mm");
            minimum_length->setRange(0,100);minimum_length->setDecimals(2);minimum_length->setValue(.8);
            layout->addWidget(minimum_length);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText((operation == "convert_layer" || pen) ? QStringLiteral("変換") : QStringLiteral("結合"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(choices->selectedItems().size() >= minimum);
        connect(choices, &QListWidget::itemSelectionChanged, &dialog, [choices, buttons, minimum] {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(choices->selectedItems().size() >= minimum);
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        Json ids = Json::array();
        for (const auto* item : choices->selectedItems()) ids.push_back(item->data(Qt::UserRole).toString().toStdString());
        if (ids.size() < static_cast<std::size_t>(minimum)) return;
        if (multi) op["ids"] = std::move(ids);
        else op["id"] = ids.front();
        if (pen) {op["to"]="pen";op["min_mm"]=minimum_length->value();op["preserve_precision"]=true;}
        else if (operation == "convert_layer") op["to"] = "paint";
    }
    if (!modal_target_unchanged(origin, snapshot, page_index)) return;
    (void)apply_ops(Json::array({op}));
}

void MainWindow::exposure_dialog() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    if (!current_page() || !origin->read_only_reason().empty()) return;
    const core::Layer* target = target_layer();
    const bool editing = target && target->kind == core::LayerKind::Adjust && target->adjust && target->adjust->value("kind", Json()) == "exposure";
    const std::string id = editing ? target->id : std::string();
    core::Exposure now;
    try {
        if (editing) now = core::Exposure::parse(*target->adjust);
    } catch (const std::exception&) {
        flash(QStringLiteral("露光量の値が不正なため調整できません。原稿は変更していません。"), 8000);
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exposure_dialog"));
    dialog.setWindowTitle(editing ? QStringLiteral("露光量の調整") : QStringLiteral("露光量の調整層を追加"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QStringLiteral("元画像の画素は変更しません。調整層より下の色を補正します。"));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    auto* ev = new QDoubleSpinBox;
    ev->setObjectName(QStringLiteral("exposure_ev"));
    ev->setRange(-20, 20); ev->setDecimals(3); ev->setSingleStep(.1); ev->setValue(now.stops);
    auto* offset = new QDoubleSpinBox;
    offset->setObjectName(QStringLiteral("exposure_offset"));
    offset->setRange(-.5, .5); offset->setDecimals(3); offset->setSingleStep(.01); offset->setValue(now.offset);
    auto* gamma = new QDoubleSpinBox;
    gamma->setObjectName(QStringLiteral("exposure_gamma"));
    gamma->setRange(.01, 9.99); gamma->setDecimals(3); gamma->setSingleStep(.1); gamma->setValue(now.gamma);
    form->addRow(QStringLiteral("露光量（EV）"), ev);
    form->addRow(QStringLiteral("オフセット"), offset);
    form->addRow(QStringLiteral("ガンマ"), gamma);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? QStringLiteral("適用") : QStringLiteral("調整層を追加"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(400, dialog.sizeHint().height());
    if (ask::exec(&dialog) != QDialog::Accepted || !modal_target_unchanged(origin, snapshot, page_index)) return;
    const Json spec = {{"kind", "exposure"}, {"exposure", ev->value()}, {"offset", offset->value()}, {"gamma", gamma->value()}};
    Json op = {{"op", editing ? "set_layer" : "add_layer"},
               {"page", snapshot->page(static_cast<std::size_t>(page_index)).index.json()}, {"adjust", spec}};
    if (editing) op["id"] = id;
    else { op["layer"] = "finish"; op["kind"] = "adjust"; op["title"] = "露光量"; }
    apply_ops(Json::array({op}));
}

void MainWindow::border_colour() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    QColor now(20, 20, 20);
    if (frame->line && frame->line->is_object() && frame->line->contains("rgb") && core::py_truthy((*frame->line)["rgb"])) {
        const Json& rgb = (*frame->line)["rgb"];
        now = QColor(static_cast<int>(core::py_int(rgb[0])), static_cast<int>(core::py_int(rgb[1])), static_cast<int>(core::py_int(rgb[2])));
    }
    const auto colour = ask::colour(this, now, QStringLiteral("枠線の色"));
    if (!colour) return;
    if (!modal_target_unchanged(origin, snapshot, page_index)) return;
    Json style = frame->line && frame->line->is_object() && !frame->line->empty() ? *frame->line : Json::object({{"kind", "solid"}});
    style["rgb"] = Json::array({colour->red(), colour->green(), colour->blue()});
    set_selected_frame(Json::object({{"line", style}}));
}

void MainWindow::toggle_bleed() {
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) {
        flash(QStringLiteral("先にコマをクリックして選びます"), 6000);
        return;
    }
    const bool was = frame->bleed;
    set_selected_frame(Json::object({{"bleed", !was}}));
    flash(!was ? QStringLiteral("断ち切りにしました（紙の端に接する辺は枠線なし）") : QStringLiteral("断ち切りをやめました"));
}

void MainWindow::frame_to_selection() {
    const core::Page* page = current_page();
    if (page == nullptr || !core::py_truthy(page->selected_frame_id)) {
        flash(QStringLiteral("先にコマをクリックして選びます（コマツールか選択ツール）"), 6000);
        return;
    }
    const core::Frame* frame = selected_frame();
    if (frame == nullptr) return;
    core::Json poly = core::Json::array();
    for (const QPointF& p : outline_of(*frame)) poly.push_back(core::Json::array({core::py_round(p.x(), 3), core::py_round(p.y(), 3)}));
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    canvas_->set_selection(core::Json{{"poly", poly}});
    flash(QStringLiteral("コマの形を選択範囲にしました"), 3000);
}

void MainWindow::templates_dialog() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Page* page = current_page();
    if (page == nullptr) return;
    TemplateDialog dialog(this, session_->snapshot(), static_cast<std::size_t>(page_index_), session_->actor());
    if (ask::exec(&dialog) != QDialog::Accepted || !dialog.plan) return;
    if (!modal_target_unchanged(origin, snapshot, page_index)) return;
    if (dialog.needs_clearing &&
        !ask::question(this, QStringLiteral("Genko"),
                       QStringLiteral("%1 ページのコマと台詞を消して、テンプレートで割り直します。\n（元に戻す で取り消せます）").arg(QString::fromStdString(page->index.repr())))) {
        return;
    }
    if (!modal_target_unchanged(origin, snapshot, page_index)) return;
    apply_ops(dialog.plan->ops, dialog.plan->ids);
}

void MainWindow::save_template() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const QString suggested = QStringLiteral("%1 %2 ページ").arg(book().title.empty() ? QStringLiteral("無題") : QString::fromStdString(book().title),
                                                              QString::fromStdString(page->index.repr()));
    const auto typed = ask::get_text(this, QStringLiteral("テンプレートに残す"), QStringLiteral("テンプレートの名前"), suggested);
    if (!typed) return;
    if (!modal_target_unchanged(origin, snapshot, page_index)) return;
    const QString name = typed->trimmed();
    if (name.isEmpty()) return;
    const auto existing = templates::mine();
    if (std::any_of(existing.begin(), existing.end(), [&](const templates::Template& t) { return QString::fromStdString(t.key) == name; }) &&
        !ask::question(this, QStringLiteral("Genko"), QStringLiteral("「%1」はもうあります。置き換えますか？").arg(name))) {
        return;
    }
    try {
        if (!modal_target_unchanged(origin, snapshot, page_index)) return;
        templates::save_mine(name, *page);
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    flash(QStringLiteral("コマ割りを「%1」として残しました（テンプレートでコマを割る… の最初に出ます）").arg(name), 6000);
}

void MainWindow::add_page() {
    const core::Page* page = current_page();
    if (page == nullptr) {
        if (apply_ops(Json::array({Json::object({{"op", "add_page"}})}))) {
            page_index_ = static_cast<int>(book().pages.size()) - 1;
            reload_pages();
        }
        return;
    }
    const int index = static_cast<int>(core::py_int(page->index.json()));
    if (apply_ops(Json::array({Json::object({{"op", "add_page"}, {"count", 1}, {"after", index}})}))) {
        page_index_ = index;  // the new page
        reload_pages();
    }
}

void MainWindow::del_page() {
    const auto origin = session_;
    const auto snapshot = origin->snapshot();
    const int page_index = page_index_;
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const QString extra = page->name_ok ? QStringLiteral("\nこのページのネームは承認済みです。") : QString();
    if (ask::question(this, QStringLiteral("Genko"),
                      QStringLiteral("%1 ページを消しますか？%2\n（元に戻す で取り消せます）").arg(QString::fromStdString(page->index.repr()), extra))) {
        if (!modal_target_unchanged(origin, snapshot, page_index)) return;
        apply_ops(Json::array({Json::object({{"op", "delete_page"}, {"page", page->index.json()}})}));
    }
}

void MainWindow::duplicate_page(int index) {
    if (apply_ops(Json::array({Json::object({{"op", "duplicate_page"}, {"page", index}, {"next_to", true}})}))) {
        page_index_ = index;
        reload_pages();
    }
}

void MainWindow::reorder_pages(const std::vector<int>& order, int follow) {
    if (apply_ops(Json::array({Json::object({{"op", "reorder"}, {"order", Json(order)}})}))) {
        const auto it = std::find(order.begin(), order.end(), follow);
        page_index_ = it == order.end() ? 0 : static_cast<int>(it - order.begin());
    }
    reload_pages();
}

void MainWindow::page_overview() {
    auto* overview = new PageOverview(this, session_->snapshot(), current_page() != nullptr ? static_cast<int>(core::py_int(current_page()->index.json())) : 1);
    overview->setAttribute(Qt::WA_DeleteOnClose);
    connect(overview, &PageOverview::pageChosen, this, &MainWindow::go_to_page);
    overview->show();
}

void MainWindow::name_ok() {
    if (const core::Page* page = current_page()) apply_ops(Json::array({Json::object({{"op", "name_ok"}, {"page", page->index.json()}})}));
}

void MainWindow::point_width(double factor) {
    // 制御点ごとの線幅: the chosen control point wider (the vector tool that chooses it comes with M3)
    const std::vector<std::string> ids = canvas_->vector_ids;
    const std::optional<int> point = canvas_->vector_point;
    const core::Layer* layer = paint_layer();
    if (ids.empty() || !point || layer == nullptr) {
        flash(QStringLiteral("先に「線の編集」で線を選び、□（制御点）をクリックします"), 4000);
        return;
    }
    const core::Stroke* stroke = nullptr;
    for (const auto& s : layer->strokes->items) {
        if (s->id == ids.front()) {
            stroke = s.get();
            break;
        }
    }
    if (stroke == nullptr || *point < 0) return;
    const auto at = static_cast<std::size_t>(*point);
    if (stroke->pressure.size() == stroke->points.size() && at >= stroke->pressure.size()) return;  // (no such point)
    const double now = stroke->pressure.size() == stroke->points.size() ? stroke->pressure[at] : 0.7;
    const Json change = Json::object({{"action", "set_pressure"},
                                      {"stroke_id", ids.front()},
                                      {"index", *point},
                                      {"pressure", core::py_round(core::py_max(0.05, core::py_min(1.5, now * factor)), 3)}});
    // (_vector_edit)
    const core::Layer* target = paint_layer();
    const core::Page* page = current_page();
    if (target == nullptr || page == nullptr) return;
    Json op = Json::object({{"op", "vector_edit"}, {"page", page->index.json()}, {"layer_id", target->id}});
    for (const auto& [key, value] : change.items()) op[key] = value;
    apply_ops(Json::array({op}));
    canvas_->update();
}

void MainWindow::pick_colour() {
    const QColor now(static_cast<int>(pen_.rgb[0]), static_cast<int>(pen_.rgb[1]), static_cast<int>(pen_.rgb[2]));
    if (const auto colour = ask::colour(this, now, QStringLiteral("色"))) brush_->set_colour({colour->red(), colour->green(), colour->blue()});
}

void MainWindow::ask_zoom() {
    if (const auto value = ask::get_int(this, QStringLiteral("表示倍率"), QStringLiteral("倍率（%。100 で紙の大きさ）"), canvas_->zoom_percent(), 5, 6400)) {
        const int percent = *value;
        canvas_->glide([this, percent] { canvas_->set_zoom_percent(percent); });
    }
}

void MainWindow::apply_stage(const QString& key) {
    // show the panels a stage needs (in front, the ones used most), hide the rest
    const auto it = std::find_if(stages().begin(), stages().end(), [&](const Stage& s) { return key == QLatin1String(s.key); });
    if (it == stages().end()) return;
    const auto docks = findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget* dock : docks) {
        const bool wanted = std::any_of(it->show.begin(), it->show.end(), [&](const char* t) { return dock->windowTitle() == QString::fromUtf8(t); });
        dock->setVisible(wanted);
    }
    for (auto front = it->front.rbegin(); front != it->front.rend(); ++front) {
        for (QDockWidget* dock : docks) {
            if (dock->windowTitle() == QString::fromUtf8(*front) && dock->isVisible()) dock->raise();
        }
    }
    settings()->setValue(QStringLiteral("ui/stage"), key);
    flash(QStringLiteral("「%1」の並びにしました（ウィンドウ → 作業の段階）").arg(QString::fromUtf8(it->label)), 2500);
}

void MainWindow::delete_frame() {
    const core::Page* page = current_page();
    if (page == nullptr || !core::py_truthy(page->selected_frame_id)) {
        flash(QStringLiteral("先にコマをクリックして選びます（コマツールか選択ツール）"), 6000);
        return;
    }
    apply_ops(Json::array({Json::object({{"op", "delete_frame"}, {"page", page->index.json()}, {"frame_id", page->selected_frame_id}})}));
}

void MainWindow::merge() {
    const core::Page* page = current_page();
    if (page == nullptr || !core::py_truthy(page->selected_frame_id)) {
        flash(QStringLiteral("先にコマをクリックして選びます（選択ツール）"), 6000);
        return;
    }
    apply_ops(Json::array({Json::object({{"op", "merge_frame"}, {"page", page->index.json()}, {"frame_id", page->selected_frame_id}})}));
}

void MainWindow::cut_frame(const QString& frame_id, const QPointF& p0, const QPointF& p1) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const bool horizontal = std::abs(p1.x() - p0.x()) >= std::abs(p1.y() - p0.y());
    apply_ops(Json::array({Json::object({{"op", "cut_frame"},
                                         {"page", page->index.json()},
                                         {"frame_id", frame_id.toStdString()},
                                         {"p0", point_json(p0)},
                                         {"p1", point_json(p1)},
                                         {"gutter_mm", gutter_mm(horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"))}})}));
}

void MainWindow::frame_drawn(const QVector<QPointF>& points) {
    // a panel drawn with the panel tool (長方形・折れ線・フリーハンド)
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const bool rect = points.size() == 4 && points[0].y() == points[1].y() && points[1].x() == points[2].x();
    Json op = Json::object({{"op", "add_frame"}, {"page", page->index.json()}});
    if (rect) {
        op["rect"] = Json::array({points[0].x(), points[0].y(), core::py_round(points[2].x() - points[0].x(), 2), core::py_round(points[2].y() - points[0].y(), 2)});
    } else {
        Json list = Json::array();
        for (const QPointF& p : points) list.push_back(Json::array({p.x(), p.y()}));
        op["points"] = list;
    }
    const bool first = !(!page->frames.empty() && page->frames[0].split_axis && *page->frames[0].split_axis == "free");
    if (apply_ops(Json::array({op})) && first) {
        flash(QStringLiteral("コマを描きました。最初に描いたコマは、元の基本枠と入れ替わります（元に戻す: Ctrl+Z）"), 6000);
    }
}

void MainWindow::layer_move_started() {
    // the picture of the layer being moved, for the canvas to carry under the pen (drawn on a worker thread)
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    const int dpi = std::max(24, std::min(150, static_cast<int>(std::nearbyint(canvas_->scale() * 25.4))));
    const auto doc = session_->snapshot();
    const std::size_t index = static_cast<std::size_t>(page_index_);
    const std::string layer_id = layer->id;
    const std::uint64_t ticket = ++move_ticket_;
    auto* watcher = new QFutureWatcher<std::pair<QImage, QRectF>>(this);
    connect(watcher, &QFutureWatcher<std::pair<QImage, QRectF>>::finished, this, [this, watcher, ticket] {
        const auto [image, where] = watcher->result();
        watcher->deleteLater();
        if (ticket == move_ticket_ && !image.isNull()) canvas_->set_move_image(image, where);
    });
    watcher->setFuture(QtConcurrent::run([doc, index, layer_id, dpi]() -> std::pair<QImage, QRectF> {
        try {
            const core::Page& p = doc->page(index);
            for (const core::Layer& l : p.layers) {
                if (l.id != layer_id) continue;
                const render::Image drawn = render::layer_image(p, l, dpi, doc.get(), true);
                const render::Image image = drawn.mode() == "RGBA" ? drawn : drawn.convert("RGBA");
                const auto box = image.getbbox();
                if (!box) return {};
                const render::Image piece = image.crop(*box);
                const std::string bytes = piece.tobytes();
                const QImage q = QImage(reinterpret_cast<const uchar*>(bytes.data()), piece.width(), piece.height(), piece.width() * 4,
                                        QImage::Format_RGBA8888)
                                     .copy();
                const double mm = 25.4 / dpi;
                return {q, QRectF(box->x0 * mm, box->y0 * mm, piece.width() * mm, piece.height() * mm)};
            }
        } catch (const std::exception&) {
        }
        return {};
    }));
}

void MainWindow::layer_moved(double dx, double dy) {
    const core::Page* page = current_page();
    const core::Layer* layer = paint_layer();
    if (page == nullptr || layer == nullptr) return;
    // (Python's w + m: an int paper size and the float margin make a float)
    const core::Num m = 30.0;  // everything on the layer, and a little beyond the paper
    const core::Num w = page->spec.width_mm;
    const core::Num h = page->spec.height_mm;
    const Json whole = Json::object({{"poly", Json::array({Json::array({(-m).json(), (-m).json()}), Json::array({(w + m).json(), (-m).json()}),
                                                           Json::array({(w + m).json(), (h + m).json()}), Json::array({(-m).json(), (h + m).json()})})}});
    apply_ops(Json::array({Json::object({{"op", "transform_area"},
                                         {"page", page->index.json()},
                                         {"layer_id", layer->id},
                                         {"area", whole},
                                         {"matrix", Json::array({1, 0, 0, 1, dx, dy})}})}));
}

}  // namespace genko::app
