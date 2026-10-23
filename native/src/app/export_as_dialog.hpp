#pragma once

#include <QDialog>
#include <QImage>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/model.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/web_export.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
template <typename T>
class QFutureWatcher;

// PC4 F-c (SPEC.md FILE-01; Photoshop's ファイル → 書き出し ▸ and レイヤー's; PhotoCraft v0.2.0 ui-egui/src/export_dialog.rs and
// engine/src/web_cmds.rs, MIT):
//   - 書き出しの環境設定… and 環境設定 → 書き出し: the export preferences (formats::ExportPreferences) kept on this computer
//     (QSettings export/*), one form for both;
//   - 書き出し形式… (file.export.exportAs, layer.exportAs): the page in front, every page or a layer alone (cut to what it
//     holds) as PNG, JPEG, GIF, WebP, TIFF or AVIF — its quality, transparency (a layer's), scale or size and resample,
//     finished size, metadata and sRGB —, a small preview and the file's size estimated on a worker from a picture at
//     most 512 px long (the window never waits for a large page), what the file will not keep said before it is written;
//   - レイヤーからファイル… (file.export.layersToFiles): formats::export_layers with its choices.

namespace genko::app {

// 書き出しの環境設定 as this computer keeps them (PhotoCraft's defaults: PNG, ask each time, 85, the copyright and contact,
// sRGB).
formats::ExportPreferences export_preferences();
void set_export_preferences(const formats::ExportPreferences& prefs);

// The five values' fields (環境設定 → 書き出し and 書き出しの環境設定…), named prefs_export_<format|location|quality|metadata|srgb>.
class ExportPreferencesForm : public QWidget {
public:
    explicit ExportPreferencesForm(const formats::ExportPreferences& prefs, QWidget* parent = nullptr);
    formats::ExportPreferences value() const;

    QComboBox* format = nullptr;
    QComboBox* location = nullptr;
    QSpinBox* quality = nullptr;
    QComboBox* metadata = nullptr;
    QCheckBox* srgb = nullptr;
};

// 書き出しの環境設定…: the form; 決める keeps its values.
class ExportPreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExportPreferencesDialog(QWidget* parent);
    ExportPreferencesForm* form = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

class ExportAsDialog : public QDialog {
    Q_OBJECT

public:
    // The book as it is (a snapshot), the page in front (its place), and — layer.exportAs — the layer written alone.
    ExportAsDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page, std::optional<std::string> layer = std::nullopt);
    ~ExportAsDialog() override;

    // The extension chosen (png, jpg, gif, webp, tif, avif), the copy's options, every page or the one in front.
    QString extension() const;
    formats::CopyOptions options() const;
    bool all_pages() const;
    // The estimates finished (tests wait for one after a change).
    int estimates() const { return estimates_; }
    bool estimating() const;

    QComboBox* target = nullptr;  // このページ | すべてのページ (a layer's: its name alone)
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;    // 1..100 (JPEG, WebP not lossless, AVIF)
    QCheckBox* lossless = nullptr;  // (WebP)
    QCheckBox* transparency = nullptr;
    QDoubleSpinBox* scale = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QComboBox* resample = nullptr;
    QCheckBox* trim = nullptr;
    QComboBox* metadata = nullptr;
    QCheckBox* srgb = nullptr;
    QLabel* preview = nullptr;
    QLabel* estimate = nullptr;  // 「約 1.2 MB（1240 × 1754 px）」
    QLabel* notes = nullptr;     // what the file will not keep
    QDialogButtonBox* buttons = nullptr;

private:
    struct Result {
        bool ok = false;
        std::uint64_t bytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        QImage picture;
        QStringList notes;
        QString error;
    };
    void changed();
    void size_edited(int which);  // (0 scale, 1 width, 2 height)
    void start();
    void show_result(const Result& result);

    std::shared_ptr<const core::Document> doc_;
    std::size_t page_ = 0;
    std::optional<std::string> layer_;
    int sized_by_ = 0;  // the field last edited: 0 scale, 1 width, 2 height
    QTimer timer_;
    QFutureWatcher<Result>* watcher_ = nullptr;
    bool pending_ = false;
    int estimates_ = 0;
};

// レイヤーからファイル…: where, the prefix, the hidden layers too, the format and a JPEG's quality.
class LayersToFilesDialog : public QDialog {
    Q_OBJECT

public:
    LayersToFilesDialog(QWidget* parent, const QString& folder, const QString& prefix);
    formats::LayerFiles files() const;
    QLineEdit* folder = nullptr;
    QLineEdit* prefix = nullptr;
    QCheckBox* visible_only = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

// "約 1.2 MB" (bytes in 1024s, as Photoshop's estimate says it).
QString human_bytes(std::uint64_t bytes);

}  // namespace genko::app
