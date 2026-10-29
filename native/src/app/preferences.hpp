#pragma once

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QMainWindow;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTabWidget;

// 環境設定 (Python's genko/app/preferences.py): the person's own ways of working, kept in the app's settings (not in
// the book).
// - Shortcuts: any command's keys can be changed; two commands on the same keys are refused.
// - The pen tablet: a scribble pad measures how hard the person presses and sets the pressure curve to match; the
//   pen's side button can be the eyedropper, the hand (move the view) or the right click.
// - The screen and the work: the letters, the colours, the pointer, the keys held, the round menu, the rest reminder,
//   fewer moving things, the paper a new book starts on, how soon changes are saved, the backups.

namespace genko::app {

class MainWindow;
class ExportPreferencesForm;  // (PC4 F-c: app/export_as_dialog.hpp)

namespace preferences {

// The pen's side button: (the words shown, the value kept).
const std::vector<std::pair<QString, QString>>& pen_buttons();
// How soon a change is saved (ms): 環境設定's 変更を保存するまで, 2 s unless chosen (SPEC: the pause after a change
// before it is saved is 2 s by default and the work goes on saving within 10 s: 1 … 10 s can be chosen).
constexpr int kSaveAfterMin = 500;
constexpr int kSaveAfterMax = 10000;
constexpr int kSaveAfterDefault = 2000;

int ui_font_pt();  // 0: the system's size
QString default_paper();
int save_after_ms();
// ファイル管理 (PC4 F-b, edit.preferences.fileHandling; PhotoCraft v0.2.0's prefs.rs FileHandling, MIT): how many files
// 最近使用したファイルを開く lists (0..100; Photoshop's 20 by default), whether a file written takes its extension in lower
// case (コピーを保存…), whether an RGB picture's EXIF saying sRGB is ignored when it carries no profile (開く…). Genko's
// PSD always holds its merged picture (Photoshop's 「PSD と PSB の互換性を優先」 always on: said, not chosen).
constexpr int kRecentDefault = 20;
constexpr int kRecentMax = 100;
int recent_count();
bool lowercase_extension();
bool ignore_exif_profile();
std::optional<double> tablet_gamma();
QString pen_button();
// brushes@1 (PC4 E1, edit.preferences.cursors and view.show.brushPreview): 表示 › ブラシプレビュー (the brush's outline at
// the pointer; on unless turned off) and 環境設定's 描いている間は十字だけ (off unless chosen).
bool brush_preview();
void set_brush_preview(bool on);
bool cross_while_drawing();
// The pressure curve that turns the person's usual pressing (the median) into the middle width. Throws
// std::invalid_argument("もう少し長く試し描きします") with too few presses.
double gamma_for(const std::vector<double>& pressures);

// Every command (an action with words, not a menu or a line) named "cmd:<its words>" once by its name (workspace's
// command_names: its words, with its place when another has the same words — the user's decision 8), in the order of
// the names; name_commands gives the unnamed ones their name and remembers each command's own keys by its name
// (MainWindow::default_shortcuts). The keys chosen are kept by the name ("shortcuts/<name>").
std::vector<QAction*> commands(QMainWindow* window);
void name_commands(MainWindow* window);
// (new name: old name, one or more) a command renamed later keeps the keys and places chosen under an old name
const std::multimap<QString, QString>& renamed();
// PC4 E4: the old names a later command took for its own (グラデーション: the グラデーションツール's before PC4 E-0, the
// グラデーション panel's since E4). A list of commands or the keys kept by a build that knows the new command (marked when
// it writes them: names_current) mean the new command by such a name; kept before, the renamed one.
const std::vector<QString>& reused_names();
bool reused_name(const QString& name);
// PC5 H-0: words two commands came to share by a rename, and the attribute of the one that had them before ("定規": the
// 定規 tool's, not 表示 → 定規 — the user's decision 12): keys and lists kept under the words alone are its (by_text,
// apply_shortcuts), not the first of them in the menus'.
const std::map<QString, QString>& words_owner();
// 編集 → 環境設定 ▸ (PC5 H-0, 節 H 5.4): Photoshop's panes in its order (id: core/prefs_table's section), and the page of the
// dialog each opens — none: its page not made yet (H-e), not in the menu.
struct Pane {
    QString id;
    QString label;
    QString page;  // the dialog's page by its title
};
const std::vector<Pane>& panes();
// Whether the settings' `key` ("ui/quick", "ui/commandbar", or "shortcuts/<a reused name>") holds what a build that knows
// reused_names() wrote there — marked as it wrote it: the value then kept beside it (names_current/<key>), so a value a
// build before them wrote since reads as theirs —, and that it is now.
bool names_current(const QString& key);
void mark_names_current(const QString& key);
void apply_shortcuts(MainWindow* window);
// PC5 H-0: a command's keys as the screen's words say them — "（K）", several "（O・Shift+O）" — from its keys now (the key
// set chosen, the person's own), or nothing when it has none: never a key written in the words, which a key set changes.
QString keys_in_words(const std::vector<const QAction*>& actions);
// Every command's tooltip says its name, its keys as they are now, and what it does (written again when the keys
// change, so a changed key never shows the old one).
void retip(MainWindow* window);
// (keys, one command, the other) for keys given to two commands — the commands in their order.
struct Clash {
    QString keys;
    QString first;
    QString second;
};
std::vector<Clash> conflicts(const std::vector<std::pair<QString, QStringList>>& mapping);
// Everything the preferences change, applied to an open window.
void apply_all(MainWindow* window);

// 透明部分・色域 (PhotoCraft v0.2.0's prefs.rs TransparencyAndGamut, MIT; SPEC COLOR-03 PC1b): the checker shown under
// what is transparent (the layers' small pictures) — its square's side in pixels (0: none, white) and its two colours —
// and the gamut warning's mark (色域外警告): its colour and opacity (%).
struct Checker {
    int square = 8;
    QColor light = QColor(255, 255, 255);
    QColor dark = QColor(204, 204, 204);
};
Checker checker();
// The picture filled with the checker (or white without one).
void paint_checker(QImage& image, const Checker& checker);
// The checker sizes and colour sets: (the words shown, the value kept).
const std::vector<std::pair<QString, QString>>& checker_sizes();
const std::vector<std::pair<QString, QString>>& checker_colour_sets();
QColor gamut_colour();
int gamut_opacity();
// 道具ごとの修飾キー: for each tool, what Alt, Ctrl and Shift held switch to (共通の設定のまま: the common one). The dialog
// is exec'ed; `before_exec` (tests) is called with it first. True when kept.
bool tool_keys_dialog(QWidget* parent, MainWindow* window, const std::function<void(QDialog&)>& before_exec = {});

// Scribble here as usual: the pad keeps the pressures the pen reported.
class PressurePad : public QWidget {
    Q_OBJECT

public:
    PressurePad();
    std::vector<double> pressures;
    std::vector<std::pair<QPointF, double>> points;
    void clear();

protected:
    void tabletEvent(QTabletEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(MainWindow* window);

    // the pages, chosen at the side (as the main window's panels), not by tabs across the top
    QTabWidget* tabs = nullptr;
    QListWidget* pages_list = nullptr;
    // ショートカット
    QTableWidget* table = nullptr;
    std::vector<std::pair<QString, QKeySequenceEdit*>> editors;  // (the command's words, its keys) in order
    QKeySequenceEdit* editor(const QString& command) const;
    QLabel* clash = nullptr;
    // キーの組 (PC5 H-0, the user's decision 8: key_sets), and each command's keys in the set chosen (by its name)
    QComboBox* key_set = nullptr;
    std::map<QString, QList<QKeySequence>> set_keys;
    void choose_key_set();
    // The page of a pane (panes(); keyboardShortcuts: ショートカット) in front.
    void show_pane(const QString& id);
    // ペンタブレット
    PressurePad* pad = nullptr;
    QLabel* gamma_note = nullptr;
    std::optional<double> gamma;
    QComboBox* button = nullptr;
    // 表示・作業
    QComboBox* ui_font = nullptr;
    QSpinBox* font_pt = nullptr;
    QComboBox* paper = nullptr;
    QSpinBox* save_after = nullptr;
    QComboBox* theme = nullptr;
    QSlider* brightness = nullptr;
    QComboBox* surround = nullptr;
    QCheckBox* mono_icons = nullptr;
    QCheckBox* hints = nullptr;
    QCheckBox* radial = nullptr;
    QComboBox* rest = nullptr;
    QCheckBox* requests = nullptr;
    QCheckBox* gpu = nullptr;
    QSpinBox* gpu_memory = nullptr;  // (PC5 H-a: グラフィックボードの記憶の上限, MB)
    QCheckBox* motion = nullptr;
    QComboBox* cursor = nullptr;
    QCheckBox* cross_drawing = nullptr;  // (PC4 E1: 描いている間は十字だけ)
    QComboBox* alt_tool = nullptr;
    QComboBox* ctrl_tool = nullptr;
    QLineEdit* backup_folder = nullptr;
    QSpinBox* backup_minutes = nullptr;
    QSpinBox* backup_keep = nullptr;
    QCheckBox* hold_swap = nullptr;
    QPushButton* per_tool = nullptr;
    // ファイル管理 (PC4 F-b: 原稿と保存 moved here, and the files' own)
    QSpinBox* recent_count = nullptr;
    QCheckBox* lowercase_extension = nullptr;
    QCheckBox* ignore_exif_profile = nullptr;
    // 書き出し (PC4 F-c, edit.preferences.export: 書き出しの環境設定…'s values)
    ExportPreferencesForm* export_form = nullptr;
    // Camera Raw (PC4 F-e, edit.preferences.rawDefaults: the ワークフロー a camera RAW file opens with — app/camera_raw_open)
    QComboBox* raw_space = nullptr;
    QComboBox* raw_depth = nullptr;
    QSpinBox* raw_resolution = nullptr;
    QComboBox* raw_sharpen = nullptr;
    QCheckBox* raw_smart = nullptr;
    QCheckBox* raw_auto = nullptr;
    // 透明部分・色域
    QComboBox* checker_size = nullptr;
    QComboBox* checker_colours = nullptr;
    QPushButton* checker_light = nullptr;
    QPushButton* checker_dark = nullptr;
    QLabel* checker_preview = nullptr;
    QPushButton* gamut_button = nullptr;
    QSpinBox* gamut_percent = nullptr;
    void show_checker();

    void measure();
    void forget_gamma();
    void reset_keys();
    std::vector<std::pair<QString, QStringList>> mapping() const;
    // 決める: kept and applied, the dialog accepted — or, with two commands on one key, the clash said and nothing kept.
    void save();

private:
    void show_gamma();
    void pick_backup();

    MainWindow* window_;
};

}  // namespace preferences
}  // namespace genko::app
