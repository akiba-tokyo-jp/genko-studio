// The window's commands (M2-G1 試験 1): the 52 actions of M2 and their three groups (docs/cpp-migration/ledger.json,
// kinds gui_action and gui_action_group) are in the menus with Python's words, keys and tips (genko/app/main.py
// MainWindow._build_actions), and each does what it says — a change to the book (checked in the book afterwards), to
// the view or the tool, or the dialog it opens. Every action is triggered at least once (checked at the end).

#include <QtTest>

#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QKeySequenceEdit>
#include <QTabWidget>
#include <QToolBar>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>

#include <map>
#include <memory>
#include <set>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/navigator.hpp"
#include "app/open_dialog.hpp"
#include "app/pages_panel.hpp"
#include "app/preferences.hpp"
#include "app/templates.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/key_sets.hpp"
#include "app/settings_watch.hpp"
#include "app/workspace.hpp"
#include "core/frames.hpp"
#include "core/paths.hpp"
#include "gui_support.hpp"
#include "render/ops_registry.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::SaveKind;
using genko::app::Session;
namespace inject = genko::app::inject;
namespace core = genko::core;

namespace {

using Std = QKeySequence::StandardKey;

struct Expected {
    const char* attribute;
    const char* text;
    QList<QKeySequence> keys;
    const char* tip;
    bool checkable;
    const char* menu;  // where it is: "ページ/コマ" is the コマ submenu of ページ
};

// (each key once — on Windows the standard Redo is Ctrl+Y itself, and a key bound twice would never fire —, and none
// where the system has no standard one: Quit on Windows)
QList<QKeySequence> k(std::initializer_list<QKeySequence> keys) {
    QList<QKeySequence> unique;
    for (const QKeySequence& key : keys) {
        if (!key.isEmpty() && !unique.contains(key)) unique << key;
    }
    return unique;
}
QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// Python's words, keys and tips (main.py _build_actions), and the menu each is in — with PC5 H-0's Photoshop names
// (節 H 1.2, the user's decision 12) and the Photoshop key set, the default since (the user's decision 8: key_sets; the
// Genko set's keys — Python's — in theKeySets).
const std::vector<Expected>& expected() {
    static const std::vector<Expected> list = {
        // (PC4 F-0, 節 F 5.1: ファイル's commands by Photoshop's names — 新規…, 別名で保存…, 終了)
        {"act_new", "新規…", k({QKeySequence(Std::New)}), "", false, "ファイル"},
        {"act_open", "開く…", k({QKeySequence(Std::Open)}), "", false, "ファイル"},
        {"act_save", "保存", k({QKeySequence(Std::Save)}), "変更は自動で保存されます。今すぐ書き込むときに使います", false, "ファイル"},
        {"act_save_as", "別名で保存…", k({QKeySequence(Std::SaveAs)}), "", false, "ファイル"},
        {"act_undo", "取り消し", k({QKeySequence(Std::Undo)}), "", false, "編集"},
        {"act_redo", "やり直し", k({s("Ctrl+Shift+Z")}), "", false, "編集"},
        {"act_fit", "画面サイズに合わせる", k({s("Ctrl+0")}), "", false, "表示"},
        {"act_zoom_in", "ズームイン", k({QKeySequence(Std::ZoomIn), s("Ctrl+=")}), "", false, "表示"},
        {"act_zoom_out", "ズームアウト", k({QKeySequence(Std::ZoomOut)}), "", false, "表示"},
        {"act_actual", "プリントサイズ", {}, "", false, "表示"},
        {"act_turn_left", "左に回す（15°）", k({s("-"), s("Ctrl+Alt+Left")}),
         "表示だけを回します（原稿は回りません）。Shift＋スペースを押しながらドラッグでも回せます", false, "表示"},
        {"act_turn_right", "右に回す（15°）", k({s("^"), s("Ctrl+Alt+Right")}), "表示だけを回します（原稿は回りません）", false, "表示"},
        {"act_turn_reset", "回転・反転を戻す", k({s("Ctrl+Alt+0")}), "", false, "表示"},
        {"act_zoom_tool", "ズームツール", k({s("Z")}), "クリックで拡大、Alt＋クリックで縮小、ドラッグで囲んだ所を画面いっぱいに", true, "表示"},
        {"act_zoom_value", "表示倍率を打ち込む…", {}, "倍率（%）を数で決めます。ステータスバーの倍率でも", false, "表示"},
        {"act_mirror", "水平方向に反転", {}, "表示だけを左右反転します（絵の歪みを見つける）。原稿は変わりません", true, "表示"},
        {"act_overview", "ページを並べて見る", k({s("Ctrl+Shift+O")}), "全ページを縮小図で並べ、ダブルクリックで開きます", false, "表示"},
        {"act_prev", "◀ 前のページ", k({QKeySequence(Std::MoveToPreviousPage), s("Ctrl+Left")}), "", false, "表示"},
        {"act_next", "次のページ ▶", k({QKeySequence(Std::MoveToNextPage), s("Ctrl+Right")}), "", false, "表示"},
        {"act_guides", "仕上がり線・基本枠を表示", {}, "断ち切り（裁ち落とし）・仕上がり線・基本枠", true, "表示"},
        // (PC1b, SPEC COLOR-03: Python's 「CMYK で見る（色校正）」 is now 色の校正 — View → Proof Colors — on Ctrl+Alt+Y
        // (Ctrl+Y is やり直す); it shows what 校正の設定 chooses, the working CMYK unless another: the user's decision
        // (working CMYK coated-cmyk), Python's own conversion kept there as 書き出しの CMYK)
        {"act_cmyk_proof", "色の校正", k({s("Ctrl+Y")}), "印刷したときの色など、校正の設定で選んだ見え方で表示します（表示だけ。原稿は変わりません）", true, "表示"},
        {"act_select", "選択", k({s("Shift+N")}), "コマを選ぶ・フキダシを動かす・ドラッグで表示を動かす", true, "ツール"},
        {"act_pen", "ブラシツール", k({s("B")}), "レイヤー パネルで選んだレイヤーに描きます", true, "ツール"},
        {"act_eraser", "消しゴムツール", k({s("E")}), "ペンの線は触れた所で切れます", true, "ツール"},
        {"act_frame", "コマ割り", k({s("K")}),
         "コマの中をドラッグして割る（斜めも。水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで間隔を動かす・選んだコマの角をドラッグで形を変える", true,
         "ツール"},
        // (PC3 C-d, decision 5's 移動ツール: the layers chosen and their linked ones, Ctrl+click picking a layer)
        {"act_move", "移動ツール", k({s("V")}), "選んだレイヤーをリンクしたレイヤーと一緒にドラッグで動かす（Shift で縦・横・45°、Ctrl+クリックでそこのレイヤーを選ぶ）", true, "ツール"},
        {"act_point_wider", "選んだ点を太く", k({s("Ctrl+Alt+]")}), "部分選択ツールで選んだ制御点のところだけ、線を太くします", false, "ツール"},  // (PC3 C-b)
        {"act_color", "カラーピッカー（描画色）…", {}, "", false, "ツール"},  // (PC4 E-0, decision 2)
        {"act_split_h", "コマを横に割る（上下に分ける）", {}, "", false, "ページ/コマ"},
        {"act_split_v", "コマを縦に割る（左右に分ける）", {}, "", false, "ページ/コマ"},
        {"act_merge", "コマを結合（割る前に戻す）", k({s("Ctrl+Shift+M")}), "", false, "ページ/コマ"},
        {"act_delete_frame", "このコマを消す（ほかのコマはそのまま）", {}, "", false, "ページ/コマ"},
        {"act_frame_selection", "このコマを選択範囲にする", {}, "選んだコマの形を選択範囲にします（塗りつぶし・トーン・消去をコマの中だけに）", false, "ページ/コマ"},
        {"act_gutters", "コマ間隔の設定…", {}, "新しく割るときの上下・左右の間隔", false, "ページ/コマ"},
        {"act_border", "選んだコマの枠線の太さ…", {}, "", false, "ページ/コマ"},
        {"act_corner", "選んだコマの角の丸み…", {}, "角を丸くします（0 で角ばる）", false, "ページ/コマ"},
        {"act_no_border", "選んだコマの枠線をなくす", {}, "", false, "ページ/コマ"},
        {"act_bleed", "選んだコマを断ち切りにする（紙の端まで）", {}, "", false, "ページ/コマ"},
        {"act_border_colour", "選んだコマの枠線の色…", {}, "", false, "ページ/コマ"},
        {"act_frame_numbers", "コマ番号（読み順）を表示", {}, "コマの読み順を番号で見ます（印刷には出ません）", true, "ページ/コマ"},
        {"act_template", "テンプレートでコマを割る…", {}, "今のページのコマと台詞を作り直します", false, "ページ/コマ"},
        {"act_save_template", "今のコマ割りをテンプレートに残す…", {},
         "このページのコマ割り（形・枠線・断ち切り・角の丸み）を、自分のテンプレートとして残します", false, "ページ/コマ"},
        {"act_add_page", "ページを追加（この後ろに）", {}, "", false, "ページ"},
        {"act_del_page", "このページを消す…", {}, "", false, "ページ"},
        {"act_dup_page", "このページを複製", {}, "", false, "ページ"},
        {"act_page_up", "このページを前へ", k({s("Ctrl+Shift+Up")}), "", false, "ページ"},
        {"act_page_down", "このページを後ろへ", k({s("Ctrl+Shift+Down")}), "", false, "ページ"},
        {"act_name_ok", "ネーム完了 → 作画へ進む", {}, "承認の要らない原稿（AI を使わない原稿）で使います", false, "ページ"},
        // (every key the system has for Close, Ctrl+W among them: Python binds only the first, Ctrl+F4 on Windows)
        {"act_close", "閉じる", QKeySequence::keyBindings(Std::Close), "この原稿を閉じます（最後の原稿ならウィンドウも）", false, "ファイル"},
        {"act_new_window", "新規ウィンドウ", {}, "この原稿をもう 1 つのウィンドウで開きます。拡大して描きながら、別の窓で全体を見る", false,
         "ウィンドウ/アレンジ"},
        {"act_next_doc", "次の原稿", k({s("Ctrl+Tab")}), "", false, "ウィンドウ"},
        {"act_prev_doc", "前の原稿", k({s("Ctrl+Shift+Tab")}), "", false, "ウィンドウ"},
        {"act_quit", "終了", k({QKeySequence(Std::Quit)}), "", false, "ファイル"},
    };
    return list;
}

// Every menu path an action is in ("ページ/コマ").
void find_in(QMenu* menu, const QString& path, QAction* wanted, QStringList& found) {
    for (QAction* a : menu->actions()) {
        if (a == wanted) found << path;
        if (a->menu() != nullptr) find_in(a->menu(), path + QStringLiteral("/") + a->menu()->title(), wanted, found);
    }
}

QStringList menus_of(MainWindow* window, QAction* action) {
    QStringList found;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr) find_in(top->menu(), top->menu()->title(), action, found);
    }
    return found;
}

std::set<QString>& fired() {
    static std::set<QString> names;
    return names;
}

// A book of three pages in a window (the session saves 50 ms after a change), the questions answered by the test.
struct Studio {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;
    std::vector<Json> ops;

    explicit Studio(int pages = 3, bool agent = false) {
        book = path_of(dir.filePath("book.genko"));
        core::Document doc = new_doc(pages);
        doc.strict_gates = agent;
        write_book(book, doc);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        for (const auto& [name, action] : window->actions_by_name()) {
            const QString attribute = name;
            QObject::connect(action, &QAction::triggered, window.get(), [attribute] { fired().insert(attribute); });
        }
        watch(session.get());
    }
    ~Studio() {
        window.reset();
        session.reset();
        answers.reset();
    }
    void watch(Session* s) {
        QObject::connect(s, &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    void trigger(const char* name) const {
        QAction* a = act(name);
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    const core::Document& doc() const { return window->session().document(); }
    const core::Page& page() const { return *window->current_page(); }
    PageCanvas* canvas() const { return window->canvas(); }
    QPointF centre_of(const core::Frame& frame) const {
        return QPointF(frame.rect.x.value() + frame.rect.width.value() / 2, frame.rect.y.value() + frame.rect.height.value() / 2);
    }
    // Choose a panel the way a person does: the select tool, a click inside it.
    void choose(const core::Frame& frame) {
        window->choose_tool(QStringLiteral("select"));
        canvas()->fit_page();
        const std::string id = frame.id;
        inject::click_mm(canvas(), centre_of(frame));
        QVERIFY(page().selected_frame_id.is_string());
        QCOMPARE(page().selected_frame_id.get<std::string>(), id);
    }
    std::size_t leaves() const { return page().leaf_frames().size(); }
    const core::Frame& selected() const { return *page().find_frame(page().selected_frame_id.get<std::string>()); }
};

}  // namespace

class TestGuiActions : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    void cleanupTestCase() {
        // every one of the 53 was triggered by a test below
        QStringList missing;
        for (const Expected& e : expected()) {
            if (fired().count(QString::fromLatin1(e.attribute)) == 0) missing << QString::fromLatin1(e.attribute);
        }
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not triggered: ") + missing.join(QStringLiteral(", "))));
    }

    void theMenusHoldPythonsCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        QCOMPARE(expected().size(), std::size_t{53});
        // Preserve all 53 Python actions; exposure, nombre and the six layer operations (merge, flatten, convert) have
        // their own GUI E2E tests (test_gui_materials, test_gui_color), the 31 of the selection (M3) theirs
        // (test_gui_select), the 3 of animation and the timelapse and the 2 of the filter plugins (M3) theirs
        // (test_gui_anim), the 13 of the drawing tools and the colour and the 7 of editing lines (M3) theirs
        // (test_gui_paint), the 31 of the rulers and 3D theirs (test_gui_guides), the 23 of effect lines, tones, materials,
        // the view's extras, the layer commands and the scans theirs (test_gui_effects), the 8 of the lines of dialogue
        // (M4: act_text, act_line_type, act_balloon_pen, act_line_edit, act_line_delete, act_line_wrap, act_story_editor,
        // act_replace) theirs (test_gui_lines), the 7 of the book and its pages (M4②c: act_spread, act_paper,
        // act_page_nombre, act_add_cover, act_assignee, act_merge_book, act_import_psd; act_nombre, Python's now, was
        // here before) theirs (test_gui_book), the 6 of the book going out (M4③b: act_export, act_print, act_history,
        // act_phone, act_scale, act_checks) theirs (test_gui_output), the 2 of the studio (M5④: act_chat_approval,
        // act_style) theirs (test_gui_studio), the colour mode of pages (CMYK/Lab: act_color_mode) its own (test_gui_book), the
        // 9 of color-model@1 — ページ → 色モード (act_mode_rgb, act_mode_gray, act_mode_cmyk, act_mode_lab, act_depth_8,
        // act_depth_16, act_depth_32) and 編集 → プロファイルの指定… / プロファイルに変換… (act_assign_profile,
        // act_convert_profile) — theirs (test_gui_book), the 9 of channels@1 — the targets Ctrl+Alt+2…9 (act_channel_composite,
        // act_channel_3 … act_channel_9) and 選択範囲 → 選択範囲を保存… (act_sel_to_channel) — theirs (test_gui_channels,
        // test_gui_select), the
        // 17 of M6 — 環境設定 and the work (act_prefs, act_canvas_only, act_hints, act_tool_names, act_edit_commandbar,
        // act_edit_quick, act_save_workspace, act_next_steps, act_find_command, act_ai) theirs (test_gui_look,
        // test_gui_workspace, test_gui_studio), the help (act_help_keys, act_help_guide, act_help_faq, act_about), the
        // book preview, the border detail and the picture taken in (act_book_preview, act_border_detail, act_import) theirs
        // (test_gui_carried), and the 15 of PC1b (SPEC COLOR-03) — 編集 → カラー設定… (act_color_settings), 表示 → 色域外警告
        // (act_gamut_warning) and the 13 kinds of 表示 → 校正の設定 (act_proof_custom, act_proof_working_cmyk, act_proof_export,
        // act_proof_cyan, act_proof_magenta, act_proof_yellow, act_proof_black, act_proof_cmy, act_proof_srgb, act_proof_monitor,
        // act_proof_legacy_mac, act_proof_protan, act_proof_deutan) — theirs (test_gui_color), and the 5 of PC1d (SPEC COLOR-03)
        // — ページ → 色モード の 2 階調…・インデックスカラー…・ダブルトーン…・マルチチャンネル・カラーテーブル… (act_mode_bitmap,
        // act_mode_indexed, act_mode_duotone, act_mode_multichannel, act_color_table) — theirs (test_gui_book), and the 8 of
        // PC2c (SPEC COLOR-04) — レイヤー → 色調補正 の シャドウ・ハイライト…・HDR トーン…・色の置き換え…・色の一致…・画像操作…・
        // 演算…・トラッピング… and 編集 → フェード… (act_shadows_highlights, act_hdr_toning, act_replace_color, act_match_color,
        // act_apply_image, act_calculations, act_trap, act_fade) — theirs (test_gui_image_ops).
        // PC2b (SPEC COLOR-04) — レイヤー → 色調補正 の色の補正 (act_adjust_vibrance, act_adjust_hue_saturation,
        // act_adjust_color_balance, act_adjust_black_white, act_adjust_photo_filter, act_adjust_channel_mixer,
        // act_adjust_selective_color, act_adjust_desaturate) — theirs (test_gui_adjust_color).
        // and the 14 of PC2a (SPEC COLOR-04) — レイヤー → 色調補正 の 明るさ・コントラスト…・レベル補正…・トーンカーブ…・露光量…・
        // カラールックアップ…・階調の反転・階調化…・2 値化…・グラデーションマップ…・平均化・自動トーン補正・自動コントラスト・
        // 自動カラー補正 (Photoshop's names, the user's decision 8) and ファイル → カラールックアップテーブルを書き出す…
        // (act_adjust_brightness, act_adjust_levels, act_adjust_curves, act_adjust_exposure, act_adjust_lookup,
        // act_adjust_invert, act_adjust_posterize, act_adjust_threshold, act_adjust_gradient_map, act_adjust_equalize,
        // act_auto_tone, act_auto_contrast, act_auto_color, act_export_lookup) — theirs (test_gui_adjust).
        // and the 1 of PC2d (SPEC COLOR-04) — 編集 → プロファイルの情報… (act_profile_info) — its own (test_gui_info).
        // and the 41 of PC3 A0 (SPEC「スマートオブジェクト」) — ファイル → 埋め込みを配置…・リンクを配置…・パッケージ…
        // (act_place_embedded, act_place_linked, act_package), レイヤー → スマートオブジェクト (act_smart_convert,
        // act_smart_copy, act_smart_edit, act_smart_export, act_smart_replace, act_smart_relink, act_smart_embed,
        // act_smart_link, act_smart_update, act_smart_update_all, act_smart_reveal, the 12 of スタックモード
        // act_smart_stack_<mode>, act_smart_warp, act_smart_perspective_warp, act_smart_puppet_warp, act_smart_rasterize),
        // レイヤー → スマートフィルター (act_smart_filters_convert, act_smart_filters_off, act_smart_filter_mask_delete,
        // act_smart_filter_mask_off, act_smart_filters_clear), レイヤー → ラスタライズ (act_rasterize_type, act_rasterize_fill,
        // act_rasterize_smart, act_rasterize_layer, act_rasterize_all) and レイヤーの内容オプション… (act_layer_content_options)
        // — theirs (test_gui_smart; act_import stays test_gui_carried's).
        // and the 10 of PC3 B1 (SPEC COLOR-06) — 選択範囲 の 再選択・色域指定…・境界線…・滑らかに…・選択範囲を拡大・近似色を選択・
        // 選択範囲を変形・選択範囲を読み込む…, 表示 → 選択範囲の境界線 and ツール → マジック消しゴムツール (act_sel_reselect,
        // act_color_range, act_sel_border, act_sel_smooth, act_sel_grow_colour, act_sel_similar, act_sel_transform, act_sel_load,
        // act_sel_edges, act_magic_eraser), less 色域選択 (act_sel_colour: 自動選択ツール's 色の近さ now, the user's decision 5)
        // — theirs (test_gui_select).
        // and the 14 of PC3 C-a (SPEC COLOR-07, vector@1) — ツール の ペンツール・曲線ペンツール・パス選択ツール (act_path_pen,
        // act_curvature_pen, act_path_select), 選択範囲 → 作業用パスを作成… (act_work_path), ページ → 台詞 → 作業用パスを作成
        // (act_line_work_path), 表示 → ターゲットパス (act_show_target_path), ファイル → パスを Illustrator へ… (act_export_paths_ai),
        // レイヤー → ベクトルマスク (act_vmask_reveal_all, act_vmask_hide_all, act_vmask_current_path, act_vmask_delete,
        // act_vmask_disable, act_vmask_link) and ラスタライズ → ベクトルマスク (act_rasterize_vmask) — theirs (test_gui_paths).
        // and the 12 of PC3 C-e (SPEC COLOR-07「判型」) — ページ → 画像解像度…・カンバスサイズ…・画像の回転 ▸・切り抜き・トリミング…・
        // すべての領域を表示 (act_image_size, act_canvas_size, act_rotate_180, act_rotate_90cw, act_rotate_90ccw, act_rotate_angle,
        // act_flip_canvas_h, act_flip_canvas_v, act_crop, act_trim, act_reveal_all) and ツール → 切り抜きツール (act_crop_tool) —
        // theirs (test_gui_page_size).
        // and the 13 of PC3 B2 (SPEC COLOR-06) — 編集 の 結合部分をコピー・特殊ペースト ▸ 3 つ・塗りつぶし…・境界線を描く…
        // (act_copy_merged, act_paste_in_place, act_paste_into, act_paste_outside, act_fill_dialog, act_stroke_dialog),
        // レイヤー → 新規 の 選択範囲をコピー／カットしたレイヤー (act_layer_via_copy, act_layer_via_cut), 選択範囲 の
        // すべてのレイヤー・レイヤーの選択を解除・レイヤーを検索・レイヤーを分離 (act_select_all_layers, act_deselect_layers,
        // act_find_layers, act_isolate_layers) and ファイル → スクリプト → すべてのマスクを統合 (act_flatten_masks) — theirs
        // (test_gui_clipboard).
        // and the 5 of PC3 B4 (SPEC COLOR-06「修復」) — ツール の スポット修復ブラシツール・修復ブラシツール・コピースタンプツール・
        // ヒストリーブラシツール (act_spot_healing, act_healing, act_clone_stamp, act_history_brush) and 編集 → コンテンツに応じた
        // 塗りつぶし… (act_content_aware_fill) — theirs (test_gui_retouch).
        // and the 16 of PC3 C-b (SPEC COLOR-07, pen-shapes@1) — ツール の 楕円形ツール・三角形ツール・多角形ツール・ラインツール・
        // カスタムシェイプツール・次のシェイプツール (act_shape_ellipse, act_shape_triangle, act_shape_polygon, act_shape_line,
        // act_shape_custom, act_next_shape_tool), 編集 → カスタムシェイプを定義… (act_define_custom_shape), レイヤー → シェイプを結合
        // (act_combine_unite, act_combine_subtract, act_combine_intersect, act_combine_exclude, act_merge_components), ラスタライズ →
        // シェイプ (act_rasterize_shape), 選択範囲 → シェイプに変換 (act_sel_to_shape), ページ → 台詞 → シェイプに変換
        // (act_line_to_shape) and 表示 → シェイプから新規ガイドを作成 (act_guides_from_shape) — theirs (test_gui_shapes).
        // and the 18 of PC3 C-c (SPEC COLOR-07「変形」) — 編集 の 自由変形・パペットワープ・遠近法ワープ・コンテンツに応じて拡大・縮小…・
        // ゆがみ…, 編集 → 変形 の 再実行・拡大・縮小・回転・ゆがみ・遠近法・水平方向に／垂直方向に／交差方向に分割ワープ・ワープ分割を削除・
        // 180° 回転・90° 回転（時計回り）・90° 回転（反時計回り）, ページ → 台詞 → ワープテキスト… (act_free_transform,
        // act_puppet_warp, act_perspective_warp, act_content_aware_scale, act_liquify_filter, act_transform_again,
        // act_transform_scale, act_transform_rotate, act_transform_skew, act_transform_perspective, act_split_warp_h,
        // act_split_warp_v, act_split_warp_cross, act_remove_warp_split, act_transform_rotate_180, act_transform_rotate_90cw,
        // act_transform_rotate_90ccw, act_warp_text) — theirs (test_gui_transform).
        // and the 11 of PC3 B3 (SPEC COLOR-06) — ツール の オブジェクト選択ツール・クイック選択ツール (act_object_select,
        // act_quick_select), 選択範囲 の フォーカス領域…・被写体を選択・空を選択・選択とマスク… (act_focus_area,
        // act_select_subject, act_select_sky, act_select_mask), レイヤー → すべてのオブジェクトをマスク (act_mask_all_objects)
        // and マッティング ▸ カラーの除去…・フリンジ削除…・黒マット削除・白マット削除 (act_matting_decontaminate,
        // act_matting_defringe, act_matting_black, act_matting_white) — theirs (test_gui_select_mask).
        // and the 33 of PC3 C-d (SPEC COLOR-07「移動・整列」) — レイヤー → 重ね順 の 最前面へ・最背面へ・逆順 (act_layer_front,
        // act_layer_back, act_layer_reverse), 整列 ▸ 6 and its 4 bases (act_align_top, act_align_vcenter, act_align_bottom,
        // act_align_left, act_align_hcenter, act_align_right, act_align_to_auto, act_align_to_trim, act_align_to_frame,
        // act_align_to_panel), 分布 ▸ 8 (act_distribute_top … act_distribute_right, act_distribute_horizontal,
        // act_distribute_vertical), レイヤーをリンク・リンクしたレイヤーを選択・レイヤーをロック… (act_link_layers, act_select_linked,
        // act_lock_layers), 表示 → スナップ and スナップ先 ▸ (act_snap_on, act_snap_guides, act_snap_grid, act_snap_layers,
        // act_snap_bounds, act_snap_all, act_snap_none) and the move tool's 自動選択・バウンディングボックスを表示
        // (act_move_auto_select, act_move_show_box) — theirs (test_gui_align).
        // and the 10 of PC4 F-0 (SPEC FILE-01) — ファイル の 指定形式で開く…・すべてを閉じる・その他を閉じる・コピーを保存…・復帰・
        // ファイル情報… and 書き出し ▸ PNG としてクイック書き出し・書き出し形式…・書き出しの環境設定…・Web 用に保存（従来）…
        // (act_open_as, act_close_all, act_close_others, act_save_copy, act_revert, act_file_info, act_quick_export_png,
        // act_export_as, act_export_prefs, act_save_for_web): frames until F-b and F-c — theFileMenuIsPhotoshops below.
        // and the 2 of PC4 E-0 (SPEC COLOR-09) — ツール → 描画色と背景色を初期設定に戻す (act_default_colours) and ウィンドウ →
        // スウォッチ (act_swatches) — theirs (test_gui_paint).
        // and the 78 of PC4 節 D0 (SPEC COLOR-08「フィルター」, the user's decision 1) — the フィルター menu's commands
        // act_filter_<kind> (77: Photoshop's, their kinds' groups D1…D6 lighting them; ぼかし・ぼかし（強）・シャープ・シャープ（強）・
        // シャープ（輪郭のみ）・輪郭以外をぼかす as act_filter_blur, act_filter_blur_more, act_filter_sharpen, act_filter_sharpen_more,
        // act_filter_sharpen_edges, act_filter_despeckle; 漫画's five act_filter_manga_<kind>) and 前回のフィルター (act_filter_last)
        // — theirs (test_gui_filters).
        // and the 1 of PC4 F-b (SPEC FILE-01) — ページ → 複製… (act_duplicate_image, image.duplicate) — theirs (test_gui_file),
        // which connects F-0's six frames of ファイル too; and the 2 of PC4 E1 (SPEC COLOR-09「ブラシエンジンと線」) — ツール →
        // 鉛筆ツール (act_pencil) and 表示 → ブラシプレビュー (act_brush_preview) — theirs (test_gui_brushes).
        // and the 3 of PC4 E3 (SPEC COLOR-09「パターン」) — 編集 → パターンを定義… (act_define_pattern), レイヤー パネルの
        // パターンのレイヤー… (act_layer_pattern, layer.newFillLayer.pattern) and 表示 → パターンプレビュー (act_pattern_preview)
        // — theirs (test_gui_patterns).
        // and the 9 of PC4 E5 (SPEC COLOR-09「画素を変える筆」) — ツール → ぼかしツール・シャープツール・指先ツール・覆い焼きツール・
        // 焼き込みツール・スポンジツール・混合ブラシツール・背景消しゴムツール・色の置き換えツール (act_blur_tool, act_sharpen_tool,
        // act_smudge_tool, act_dodge_tool, act_burn_tool, act_sponge_tool, act_mixer_brush, act_background_eraser,
        // act_color_replacement) — theirs (test_gui_paint_tools).
        // and the 4 of PC4 E2 (SPEC COLOR-09「ブラシのプリセット」「プリセットマネージャー」) — 編集 → ブラシを定義… (act_define_brush)
        // and プリセット ▸ プリセットマネージャー…・プリセットを移行・プリセットを書き出し／読み込み… (act_preset_manager,
        // act_migrate_presets, act_export_import_presets) — theirs (test_gui_brush_settings).
        // and the 3 of PC4 F-c (SPEC FILE-01) — ファイル → 書き出し → レイヤーからファイル… (act_layers_to_files), レイヤー の
        // PNG としてクイック書き出し・書き出し形式… (act_layer_quick_export_png, act_layer_export_as) — theirs (test_gui_output), which
        // connects F-0's four frames of 書き出し ▸ too.
        // and the 19 of PC4 E6 (SPEC COLOR-09「レイヤースタイル」) — レイヤー → レイヤースタイル ▸ 混合オプション…・each effect's 10
        // (act_layer_style_blending, act_layer_style_<kind>), レイヤースタイルをコピー・ペースト・消去 (act_layer_style_copy,
        // act_layer_style_paste, act_layer_style_clear), 包括光源… (act_global_light), レイヤーを作成 (act_layer_style_layers),
        // すべての効果を隠す・表示 (act_layer_style_hide_all, act_layer_style_show_all), 効果を拡大・縮小… (act_layer_style_scale)
        // — theirs (test_gui_styles).
        // and the 11 of PC5 H-0 (SPEC VIEW-01) — 編集 → キーボードショートカット… and 環境設定 ▸'s panes with a page
        // (act_keyboard_shortcuts, act_prefs_<pane>; 一般… is act_prefs) — theKeySets and theMenusOfPC5 below.
        // and the 23 frames of PC5 J-0 (SPEC PROD-01) — ページ → 解析 ▸'s 7 (act_measure_*), レイヤー → ビデオレイヤー ▸'s 12
        // (act_video_*), ウィンドウ's アクション・レイヤーカンプ・計測ログ・注釈 (act_panel_*) — theProductionFramesSayNotYet's.
        // and the 17 of PC5 G-a (SPEC TEXT-02) — ツール → 横書き文字ツール (act_text_h), 書式 → アンチエイリアス ▸ 7 (act_type_aa_<method>),
        // 方向 ▸ 横書き・縦書き (act_type_horizontal, act_type_vertical), テキストレイヤーをラスタライズ・ポイントテキストに変換・
        // 段落テキストに変換・すべてのテキストレイヤーを更新・Lorem Ipsum をペースト (act_type_rasterize, act_type_to_point,
        // act_type_to_box, act_type_update_all, act_type_lorem), 台詞をテキストレイヤーに・テキストレイヤーを台詞に
        // (act_line_to_type_layer, act_type_layer_to_line) — theirs (test_gui_type).
        // and the 16 of PC5 G-b (SPEC TEXT-02) — 書式 → OpenType ▸ 11 (act_type_ot_<tag>: liga, calt, dlig, swsh, salt, titl, ornm,
        // ordn, frac, onum and palt), 言語オプション ▸ 初期設定・東アジア言語・中東言語の機能 (act_type_lang_<which>), 中東および
        // 南アジア言語用 Composer (act_type_composer_world), 右から左の段落 (act_type_rtl) — theirs (test_gui_type).
                // and PC5 J-a's ファイル → 書き出し → レイヤーカンプからファイル… (act_comps_to_files; its ウィンドウ → レイヤーカンプ is the
        // frame's attribute made real) — test_gui_layer_comps.
        QCOMPARE(w->actions_by_name().size(), std::size_t{652});
        QVERIFY(w->action("act_chat_approval"));
        QVERIFY(w->action("act_style"));
        QVERIFY(w->action("act_exposure"));
        QVERIFY(w->action("act_nombre"));
        for (const char* name : {"act_layer_merge_down", "act_layer_merge_layers", "act_layer_merge_visible", "act_layer_flatten",
                                 "act_layer_convert_paint", "act_layer_convert_pen"}) {
            QVERIFY2(w->action(name), name);
        }
        QCOMPARE(w->action("act_nombre")->text(),QStringLiteral("ノンブルの設定…"));  // (Python's words)
        QVERIFY(menus_of(w,w->action("act_nombre")).contains(QStringLiteral("ページ")));
        for (const Expected& e : expected()) {
            const QString name = QString::fromLatin1(e.attribute);
            QAction* a = w->action(name);
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcuts(), e.keys);
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            const QStringList where = menus_of(w, a);
            QVERIFY2(where.contains(QString::fromUtf8(e.menu)), qPrintable(name + QStringLiteral(" is in ") + where.join(QStringLiteral(", "))));
            // the tooltip, as Python's window has it from its start (preferences.retip): "<name>（<keys>）\n<tip>" — the
            // palette's tools so from the first
            const bool palette_tool = name == QLatin1String("act_select") || name == QLatin1String("act_pen") || name == QLatin1String("act_eraser") ||
                                      name == QLatin1String("act_frame") || name == QLatin1String("act_move");
            if (palette_tool) {
                const QString keys = a->shortcut().toString();
                QCOMPARE(a->toolTip(), a->text() + (keys.isEmpty() ? QString() : QStringLiteral("（%1）").arg(keys)) + QStringLiteral("\n") + a->statusTip());
            } else if (*e.tip != '\0') {
                const QString keys = a->shortcut().toString(QKeySequence::NativeText);
                // (its name: its place too when another command has the same words — PC5 H-0's 水平方向に反転（表示）)
                const QString called = genko::app::command_name(genko::app::command_names(w), a);
                QCOMPARE(a->toolTip(), called + (keys.isEmpty() ? QString() : QStringLiteral("（%1）").arg(keys)) + QStringLiteral("\n") + QString::fromUtf8(e.tip));
            }
        }
        // the palette and the command bar (Python's order); with M3's eight ways of the marquee (test_gui_select) and its
        // drawing tools and line editing (test_gui_paint), the effect lines and the material tool (test_gui_effects), the
        // text tool (test_gui_lines); PC3 C-a's three path tools (test_gui_paths); the crop tool (PC3 C-e, test_gui_page_size);
        // PC3 B4's four retouching tools (test_gui_retouch); PC3 C-b's five shape tools besides 長方形ツール (test_gui_shapes)
        // PC3 B3's オブジェクト選択ツール and クイック選択ツール (test_gui_select_mask); PC4 E1's 鉛筆ツール (test_gui_brushes); PC4 E5's
        // nine brushes that change pixels (test_gui_paint_tools); PC5 G-a's 横書き文字ツール (test_gui_type)
        QCOMPARE(w->tools_group()->actions().size(), 54);
    }

    // PC4 F-0 (SPEC FILE-01; 節 F 5.1・5.2): ファイル in Photoshop's order and names — the book's export is 書き出し ▸
    // 原稿を書き出す… (Ctrl+E) —, and 節 F's new commands in their places with Photoshop's keys: F-b's connected (what they
    // do: test_gui_file), F-c's frames that say they are not in this build yet and change nothing until F-c connects them.
    void theFileMenuIsPhotoshops() {
        Studio studio;
        MainWindow* w = studio.window.get();
        QMenu* file = nullptr;
        for (QAction* top : w->menuBar()->actions()) {
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("ファイル")) file = top->menu();
        }
        QVERIFY(file != nullptr);
        const auto items = [](QMenu* menu) {
            QStringList out;
            for (QAction* a : menu->actions()) {
                if (a->isSeparator()) out << QStringLiteral("|");
                else if (a->menu() != nullptr) out << a->menu()->title() + QStringLiteral(" ▸");
                else out << a->text();
            }
            return out;
        };
        QCOMPARE(items(file),
                 (QStringList{QStringLiteral("新規…"), QStringLiteral("開く…"), QStringLiteral("指定形式で開く…"), QStringLiteral("最近使用したファイルを開く ▸"),
                              QStringLiteral("|"), QStringLiteral("AI と作る…"), QStringLiteral("|"), QStringLiteral("閉じる"), QStringLiteral("すべてを閉じる"),
                              QStringLiteral("その他を閉じる"), QStringLiteral("|"), QStringLiteral("保存"), QStringLiteral("別名で保存…"), QStringLiteral("コピーを保存…"),
                              QStringLiteral("復帰"), QStringLiteral("|"), QStringLiteral("書き出し ▸"), QStringLiteral("|"), QStringLiteral("埋め込みを配置…"),
                              QStringLiteral("リンクを配置…"), QStringLiteral("パッケージ…"), QStringLiteral("|"), QStringLiteral("画像を読み込む…"),
                              QStringLiteral("スキャン画像を線画にして取り込む…"), QStringLiteral("スキャナーから取り込む…"), QStringLiteral("ほかの原稿のページを取り込む…"),
                              QStringLiteral("PSD をレイヤーのまま読み込む…"), QStringLiteral("|"), QStringLiteral("ファイル情報…"), QStringLiteral("|"),
                              QStringLiteral("プリント…"), QStringLiteral("|"), QStringLiteral("タイムラプスを記録する"), QStringLiteral("タイムラプスを書き出す…"),
                              QStringLiteral("|"), QStringLiteral("自動処理 ▸"), QStringLiteral("スクリプト ▸"), QStringLiteral("オートアクション ▸"),
                              QStringLiteral("|"), QStringLiteral("終了")}));  // (PC5 H-0: 環境設定 in 編集)
        // (PC5 I-0, SPEC.md AUTO-01: 自動処理 ▸ is a frame, shown once a group puts a command in it)
        QVERIFY(w->findChild<QMenu*>(QStringLiteral("menu_automate")) != nullptr);
        QVERIFY(!w->findChild<QMenu*>(QStringLiteral("menu_automate"))->menuAction()->isVisible());
        QCOMPARE(items(w->findChild<QMenu*>(QStringLiteral("menu_scripts"))), QStringList{QStringLiteral("すべてのマスクを統合")});
        QMenu* exports = w->findChild<QMenu*>(QStringLiteral("menu_export"));
        QVERIFY(exports != nullptr);
        QCOMPARE(items(exports), (QStringList{QStringLiteral("PNG としてクイック書き出し"), QStringLiteral("書き出し形式…"), QStringLiteral("|"),
                                             QStringLiteral("書き出しの環境設定…"), QStringLiteral("|"), QStringLiteral("Web 用に保存（従来）…"), QStringLiteral("|"),
                                             QStringLiteral("原稿を書き出す…"), QStringLiteral("|"), QStringLiteral("レイヤーからファイル…"),
                                             QStringLiteral("レイヤーカンプからファイル…"),  // (PC5 J-a)
                                             QStringLiteral("パスを Illustrator へ…"), QStringLiteral("カラールックアップテーブル…")}));
        // (PC5 H-0: no key in the Photoshop key set — Ctrl+E is 下のレイヤーと結合; Ctrl+E in the Genko set, theKeySets)
        QVERIFY(studio.act("act_export")->shortcuts().isEmpty());
        QCOMPARE(studio.act("act_print")->text(), QStringLiteral("プリント…"));
        // (節 F's new ones: Photoshop's keys — F-b's connected (PC4 F-b: test_gui_file says what they do), F-c's too (PC4 F-c:
        // test_gui_output) —; F-c's open their windows, the book unchanged when they are closed)
        const std::vector<std::pair<const char*, const char*>> connected{
            {"act_open_as", "Ctrl+Alt+Shift+O"}, {"act_close_all", "Ctrl+Alt+W"}, {"act_close_others", "Ctrl+Alt+P"},
            {"act_save_copy", "Ctrl+Alt+S"},     {"act_revert", "F12"},           {"act_file_info", "Ctrl+Alt+Shift+I"}};
        for (const auto& [name, key] : connected) {
            QAction* a = studio.act(name);
            QVERIFY2(a != nullptr, name);
            QCOMPARE(a->shortcuts(), QList<QKeySequence>{QKeySequence(QString::fromLatin1(key))});
        }
        const std::vector<std::pair<const char*, const char*>> frames{
            {"act_quick_export_png", ""}, {"act_export_as", "Ctrl+Alt+Shift+W"}, {"act_export_prefs", ""}, {"act_save_for_web", "Ctrl+Alt+Shift+S"}};
        const std::int64_t revision = studio.doc().revision;
        const std::size_t books = w->documents().size();
        // (each opens its window — or, クイック書き出し, asks where —: counted as the responder is asked)
        int opened = 0;
        const auto shown = studio.answers->responder->exec;
        const auto where = studio.answers->responder->save_path;
        studio.answers->responder->exec = [&](QDialog* dialog) {
            ++opened;
            return shown ? shown(dialog) : int(QDialog::Rejected);
        };
        studio.answers->responder->save_path = [&](const QString& caption, const QString& suggested) {
            ++opened;
            return where ? where(caption, suggested) : QString();
        };
        for (const auto& [name, key] : frames) {
            QAction* a = studio.act(name);
            QVERIFY2(a != nullptr, name);
            QCOMPARE(a->shortcuts(), *key == '\0' ? QList<QKeySequence>() : QList<QKeySequence>{QKeySequence(QString::fromLatin1(key))});
            const int before = opened;
            a->trigger();  // (its window shut, or — クイック書き出し — no place given: nothing written)
            QVERIFY2(!w->last_notice().contains(QStringLiteral("まだ使えません")), qPrintable(w->last_notice()));
            QVERIFY2(opened == before + 1, name);
        }
        studio.answers->responder->exec = shown;
        studio.answers->responder->save_path = where;
        QCOMPARE(studio.doc().revision, revision);
        QCOMPARE(w->documents().size(), books);
    }

    // PC5 J-0 (SPEC PROD-01; 節 J 5.1・5.3・4.2): the production's frames in Photoshop's places — ページ (イメージ) → 解析 ▸,
    // レイヤー → ビデオレイヤー ▸ before ラスタライズ ▸, ウィンドウ's アクション・レイヤーカンプ・計測ログ・注釈 —, each saying it is
    // not in this build yet and changing nothing; the canvas's extras drawn by their painters, shown and hidden by key and
    // by the master switch.
    void theProductionFramesSayNotYet() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const auto items = [](QMenu* menu) {
            QStringList out;
            for (QAction* a : menu->actions()) {
                if (a->isSeparator()) out << QStringLiteral("|");
                else if (a->menu() != nullptr) out << a->menu()->title() + QStringLiteral(" ▸");
                else out << a->text();
            }
            return out;
        };
        QMenu* analysis = w->findChild<QMenu*>(QStringLiteral("menu_analysis"));
        QVERIFY(analysis != nullptr);
        QCOMPARE(items(analysis), (QStringList{QStringLiteral("計測スケールを設定 ▸"), QStringLiteral("データポイントを選択 ▸"), QStringLiteral("計測値を記録"),
                                              QStringLiteral("|"), QStringLiteral("ものさしツール"), QStringLiteral("カウントツール"), QStringLiteral("|"),
                                              QStringLiteral("目盛りマーカーを配置…")}));
        QCOMPARE(items(w->findChild<QMenu*>(QStringLiteral("menu_measure_scale"))), (QStringList{QStringLiteral("初期設定"), QStringLiteral("カスタム…")}));
        QCOMPARE(items(w->findChild<QMenu*>(QStringLiteral("menu_measure_points"))), (QStringList{QStringLiteral("カスタム…")}));
        QMenu* video = w->findChild<QMenu*>(QStringLiteral("menu_video_layers"));
        QVERIFY(video != nullptr);
        QCOMPARE(items(video), (QStringList{QStringLiteral("ファイルから新規ビデオレイヤー…"), QStringLiteral("新規空白ビデオレイヤー"), QStringLiteral("|"),
                                           QStringLiteral("空白フレームを挿入"), QStringLiteral("フレームを複製"), QStringLiteral("フレームを削除"), QStringLiteral("|"),
                                           QStringLiteral("フッテージを置き換え…"), QStringLiteral("フッテージを解釈…"), QStringLiteral("|"),
                                           QStringLiteral("変更されたビデオを表示"), QStringLiteral("フレームを復元"), QStringLiteral("すべてのフレームを復元"),
                                           QStringLiteral("フレームを再読み込み"), QStringLiteral("|"), QStringLiteral("ラスタライズ")}));
        QMenu* layers = nullptr;
        QMenu* window_menu = nullptr;
        for (QAction* top : w->menuBar()->actions()) {
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("レイヤー")) layers = top->menu();
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("ウィンドウ")) window_menu = top->menu();
        }
        QVERIFY(layers != nullptr && window_menu != nullptr);
        const QStringList in_layers = items(layers);
        QCOMPARE(in_layers.indexOf(QStringLiteral("ビデオレイヤー ▸")) + 1, in_layers.indexOf(QStringLiteral("ラスタライズ ▸")));
        const QStringList in_window = items(window_menu);
        for (const char* panel : {"アクション", "レイヤーカンプ", "計測ログ", "注釈", "タイムライン"}) {
            QVERIFY2(in_window.contains(QString::fromUtf8(panel)), panel);
        }
        // (the smart object's ラスタライズ keeps its own name: the first of the words in the menus)
        QCOMPARE(studio.act("act_smart_rasterize")->text(), QStringLiteral("ラスタライズ"));
        const std::int64_t revision = studio.doc().revision;
        for (const char* name : {"act_measure_scale_default", "act_measure_scale_custom", "act_measure_data_points", "act_measure_record",
                                 "act_measure_ruler_tool", "act_measure_count_tool", "act_measure_scale_marker", "act_video_from_file", "act_video_blank",
                                 "act_video_insert_blank", "act_video_duplicate", "act_video_delete", "act_video_replace", "act_video_interpret",
                                 "act_video_show_altered", "act_video_restore", "act_video_restore_all", "act_video_reload", "act_video_rasterize",
                                 "act_panel_actions", "act_panel_measurement_log", "act_panel_notes"}) {  // (PC5 J-a: レイヤーカンプ made real)
            QAction* a = studio.act(name);
            QVERIFY2(a != nullptr, name);
            QVERIFY2(!a->statusTip().isEmpty(), name);
            a->trigger();
            QVERIFY2(w->last_notice().contains(QStringLiteral("この版ではまだ使えません")) && w->last_notice().contains(a->text()), qPrintable(w->last_notice()));
        }
        QCOMPARE(studio.doc().revision, revision);
        // (P2-3: one command an attribute — a frame made real takes its place: make() lets the old one go —, and the keys'
        // window, the search and the keys laid again find the command the menus hold)
        std::map<QString, int> made;
        for (QAction* a : w->findChildren<QAction*>()) {
            const QString attribute = a->property("genko_attribute").toString();
            if (!attribute.isEmpty()) ++made[attribute];
        }
        for (const auto& [attribute, count] : made) QVERIFY2(count == 1, qPrintable(attribute));
        const std::vector<QAction*> listed = genko::app::preferences::commands(w);
        for (const char* name : {"act_measure_record", "act_measure_scale_custom", "act_measure_data_points", "act_video_rasterize", "act_panel_notes",
                                 "act_panel_layer_comps", "act_comps_to_files"}) {
            QAction* own = studio.act(name);
            QVERIFY2(std::find(listed.begin(), listed.end(), own) != listed.end(), name);
        }
        // the extras: a painter a key, over the page, shown and hidden
        PageCanvas* canvas = w->canvas();
        int drawn = 0;
        canvas->add_extra({QStringLiteral("count"), [&drawn](QPainter&, const core::Page&, const PageCanvas::ExtraView& view) {
                               if (view.per_mm > 0 && view.at.map(QPointF(10, 10)) != view.at.map(QPointF(20, 20)) && view.seen_mm.contains(QPointF(10, 10)))
                                   ++drawn;
                           }});
        QCOMPARE(canvas->extra_keys(), std::vector<QString>{QStringLiteral("count")});
        (void)canvas->grab();
        QVERIFY(drawn > 0);
        canvas->show_extra(QStringLiteral("count"), false);
        QVERIFY(!canvas->extra_shown(QStringLiteral("count")));
        drawn = 0;
        (void)canvas->grab();
        QCOMPARE(drawn, 0);
        canvas->show_extra(QStringLiteral("count"), true);
        canvas->extras_shown = false;
        (void)canvas->grab();
        QCOMPARE(drawn, 0);
        canvas->extras_shown = true;
        (void)canvas->grab();
        QVERIFY(drawn > 0);
    }

    // No two commands share a key (Qt would take the key as ambiguous and do neither): PC1b's 色の校正 (Ctrl+Y in the Photoshop
    // key set — やり直し Ctrl+Shift+Z — since PC5 H-0), 色域外警告 (Ctrl+Shift+Y) and カラー設定… (Ctrl+Shift+K) among them.
    void noTwoCommandsShareAKey() {
        Studio studio;
        MainWindow* w = studio.window.get();
        std::map<QString, QString> owner;
        QStringList clashes;
        for (const auto& [name, action] : w->actions_by_name()) {
            for (const QKeySequence& key : action->shortcuts()) {
                const QString text = key.toString(QKeySequence::PortableText);
                if (text.isEmpty()) continue;
                const auto [it, fresh] = owner.emplace(text, name);
                if (!fresh) clashes << QStringLiteral("%1: %2 / %3").arg(text, it->second, name);
            }
        }
        QVERIFY2(clashes.isEmpty(), qPrintable(clashes.join(QStringLiteral(", "))));
        QCOMPARE(owner.at(QStringLiteral("Ctrl+Y")), QStringLiteral("act_cmyk_proof"));  // (PC5 H-0: Photoshop's)
        QCOMPARE(owner.at(QStringLiteral("Ctrl+Shift+Y")), QStringLiteral("act_gamut_warning"));
        QCOMPARE(owner.at(QStringLiteral("Ctrl+Shift+K")), QStringLiteral("act_color_settings"));
        QCOMPARE(owner.at(QStringLiteral("Ctrl+Shift+Z")), QStringLiteral("act_redo"));
        // (選択を解除 keeps Ctrl+D on X11 too, whose keys for Delete hold it: 消去 takes Delete's first and Backspace,
        // as Python's)
        QCOMPARE(owner.at(QStringLiteral("Ctrl+D")), QStringLiteral("act_deselect"));
    }

    // A key the person gave to a command before a new command took it as its own (PC1b's Ctrl+Shift+Y, Ctrl+Shift+K,
    // Ctrl+Alt+Y): the person's stays, the new command's own key is taken off (both would do nothing), and it is said; the
    // key back to the new command when the person's goes.
    void aKeyThePersonChoseWinsOverANewOne() {
        genko::app::settings()->setValue(QStringLiteral("shortcuts/ページを並べて見る"), QStringLiteral("Ctrl+Shift+Y"));
        {
            Studio studio;
            MainWindow* w = studio.window.get();
            QCOMPARE(w->action(QStringLiteral("act_overview"))->shortcuts(), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+Y"))});
            QVERIFY(w->action(QStringLiteral("act_gamut_warning"))->shortcuts().isEmpty());
            QVERIFY2(w->last_notice().contains(QStringLiteral("色域外警告")) && w->last_notice().contains(QStringLiteral("ページを並べて見る")),
                     qPrintable(w->last_notice()));
            // (said once: the keys applied again — the same key taken off again — not said again; PC5 H-0)
            w->flash(QStringLiteral("ほかの知らせ"), 3000);
            genko::app::preferences::apply_all(w);
            QCOMPARE(w->last_notice(), QStringLiteral("ほかの知らせ"));
            QVERIFY(w->action(QStringLiteral("act_gamut_warning"))->shortcuts().isEmpty());
            genko::app::settings()->remove(QStringLiteral("shortcuts/ページを並べて見る"));
            genko::app::preferences::apply_all(w);
            QCOMPARE(w->action(QStringLiteral("act_gamut_warning"))->shortcuts(), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+Y"))});
            QCOMPARE(w->action(QStringLiteral("act_overview"))->shortcuts(), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+O"))});
        }
    }

    // PC5 H-0 (the user's decision 8, SPEC VIEW-01): the keys come in sets — Photoshop's by default, Genko's before PC5
    // chosen back in 環境設定 → ショートカット (キーボードショートカット…) —; every row of the Photoshop set names a command
    // of the window, neither set gives one key to two commands, and a key the person chose stays over either set.
    void theKeySets() {
        namespace key_sets = genko::app::key_sets;
        Studio studio;
        MainWindow* w = studio.window.get();
        const auto clashes = [w] {
            std::map<QString, QAction*> owner;
            QStringList out;
            for (QAction* a : w->findChildren<QAction*>()) {
                if (a->text().isEmpty() || a->menu() != nullptr) continue;
                for (const QKeySequence& key : a->shortcuts()) {
                    const auto [it, fresh] = owner.emplace(key.toString(QKeySequence::PortableText), a);
                    if (!fresh && it->second != a) out << key.toString() + QStringLiteral(": ") + it->second->text() + QStringLiteral(" / ") + a->text();
                }
            }
            return out;
        };
        const auto dock = [w](const char* object) {
            for (QDockWidget* d : w->findChildren<QDockWidget*>(QString::fromUtf8(object))) return d;
            return static_cast<QDockWidget*>(nullptr);
        };
        const auto keys = [w](const char* name) { return w->action(QString::fromLatin1(name))->shortcuts(); };
        QCOMPARE(key_sets::current(), QStringLiteral("photoshop"));
        QCOMPARE(key_sets::sets().size(), std::size_t{2});
        for (const auto& [row, set] : key_sets::photoshop_keys()) {
            if (row.startsWith(QStringLiteral("dock:"))) {
                QVERIFY2(!w->findChildren<QDockWidget*>(row.mid(5)).isEmpty(), qPrintable(row));
            } else {
                QVERIFY2(w->action(row) != nullptr, qPrintable(row));
                QCOMPARE(w->action(row)->shortcuts(), set);
            }
        }
        QCOMPARE(clashes(), QStringList());
        // Photoshop's: 移動ツール V, クイックマスク Q, コマ割り K (Photoshop's フレームツール), 検索 Ctrl+F, フィルターの再実行 Ctrl+Alt+F,
        // 下のレイヤーと結合 Ctrl+E, 一般… Ctrl+K, the channels Ctrl+2…, レイヤー F7; Genko's that Photoshop gives another: none
        QCOMPARE(keys("act_move"), k({s("V")}));
        QCOMPARE(keys("act_quick_mask"), k({s("Q")}));
        QCOMPARE(keys("act_frame"), k({s("K")}));
        QCOMPARE(keys("act_find_command"), k({s("Ctrl+F")}));
        QCOMPARE(keys("act_filter_last"), k({s("Ctrl+Alt+F")}));
        QCOMPARE(keys("act_layer_merge"), k({s("Ctrl+E")}));
        QCOMPARE(keys("act_prefs"), k({s("Ctrl+K"), s("Ctrl+,")}));
        QCOMPARE(keys("act_keyboard_shortcuts"), k({s("Ctrl+Alt+Shift+K")}));
        QCOMPARE(keys("act_channel_composite"), k({s("Ctrl+2")}));
        QCOMPARE(keys("act_scale"), k({s("Ctrl+R")}));  // (定規: Photoshop's the same)
        QCOMPARE(dock("レイヤー")->toggleViewAction()->shortcuts(), k({s("F7")}));
        for (const char* none : {"act_mirror", "act_actual", "act_guides", "act_history", "act_snap", "act_show_rulers", "act_export"}) {
            QVERIFY2(keys(none).isEmpty(), none);
        }
        // the Genko-only tools whose letter Photoshop gives another: a key Photoshop leaves free (the user's decision
        // 2026-10-29) — 選択 Shift+N, 効果線 Shift+K, 3D 操作 Shift+D, 線の修正 Shift+R, 塗りつぶし Shift+Q
        QCOMPARE(keys("act_select"), k({s("Shift+N")}));
        QCOMPARE(keys("act_effect"), k({s("Shift+K")}));
        QCOMPARE(keys("act_3d"), k({s("Shift+D")}));
        QCOMPARE(keys("act_reshape"), k({s("Shift+R")}));
        QCOMPARE(keys("act_fill"), k({s("Shift+Q")}));
        for (const QString& tool : key_sets::genko_tools()) {
            const QList<QKeySequence> own = w->action(tool)->shortcuts();
            QVERIFY2(!own.isEmpty(), qPrintable(tool));
            for (const QKeySequence& key : own) {
                QVERIFY2(!key_sets::photoshop_defaults().contains(key.toString(QKeySequence::PortableText)), qPrintable(tool + QStringLiteral(" ") + key.toString()));
            }
        }
        QVERIFY(key_sets::photoshop_defaults().contains(QStringLiteral("Shift+B")) && key_sets::photoshop_defaults().contains(QStringLiteral("V")));
        // the keys the words say are the commands' own now (the オプション panel's title, a notice): Photoshop's here
        const auto title = [w](const char* tool) {
            w->choose_tool(QString::fromLatin1(tool));
            return w->tool_settings()->title()->text();
        };
        QCOMPARE(title("select"), QStringLiteral("選択（Shift+N）"));
        QCOMPARE(title("frame"), QStringLiteral("コマ割り（K）"));
        QCOMPARE(title("move"), QStringLiteral("移動ツール（V）"));
        QCOMPARE(title("shape"), QStringLiteral("シェイプツール（U・Shift+U）"));
        // Genko's set: the keys before PC5 back
        genko::app::settings()->setValue(QStringLiteral("ui/key_set"), QStringLiteral("genko"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(key_sets::current(), QStringLiteral("genko"));
        QCOMPARE(keys("act_move"), k({s("Q")}));
        QCOMPARE(keys("act_select"), k({s("V")}));
        QCOMPARE(keys("act_frame"), k({s("F")}));
        QCOMPARE(keys("act_mirror"), k({s("H")}));
        QCOMPARE(keys("act_actual"), k({s("Ctrl+1")}));
        QCOMPARE(keys("act_guides"), k({s("Ctrl+;")}));
        QCOMPARE(keys("act_history"), k({s("Ctrl+H")}));
        QCOMPARE(keys("act_cmyk_proof"), k({s("Ctrl+Alt+Y")}));
        QCOMPARE(keys("act_redo"), k({QKeySequence(Std::Redo), s("Ctrl+Y")}));
        QCOMPARE(keys("act_find_command"), k({s("Ctrl+Shift+F")}));
        QCOMPARE(keys("act_filter_last"), k({s("Ctrl+F")}));
        QCOMPARE(keys("act_export"), k({s("Ctrl+E")}));
        QCOMPARE(keys("act_prefs"), k({s("Ctrl+,")}));
        QCOMPARE(keys("act_channel_composite"), k({s("Ctrl+Alt+2")}));
        QVERIFY(dock("レイヤー")->toggleViewAction()->shortcuts().isEmpty());
        QCOMPARE(clashes(), QStringList());
        QCOMPARE(title("select"), QStringLiteral("選択（V）"));  // (Genko's: the title follows the set)
        QCOMPARE(title("frame"), QStringLiteral("コマ割り（F）"));
        QCOMPARE(title("shape"), QStringLiteral("シェイプツール（O・Shift+O）"));
        // a key the person chose stays over either set
        genko::app::settings()->setValue(QStringLiteral("shortcuts/移動ツール"), QStringLiteral("Ctrl+Alt+M"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(keys("act_move"), k({s("Ctrl+Alt+M")}));
        key_sets::set_current(QStringLiteral("photoshop"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(keys("act_move"), k({s("Ctrl+Alt+M")}));
        genko::app::settings()->remove(QStringLiteral("shortcuts/移動ツール"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(keys("act_move"), k({s("V")}));
        // 環境設定 → ショートカット's キーの組: the keys shown follow the set chosen (the person's own stay), kept by 決める
        {
            genko::app::preferences::PreferencesDialog dialog(w);
            QCOMPARE(dialog.key_set->currentData().toString(), QStringLiteral("photoshop"));
            QCOMPARE(dialog.editor(QStringLiteral("移動ツール"))->keySequence(), s("V"));
            dialog.editor(QStringLiteral("選択"))->setKeySequence(s("Ctrl+Alt+N"));
            dialog.key_set->setCurrentIndex(dialog.key_set->findData(QStringLiteral("genko")));
            QCOMPARE(dialog.editor(QStringLiteral("移動ツール"))->keySequence(), s("Q"));
            QCOMPARE(dialog.editor(QStringLiteral("選択"))->keySequence(), s("Ctrl+Alt+N"));
            dialog.reset_keys();
            QCOMPARE(dialog.editor(QStringLiteral("選択"))->keySequence(), s("V"));
            dialog.save();
        }
        QCOMPARE(key_sets::current(), QStringLiteral("genko"));
        QCOMPARE(keys("act_move"), k({s("Q")}));
        QVERIFY(!genko::app::settings()->contains(QStringLiteral("shortcuts/選択")));
        // (the next window as it was left, then the default again)
        genko::app::settings()->remove(QStringLiteral("ui/key_set"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(keys("act_move"), k({s("V")}));
        // a preference written by another program (genko prefs set — H-e): the watch of settings.ini applies it to the
        // windows open; a write of the app's memories only is not one, nor the app's own write of a preference (what it
        // writes it has applied itself — the export's settings, ブラシプレビュー)
        const QString file = QString::fromStdString(genko::core::path_to_utf8(genko::app::config_dir() / "settings.ini"));
        const auto other = [&file](const char* key, const QString& value) {
            QSettings program(file, QSettings::IniFormat);  // (not the app's own settings(): another program)
            if (value.isNull()) program.remove(QString::fromLatin1(key));
            else program.setValue(QString::fromLatin1(key), value);
        };
        (void)genko::app::settings_changed_now();
        other("ui/key_set", QStringLiteral("genko"));
        QVERIFY(genko::app::settings_changed_now());
        QCOMPARE(keys("act_move"), k({s("Q")}));
        other("brush/kind", QStringLiteral("gpen"));
        QVERIFY(!genko::app::settings_changed_now());
        genko::app::settings()->setValue(QStringLiteral("ui/brush_preview"), QStringLiteral("false"));
        genko::app::settings()->setValue(QStringLiteral("export/jpeg_quality"), 70);
        QVERIFY(!genko::app::settings_changed_now());
        genko::app::settings()->remove(QStringLiteral("ui/brush_preview"));
        genko::app::settings()->remove(QStringLiteral("export/jpeg_quality"));
        QVERIFY(!genko::app::settings_changed_now());
        other("ui/key_set", QString());
        QVERIFY(genko::app::settings_changed_now());
        QCOMPARE(keys("act_move"), k({s("V")}));
        // a person whose settings are from before the Photoshop set became the default: told once on the app's start, and
        // the Genko set a click away (the coordinator's decision with the user's decision 8); a new person never told
        genko::app::settings()->remove(QStringLiteral("ui/key_set_told"));
        QVERIFY(!genko::app::settings()->allKeys().isEmpty());
        QVERIFY(key_sets::from_before_pc5());
        QVERIFY(!key_sets::from_before_pc5());  // (once)
        key_sets::tell_new_default(w);
        QVERIFY2(w->last_notice().contains(QStringLiteral("Photoshop")) && w->last_notice().contains(QStringLiteral("Genko の組に戻す")), qPrintable(w->last_notice()));
        auto* back = w->findChild<QPushButton*>(QStringLiteral("key_set_back"));
        QVERIFY(back != nullptr && back->text() == QStringLiteral("Genko の組に戻す"));
        back->click();
        QCOMPARE(key_sets::current(), QStringLiteral("genko"));
        QCOMPARE(keys("act_move"), k({s("Q")}));
        QVERIFY(!back->isVisible());  // (gone)
        genko::app::settings()->remove(QStringLiteral("ui/key_set"));
        genko::app::preferences::apply_all(w);
        QCOMPARE(keys("act_move"), k({s("V")}));
        {
            // (chosen already: not told)
            genko::app::settings()->remove(QStringLiteral("ui/key_set_told"));
            genko::app::settings()->setValue(QStringLiteral("ui/key_set"), QStringLiteral("photoshop"));
            QVERIFY(!key_sets::from_before_pc5());
            genko::app::settings()->remove(QStringLiteral("ui/key_set"));
        }
    }

    // PC5 H-0 (節 H 5.1・5.2・5.4, SPEC VIEW-01): 表示 in Photoshop's order with Genko's own after a line; ウィンドウ → アレンジ ▸
    // (新規ウィンドウ) and ワークスペース ▸ first; 編集 ends with 検索, キーボードショートカット… and 環境設定 ▸ (its panes, each
    // opening its page) — 環境設定… no longer in ファイル nor 検索 in ヘルプ; the panels by Photoshop's names.
    void theMenusOfPC5() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const auto menu = [w](const QString& title) -> QMenu* {
            for (QAction* top : w->menuBar()->actions()) {
                if (top->menu() != nullptr && top->menu()->title() == title) return top->menu();
            }
            return nullptr;
        };
        const auto items = [](QMenu* m) {
            QStringList out;
            for (QAction* a : m->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->menu() != nullptr ? a->menu()->title() + QStringLiteral(" ▸") : a->text());
            return out;
        };
        QCOMPARE(items(menu(QStringLiteral("表示"))),
                 (QStringList{QStringLiteral("校正の設定 ▸"), QStringLiteral("色の校正"), QStringLiteral("色域外警告"), QStringLiteral("|"),
                              QStringLiteral("ズームイン"), QStringLiteral("ズームアウト"), QStringLiteral("画面サイズに合わせる"), QStringLiteral("プリントサイズ"),
                              QStringLiteral("|"), QStringLiteral("水平方向に反転"), QStringLiteral("パターンプレビュー"), QStringLiteral("|"),
                              QStringLiteral("ターゲットパス"), QStringLiteral("ブラシプレビュー"), QStringLiteral("|"), QStringLiteral("定規"), QStringLiteral("|"),
                              QStringLiteral("スナップ"), QStringLiteral("スナップ先 ▸"), QStringLiteral("|"), QStringLiteral("シェイプから新規ガイドを作成"),
                              QStringLiteral("|"), QStringLiteral("上下反転して見る"), QStringLiteral("左に回す（15°）"), QStringLiteral("右に回す（15°）"),
                              QStringLiteral("回転・反転を戻す"), QStringLiteral("表示倍率を打ち込む…"), QStringLiteral("ズームツール"), QStringLiteral("|"),
                              QStringLiteral("ページを並べて見る"), QStringLiteral("◀ 前のページ"), QStringLiteral("次のページ ▶"), QStringLiteral("|"),
                              QStringLiteral("仕上がり線・基本枠を表示"), QStringLiteral("スマホの画面の範囲を表示"), QStringLiteral("選択範囲の境界線"),
                              QStringLiteral("前のページを透かす（オニオンスキン）"), QStringLiteral("網点を画面で見る"), QStringLiteral("|"), QStringLiteral("道具の名前を表示")}));
        const QStringList window = items(menu(QStringLiteral("ウィンドウ")));
        QCOMPARE(window.mid(0, 4), (QStringList{QStringLiteral("アレンジ ▸"), QStringLiteral("ワークスペース ▸"), QStringLiteral("|"), QStringLiteral("原稿だけを表示")}));
        QMenu* arrange = w->findChild<QMenu*>(QStringLiteral("menu_arrange"));
        QVERIFY(arrange != nullptr);
        QCOMPARE(items(arrange), QStringList{QStringLiteral("新規ウィンドウ")});
        for (const char* panel : {"ナビゲーター", "オプション", "ヒストリー"}) QVERIFY2(window.contains(QString::fromUtf8(panel)), panel);
        for (const auto& [object, title] : {std::pair{"全体図", "ナビゲーター"}, {"ツールの設定", "オプション"}, {"履歴", "ヒストリー"}}) {
            const auto docks = w->findChildren<QDockWidget*>(QString::fromUtf8(object));  // (the workspaces' keys kept)
            QCOMPARE(docks.size(), qsizetype{1});
            QCOMPARE(docks.front()->windowTitle(), QString::fromUtf8(title));
        }
        QCOMPARE(w->tool_palette()->windowTitle(), QStringLiteral("ツール"));
        const QStringList edit = items(menu(QStringLiteral("編集")));
        QCOMPARE(edit.mid(edit.size() - 5),
                 (QStringList{QStringLiteral("|"), QStringLiteral("検索"), QStringLiteral("|"), QStringLiteral("キーボードショートカット…"), QStringLiteral("環境設定 ▸")}));
        QMenu* panes = w->findChild<QMenu*>(QStringLiteral("menu_preferences"));
        QVERIFY(panes != nullptr);
        QCOMPARE(items(panes), (QStringList{QStringLiteral("一般…"), QStringLiteral("インターフェイス…"), QStringLiteral("ツール…"), QStringLiteral("ファイル管理…"),
                                           QStringLiteral("書き出し…"), QStringLiteral("パフォーマンス…"), QStringLiteral("カーソル…"), QStringLiteral("透明部分・色域…"),
                                           QStringLiteral("Camera Raw…"), QStringLiteral("ペンタブレット…"), QStringLiteral("長い時間の作業…")}));
        QVERIFY(!items(menu(QStringLiteral("ファイル"))).contains(QStringLiteral("環境設定…")));
        QVERIFY(!items(menu(QStringLiteral("ヘルプ"))).contains(QStringLiteral("検索")));
        // each pane opens the dialog at its page
        const auto shown = studio.answers->responder->exec;
        QString page;
        studio.answers->responder->exec = [&page](QDialog* dialog) {
            if (auto* prefs = qobject_cast<genko::app::preferences::PreferencesDialog*>(dialog)) page = prefs->tabs->tabText(prefs->tabs->currentIndex());
            return int(QDialog::Rejected);
        };
        for (const auto& [pane, title] : std::vector<std::pair<const char*, const char*>>{{"act_prefs", "表示・作業"},
                                                                                          {"act_prefs_fileHandling", "ファイル管理"},
                                                                                          {"act_prefs_export", "書き出し"},
                                                                                          {"act_prefs_transparencyAndGamut", "透明部分・色域"},
                                                                                          {"act_prefs_rawDefaults", "Camera Raw"},
                                                                                          {"act_prefs_tablet", "ペンタブレット"},
                                                                                          {"act_keyboard_shortcuts", "ショートカット"}}) {
            page.clear();
            studio.trigger(pane);
            QCOMPARE(page, QString::fromUtf8(title));
        }
        studio.answers->responder->exec = shown;
    }

    // ウィンドウ → 開いている原稿 (document.activate): every book open in the window by its number and title, the one in
    // front checked; one chosen comes to the front (the same as its tab).
    void theOpenBooksAreListed() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const fs::path other = path_of(studio.dir.filePath("other.genko"));
        write_book(other, new_doc(2, "別の本"));
        w->open_project(other);
        QVERIFY(wait_for([&] { return w->documents().size() == 2; }));
        QMenu* books = nullptr;
        for (QAction* top : w->menuBar()->actions()) {
            if (top->menu() == nullptr || top->menu()->title() != QStringLiteral("ウィンドウ")) continue;
            for (QAction* a : top->menu()->actions())
                if (a->menu() != nullptr && a->menu()->title() == QStringLiteral("開いている原稿")) books = a->menu();
        }
        QVERIFY(books != nullptr);
        emit books->aboutToShow();
        QCOMPARE(books->actions().size(), 2);
        QVERIFY(books->actions().at(0)->text().contains(QStringLiteral("試し")));
        QVERIFY(books->actions().at(1)->text().contains(QStringLiteral("別の本")));
        QVERIFY(!books->actions().at(0)->isChecked() && books->actions().at(1)->isChecked());
        books->actions().at(0)->trigger();
        QCOMPARE(w->current_document(), 0);
        QCOMPARE(w->book().title, std::string("試し"));
        emit books->aboutToShow();
        QVERIFY(books->actions().at(0)->isChecked() && !books->actions().at(1)->isChecked());
        books->actions().at(0)->trigger();  // (the one in front: nothing)
        QCOMPARE(w->current_document(), 0);
        books->actions().at(1)->trigger();
        QCOMPARE(w->current_document(), 1);
        QCOMPARE(w->book().title, std::string("別の本"));
    }

    void theToolsAreOneGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::map<QString, QString> tools = {{"select", "act_select"},    {"pen", "act_pen"},       {"eraser", "act_eraser"},
                                                  {"frame", "act_frame"},      {"move", "act_move"},     {"zoom", "act_zoom_tool"},
                                                  {"rect", "act_marquee"},     {"lasso", "act_lasso"},   {"wand", "act_wand"},
                                                  {"ellipse", "act_sel_ellipse"}, {"polyline", "act_sel_polyline"},
                                                  {"selpen", "act_sel_pen"}, {"selerase", "act_sel_erase"}, {"magiceraser", "act_magic_eraser"},
                                                  {"picker", "act_picker"},   {"fill", "act_fill"},     {"lassofill", "act_lassofill"},
                                                  {"gradient", "act_gradient"}, {"shape", "act_shape"}, {"blend", "act_blend"},
                                                  {"liquify", "act_liquify"}, {"reshape", "act_reshape"}, {"vector", "act_vector"},
                                                  {"ruler", "act_ruler"},     {"3d", "act_3d"},         {"effect", "act_effect"},
                                                  {"stamp", "act_stamp"},     {"text", "act_text"},
                                                  // (PC3 C-a: ペンツール, 曲線ペンツール, パス選択ツール)
                                                  {"pathpen", "act_path_pen"}, {"curvepen", "act_curvature_pen"}, {"pathselect", "act_path_select"},
                                                  // (PC3 C-e: 切り抜きツール)
                                                  {"crop", "act_crop_tool"},
                                                  // (PC3 B4: the retouching tools)
                                                  {"spotheal", "act_spot_healing"}, {"heal", "act_healing"}, {"clone", "act_clone_stamp"},
                                                  {"historybrush", "act_history_brush"},
                                                  // (PC3 C-b: the shape tools besides 長方形ツール, the 図形 of before)
                                                  {"shape_ellipse", "act_shape_ellipse"}, {"shape_triangle", "act_shape_triangle"},
                                                  {"shape_polygon", "act_shape_polygon"}, {"shape_line", "act_shape_line"},
                                                  {"shape_custom", "act_shape_custom"},
                                                  // (PC3 B3: クイック選択ツール, オブジェクト選択ツール)
                                                  {"quick", "act_quick_select"}, {"object", "act_object_select"},
                                                  // (PC4 E1: 鉛筆ツール)
                                                  {"pencil", "act_pencil"},
                                                  // (PC4 E5: the brushes that change pixels)
                                                  {"blurtool", "act_blur_tool"}, {"sharpentool", "act_sharpen_tool"}, {"smudgetool", "act_smudge_tool"},
                                                  {"dodge", "act_dodge_tool"}, {"burn", "act_burn_tool"}, {"sponge", "act_sponge_tool"},
                                                  {"mixer", "act_mixer_brush"}, {"bgeraser", "act_background_eraser"},
                                                  {"colorreplace", "act_color_replacement"},
                                                  // (PC5 G-a: 横書き文字ツール)
                                                  {"text_h", "act_text_h"}};
        QCOMPARE(w->tool_actions().size(), tools.size());
        QVERIFY(w->tools_group()->isExclusive());
        QVERIFY(w->action(QStringLiteral("act_select"))->isChecked());
        for (const auto& [tool, attribute] : tools) {
            QAction* a = w->tool_actions().at(tool);
            QCOMPARE(a, w->action(attribute));
            QCOMPARE(a->actionGroup(), w->tools_group());
            QVERIFY(!a->autoRepeat());  // (a held key chooses the tool once)
            studio.trigger(attribute.toLatin1().constData());
            QCOMPARE(studio.canvas()->tool(), MainWindow::is_marquee_tool(tool) ? QStringLiteral("marquee")
                                              : MainWindow::is_shape_tool(tool) ? QStringLiteral("shape")  // (PC3 C-b)
                                                                                 : tool);
            QVERIFY(a->isChecked());
            for (const auto& [other, other_attribute] : tools) {
                if (other != tool) QVERIFY(!w->action(other_attribute)->isChecked());
            }
        }
    }

    void theBorderKindsAreAGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::vector<std::pair<QString, std::string>> kinds = {
            {"枠線: 実線", "solid"}, {"枠線: 二重線", "double"}, {"枠線: 破線", "dashed"}, {"枠線: 点線", "dotted"}, {"枠線: 手描き風", "rough"}};
        QCOMPARE(w->border_kind_actions().size(), kinds.size());
        studio.choose(*studio.page().leaf_frames().front());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            QAction* a = w->border_kind_actions()[i];
            QCOMPARE(a->text(), kinds[i].first);
            QVERIFY(menus_of(w, a).contains(QStringLiteral("ページ/コマ")));
            a->trigger();
            const core::Frame& frame = studio.selected();
            if (kinds[i].second == "solid") {
                QVERIFY(!frame.line || frame.line->is_null());  // (a plain solid border is no style at all)
            } else {
                QVERIFY(frame.line.has_value());
                QCOMPARE((*frame.line)["kind"].get<std::string>(), kinds[i].second);
            }
        }
        // back to solid from another kind: the style goes
        w->border_kind_actions()[0]->trigger();
        QVERIFY(!studio.selected().line || studio.selected().line->is_null());
    }

    void theStagesAreAGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::vector<std::tuple<QString, QString, bool, bool>> stages = {
            // key, label, 全体図 shown, ページ shown
            {"name", "ネーム", false, true}, {"ink", "作画", true, false}, {"finish", "仕上げ", false, false},
            {"letter", "写植", false, true}, {"review", "承認", false, true}};
        QCOMPARE(w->stage_actions().size(), stages.size());
        for (const auto& [key, label, navigator, pages] : stages) {
            QAction* a = w->stage_actions().at(key);
            QCOMPARE(a->text(), label + QStringLiteral("の並び"));
            QCOMPARE(a->statusTip(), QStringLiteral("この段階でよく使うパネルだけを出します"));
            QVERIFY(menus_of(w, a).contains(QStringLiteral("ウィンドウ/作業の段階")));
            a->trigger();
            QCOMPARE(genko::app::settings()->value(QStringLiteral("ui/stage")).toString(), key);
            QCOMPARE(w->last_notice(), QStringLiteral("「%1」の並びにしました（ウィンドウ → 作業の段階）").arg(label));
            for (QDockWidget* dock : w->findChildren<QDockWidget*>()) {
                if (dock->windowTitle() == QLatin1String("全体図")) QCOMPARE(dock->isVisible(), navigator);
                if (dock->windowTitle() == QLatin1String("ページ")) QCOMPARE(dock->isVisible(), pages);
            }
        }
    }

    void undoAndRedo() {
        Studio studio;
        studio.window->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3), c + QPointF(20, 5)});
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{1});
        studio.trigger("act_undo");
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{0});
        studio.trigger("act_redo");
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{1});
        // a saved change goes back through the book's journal
        QVERIFY(wait_for([&] { return studio.session->status().kind == SaveKind::Saved; }));
        studio.trigger("act_undo");
        QVERIFY(wait_for([&] { return ink_strokes(studio.doc()) == 0 && studio.session->status().kind == SaveKind::Saved; }));
        QCOMPARE(ink_strokes(read_book(studio.book)), std::size_t{0});
        // nothing more to take back: said in the status line, the book as it was
        studio.trigger("act_undo");
        QVERIFY(wait_for([&] { return !studio.session->job_running(); }));
        studio.trigger("act_undo");
        QVERIFY(!studio.window->last_notice().isEmpty());
    }

    void theViewCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        PageCanvas* c = studio.canvas();
        c->fit_page();
        const double fitted = c->scale();
        studio.trigger("act_zoom_in");
        QVERIFY(qFuzzyCompare(c->scale(), fitted * 1.25));
        QVERIFY(!c->fitted());
        studio.trigger("act_zoom_out");
        QVERIFY(qFuzzyCompare(c->scale(), fitted * 1.25 * 0.8));
        studio.trigger("act_actual");
        QCOMPARE(c->zoom_percent(), 100);
        studio.trigger("act_fit");
        QVERIFY(c->fitted());
        QVERIFY(qFuzzyCompare(c->scale(), fitted));
        studio.answers->responder->get_int = [](const QString&, const QString& label, int, int lo, int hi) {
            return label == QStringLiteral("倍率（%。100 で紙の大きさ）") && lo == 5 && hi == 6400 ? std::optional<int>(250) : std::nullopt;
        };
        studio.trigger("act_zoom_value");
        QCOMPARE(c->zoom_percent(), 250);
        studio.trigger("act_turn_left");
        QCOMPARE(c->rotation(), -15.0);
        studio.trigger("act_turn_right");
        studio.trigger("act_turn_right");
        QCOMPARE(c->rotation(), 15.0);
        studio.trigger("act_mirror");
        QVERIFY(c->flipped());
        QVERIFY(w->action(QStringLiteral("act_mirror"))->isChecked());
        QVERIFY(w->zoom_label()->text().contains(QStringLiteral("回転 +15°")));
        QVERIFY(w->zoom_label()->text().contains(QStringLiteral("左右反転")));
        studio.trigger("act_turn_reset");
        QCOMPARE(c->rotation(), 0.0);
        QVERIFY(!c->flipped());
        QVERIFY(!w->action(QStringLiteral("act_mirror"))->isChecked());
        studio.trigger("act_zoom_tool");
        QCOMPARE(c->tool(), QStringLiteral("zoom"));
        studio.trigger("act_guides");
        QVERIFY(!c->show_guides);
        studio.trigger("act_guides");
        QVERIFY(c->show_guides);
        studio.trigger("act_cmyk_proof");  // (its pictures: test_gui_color cmykProofShowsThePageAsItPrints)
        QVERIFY(c->renderer().cmyk_proof());
        studio.trigger("act_cmyk_proof");
        QVERIFY(!c->renderer().cmyk_proof());
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_next");
        QCOMPARE(w->page_index(), 1);
        QCOMPARE(c->page_index(), std::size_t{1});
        studio.trigger("act_prev");
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_prev");  // (no page before the first)
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_overview");
        genko::app::PageOverview* overview = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* o = qobject_cast<genko::app::PageOverview*>(top); o != nullptr && o->isVisible()) overview = o;
        }
        QVERIFY(overview != nullptr);
        QCOMPARE(overview->list()->count(), 3);
        overview->close();
    }

    void thePenCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        studio.answers->responder->colour = [](const QColor&, const QString& title) {
            return title == QStringLiteral("カラーピッカー（描画色）") ? std::optional<QColor>(QColor(200, 30, 40)) : std::nullopt;
        };
        studio.trigger("act_color");
        QCOMPARE(w->pen().rgb, (std::vector<std::int64_t>{200, 30, 40}));
        QCOMPARE(studio.canvas()->live_pen()["rgb"], Json::array({200, 30, 40}));
        // a line, then 選んだ点を太く on its second point (the vector tool that chooses points comes with M3)
        w->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3), c + QPointF(20, 5)});
        const std::string id = ink_of(studio.page())->strokes->items.at(0)->id;
        studio.trigger("act_point_wider");
        QCOMPARE(w->last_notice(), QStringLiteral("先に「部分選択ツール」で線を選び、□（制御点）をクリックします"));
        studio.canvas()->vector_ids = {id};
        studio.canvas()->vector_point = 1;
        const auto before = studio.doc().pages[0];
        studio.trigger("act_point_wider");
        if (genko::render::ops_registry().find("vector_edit") != nullptr) {
            const core::Stroke& s = *ink_of(studio.page())->strokes->items.at(0);
            QVERIFY(s.pressure.at(1) > 0.7);
        } else {
            // (not in this build yet: refused in the person's words, the book untouched)
            QCOMPARE(w->last_error(), QStringLiteral("この版の Genko では、まだその操作（vector_edit）はできません"));
            QCOMPARE(studio.doc().pages[0], before);
        }
        // the layer-move tool: a drag moves the layer drawn on (transform_area, M3)
        studio.trigger("act_move");
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("move"));
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(5, 0), c + QPointF(10, 0)});
        if (genko::render::ops_registry().find("transform_area") == nullptr) {
            QCOMPARE(w->last_error(), QStringLiteral("この版の Genko では、まだその操作（transform_area）はできません"));
            QCOMPARE(studio.doc().pages[0], before);
        } else {
            QVERIFY(studio.doc().pages[0] != before);
        }
    }

    void thePanelCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // nothing chosen: the commands say what to do first
        studio.trigger("act_split_h");
        QCOMPARE(w->last_notice(), QStringLiteral("先にコマをクリックして選びます（選択ツール）"));
        studio.trigger("act_border");
        QCOMPARE(w->last_notice(), QStringLiteral("先にコマをクリックして選びます"));
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_h");
        QCOMPARE(studio.leaves(), std::size_t{2});
        QCOMPARE(studio.ops.back()[0]["op"].get<std::string>(), std::string("split_frame"));
        QCOMPARE(studio.ops.back()[0]["axis"].get<std::string>(), std::string("horizontal"));
        QCOMPARE(studio.ops.back()[0]["gutter_mm"].get<double>(), 6.0);  // (the gutter between tiers)
        // コマ間隔の設定: the dialog's two spins, kept in the settings, used by the next cut
        studio.answers->responder->exec = [](QDialog* dialog) {
            auto* h = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("horizontal"));
            auto* v = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("vertical"));
            if (h == nullptr || v == nullptr || dialog->windowTitle() != QStringLiteral("コマ間隔")) return 0;
            h->setValue(8.5);
            v->setValue(2.5);
            return static_cast<int>(QDialog::Accepted);
        };
        studio.trigger("act_gutters");
        QCOMPARE(genko::app::settings()->value(QStringLiteral("gutter_vertical")).toDouble(), 2.5);
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_v");
        QCOMPARE(studio.leaves(), std::size_t{3});
        QCOMPARE(studio.ops.back()[0]["axis"].get<std::string>(), std::string("vertical"));
        QCOMPARE(studio.ops.back()[0]["gutter_mm"].get<double>(), 2.5);
        // the chosen panel's look
        studio.choose(*studio.page().leaf_frames().back());
        studio.answers->responder->get_double = [](const QString& title, const QString&, double, double, double hi, int) {
            if (title == QStringLiteral("枠線の太さ") && hi == 5.0) return std::optional<double>(1.5);
            if (title == QStringLiteral("角の丸み") && hi == 50.0) return std::optional<double>(4.0);
            return std::optional<double>();
        };
        studio.trigger("act_border");
        QCOMPARE(studio.selected().border_mm, 1.5);
        studio.trigger("act_corner");
        QCOMPARE(studio.selected().corner_mm, 4.0);
        studio.trigger("act_no_border");
        QCOMPARE(studio.selected().border_mm, 0.0);
        studio.trigger("act_bleed");
        QVERIFY(studio.selected().bleed);
        QCOMPARE(w->last_notice(), QStringLiteral("断ち切りにしました（紙の端に接する辺は枠線なし）"));
        studio.trigger("act_bleed");
        QVERIFY(!studio.selected().bleed);
        studio.answers->responder->colour = [](const QColor& now, const QString& title) {
            return title == QStringLiteral("枠線の色") && now == QColor(20, 20, 20) ? std::optional<QColor>(QColor(10, 120, 200)) : std::nullopt;
        };
        studio.trigger("act_border_colour");
        QCOMPARE(*studio.selected().line, Json::object({{"kind", "solid"}, {"rgb", Json::array({10, 120, 200})}}));
        studio.trigger("act_frame_numbers");
        QVERIFY(studio.canvas()->show_frame_numbers);
        studio.trigger("act_frame_selection");
        QVERIFY(studio.canvas()->selection().has_value());
        QVERIFY(studio.canvas()->selection()->outline.size() >= 4);
        QCOMPARE(w->last_notice(), QStringLiteral("コマの形を選択範囲にしました"));
        // 結合: a panel of the second cut, back with its neighbour (its split undone); 消す: one panel gone, the others
        // kept
        const core::Frame* nested = nullptr;
        for (const core::Frame* leaf : studio.page().leaf_frames()) {
            if (studio.page().parent_of(leaf->id) != &studio.page().frames[0]) nested = leaf;
        }
        QVERIFY(nested != nullptr);
        studio.choose(*nested);
        studio.trigger("act_merge");
        QCOMPARE(studio.leaves(), std::size_t{2});
        const auto kept = studio.page().leaf_frames().back()->id;
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_delete_frame");
        QCOMPARE(studio.leaves(), std::size_t{1});
        QCOMPARE(studio.page().leaf_frames().front()->id, kept);
    }

    void theTemplates() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // 今のコマ割りをテンプレートに残す
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_h");
        studio.answers->responder->get_text = [](const QString& title, const QString&, const QString& suggested) {
            return title == QStringLiteral("テンプレートに残す") && suggested == QStringLiteral("試し 1 ページ") ? std::optional<QString>(QStringLiteral("わたしの二段"))
                                                                                                         : std::nullopt;
        };
        studio.trigger("act_save_template");
        const auto mine = genko::app::templates::mine();
        QVERIFY(std::any_of(mine.begin(), mine.end(), [](const auto& t) { return t.key == "わたしの二段"; }));
        QVERIFY(w->last_notice().startsWith(QStringLiteral("コマ割りを「わたしの二段」として残しました")));
        // テンプレートでコマを割る: page 2 (blank) with the first built-in template
        studio.trigger("act_next");
        bool mine_first = false;
        studio.answers->responder->exec = [&mine_first](QDialog* dialog) {
            auto* d = qobject_cast<genko::app::TemplateDialog*>(dialog);
            if (d == nullptr) return 0;
            mine_first = d->list()->item(0)->text() == QStringLiteral("自分: わたしの二段");  // (a person's own layouts first)
            for (int row = 0; row < d->list()->count(); ++row) {
                if (d->list()->item(row)->text() == QStringLiteral("4段の標準的な割り")) d->list()->setCurrentRow(row);
            }
            d->choose();
            return d->result();
        };
        studio.trigger("act_template");
        QVERIFY(mine_first);
        QCOMPARE(studio.leaves(), std::size_t{6});
        const std::size_t made = studio.leaves();
        // one undo takes the whole template back
        studio.trigger("act_undo");
        QCOMPARE(studio.leaves(), std::size_t{1});
        studio.trigger("act_redo");
        QCOMPARE(studio.leaves(), made);
        // a page with 台詞 and drawn panels: made blank first as Python's dialog does (its lines deleted, the drawn panels
        // set back to the basic frame), then the template (M7: this build refused both)
        const std::size_t at = static_cast<std::size_t>(w->page_index());
        const core::Num number = studio.doc().pages[at]->index;
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", number.json()}, {"text", "消える台詞"}, {"x_mm", 40}, {"y_mm", 60}}})));
        const core::Rect inner = studio.doc().pages[at]->inner_rect_mm();
        const double x = inner.x.value(), y = inner.y.value(), wd = inner.width.value(), ht = inner.height.value();
        const Json tree = Json{{"rect_mm", Json::array({x, y, wd, ht})}, {"axis", "free"},
                               {"children", Json::array({Json{{"rect_mm", Json::array({x, y, wd, ht / 2})}, {"poly", Json::array({Json::array({x, y}), Json::array({x + wd, y}), Json::array({x + wd, y + ht / 3}), Json::array({x, y + ht / 2})})}},
                                                          Json{{"rect_mm", Json::array({x, y + ht / 3, wd, ht * 2 / 3})}, {"poly", Json::array({Json::array({x, y + ht / 2}), Json::array({x + wd, y + ht / 3}), Json::array({x + wd, y + ht}), Json::array({x, y + ht})})}}})}};
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layout"}, {"page", number.json()}, {"tree", tree}, {"force", true}}})));
        QVERIFY(core::is_free(studio.doc().pages[at]->frames[0]));
        studio.answers->asked.clear();
        studio.answers->responder->question = [](const QString&, const QString& text) {
            return text.contains(QStringLiteral("コマと台詞を消して、テンプレートで割り直します"));  // (asked first, as Python's)
        };
        studio.trigger("act_template");
        QVERIFY2(studio.doc().story_for_page(number).empty(), qPrintable(studio.answers->asked.join(QStringLiteral(" | "))));
        QVERIFY(!core::is_free(studio.doc().pages[at]->frames[0]));
        QCOMPARE(studio.leaves(), std::size_t{6});
    }

    void thePageCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::string first = studio.doc().pages[0]->id;
        const std::string second = studio.doc().pages[1]->id;
        studio.trigger("act_add_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{4});
        QCOMPARE(w->page_index(), 1);  // (the new page, after the first)
        QVERIFY(studio.doc().pages[1]->id != second);
        studio.trigger("act_dup_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{5});
        // このページを消す…: asked first (no: kept)
        studio.trigger("act_del_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{5});
        QVERIFY(studio.answers->asked.last().contains(QStringLiteral("ページを消しますか？")));
        studio.answers->responder->question = [](const QString&, const QString& text) { return text.contains(QStringLiteral("ページを消しますか？")); };
        studio.trigger("act_del_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{4});
        // the order: the page goes with the person's view
        w->select_page(0);
        studio.trigger("act_page_down");
        QCOMPARE(studio.doc().pages[1]->id, first);
        QCOMPARE(w->page_index(), 1);
        studio.trigger("act_page_up");
        QCOMPARE(studio.doc().pages[0]->id, first);
        QCOMPARE(w->page_index(), 0);
        // ネーム完了 is for books made with agents: hidden here
        QVERIFY(!studio.act("act_name_ok")->isVisible());
        studio.trigger("act_name_ok");  // (the person's own approval of the name: allowed for a person)
        QVERIFY(studio.page().name_ok);
    }

    void theNameIsApprovedInAnAgentsBook() {
        Studio studio(2, /*agent=*/true);
        QVERIFY(studio.act("act_name_ok")->isVisible());
        QVERIFY(!studio.page().name_ok);
        studio.trigger("act_name_ok");
        QVERIFY(studio.page().name_ok);
        QVERIFY(wait_for([&] { return studio.session->status().kind == SaveKind::Saved; }));
        QVERIFY(read_book(studio.book).page(0).name_ok);
    }

    void theBooksAndWindows() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // 保存: written now
        w->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3)});
        studio.trigger("act_save");
        QCOMPARE(studio.session->status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes(read_book(studio.book)), std::size_t{1});
        QVERIFY(w->last_notice().startsWith(QStringLiteral("保存しました: ")));
        // 開く…: another book in a new tab
        const fs::path other = path_of(studio.dir.filePath("other.genko"));
        write_book(other, new_doc(2, "別の本"));
        // (PC4 F-b: Genko's own open window — a .genko folder or a file, app/open_dialog.hpp — in place of the folder's)
        studio.answers->responder->exec = [other](QDialog* dialog) {
            auto* open = qobject_cast<genko::app::OpenDialog*>(dialog);
            if (open == nullptr) return 0;
            open->choose({qpath(other)});
            return static_cast<int>(QDialog::Accepted);
        };
        studio.trigger("act_open");
        QVERIFY(wait_for([&] { return w->documents().size() == 2; }));
        QCOMPARE(w->book().title, std::string("別の本"));
        QCOMPARE(w->doc_tabs()->count(), 2);
        // 次の原稿 / 前の原稿
        studio.trigger("act_next_doc");
        QCOMPARE(w->current_document(), 0);
        QCOMPARE(w->book().title, std::string("試し"));
        studio.trigger("act_prev_doc");
        QCOMPARE(w->current_document(), 1);
        // 新規… (新しい原稿…): the dialog makes the book; it opens in a tab
        studio.answers->responder->exec = [&studio](QDialog* dialog) {
            auto* d = qobject_cast<genko::app::NewProjectDialog*>(dialog);
            if (d == nullptr) return 0;
            d->title->setText(QStringLiteral("新しい本"));
            d->folder->setText(studio.dir.path());
            return d->create() ? static_cast<int>(QDialog::Accepted) : 0;
        };
        studio.trigger("act_new");
        QVERIFY(wait_for([&] { return w->documents().size() == 3; }));
        QCOMPARE(w->book().title, std::string("新しい本"));
        QVERIFY(w->session().path().has_value());
        QVERIFY(fs::exists(*w->session().path() / "project.json"));
        // 別名で保存… (別の場所に保存…): a copy with a new book id; the work goes on there
        const std::string old_id = w->book().book_id;
        const fs::path copy = path_of(studio.dir.filePath("写し"));
        studio.answers->responder->save_path = [copy](const QString& caption, const QString&) {
            return caption == QStringLiteral("原稿を保存する場所") ? qpath(copy) : QString();
        };
        studio.trigger("act_save_as");
        const fs::path copied = path_of(studio.dir.filePath("写し.genko"));
        QCOMPARE(*w->session().path(), copied);
        QVERIFY(read_book(copied).book_id != old_id);
        QCOMPARE(w->session().status().kind, SaveKind::Saved);
        // 閉じる: the tab (its book saved: nothing asked)
        studio.trigger("act_close");
        QCOMPARE(w->documents().size(), std::size_t{2});
        // 新しいウィンドウ（同じ原稿）: the same session in another window
        const std::size_t windows = genko::app::documents::windows().size();
        studio.trigger("act_new_window");
        const auto all = genko::app::documents::windows();
        QCOMPARE(all.size(), windows + 1);
        MainWindow* second = all.back();
        QCOMPARE(second->session_ptr(), w->session_ptr());
        second->close();
        QCOMPARE(genko::app::documents::windows().size(), windows);
        // 終了 (Genko を終わる): every window (each book saved: nothing asked)
        studio.trigger("act_quit");
        QVERIFY(w->closed());
        QVERIFY(!studio.answers->asked.contains(QStringLiteral("genko::app::CloseGuard")));
    }
};

QTEST_MAIN(TestGuiActions)
#include "test_gui_actions.moc"
