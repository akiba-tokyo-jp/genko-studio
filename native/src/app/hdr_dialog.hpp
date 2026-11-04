#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include <QStringList>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "render/hdr_merge.hpp"
#include "service/automate_photos.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QTimer;
class QWidget;

// ファイル → 自動処理 ▸ HDR Pro に統合… (file.automate.mergeToHdrPro, PC5 I-h; SPEC.md AUTO-01): Photoshop's two windows —
// the exposures to merge (使用: ファイル・フォルダー, 開いているファイルを追加, 可能な場合はソース画像を自動的に配置), then HDR Pro に統合
// itself (モード 32/16/8 bit, the toning of 16 and 8 bits with its preview, ゴーストを除去 and its base image, each image's
// EV). Each only asks: app/main_window_automate.cpp reads the files on a worker (service::automate::hdr_shots) and merges
// them there (merge_to_hdr), the window going on, its progress window stopping it. The preview works on the exposures made
// small (render::hdr::reduced). Their widgets are public for the GUI tests.

namespace genko::app {

// The first window: what to merge.
class HdrFilesDialog : public QDialog {
    Q_OBJECT
public:
    // `open`: the books open in the window (their tab's title, their number among the tabs) — 開いているファイルを追加.
    HdrFilesDialog(QWidget* parent, std::vector<std::pair<QString, int>> open);
    QComboBox* use = nullptr;      // data "files" | "folder"
    QLineEdit* folder = nullptr;
    QListWidget* files = nullptr;  // each item's data (Qt::UserRole): "file:<path>" | "open:<tab>"
    QCheckBox* align = nullptr;    // 可能な場合はソース画像を自動的に配置
    // What is not chosen yet, in Japanese (none: ready).
    std::optional<QString> problem() const;

private:
    std::vector<std::pair<QString, int>> open_;
};

// The second window: how to merge. `shots`: the exposures made small for the preview, in the order read.
class HdrDialog : public QDialog {
    Q_OBJECT
public:
    HdrDialog(QWidget* parent, std::vector<render::hdr::Shot> shots, bool align);
    QComboBox* mode = nullptr;           // data 32 | 16 | 8
    QComboBox* method = nullptr;         // data local_adaptation | exposure_gamma | highlight_compression | equalize_histogram
    std::map<std::string, QDoubleSpinBox*> local;  // ローカル適応: radius, strength, gamma, exposure, detail, shadow, highlight, vibrance, saturation
    QDoubleSpinBox* exposure = nullptr;  // 露光量とガンマ
    QDoubleSpinBox* gamma = nullptr;
    QDoubleSpinBox* view = nullptr;      // 32 bit: the preview's exposure (EV; not the page's)
    QCheckBox* ghosts = nullptr;         // ゴーストを除去
    QComboBox* base = nullptr;           // the ghosts' base: data -1 (自動) | the image's number from 0
    QTableWidget* images = nullptr;      // 画像・EV (EV editable: once edited, the EVs are the given ones)
    QLabel* preview = nullptr;
    // The settings chosen (the title left to the caller).
    service::automate::HdrSettings settings() const;
    // The preview made now (the tests' own: the window makes it a moment after a change).
    void refresh();
    // What the preview shows now (empty when it could not be made: `said` says why).
    QImage shown() const { return shown_; }
    QString said() const { return said_; }

private:
    std::vector<render::hdr::Shot> shots_;
    bool align_ = true;
    bool ev_given_ = false;
    bool filling_ = false;
    std::optional<service::automate::HdrRadiance> radiance_;
    std::string radiance_key_;
    QImage shown_;
    QString said_;
    QTimer* later_ = nullptr;
    QWidget* local_group_ = nullptr;
    QWidget* gamma_group_ = nullptr;
    QWidget* toning_ = nullptr;
    QWidget* view_row_ = nullptr;
    void shown_rows();
};

}  // namespace genko::app
