#pragma once

#include <QAction>
#include <QActionGroup>
#include <QFileSystemWatcher>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/ai_dialog.hpp"
#include "app/canvas.hpp"
#include "app/pen.hpp"
#include "app/session.hpp"
#include "render/image.hpp"
#include "render/proof.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QFormLayout;
class QTabBar;
class QToolBar;
class QMenu;
class QDockWidget;
class QPushButton;
class QVBoxLayout;

// The main window (Python's genko/app/main.py MainWindow): the page in the middle, the tools on the left, the
// commands and the open books' tabs above, the navigator and the page list beside it, and in the status bar the page,
// the save state with where the book is kept, and the zoom. The menus hold the commands of M2 with Python's words and
// keys (docs/cpp-migration/ledger.json: the 52 gui actions of M2 and their groups).
//
// Every change goes through the book's Session (and so the CommandBus and the Saver): nothing here writes the book or
// its files. A book with changes not saved (or a save that failed, or one running) is never closed without asking.

namespace genko::app {

class BrushPanel;
class ChannelPanel;
class CloneSourcePanel;
class PathsPanel;
class ShapesPanel;
class HistogramPanel;
class InfoPanel;
class CheckPanel;
class ColourPanel;
class HistoryPanel;
class GuidePanel;
class SubView;
class MaterialPanel;
class LayerPanel;
class TimelinePanel;
class ToolSettings;
class TextToolSettings;
class StoryPanel;
class StoryEditor;
class ReplaceDialog;
class ApprovalBox;
class PanelView;
class QuickAccess;
class CommandSearch;
class CanvasOnly;
class RestReminder;
class RadialMenu;
class Library;
class ProcessBar;

class Navigator;
class PageList;
class SaveFailureBar;
class SaveStatusLabel;
class ImeEntry;

// 色の校正・校正の設定・色域外警告 of one book in a window (SPEC COLOR-03 PC1b; PhotoCraft's ProofView): display only,
// never the book's.
struct ProofView {
    // "working": カラー設定's 作業用 CMYK; "export": the CMYK profile chosen for export or Genko's own conversion
    // (Python's 「CMYK で見る」); else a built-in profile's id or an ICC file's path (カスタム…)
    QString profile = QStringLiteral("working");
    render::proof::Kind kind = render::proof::Kind::Profile;
    int plate = 3;
    QString intent = QStringLiteral("relative");
    bool bpc = true;
    bool simulate_paper = false;
    bool on = false;      // 色の校正
    bool gamut = false;   // 色域外警告
};

// One open book in a window: its session and how the window last showed it.
struct Document {
    explicit Document(std::shared_ptr<Session> s = nullptr) : session(std::move(s)) {}

    std::shared_ptr<Session> session;
    int page_index = 0;
    std::optional<ViewState> view;
    std::optional<std::string> target_layer;
    ProofView proof;
    // smart-objects@1 (PC3 A1, the user's decision 1: コンテンツを編集 in a tab of the same book): the smart object
    // contents this tab shows (its page; every op it sends goes there), and the name of the layer it was opened from.
    std::optional<std::string> contents;
    QString contents_title;
    QString title() const;
};

// transform_matrix: [a, b, c, d, e, f] (x' = ax + cy + e, y' = bx + dy + f): scale, then turn (degrees, clockwise on
// the page), both about `pivot`, then move by (dx, dy) mm.
std::array<double, 6> transform_matrix(QPointF pivot, double dx = 0, double dy = 0, double sx = 1, double sy = 1, double angle = 0);

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class GuidePanel;     // (its edits are the window's: the chosen panel, the layer drawn on)
    friend class MaterialPanel;  // (likewise)
    friend class StoryPanel;     // (the chosen panel, a line's panel)
    friend class HistoryPanel;   // (after its undo and redo: the book watched and shown again)
    friend class InfoPanel;      // (PC2d: the page's values in the status bar's words)

public:
    // A window on a book (shared with any other window on it); none: a new untitled book in memory (8 pages, A4).
    explicit MainWindow(std::shared_ptr<Session> session = nullptr);
    ~MainWindow() override;

    // --- the book in front ------------------------------------------------------------------------------------
    Session& session() const { return *session_; }
    const std::shared_ptr<Session>& session_ptr() const { return session_; }
    const core::Document& book() const { return session_->document(); }
    const core::Page* current_page() const;
    int page_index() const { return page_index_; }
    // The layer the pen draws on (the one chosen, else the ink — or the name, for a book made with agents at its name
    // stage).
    const core::Layer* target_layer() const;
    void set_target_layer(const std::string& layer_id);
    // A pen, paint or tone layer, not locked.
    static bool drawable(const core::Layer& layer);
    // The selection as an op's area ({"poly": …} or {"mask": …}), or none.
    std::optional<core::Json> selection_area() const;
    // 自動選択ツール's 色の近さ: how different a colour may be and still be the same (許容値, 0..255); only the touching
    // ones (隣接).
    double colour_tolerance = 24;
    bool colour_contiguous = false;
    // 自動選択ツール (PC3 B1, the user's decision 5): what it looks at — 線で囲まれた所 ("lines": the closed region, as the
    // fill tool finds it) or 色の近さ ("colour": the colours within 許容値 of the one clicked, as Photoshop's magic wand) —
    // and, for the colour, whether it sees the page as it shows (全レイヤーを対象) or the drawing layer alone.
    QString wand_criterion = QStringLiteral("lines");
    bool wand_all_layers = true;
    // マジック消しゴムツール (PC3 B1): its 許容値 (0..255), 隣接, アンチエイリアス, 全レイヤーを対象 and 不透明度 (%).
    struct MagicEraserSettings {
        double tolerance = 32;
        bool contiguous = true;
        bool antialias = true;
        bool all_layers = false;
        double opacity = 100;
    };
    MagicEraserSettings magic_eraser_settings;
    // 修復の道具 (PC3 B4, retouch_tools.cpp): the brush (直径 mm; 硬さ・不透明度・流量・間隔 %; 描画モード), the Clone Stamp's
    // and the Healing Brush's 調整あり and サンプル (current | current_and_below | all), the Spot Healing Brush's 種類
    // (content_aware | create_texture | proximity_match).
    struct RetouchSettings {
        double size_mm = 5;
        double hardness = 100;
        double opacity = 100;
        double flow = 100;
        double spacing = 25;
        std::string blend = "normal";
        bool aligned = true;
        std::string sample = "current";
        std::string spot_type = "content_aware";
    };
    RetouchSettings retouch_settings;
    // コピーソース (window.panel.cloneSource): five sources — a point set by Alt+click (mm) on a page, the destination the
    // first stroke after it paired with it (調整あり), the layer it was set on, 幅・高さ (%), 回転 (°) and flips — the one
    // in use and the overlay (表示・不透明度 %・クリップ・自動的に隠す・反転・描画モード). The window's, not the book's (as
    // Photoshop keeps them).
    struct CloneSource {
        std::optional<QPointF> source;
        std::optional<QPointF> anchor;
        std::string page_id;
        std::string layer_id;
        double width = 100, height = 100, rotation = 0;
        bool flip_h = false, flip_v = false;
    };
    struct CloneSources {
        std::array<CloneSource, 5> items;
        int active = 0;
        bool overlay = true;
        double opacity = 100;
        bool clipped = true, auto_hide = true, invert = false;
        std::string blend = "normal";
        CloneSource& now() { return items[static_cast<std::size_t>(std::clamp(active, 0, 4))]; }
    };
    CloneSources clone_sources;
    // The clone source set and shown again (the panel and the canvas's cross and overlay) after it changed.
    void clone_sources_follow();
    CloneSourcePanel* clone_source_panel() const { return clone_source_panel_; }
    // ヒストリーブラシのソース: 履歴's row painted back from (none: （はじめ）, the first row).
    std::optional<int> history_brush_row;
    // コンテンツに応じた塗りつぶし… (編集; retouch_tools.cpp).
    void content_aware_fill_dialog();
    // PC3 B3 (main_window_smart_select.cpp): クイック選択ツール's エッジを強調, and whether the smart selection (クイック選択・
    // オブジェクト選択・被写体を選択・空を選択・フォーカス領域・選択とマスク) sees the page as it shows (全レイヤーを対象) or the
    // drawing layer alone — by default the layer, as Photoshop's: the page as it shows has the panels' borders and the
    // nombre, which the heuristics take for things. (The brush's 直径 is the canvas's quick_size_mm.)
    struct SmartSelectSettings {
        bool enhance_edge = false;
        bool all_layers = false;
    };
    SmartSelectSettings smart_select_settings;
    // The smart selection's work on its worker thread (a progress with やめる; Esc stops it): whether one is running, and
    // stopping it (nothing changes then).
    bool smart_select_busy() const;
    void cancel_smart_select();
    // The tools of the marquee (rect, lasso, wand, ellipse, polyline, selpen, selerase), and the one of a way.
    static bool is_marquee_tool(const QString& tool);
    static QString marquee_tool_of(const QString& way);
    // The shape tools (PC3 C-b: shape — 長方形ツール —, shape_ellipse, shape_triangle, shape_polygon, shape_line,
    // shape_custom: the canvas's one tool "shape" in one of its figures), and the one of a figure.
    static bool is_shape_tool(const QString& tool);
    static QString shape_tool_of(const QString& kind);
    // What a question or a dialog is about to be asked about (the book in front as it is now, and its page), and
    // whether it is still so when the answer comes (otherwise a notice, and nothing is done): the book may be read
    // further or changed from outside while it waits, and the pages and layers asked about are gone then.
    struct Asked {
        std::shared_ptr<Session> session;
        DocPtr snapshot;
        int page_index = 0;
    };
    Asked asking() const { return Asked{session_, session_->snapshot(), page_index_}; }
    bool still(const Asked& asked) { return modal_target_unchanged(asked.session, asked.snapshot, asked.page_index); }
    bool modal_target_unchanged(const std::shared_ptr<Session>& origin, const DocPtr& snapshot, int page_index);
    // The page shown as these ops would leave it, without changing the book (フィルターのプレビュー); none: as it is.
    void preview_ops(const std::optional<core::Json>& ops);
    LayerPanel* layer_panel() const { return layer_panel_; }
    // channels@1 (main_window_channels.cpp): the channel panel, and what the pixel tools work on and what is shown of a
    // page's channels — the window's, never saved; each page its own —: the target ("" the composite, a colour plate's
    // name, an alpha or spot channel's id; the pixel ops sent are given it as their "channel"), the colour plates
    // hidden, the alpha channels laid over in their colours, the spot channels not printed on screen.
    struct ChannelView {
        std::string target;
        std::vector<bool> hidden;
        std::set<std::string> shown;
        std::set<std::string> hidden_spots;
    };
    ChannelPanel* channel_panel() const { return channel_panel_; }
    // vector@1 (PC3 C-a, app/main_window_paths.cpp): the Paths panel (ウィンドウ → パス) and the paths' commands — the target
    // path (the panel's row: PageCanvas::target_path) filled, stroked, made the selection, a work path made from the
    // selection, the vector mask of the layer drawn on made from it; each an op through apply_ops.
    PathsPanel* paths_panel() const { return paths_panel_; }
    QDockWidget* paths_dock() const { return paths_dock_; }
    void path_fill();
    void path_stroke();
    void path_to_selection();
    bool work_path_from_selection(std::optional<double> tolerance_mm = std::nullopt);
    void line_work_path();
    // ("reveal_all", "hide_all", "current_path", "delete", "enabled", "linked", "rasterize": レイヤー → ベクトルマスク's)
    void vector_mask_command(const std::string& what);
    bool export_paths_ai(std::optional<QString> file = std::nullopt);
    // The target path as an op names it: {"name": "work" | a saved path's name} or {"layer": the layer whose vector mask it is}
    // or {"shape": {"layer", "id"}} (a pen layer's shape part, PC3 C-b).
    core::Json target_path_terms() const;
    // pen-shapes@1 (PC3 C-b, app/main_window_shapes.cpp): the shape tools' choices as add_shape_part's fill and stroke
    // (the tool settings: 塗り, 線), the Shapes panel (ウィンドウ → シェイプ) and the shape parts' commands — a preset
    // placed (its rect on the page, mm; none: a quarter of the page in its middle), 編集 → カスタムシェイプを定義… (the
    // target path kept under a name), レイヤー → シェイプを結合 ("combine", "subtract", "intersect", "exclude", "merge":
    // the parts of the layer drawn on), ラスタライズ → シェイプ, 選択範囲 → シェイプに変換, ページ → 台詞 → シェイプに変換 and
    // 表示 → シェイプから新規ガイドを作成.
    core::Json shape_fill_now() const;
    core::Json shape_stroke_now() const;
    ShapesPanel* shapes_panel() const { return shapes_panel_; }
    QDockWidget* shapes_dock() const { return shapes_dock_; }
    bool place_shape_preset(const QString& group, const QString& name, std::optional<std::array<double, 4>> rect = std::nullopt);
    void choose_custom_shape(const QString& group, const QString& name);
    bool define_custom_shape(std::optional<QString> name = std::nullopt);
    bool combine_shapes(const std::string& how);
    bool rasterize_shapes();
    bool selection_to_shape();
    bool line_to_shape();
    bool guides_from_shape();
    // What a conversion that flattens a page says it does to its hidden layers (画像をフラット化's words: the hidden
    // drawing layers discarded — their count when there are any —, the name, draft and frame layers kept).
    static QString flatten_discards(int discarded);
    QDockWidget* channels_dock() const { return channels_dock_; }
    // PC2d (app/info_panels.cpp): ウィンドウ → ヒストグラム and 情報 (F8), and 編集 → プロファイルの情報….
    HistogramPanel* histogram_panel() const { return histogram_panel_; }
    InfoPanel* info_panel() const { return info_panel_; }
    QDockWidget* histogram_dock() const { return histogram_dock_; }
    QDockWidget* info_dock() const { return info_dock_; }
    void profile_info_dialog();
    ChannelView channel_view() const;
    // A row of the panel made what the tools work on (PhotoCraft's click on a row: a channel shown alone, a plate alone,
    // the composite back); Ctrl+Alt+2…9 (2 the composite, then the plates, then the channels).
    void target_channel(const std::string& target);
    void target_slot(int slot);
    // A row's eye: the composite (every plate), a plate, an alpha channel laid over, a spot channel printed.
    void show_channel(const std::string& key, bool shown);
    // 選択 → 選択範囲をチャンネルに保存… / チャンネルから読み込む.
    bool save_selection_to_channel(std::optional<QString> name = std::nullopt);
    void load_selection_from_channel(const std::string& id);
    // PC3 B1: the selection's right-click menu at a point of the page (mm) — Photoshop's items (選択を解除, 再選択, 選択範囲を
    // 反転, 境界をぼかす…, 選択範囲を保存…, 選択範囲を変形, … and the later groups' as they come) — when a selection tool is in
    // hand and the point is in the selection; false (nothing added) otherwise.
    bool fill_selection_menu(QMenu& menu, const QPointF& mm);
    // A kept area (ストック) made the selection (選択 → ストックから選ぶ; the channel panel's rows).
    void select_stock(const std::string& name) { use_stock(name); }
    BrushPanel* brush_panel() const { return brush_; }
    ColourPanel* colours() const { return colours_; }
    ToolSettings* tool_settings() const { return tool_settings_; }
    // Brushes from a .genkobrush or a Photoshop .abr into one's own list: their keys (the file's errors throw).
    std::vector<std::string> import_brushes(const QString& path);
    // The chosen brush in a file (.genkobrush); none: asked where.
    bool export_brush(std::optional<QString> path = std::nullopt);
    PenSettings& pen() { return pen_; }
    void pen_changed();
    // アニメーション: the timeline panel, the frame shown of each animation page (page id → frame) and of one page
    // (none: the one in front).
    TimelinePanel* timeline() const { return timeline_; }
    std::map<std::string, std::int64_t> anim_frames;
    std::int64_t current_frame(const core::Page* page = nullptr) const;
    // A panel by its title brought to the front (タイムライン, 全体図, …).
    void show_dock(const QString& title);
    // 定規・3D: the panel; the page's 3D traced as pencil lines on the layer drawn on (only the chosen one).
    GuidePanel* guides() const { return guides_; }
    void trace_prims(bool selected_only = false);
    // 素材: the panel; a material put as Python's use_material puts it — 貼る, or a double click in the list, as in
    // Python (a brush chosen, a tone on the selection or the chosen panel, an effect in the chosen panel, else the
    // material tool waiting for a click on the page); the wait given up (Esc, a C++ addition: the tool chosen before
    // comes back); the selection's lines and fills as a part ({strokes, patches}); the next pen line as an effect's
    // path or clear middle (key: path | inner_path).
    MaterialPanel* materials() const { return materials_; }
    // サブビュー: the reference pictures beside the page.
    SubView* subview() const { return subview_; }
    void use_material(const core::Json& item);
    bool cancel_stamp();
    std::optional<core::Json> copy_selection_items();
    void draw_effect_shape(const std::string& effect_id, const std::string& key);
    // The material the material tool puts (none: the one chosen in the panel); the effect kind the effect tool puts.
    const std::optional<core::Json>& pending_material() const { return pending_material_; }
    const std::string& effect_kind() const { return effect_kind_; }
    // スキャナーから取り込む: where the scan comes from (tests; none: the scanner, scanner::scan).
    std::function<std::string(int dpi)> scanner_source;
    // 台詞 (main_window_lines.cpp): the panel, the text tool's settings, the story editor open (none: closed); a line
    // chosen (the panel shows it; open_panel: its tab comes to the front); a balloon's right-click menu (not shown).
    StoryPanel* story() const { return story_; }
    TextToolSettings* text_settings() const { return text_settings_; }
    StoryEditor* story_editor() const;
    void on_line_selected(const std::string& line_id, bool open_panel);
    std::unique_ptr<QMenu> line_menu(const std::string& line_id);
    // A line of the book by its id (none: not there).
    const core::StoryLine* line_by_id(const std::string& line_id) const;
    // 履歴 and 点検 (main_window_output.cpp): the panels; a problem the checks found shown on its page (its page in
    // front, its place marked, its line chosen or its layer drawn on, its words in the status line).
    HistoryPanel* history() const { return history_; }
    CheckPanel* checks() const { return checks_; }
    void show_issue(const core::Json& issue);

    // The studio (main_window_studio.cpp, Python's studio_widgets and ai_link in main.py): the approval box over the
    // panels while something waits for the person, the panel view and the library as tabs (books made with agents
    // only), the process bar and the AI button in the status bar; AI と作る, チャットでの承認, genko:// links.
    ApprovalBox* approvals() const { return approvals_; }
    PanelView* panel_view() const { return panel_view_; }
    Library* library() const { return library_; }
    ProcessBar* process_bar() const { return process_; }
    QPushButton* ai_button() const { return ai_button_; }
    QDockWidget* box_dock() const { return box_dock_; }
    // The book and conversation this window asks its AI to work in (AI と作る's choice, kept while the window is open).
    AiTarget& ai_target() { return ai_target_; }
    // The approval box as tall as its request needs (a preview gets room; only words and buttons otherwise).
    void fit_box();
    void open_ai_dialog();
    // The status line's AI button: who worked on the book lately (● while an AI works; C++: 失敗 when its last call
    // failed).
    void refresh_ai();
    // チャットでの承認…: whether the AI records the approvals the person sends it in a chat (this book only).
    void chat_approval();
    // A genko:// link from a web page (the style catalog's 「この絵柄を使う」); when_ready: after the book being read is
    // shown (a link that started Genko with a book).
    void open_link(const QString& link);
    void open_link_when_ready(const QString& link);
    // Keep a branch of the style catalog as this book's style (asked first when `ask`, or when the style is fixed or the
    // name is used elsewhere). False: not changed.
    bool use_style(const std::string& style_id, bool ask = true);
    // 絵柄を選ぶ…: the style picker (StylePicker) until a style is taken, Genko's own words asked for, or it is closed (a
    // "no" to use_style's question goes back to the picker).
    void pick_style();
    // New requests from the AI since `before` (their ids): a quiet note, the taskbar's mark, and (unless turned off in
    // ui/raise_requests) the approval box in front with the newest chosen.
    void notice_requests(const std::set<std::string>& before);
    // The note of new requests and the box in front (waiting while the pen is down: at most 40 tries 250 ms apart).
    void show_requests(const QString& title, const std::string& newest, const QString& more, int tries);

    PageCanvas* canvas() const { return canvas_; }
    PageList* pages() const { return pages_; }
    Navigator* navigator() const { return navigator_; }
    SaveStatusLabel* save_label() const { return save_label_; }
    SaveFailureBar* failure_bar() const { return failure_bar_; }
    QLabel* status_label() const { return status_; }
    QLabel* zoom_label() const { return zoom_label_; }
    QLabel* unported_band() const { return unported_; }
    QLabel* read_only_band() const { return read_only_; }
    QTabBar* doc_tabs() const { return doc_tabs_; }
    // (M6) The tool palette, the command bar under the menus and the quick-access panel (workspace.cpp).
    QToolBar* tool_palette() const { return palette_; }
    QToolBar* command_bar() const { return commands_; }
    QuickAccess* quick_access() const { return quick_access_; }
    QDockWidget* quick_dock() const { return quick_dock_; }
    // コマンドを探す… (the dialog, shown and modeless; tests).
    CommandSearch* find_command();
    // 道具の名前を表示: icons alone, or icons with their names (kept in the settings unless `save` is off).
    void show_tool_names(bool on, bool save = true);
    // 今の配置をワークスペースとして残す…: asked for a name when none is given; false when nothing was kept.
    bool save_workspace(std::optional<QString> name = std::nullopt);
    // はじめの配置に戻す: the panels as the window first showed them.
    void default_layout();
    // Over a book with nothing in it yet, the first steps (Python's first_steps bar: it goes with the first panel, line or
    // stroke, or with its ×); after the first panels, what comes next (I11: 描く・台詞・保存・書き出し, each marked when
    // done; × hides it and ヘルプ → 次の作業の案内を表示 brings it back). Null until first needed (tests).
    QWidget* first_steps() const { return first_steps_; }
    QWidget* next_steps() const { return next_steps_; }
    // The canvas's menu (a panel's commands when one is under the pointer), filled (tests).
    void fill_context_menu(QMenu& menu, const QString& frame_id);
    // --- 環境設定 and comfort (M6: preferences.cpp, comfort.cpp) ---------------------------------------------------
    // Each command's own keys (by its words), as the window made them (preferences::name_commands).
    std::map<QString, QList<QKeySequence>> default_shortcuts;
    // 環境設定… (the dialog, exec'ed).
    void open_preferences();
    // ヘルプ: keys | guide | faq | about, shown (modeless); the one shown last (tests).
    void show_help(const QString& what);
    QDialog* help_dialog() const { return help_dialog_; }
    // How soon a change is saved (ms), for every book of this window and the ones it opens later.
    void set_save_after(int ms);
    int save_after() const { return save_after_ms_; }
    // The tool pictures drawn again (after the screen's colours change).
    Q_INVOKABLE void refresh_icons();
    // The first steps and the next steps over the page: shown while they apply, with the keys as they are now.
    void refresh_first_steps();
    // 原稿だけを表示 (Tab), the rest reminder, and the round menu last opened at the pen (null: none open).
    CanvasOnly* canvas_only() const { return canvas_only_; }
    RestReminder* rest() const { return rest_; }
    RadialMenu* radial() const;  // (out of line: RadialMenu is only declared here)
    // キーを押している間だけ持ち替え: the tool last chosen (its key), the one before and when (seconds); the key of the
    // tool chosen last let go after a hold brings back the tool before (true when it did; `now`: tests).
    struct ToolSwitch {
        QString tool;
        QString before;
        double since = 0.0;
    };
    static constexpr double kHoldSeconds = 0.35;
    const std::optional<ToolSwitch>& tool_switch() const { return tool_switch_; }
    bool hold_key_released(int key, std::optional<double> now = std::nullopt);
    // --- オートアクション (actions.cpp: ファイル → オートアクション) -------------------------------------------------------
    // What is being recorded (the ops applied since 記録を始める; none: not recording).
    const std::optional<core::Json>& recording() const { return recording_; }
    void start_recording();
    void drop_recording();
    // Keep what was recorded under a name (asked for when not given). False: nothing kept.
    bool stop_recording(std::optional<QString> name = std::nullopt);
    // The action on the page in front (its layer drawn on, its chosen panel), or on every page but the covers (asked
    // first; one step to undo). False: nothing done.
    bool play_action(const QString& name);
    bool play_action_on_all(const QString& name);
    void remove_action(const QString& name);
    // 管理（手順を見る・並べ替える・外す）… (shown, modeless; tests)
    QDialog* manage_actions();
    QMenu* actions_menu() const { return actions_menu_; }
    // マスクを編集する (the layer panel's マスク menu): the pen and the eraser paint the mask of the layer drawn on.
    bool mask_edit = false;
    // 画像を読み込む…: a picture into the chosen panel (else over the whole page, out to the bleed), kept in the book as a
    // PNG (asked where when no path is given). False: nothing placed.
    bool import_image(std::optional<QString> path = std::nullopt);
    // 選択範囲ランチャー's buttons: (their word, the command) in order.
    const std::vector<std::pair<QString, QAction*>>& launcher_actions() const { return launcher_actions_; }
    ImeEntry* ime() const { return ime_; }
    const std::vector<Document>& documents() const { return documents_; }
    int current_document() const { return doc_; }

    // --- the commands (the actions by Python's attribute names: "act_new" …) ---------------------------------
    QAction* action(const QString& attribute) const;
    // "（B）": a command's key as it is now (the person may have changed it), for the hints; nothing when it has none.
    QString key_hint(const char* attribute) const;
    const std::map<QString, QAction*>& actions_by_name() const { return actions_; }
    const std::map<QString, QAction*>& tool_actions() const { return tool_actions_; }
    const std::vector<QAction*>& border_kind_actions() const { return border_kind_actions_; }
    const std::map<QString, QAction*>& stage_actions() const { return stage_actions_; }
    QActionGroup* tools_group() const { return tools_; }

    // --- doing things ------------------------------------------------------------------------------------------
    // Apply in memory as the person, show it at once (saved after a pause). A refusal shows in the status line.
    bool apply_ops(const core::Json& ops, const std::vector<std::string>& ids = {});
    // For approvals (Python's apply_and_commit): applied as the person and written at once, so the agent sees the
    // decision immediately.
    bool apply_and_commit(const core::Json& ops);
    void go_to_page(int index);
    void select_page(int row);
    void choose_tool(const QString& tool);
    void add_document(std::shared_ptr<Session> session);
    void open_project(const std::filesystem::path& path);
    void next_document(int step);
    void switch_document(int index);
    // Close a book's tab (asking first when it has unsaved changes); the last one closes the window. False: kept.
    bool close_document(int index = -1);
    MainWindow* new_window();
    // 色の校正・校正の設定・色域外警告・カラー設定 (main_window_color.cpp): the proof of the book in front (choose_proof:
    // an id of render/proof's kinds, "workingCmyk" or "export"; it turns 色の校正 on), shown again after カラー設定 or 環境設定
    // change; the gamut warning's count in the status bar.
    const ProofView& proof_view() const;
    void choose_proof(const QString& id);
    void apply_proof_view();
    QLabel* gamut_label() const { return gamut_label_; }
    // カラー設定's policies for a book opened (its pages of another profile than the working ones: kept, converted or
    // their profile replaced, through ops — asked first when chosen); never a book opened read-only.
    void color_policy_on_open(const std::shared_ptr<Session>& session);
    // ... and for a picture taken in (its embedded profile, or none and a working space not the default): kept as its
    // numbers (Python's) or taken to sRGB, as chosen (asked first when chosen; closed without an answer: the policy's).
    bool picture_policy(const std::string& bytes, render::Image& picture);
    // smart-objects@1 (PC3 A0, decision 2): the same policies for a file placed as a smart object, as the op's
    // embedded_profile (keep, convert: from its own profile; assign: the working space's, its bytes as "icc"), with the
    // intent and black point compensation — the file's pixels are never converted (the attributes panel changes it later).
    // `mode`: open_image's mode of the file.
    core::Json picture_use(const std::string& bytes, std::string_view mode);
    // smart-objects@1 (app/main_window_smart.cpp): ファイル → 埋め込みを配置… (a file in the middle of the page at its own
    // size, made smaller to fit), and a file placed as a smart object (画像を読み込む… with the panel chosen, else over the
    // page out to the bleed; 埋め込みを配置… as above). False when nothing was placed (said why).
    bool place_embedded(std::optional<QString> path = std::nullopt);
    // (`linked`, PC3 A1: リンクを配置… — the file's place kept with its copy)
    bool place_smart(const QString& file, bool as_placed, bool linked = false);
    // レイヤー → スマートオブジェクト・スマートフィルター・ラスタライズ, レイヤーの内容オプション…
    void smart_convert();
    void smart_copy();
    void rasterize(const std::string& what);
    void content_options();
    // PC3 A4: スタックモード ▸ (`mode` an op's name, "none": なし) of the smart object drawn on, and ラスタライズ → テキスト
    // (the line chosen with the select tool, and the lines joined to its balloon, as a new paint layer).
    void smart_stack(const std::string& mode);
    void rasterize_type();
    // smart-objects@1, PC3 A1 (app/main_window_smart.cpp): コンテンツを編集 — a page's contents in a tab of this book
    // (decision 1), a picture's made a page first (asked), a linked file opened in its own app —, コンテンツを書き出し…,
    // コンテンツを置換…, ファイルに再リンク…, 埋め込みに変換, リンクに変換…, 変更されたコンテンツを更新 (`all`: すべての),
    // ファイルマネージャーで表示, ファイル → リンクを配置… and パッケージ…. False when nothing was done (said why).
    bool edit_contents();
    bool open_contents_tab(const std::string& contents_id, const QString& title);
    bool export_contents(std::optional<QString> path = std::nullopt);
    bool replace_contents(std::optional<QString> path = std::nullopt);
    bool relink_contents(std::optional<QString> path = std::nullopt);
    bool embed_contents();
    bool link_contents(std::optional<QString> path = std::nullopt);
    bool update_contents(bool all);
    bool reveal_contents();
    bool place_linked(std::optional<QString> path = std::nullopt);
    bool package_book(std::optional<QString> folder = std::nullopt);
    // The contents the tab in front shows (コンテンツを編集's tab), none for the book's pages.
    const std::optional<std::string>& contents_tab() const { return contents_; }
    // The linked files looked at again (the book opened, the window come back to, a file changed): each linked
    // contents' "ok", "modified" or "missing", marked in the layer panel; a file changed said in the status line.
    void check_links();
    std::string link_status(const std::string& contents_id) const;
    // PC3 C-d (main_window_align.cpp): レイヤー → 重ね順 ▸ (to: front | forward | backward | back | reverse — arrange_layers,
    // among siblings), 整列 ▸ (edge: top | vcenter | bottom | left | hcenter | right — align_layers to the base chosen in the
    // menu) and 分布 ▸ (by: those and horizontal | vertical — distribute_layers), on the layers chosen in the layer panel
    // (else the layer drawn on) and the line chosen; レイヤーをリンク, リンクしたレイヤーを選択, レイヤーをロック…. False
    // when nothing was done (said why).
    bool arrange_layers(const char* to);
    bool align_layers(const char* edge);
    bool distribute_layers(const char* by);
    bool link_layers();
    bool select_linked_layers();
    // The move tool's 自動選択 at a point (mm): the topmost layer showing there (group: its outermost folder) drawn on and
    // chosen; false when none shows there.
    bool pick_layer_at(double x_mm, double y_mm, bool group = false);
    // The page's layers' boxes the canvas snaps to (x0, y0, x1, y1 mm; render/layer_bounds): drawn on a worker once for the
    // page as it is (none until then: asked again, they are there), the shown ones only.
    std::vector<std::pair<std::string, std::array<double, 4>>> layer_snap_boxes();
    // What the move tool moves: the layers chosen (else the layer drawn on) with their folders' and linked layers.
    std::vector<std::string> move_targets() const;
    // A notice in the status line that never stops the work (problems in red, a little longer).
    void flash(const QString& message, int ms = 3000, bool error = false);
    QString last_notice() const { return last_notice_; }
    QString last_error() const { return last_error_; }
    // Before a book goes (its tab closes, the window, Genko): saved, or the person's choice. False: keep it open.
    bool settle(int index, const QString& verb = QStringLiteral("閉じる"), bool* defer_discard = nullptr);
    void refresh_status();
    // The window is closing for good (tests).
    bool closed() const { return closed_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;  // (smart-objects@1: the window come back to looks at its linked files)
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void build_actions();
    void build_menus();
    void build_toolbars();
    void build_docks();
    QAction* make(const QString& attribute, const QString& title, std::function<void()> slot, const QList<QKeySequence>& keys = {},
                  const QString& tip = {}, bool checkable = false);
    // PC2 (補正・色管理, app/main_window_adjust.cpp): レイヤー → 色調補正 and the corrections' commands of 編集, each
    // group's added by its own file (app/adjust_forms.hpp); the submenu shown only when it holds a command.
    void build_adjust_menus(QMenu* layers, QMenu* edit);
    // smart-objects@1 (PC3 A0, app/main_window_smart.cpp): the commands of smart objects, and their menus — ファイル's
    // placing ones after 画像を読み込む…, レイヤー → スマートオブジェクト, スマートフィルター, ラスタライズ, レイヤーの内容オプション…
    // (PC3's later steps' commands in their places, not usable yet).
    void build_smart_actions();
    void build_smart_menus(QMenu* layers, QMenu* file);
    // vector@1 (PC3 C-a, app/main_window_paths.cpp): the path tools and the paths' commands, their menus (ツール, 選択範囲 →
    // 作業用パスを作成…, レイヤー → ベクトルマスク and ラスタライズ → ベクトルマスク, ページ → 台詞 → 作業用パスを作成, 表示 →
    // ターゲットパス, ファイル → パスを Illustrator へ…) and the Paths panel.
    void build_path_actions();
    void build_path_menus(QMenu* file, QMenu* view, QMenu* tools, QMenu* layers);
    void build_path_dock();
    void path_drawn(const core::Json& subpath);
    void path_edited(const core::Json& path);
    // pen-shapes@1 (PC3 C-b, app/main_window_shapes.cpp): the shape tools and the shape parts' commands, their menus
    // (ツール, 編集, レイヤー → シェイプを結合 and ラスタライズ → シェイプ, 選択範囲, ページ → 台詞, 表示), the tool settings
    // (the shape tools' and the pen tools' モード) and the Shapes panel. A figure drawn becomes a shape part (シェイプ), a
    // subpath of the target path (パス) or the 図形 of before (ピクセル); a pen's subpath the same by the pen's モード
    // (pen_path_drawn: false in パス, the path's own way then).
    void build_shape_actions();
    void build_shape_menus(QMenu* edit, QMenu* view, QMenu* tools, QMenu* layers);
    void build_shape_dock();
    void build_shape_pages(ToolSettings* ts, QFormLayout* shape_form);
    void choose_shape_tool(const QString& tool);
    void shape_part_drawn(const core::Json& shape, const QString& mode);
    bool pen_path_drawn(const core::Json& subpath);
    // A shape part made of `what` (add_shape_part's "shape" or "area", "name"?) on the pen layer drawn on or a new one
    // (新しいレイヤー), in the tool settings' fill and stroke (`closed`: false, an open path's — no fill, a stroke —), or
    // the path given added to the target part by the tool's 合成 (`more`); and the path a figure of the canvas makes.
    bool add_shape_part_op(core::Json what, bool closed, const std::optional<core::vector::Path>& more = std::nullopt);
    std::optional<core::vector::Path> figure_path(const core::Json& shape) const;
    // PC2a: the 色調補正 and 属性 panels (app/adjust_panels.cpp), listed in ウィンドウ.
    void build_adjust_docks();
    void connect_session();
    void disconnect_session();
    void on_book_changed(const BookChange& change);
    void reload_pages();
    void show_page();
    void store_document();
    void show_document(int index);
    void build_color_actions();
    void color_settings_dialog();
    void proof_custom_dialog();
    void show_proof_marks();
    void refresh_gamut_count();
    void refresh_pixel_value();
    static QString pixel_words(const core::Page& page, const core::Json& at);
    void fill_books_menu();
    void watch();
    void on_disk_change();
    void on_stroke(const StrokeInput& stroke);
    void on_frame_selected(const QString& frame_id);
    void context_menu(const QString& frame_id, const QPoint& global);
    void refresh_zoom();
    void refresh_actions();
    void layer_move_started();
    void undo();
    void redo();
    void save();
    void save_as();
    bool save_as_and_wait(const QString& verb, std::shared_ptr<Session> origin = {});
    void new_book();
    void open_book();
    void split(const QString& axis);
    double gutter_mm(const QString& cut) const;
    void gutter_settings();
    void set_selected_frame(const core::Json& change);
    const core::Frame* selected_frame() const;
    void border_width();
    void corner_radius();
    void border_kind(const QString& kind);
    void border_colour();
    void border_detail();
    void toggle_bleed();
    void frame_to_selection();
    void templates_dialog();
    void save_template();
    void add_page();
    void del_page();
    void duplicate_page(int index);
    void reorder_pages(const std::vector<int>& order, int follow);
    void page_overview();
    void name_ok();
    void point_width(double factor);
    void pick_colour();
    void exposure_dialog();
    void nombre_dialog();
    void layer_operation(const std::string& operation);
    void ask_zoom();
    void jump(int delta);
    void apply_stage(const QString& key);
    void delete_frame();
    void merge();
    void cut_frame(const QString& frame_id, const QPointF& p0, const QPointF& p1);
    void frame_drawn(const QVector<QPointF>& points);
    void layer_moved(double dx, double dy);
    void show_omitted(const QStringList& elements);
    bool agent_book() const;
    // The layer drawn on when it can be painted on; otherwise a notice and null.
    const core::Layer* paint_layer();
    // the brushes and the tool settings (main_window_brushes.cpp)
    void build_tool_settings();
    void brush_changed();
    void make_brush();
    void edit_brush();
    void forget_brush();
    void import_brushes_dialog();
    core::Json eraser_fields(const core::Layer& layer) const;
    // the drawing tools and the colour (main_window_paint.cpp)
    void build_paint_actions();
    void build_paint_pages(ToolSettings* ts);
    void build_colour_dock();
    core::Json paint_fields() const;
    std::optional<std::array<int, 3>> layer_colour_at(double x_mm, double y_mm) const;
    void colour_picked(const std::array<int, 3>& rgb);
    // color-mode@1: these inks for the pen on the CMYK or Lab page in front (an RGB colour for them, in the page's table
    // when its rules would put other inks on it: set_inks).
    void choose_inks(const std::vector<double>& ink, bool picked);  // (picked: by the eyedropper)
    void fill_at(double x_mm, double y_mm);
    void lasso_filled(const QVector<QPointF>& points);
    void shape_drawn(const core::Json& shape);
    void gradient(const QPointF& from, const QPointF& to);
    void fill_gaps();
    void nudge_brush(int step);
    bool paint_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer);
    // rulers, the grid and 3D (main_window_guides.cpp)
    void build_guide_actions();
    void build_guide_menus(QMenu* tools);
    void build_guide_pages(ToolSettings* ts);
    void build_guide_dock();
    void guide_toggles();
    void grid_spacing();
    void choose_ruler(const std::string& kind, int vps, int copies, bool ask_copies);
    void place_ruler(const core::Json& ruler);
    const core::Json* selected_ruler();
    void ruler_to_target_layer();
    void ruler_pen();
    std::optional<core::Json> closed_ruler_outline(const core::Json& ruler) const;
    void ruler_selection();
    void perspective_grid();
    void ruler_from_3d();
    void camera_from_ruler();
    void ruler_frame();
    void ruler_flag(const std::string& key);
    void clear_rulers();
    void after_prim(const std::string& id);
    void add_prim(const std::string& kind, const std::string& prop = {});
    void import_model();
    void add_scene(const std::string& kind);
    void pose(const std::string& preset);
    void prim_posed(const QString& prim_id, const QString& handle, const core::Json& to);
    // materials, tones, effect lines, the view's extras and the layer commands (main_window_materials.cpp)
    void build_material_actions();
    void build_material_dock();
    void build_material_pages(ToolSettings* ts);
    core::Json default_tone() const;
    void after_tone(const std::string& layer_id);
    void tone_here();
    void tone_click();
    void put_tone(const core::Json& item, bool ask_click);
    void stamp_at(double x_mm, double y_mm);
    void choose_effect(const std::string& kind);
    void effect_at(double x_mm, double y_mm);
    std::optional<core::Json> selection_shape() const;
    void effect_clearing(const std::optional<std::string>& how);
    bool effect_shape_stroke(const StrokeInput& stroke, const core::Page& page);
    void onion();
    void import_scan(bool from_scanner);
    // the lines of dialogue (main_window_lines.cpp)
    void build_line_actions();
    void build_line_menus(QMenu* pages);
    void build_line_dock();
    QWidget* line_select_page();
    void panel_for_tool(const QString& tool);
    void refresh_lines();
    const core::Frame* frame_by_id(const std::optional<std::string>& frame_id) const;
    void type_new_line(double x_mm, double y_mm);
    void balloon_drawn(const core::Json& outline);
    void edit_line_inline(const std::string& line_id);
    const core::StoryLine* selected_line_or_say();
    void edit_selected_line();
    void delete_selected_line();
    void toggle_selected_wrap();
    void set_selected_balloon(const QString& kind);
    std::optional<QString> panel_reference(const std::string& frame_id) const;
    void picture_balloon(const std::string& line_id);
    void draw_text_path(const std::string& line_id);
    bool text_path_stroke(const StrokeInput& stroke);
    bool balloon_eraser_stroke(const StrokeInput& stroke, const core::Page& page);
    void open_story_editor();
    void replace_dialog();
    // the book and its pages (main_window_book.cpp)
    void build_book_actions();
    void set_spread(const core::Num& index, const std::optional<core::Num>& other);
    void toggle_spread();
    void toggle_page_nombre();
    void paper_settings();
    void color_mode_dialog();
    void set_page_mode(const QString& mode, int depth);
    void show_color_mode();
    void profile_dialog(bool convert);
    // ページ → 色モード's own modes (color-model@1, PC1d; app/mode_dialogs): インデックスカラー…, カラーテーブル…, 2 階調…,
    // ダブルトーン…, マルチチャンネル — those that flatten asked first when the page has layers to merge
    void indexed_dialog();
    void color_table_dialog();
    void bitmap_dialog();
    void duotone_dialog();
    void to_multichannel();
    bool flatten_asked(const core::Page* page, const QString& what);
    void add_cover_dialog();
    void assignee_dialog();
    void merge_book();
    void import_psd();
    // 書き出し, 印刷, 履歴, 入稿前の点検, the phone screens and the scales (main_window_output.cpp)
    void build_output_actions();
    void build_output_docks();
    void output_panels_follow(bool now = false);
    void history_follows(bool now = false);
    void run_checks();
    void export_book();
    void print_book();
    void toggle_phone();
    // editing lines (main_window_vector.cpp)
    void build_vector_actions();
    void build_vector_pages(ToolSettings* ts);
    void vector_edit(const core::Json& change);
    void vector_traced(const core::Json& points, const QString& mode);
    void vector_simplify();
    void vector_selected(const std::string& what);
    void reshaped(const QString& stroke_id, const core::Json& points);
    // the studio (main_window_studio.cpp)
    void build_studio_actions();
    void build_studio_bar();
    void build_studio_docks();
    void studio_follow(bool now);
    void refresh_studio_docks();
    void agent_view(bool on);
    void sync_box();
    std::set<std::string> request_ids() const;
    // animation and the timelapse (main_window_anim.cpp)
    void build_anim_actions();
    void build_anim_dock();
    void toggle_timelapse(bool on);
    void export_timelapse();
    // the selection (main_window_select.cpp)
    void build_selection_actions();
    void build_selection_menu(QMenu* menu);
    // (decision 5: 編集 → 変形, the tools menu's groups; 編集's clipboard items: main_window_clipboard.cpp)
    void build_transform_menu(QMenu* menu);
    // PC3 C-c (main_window_transform.cpp): 自由変形 and 変形's commands, パペットワープ, 遠近法ワープ, コンテンツに応じて拡大・
    // 縮小…, ゆがみ…, ワープテキスト… — on the layers chosen in the layer panel (else the layer drawn on), within the
    // selection when there is one; the box's options bar (補間方法, the warp's 形 and splits, the puppet's モード・密度・拡張,
    // Perspective Warp's レイアウト・ワープ and 揃える), its preview, its op (transform_layers) and 再実行.
    void build_transform_actions();
    void build_transform_items(QMenu* edit);
    std::vector<std::string> transform_targets() const;
    core::Json transform_op(const core::Json& spec) const;
    void start_layer_transform(const QString& mode);
    void transform_previewed(const core::Json& spec);
    void transform_committed(const core::Json& spec);
    void transform_cancelled();
    void transform_again();
    void turn_layers(const char* turn);
    void split_warp_box(int how);
    void show_transform_bar(const QString& mode);
    void hide_transform_bar();
    // The selection's own (変形を数で決める…, its 自由な形に and ワープ, 水平方向に反転…): transform_layers within it (the
    // spec's way; {"grid_warp": {mesh, grid?}} a grid of points pulled), the selection following a matrix; false when
    // there was nothing to do.
    bool transform_within_selection(const core::Json& spec);
    void content_aware_scale_dialog();
    void liquify_dialog();
    void warp_text_dialog();
    // PC3 C-d (main_window_align.cpp): the commands and menus of レイヤー → 重ね順・整列・分布・リンク・ロック and 表示 →
    // スナップ・スナップ先, the move tool's page (自動選択, バウンディングボックスを表示), the snapping set on the canvas,
    // レイヤーをロック…'s window, the move tool's box (the union of the moved layers' snap boxes).
    void build_align_actions();
    void build_align_menus(QMenu* layers, QMenu* view);
    void build_move_page(ToolSettings* ts);
    void snap_toggles();
    void lock_layers_dialog();
    std::optional<std::array<double, 4>> move_box();
    struct SnapBoxes {
        std::weak_ptr<core::Page> page;
        std::vector<std::pair<std::string, std::array<double, 4>>> boxes;
        bool ready = false;
    };
    SnapBoxes snap_boxes_;
    std::uint64_t snap_ticket_ = 0;
    void build_tool_groups(QMenu* tools);
    void build_clipboard_menu(QMenu* edit);
    static const std::vector<const char*>& selection_tools();
    static const std::vector<const char*>& retouch_tools();
    void choose_marquee(const QString& tool);
    std::optional<core::Json> need_area();
    void join_selection(const std::optional<core::Json>& area, const QString& how);
    void selection_drawn(const core::Json& area, const QString& how);
    void selection_painted(const QVector<QPointF>& points, bool add);
    void wand(double x_mm, double y_mm);
    void select_colour(double x_mm, double y_mm);
    // PC3 B1 (main_window_select.cpp): 選択範囲を拡大・近似色を選択, 再選択, 選択範囲を変形, the Quick Mask followed on the
    // page shown and painted (its channel the target), the Magic Eraser's click, the selection's right-click menu and the
    // tools' settings; (select_dialogs.cpp) 色域指定…, 選択範囲を保存…, 選択範囲を読み込む….
    void grow_selection(bool contiguous);
    void reselect();
    void transform_selection_outline();
    void quick_mask_follow();
    void target_quick_mask(const std::string& id);
    void magic_erase(double x_mm, double y_mm);
    // PC3 B4 (retouch_tools.cpp): the retouching tools and コンテンツに応じた塗りつぶし…, their settings, a stroke made their
    // op, the clone source set by Alt+click and its overlay drawn, コピーソース panel, the History Brush's source state
    // put in the book's assets as the op's `from`.
    void build_retouch_actions();
    void build_retouch_pages(ToolSettings* ts);
    void build_clone_source_dock();
    bool retouch_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer);
    void clone_source_picked(double x_mm, double y_mm);
    void clone_overlay_picture();
    std::optional<core::Json> history_brush_from(const core::Page& page, const core::Layer& layer);
    bool selection_context_menu(const QPoint& global);
    void build_selection_pages(ToolSettings* ts);
    void color_range_dialog();
    bool save_selection_dialog();
    void load_selection_dialog();
    void change_selection(const core::Json& change);
    void change_selection_by(const char* key, int sign);
    void select_all();
    void select_drawn();
    bool keep_selection(std::optional<QString> name = std::nullopt);
    void fill_stock();
    void use_stock(const std::string& name);
    void quick_mask(bool on);
    static core::Json moved_area(const core::Json& area, const std::array<double, 6>& m);
    void transform_selection(const QVector<double>& matrix);
    void start_warp(const QString& kind, int columns = 2, int rows = 2);
    void start_mesh_grid();
    void warp_selection(const core::Json& warp);
    void move_pivot();
    void transform_numbers();
    void flip(int sx, int sy);
    void delete_area();
    bool copy();
    void cut();
    void paste();
    void fill_selection();
    void line_width();
    // 判型 (PC3 C-e, main_window_page_size.cpp): ページ → 画像解像度…, カンバスサイズ…, 画像の回転 ▸, 切り抜き, トリミング…,
    // すべての領域を表示; ツール → 切り抜きツール and its settings. page_size_op applies one op of them (asked whether to fit
    // the finished size to the new paper when it would not fit), the selection and the crop box let go, the page fitted.
    void build_page_size_actions();
    void build_page_size_menu(QMenu* pages);
    void build_crop_page(ToolSettings* ts);
    bool page_size_op(core::Json op, const QString& done);
    void image_size_dialog();
    void canvas_size_dialog();
    void rotate_pages_by(const QString& turn);
    void rotate_angle_dialog();
    void crop_to_selection();
    void crop_drawn(const QRectF& box, double angle);
    void trim_dialog();
    void reveal_all_pages();
    // PC3 B2 (main_window_clipboard.cpp): 編集's clipboard with the system's (a copy is put there as a picture and as
    // Genko's own; a picture another program put there is pasted as a new layer), 結合部分をコピー, 特殊ペースト (同じ位置・
    // 選択範囲内へ・選択範囲外へ), 選択範囲をコピー／カットしたレイヤー, 塗りつぶし… and 境界線を描く… (app/fill_dialogs.cpp).
    void build_clipboard_actions();
    // What the layer drawn on holds in the area (none: all of it), as paste's "items" (precise pixels and inks as
    // pieces that keep them); a message said and none when there is nothing.
    std::optional<core::Json> copied_items(const std::optional<core::Json>& area);
    void copy_merged();
    void paste_special(const QString& how);
    void layer_via(bool cut);
    void fill_dialog();
    void stroke_dialog();
    // PC3 B2 (main_window_masks.cpp): すべてのマスクを統合 and the layers' selection (選択範囲 → すべてのレイヤー …).
    void build_mask_actions();
    void flatten_all_masks();
    // PC3 B3 (main_window_smart_select.cpp, select_mask_window.cpp): クイック選択ツール・オブジェクト選択ツール, 選択範囲 →
    // フォーカス領域…・被写体を選択・空を選択・選択とマスク…, レイヤー → すべてのオブジェクトをマスク・マッティング ▸. solve_smart
    // finds an area (the area language's smart kinds) on a worker thread from the book as it is, then gives it to `done`
    // on this one (none: nothing found) unless the page changed or it was stopped.
    void build_smart_select_actions();
    void build_matting_menu(QMenu* layers);
    QWidget* smart_select_settings_widget();
    void solve_smart(const core::Json& area, const QString& title, std::function<void(const std::optional<core::Json>&)> done);
    // A smart kind's settings with the drawing layer to read, unless 全レイヤーを対象.
    core::Json seen_spec(core::Json spec) const;
    void quick_selected(const QVector<QPointF>& points, const QString& how);
    void object_selected(const QRectF& box, const QString& how);
    void select_subject();
    void select_sky();
    void focus_area_dialog();
    void select_and_mask();
    void select_mask_output(const core::Json& area, const core::Json& settings);
    void mask_all_objects();
    void matting(const QString& kind);
    struct SmartJob;
    std::shared_ptr<SmartJob> smart_job_;
    // channels@1 (main_window_channels.cpp)
    void build_channel_actions();
    void build_channel_dock();
    void channel_view_changed();
    core::Json with_channel_target(const core::Json& ops) const;
    void build_info_docks();  // (PC2d: app/info_panels.cpp)
    HistogramPanel* histogram_panel_ = nullptr;
    InfoPanel* info_panel_ = nullptr;
    QDockWidget* histogram_dock_ = nullptr;
    QDockWidget* info_dock_ = nullptr;

    std::shared_ptr<Session> session_;
    std::map<std::string, ChannelView> channel_views_;  // (by page id)
    ChannelPanel* channel_panel_ = nullptr;
    CloneSourcePanel* clone_source_panel_ = nullptr;  // (PC3 B4: コピーソース)
    QDockWidget* clone_source_dock_ = nullptr;
    std::uint64_t clone_ticket_ = 0;  // (the overlay's picture drawn on a worker: the latest asked)
    PathsPanel* paths_panel_ = nullptr;  // (vector@1, PC3 C-a)
    QDockWidget* paths_dock_ = nullptr;
    // pen-shapes@1 (PC3 C-b): the Shapes panel and the shape tools' settings (their objectName the same words)
    ShapesPanel* shapes_panel_ = nullptr;
    QDockWidget* shapes_dock_ = nullptr;
    QComboBox* shape_mode_ = nullptr;
    QComboBox* pen_mode_ = nullptr;
    QCheckBox* shape_new_layer_ = nullptr;
    QComboBox* shape_fill_ = nullptr;
    QDoubleSpinBox* shape_stroke_width_ = nullptr;
    QComboBox* shape_stroke_colour_ = nullptr;
    QComboBox* shape_stroke_align_ = nullptr;
    QComboBox* shape_stroke_cap_ = nullptr;
    QComboBox* shape_stroke_join_ = nullptr;
    QComboBox* shape_stroke_dash_ = nullptr;
    QSpinBox* shape_opacity_ = nullptr;
    QCheckBox* shape_over_ = nullptr;
    QComboBox* shape_combine_ = nullptr;
    QSpinBox* shape_star_ = nullptr;
    QDoubleSpinBox* shape_weight_ = nullptr;
    QComboBox* shape_custom_ = nullptr;
    QDockWidget* channels_dock_ = nullptr;
    std::vector<Document> documents_;
    int doc_ = 0;
    int page_index_ = 0;
    std::optional<std::string> target_layer_id_;
    LayerPanel* layer_panel_ = nullptr;
    QMenu* stock_menu_ = nullptr;
    BrushPanel* brush_ = nullptr;
    ColourPanel* colours_ = nullptr;
    GuidePanel* guides_ = nullptr;
    MaterialPanel* materials_ = nullptr;
    SubView* subview_ = nullptr;
    QDockWidget* materials_dock_ = nullptr;
    std::optional<core::Json> pending_material_;
    QString stamp_return_tool_;  // (the tool chosen before a material waited for its click: Esc gives it back)
    void wait_for_stamp(const core::Json& item);
    std::string effect_kind_ = "focus";
    std::optional<std::pair<std::string, std::string>> effect_shape_for_;  // (effect id, path | inner_path)
    StoryPanel* story_ = nullptr;
    HistoryPanel* history_ = nullptr;
    QDockWidget* history_dock_ = nullptr;
    QTimer history_timer_;  // (the history follows a change a moment later)
    CheckPanel* checks_ = nullptr;
    QDockWidget* checks_dock_ = nullptr;
    TextToolSettings* text_settings_ = nullptr;
    QDockWidget* lines_dock_ = nullptr;
    QPointer<StoryEditor> story_editor_;
    QTimer lines_timer_;                       // (the lines panel follows an edit a moment later)
    bool lines_later_ = false;
    std::optional<std::string> text_path_for_;  // the next pen line becomes this line's path (文字をパスに沿わせる)
    std::vector<QAction*> effect_actions_;  // the effect kinds (Python's effect_actions)
    std::vector<QAction*> ruler_actions_;  // the ruler kinds (Python's ruler_actions)
    std::vector<QAction*> prop_actions_;
    std::vector<QAction*> scene_actions_;
    std::vector<QAction*> pose_actions_;
    ToolSettings* tool_settings_ = nullptr;
    QComboBox* shape_kind_ = nullptr;
    QComboBox* shape_style_ = nullptr;
    QSpinBox* shape_sides_ = nullptr;
    QDoubleSpinBox* shape_radius_ = nullptr;
    QComboBox* blend_mode_ = nullptr;
    QSpinBox* blend_strength_ = nullptr;
    QComboBox* liquify_mode_ = nullptr;
    QSpinBox* liquify_strength_ = nullptr;
    QComboBox* gradient_mode_ = nullptr;
    QComboBox* vector_mode_ = nullptr;
    QSpinBox* vector_amount_ = nullptr;
    QDoubleSpinBox* vector_join_ = nullptr;
    QDockWidget* tool_settings_dock_ = nullptr;
    TimelinePanel* timeline_ = nullptr;
    QDockWidget* timeline_dock_ = nullptr;
    QDoubleSpinBox* eraser_size_ = nullptr;
    QComboBox* eraser_mode_ = nullptr;
    QComboBox* eraser_texture_ = nullptr;
    QCheckBox* eraser_balloons_ = nullptr;  // フキダシを削る (the eraser cuts the balloons it goes over)
    QComboBox* frame_mode_ = nullptr;
    QComboBox* marquee_mode_ = nullptr;
    QDoubleSpinBox* selection_pen_ = nullptr;
    QSpinBox* colour_tolerance_box_ = nullptr;
    QCheckBox* colour_contiguous_box_ = nullptr;
    std::string transform_interp_ = "bilinear";  // how pixels are resampled when the selection is transformed
    // PC3 C-c: what the transform box moves (the layers, its box mm, the selection then), the last transform (再実行),
    // the puppet's options and the options bar
    std::vector<std::string> transform_ids_;
    std::array<double, 4> transform_box_{0, 0, 0, 0};
    std::optional<core::Json> transform_area_;
    std::optional<core::Json> last_transform_;
    std::string puppet_mode_ = "normal";
    std::string puppet_density_ = "normal";
    double puppet_expansion_ = 0.25;
    QToolBar* transform_bar_ = nullptr;
    std::optional<core::Json> clipboard_;        // copied items (paste's "items"), where they were, its outline
    std::optional<core::Json> clipboard_area_;
    std::vector<QPointF> clipboard_outline_;
    QByteArray clipboard_stamp_;  // (PC3 B2: the system clipboard's Genko items as this window put them there)
    bool closed_ = false;
    ApprovalBox* approvals_ = nullptr;
    PanelView* panel_view_ = nullptr;
    Library* library_ = nullptr;
    ProcessBar* process_ = nullptr;
    QPushButton* ai_button_ = nullptr;
    QDockWidget* box_dock_ = nullptr;
    QDockWidget* panel_dock_ = nullptr;
    QDockWidget* library_dock_ = nullptr;
    std::vector<QDockWidget*> agent_docks_;
    std::optional<bool> agent_mode_;
    std::optional<bool> box_waiting_;
    std::set<QDockWidget*> stale_docks_;
    std::set<std::string> known_requests_;
    QStringList pending_links_;  // (links that came while the book was being read: taken when it is shown)
    // What a failed read says of pending_links_ (kept, not thrown away): "" when there are none.
    QString links_kept() const;
    bool style_declined_ = false;  // (use_style: the person said no to its question)
    // (the reply drafts' key of the book last shown and its session: a new key for the same session is the book saved in
    // another place, and its drafts are carried there — on_book_changed)
    std::weak_ptr<Session> drafts_session_;
    QString drafts_key_;
    AiTarget ai_target_;
    QTimer ai_timer_;      // (the minutes since the AI last wrote)
    QTimer studio_timer_;  // (the studio's panels follow an edit a moment later)
    PenSettings pen_;
    double eraser_mm_ = 2.0;

    PageCanvas* canvas_ = nullptr;
    CanvasOnly* canvas_only_ = nullptr;
    RestReminder* rest_ = nullptr;
    QPointer<RadialMenu> radial_;
    std::optional<ToolSwitch> tool_switch_;
    std::optional<core::Json> recording_;
    QPointer<QDialog> help_dialog_;
    QMenu* actions_menu_ = nullptr;
    void fill_actions_menu();
    int save_after_ms_ = 2000;
    // バックアップ: after a save, the book zipped into the folder chosen in 環境設定 (one at a time, on a worker)
    void maybe_backup();
    void back_up(const std::shared_ptr<Session>& session);
    // (by the book's folder: each book of the window, its tab in front or not)
    std::map<std::string, std::int64_t> backup_seen_;  // the revision last zipped (or skipped as not due)
    std::set<std::string> backups_running_;
    PageList* pages_ = nullptr;
    Navigator* navigator_ = nullptr;
    QDockWidget* pages_dock_ = nullptr;
    QDockWidget* navigator_dock_ = nullptr;
    QTabBar* doc_tabs_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* zoom_label_ = nullptr;
    QSpinBox* zoom_box_ = nullptr;
    QLabel* unported_ = nullptr;
    QLabel* read_only_ = nullptr;
    SaveStatusLabel* save_label_ = nullptr;
    SaveFailureBar* failure_bar_ = nullptr;
    ImeEntry* ime_ = nullptr;
    QToolBar* palette_ = nullptr;
    QToolBar* commands_ = nullptr;
    QMenu* recent_menu_ = nullptr;
    QMenu* view_menu_ = nullptr;
    QMenu* books_menu_ = nullptr;   // ウィンドウ → 開いている原稿 (document.activate)
    QLabel* gamut_label_ = nullptr;  // 色域外警告's count
    QLabel* pixel_label_ = nullptr;  // (color-model@1: the values under the pointer)
    QTimer* pixel_timer_ = nullptr;
    std::optional<QPointF> pixel_point_;
    QMenu* workspace_menu_ = nullptr;
    QVBoxLayout* column_ = nullptr;  // (the central column: the bands over the page, the page)
    QDockWidget* layers_dock_ = nullptr;
    std::vector<std::pair<QString, QAction*>> launcher_actions_;
    void make_launcher();
    bool fronted_ = false;
    // (Python's _settle_docks, at the first show) the overview small under the tool settings, the row of tabs as wide as
    // the screen lets it be (from 1366 px up), the layers in front; (_fit_tools) the palette's pictures shrink to keep
    // every tool within reach on a short screen.
    void settle_docks();
    void fit_tools();
    QWidget* first_steps_ = nullptr;
    bool first_steps_closed_ = false;
    QWidget* next_steps_ = nullptr;
    std::map<QString, QPushButton*> next_buttons_;
    std::vector<QPushButton*> first_buttons_;  // (the first steps' buttons: their words and action, the key shown again)
    QWidget* first_steps_bar();
    QWidget* next_steps_bar();
    QMenu* layer_special_menu_ = nullptr;
    QMenu* layer_many_menu_ = nullptr;
    QMenu* layer_effect_menu_ = nullptr;
    QMenu* layer_mask_menu_ = nullptr;
    QuickAccess* quick_access_ = nullptr;
    QDockWidget* quick_dock_ = nullptr;
    QByteArray default_state_;  // (the panels as the window first showed them: はじめの配置に戻す)
    bool settled_ = false;
    void fill_workspaces();

    std::map<QString, QAction*> actions_;
    std::map<QString, QAction*> tool_actions_;
    std::vector<QAction*> border_kind_actions_;
    std::map<QString, QAction*> stage_actions_;
    QActionGroup* tools_ = nullptr;

    QFileSystemWatcher watcher_;
    QTimer flash_timer_;
    QString last_notice_;
    QString last_error_;
    std::vector<QMetaObject::Connection> session_links_;
    std::uint64_t move_ticket_ = 0;
    // smart-objects@1, PC3 A1 (app/main_window_smart.cpp): the contents shown by the tab in front (none: the book's
    // page) and the book as the canvas draws it then (its contents' page as its only page, made once a snapshot); the
    // commands a contents' tab cannot use, greyed while it is in front; the linked files' states and their watcher.
    std::optional<std::string> contents_;
    QString contents_title_;
    mutable std::shared_ptr<const core::Document> contents_view_;
    mutable std::shared_ptr<const core::Document> contents_view_of_;
    std::set<QAction*> greyed_in_contents_;
    std::map<std::string, std::string> link_status_;
    QFileSystemWatcher link_watcher_;
    // The book the canvas draws: the session's, or (a contents' tab) the contents' page alone in it; null when the
    // contents are gone (undone).
    std::shared_ptr<const core::Document> shown_book() const;
    // The ops as sent from the tab in front: each given its contents ("contents") in a contents' tab.
    core::Json with_contents_target(const core::Json& ops) const;
    void contents_tab_follow();
    // The linked file of the smart object chosen, read for an op (asked first when the book named it, not the person:
    // a path an agent wrote is shown before anything is read). Its bytes put in the book's assets, and their ref.
    std::optional<std::pair<std::string, std::string>> read_linked(const QString& file, bool chosen_here);
};

// The app-wide watcher of key releases for キーを押している間だけ持ち替え: on from a tool's key until the next key let go
// (tests: whether it is on now; stopped).
bool key_releases_watched();
void stop_key_releases();

}  // namespace genko::app
