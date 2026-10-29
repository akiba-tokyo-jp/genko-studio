#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "service/automate.hpp"
#include "service/script_events.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

// The windows of ファイル → 自動処理 ▸ and スクリプト ▸ (PC5 I-e, SPEC.md AUTO-01; Photoshop's: バッチ…, ドロップレットを作成…,
// 条件付きモード変更…, 画像を合わせる…, レンズ補正…, 画像プロセッサー…, スクリプトイベントマネージャー…). Each only asks: what it
// chose is read back by app/main_window_automate.cpp, which runs it through service/automate (the steps through the bus, as
// the window's person; the files written by formats::write_copy — never a picture's own file, never inside a book).
// Their widgets are public for the GUI tests.

namespace genko::app {

// 実行 (the steps): an オートアクション, or a steps file (a script).
class StepsChoice {
public:
    QComboBox* box = nullptr;      // data "action:<name>" | "file" | "" (none: 画像プロセッサー's)
    QLineEdit* file = nullptr;     // a steps file, for "file"
    // The steps chosen, their name and whether they are an action's (recorded); none: none chosen (allowed) — core::Error
    // for a file that cannot be read.
    struct Chosen {
        core::Json steps = core::Json::array();
        std::string name;
        bool recorded = false;
    };
    std::optional<Chosen> chosen() const;
};

// Where the files come from: a folder (サブフォルダーを含める), files chosen one by one, or the book in front (開いている原稿).
class SourceChoice {
public:
    QComboBox* box = nullptr;         // data "folder" | "files" | "open"
    QLineEdit* folder = nullptr;
    QListWidget* files = nullptr;
    QCheckBox* recurse = nullptr;
    QString kind() const;
};

// バッチ… (file.automate.batch)
class BatchDialog : public QDialog {
    Q_OBJECT
public:
    explicit BatchDialog(QWidget* parent);
    StepsChoice steps;
    SourceChoice source;
    QComboBox* destination = nullptr;  // data "none" | "save" (保存して閉じる: a book in place, never a picture) | "folder"
    QLineEdit* folder = nullptr;
    QLineEdit* prefix = nullptr;
    QLineEdit* suffix = nullptr;
    QCheckBox* numbered = nullptr;     // 連番 (_001 …)
    QComboBox* format = nullptr;       // data: same | an extension write_copy writes
    QSpinBox* quality = nullptr;
    QComboBox* on_error = nullptr;     // data "stop" | "log"
    // What is not chosen yet, in Japanese (none: ready).
    std::optional<QString> problem() const;
};

// ドロップレットを作成… (file.automate.createDroplet)
class DropletDialog : public QDialog {
    Q_OBJECT
public:
    explicit DropletDialog(QWidget* parent);
    QLineEdit* place = nullptr;        // the .genkodroplet to write
    StepsChoice steps;
    QCheckBox* own_folder = nullptr;   // 実行先 a folder (else droplet-output beside the files)
    QLineEdit* folder = nullptr;
    QLineEdit* prefix = nullptr;
    QLineEdit* suffix = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QCheckBox* launcher = nullptr;     // 起動ファイルも作る
    std::optional<QString> problem() const;
};

// 画像プロセッサー… (file.scripts.imageProcessor)
class ImageProcessorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImageProcessorDialog(QWidget* parent);
    SourceChoice source;
    QLineEdit* folder = nullptr;
    QCheckBox* jpeg = nullptr;
    QCheckBox* psd = nullptr;
    QCheckBox* tiff = nullptr;
    QCheckBox* png = nullptr;
    QSpinBox* quality = nullptr;
    QCheckBox* fit = nullptr;          // サイズに合わせる
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QCheckBox* srgb = nullptr;         // プロファイルを sRGB に変換
    QCheckBox* run_action = nullptr;   // アクションを実行
    StepsChoice steps;
    service::automate::ImageProcessor processor() const;
    std::optional<QString> problem() const;
};

// 画像を合わせる… (file.automate.fitImage): the page in front fitted to a box of pixels.
class FitImageDialog : public QDialog {
    Q_OBJECT
public:
    FitImageDialog(QWidget* parent, int width_px, int height_px);
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QCheckBox* dont_enlarge = nullptr;
    QComboBox* resample = nullptr;
    core::Json step(const core::Json& page) const;  // the pseudo op fit_image
};

// 条件付きモード変更… (file.automate.conditionalModeChange)
class ModeChangeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModeChangeDialog(QWidget* parent);
    std::vector<std::pair<std::string, QCheckBox*>> from;  // the modes (service::automate::mode_names())
    QComboBox* to = nullptr;                               // data: rgb | cmyk | lab | gray
    core::Json step(const core::Json& page) const;        // the pseudo op conditional_mode_change
    std::optional<QString> problem() const;
};

// レンズ補正… (file.automate.lensCorrection)
class LensBatchDialog : public QDialog {
    Q_OBJECT
public:
    explicit LensBatchDialog(QWidget* parent);
    SourceChoice source;
    QLineEdit* folder = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QCheckBox* distortion = nullptr;   // 幾何学的歪み
    QCheckBox* chromatic = nullptr;    // 色収差
    QCheckBox* vignette = nullptr;     // 周辺光量
    QCheckBox* auto_scale = nullptr;   // 画像を自動的に拡大・縮小
    QComboBox* edge = nullptr;         // data: transparency | extension | black | white
    core::Json settings() const;       // filter_raster's lens_correction settings (the profile auto)
    std::optional<QString> problem() const;
};

// スクリプトイベントマネージャー… (file.scripts.scriptEventsManager): the table edited here and written by save only.
class ScriptEventsDialog : public QDialog {
    Q_OBJECT
public:
    ScriptEventsDialog(QWidget* parent, service::script_events::Table table, const std::optional<std::string>& problem);
    QCheckBox* enabled = nullptr;      // スクリプト／アクションを実行するイベントを有効にする
    QListWidget* list = nullptr;
    QComboBox* event = nullptr;        // data: an event's id
    QComboBox* kind = nullptr;         // data "script" | "action"
    QLineEdit* script = nullptr;
    QComboBox* action = nullptr;       // the オートアクション
    QLabel* said = nullptr;
    // 追加: the binding of what is chosen (a script bound with its file's sha256, an action with its ops'); false and said
    // when it cannot be.
    bool add_chosen();
    void remove_at(int row);
    void remove_all();
    service::script_events::Table table() const;

private:
    void fill();
    service::script_events::Table table_;
    bool outside_ = false;          // (the table was changed outside Genko: never turned on by 追加)
    std::size_t outside_rows_ = 0;  // (its bindings then, marked until removed)
};

}  // namespace genko::app
