#pragma once

#include <QDialog>
#include <QImage>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "formats/web_export.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTabWidget;
template <typename T>
class QFutureWatcher;

// Web 用に保存（従来）… (file.export.saveForWebLegacy; PC4 F-c, SPEC.md FILE-01; Photoshop's legacy Save for Web, PhotoCraft
// v0.2.0 engine/src/web_cmds.rs, MIT): 元・最適化・2 アップ・4 アップ tabs — the page as it is and as each setting writes it
// (4 アップ: two more with fewer colours or a lower quality) —, on the right the preset and the settings (formats::WebSettings),
// the picture's size (formats::WebSize), under the pictures each one's estimated size. Everything is made on a worker from
// the page drawn at most 1024 px long (the window never waits), the file at its whole size only when it is written.
// Slices and their HTML are PC5's.

namespace genko::app {

class SaveForWebDialog : public QDialog {
    Q_OBJECT

public:
    SaveForWebDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page);
    ~SaveForWebDialog() override;

    formats::WebSettings settings() const;
    formats::WebSize size() const;
    QString extension() const;  // gif, png, jpg, wbmp
    // The previews made (tests wait for one after a change).
    int previews() const { return previews_; }
    bool working() const;

    QTabWidget* views = nullptr;            // 元 | 最適化 | 2 アップ | 4 アップ
    std::vector<QLabel*> pictures;          // the four panes: the original, the optimised, two variants
    std::vector<QLabel*> captions;          // each pane's format and size
    QComboBox* preset = nullptr;
    QComboBox* format = nullptr;
    QComboBox* palette = nullptr;
    QSpinBox* colors = nullptr;
    QComboBox* dither = nullptr;
    QSpinBox* dither_amount = nullptr;
    QCheckBox* transparency = nullptr;
    QComboBox* matte = nullptr;
    QCheckBox* interlaced = nullptr;
    QSpinBox* web_snap = nullptr;
    QSpinBox* quality = nullptr;
    QCheckBox* progressive = nullptr;
    QCheckBox* optimized = nullptr;
    QCheckBox* embed_icc = nullptr;
    QComboBox* metadata = nullptr;
    QCheckBox* srgb = nullptr;
    QDoubleSpinBox* percent = nullptr;
    QComboBox* resample = nullptr;
    QLabel* estimate = nullptr;  // the optimised file's size, under the panes
    QDialogButtonBox* buttons = nullptr;

private:
    struct Pane {
        QImage picture;
        QString caption;
    };
    struct Result {
        bool ok = false;
        std::vector<Pane> panes;  // original, optimised, variant, variant
        std::uint64_t bytes = 0;
        QString error;
    };
    void preset_chosen();
    void changed();
    void start();
    void show_result(const Result& result);

    std::shared_ptr<const core::Document> doc_;
    std::size_t page_ = 0;
    bool presetting_ = false;
    QTimer timer_;
    QFutureWatcher<Result>* watcher_ = nullptr;
    bool pending_ = false;
    int previews_ = 0;
};

}  // namespace genko::app
