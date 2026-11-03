#pragma once

#include <QDialog>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/artboards.hpp"
#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」): the artboard's windows — レイヤー → 新規 → アートボード… and the
// artboard's settings (イメージ → アートボードの設定…, Photoshop's Properties: プリセット・幅・高さ・背景・名前, and the anchor of a
// new size), and ファイル → 書き出し → アートボードからファイル… / PDF…. Each says the op or the export it asks for; the window
// applies it.

namespace genko::app {

class ArtboardDialog : public QDialog {
    Q_OBJECT
public:
    // `now`: the artboard's settings (none: a new one, named `name`, the book's paper's size); `dpi` the book's.
    ArtboardDialog(QWidget* parent, const std::optional<core::Artboard>& now, const QString& name, int width_px, int height_px, double dpi);
    // The op's own keys (add_artboard's or set_artboard's: name, preset, width_px, height_px, background, rgb; with an
    // artboard, anchor and move_contents).
    core::Json settings() const;

    QLineEdit* name = nullptr;
    QComboBox* preset = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QComboBox* background = nullptr;
    QPushButton* colour = nullptr;
    QComboBox* anchor = nullptr;
    QCheckBox* move_contents = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void show_colour();
    std::array<int, 3> rgb_{255, 255, 255};
    double dpi_ = 72;
    bool editing_ = false;
    int start_w_ = 0, start_h_ = 0;
};

// アートボードからファイル… (pdf false) and アートボードから PDF… (pdf true): where, the artboards chosen (every one checked),
// the format, the prefix and JPEG's quality, and アートボードを 1 つの PSD に (a PSD's or PSB's format).
class ArtboardExportDialog : public QDialog {
    Q_OBJECT
public:
    ArtboardExportDialog(QWidget* parent, bool pdf, const QString& where, const QString& prefix, const std::vector<std::string>& names);
    std::vector<std::string> chosen() const;

    QLineEdit* where = nullptr;
    QLineEdit* prefix = nullptr;
    QComboBox* format = nullptr;
    QSpinBox* quality = nullptr;
    QCheckBox* one_psd = nullptr;
    QListWidget* artboards = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

}  // namespace genko::app
