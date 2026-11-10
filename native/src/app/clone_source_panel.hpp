#pragma once

#include <QWidget>

#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;

// コピーソース (PC3 B4, window.panel.cloneSource; SPEC.md COLOR-06「修復」): Photoshop's Clone Source panel over the
// window's five sources (MainWindow::clone_sources) — the one in use, where it was set (Alt+click with コピースタンプツール or
// 修復ブラシツール), its offset from the destination it was paired with, its width and height (%), rotation and flips, 変形を
// リセット — and the overlay at the brush (表示, 不透明度, クリップ, 自動的に隠す, 反転, 描画モード). What it changes is the
// window's (the book keeps none of it); the brushes send it in each op.

namespace genko::app {

class MainWindow;

class CloneSourcePanel : public QWidget {
    Q_OBJECT

public:
    explicit CloneSourcePanel(MainWindow* window);

    // The fields again from the window's sources.
    void refresh();

private:
    void changed();

    MainWindow* window_;
    std::array<QToolButton*, 5> slots_{};
    QLabel* where_ = nullptr;
    QDoubleSpinBox* offset_x_ = nullptr;
    QDoubleSpinBox* offset_y_ = nullptr;
    QDoubleSpinBox* width_ = nullptr;
    QDoubleSpinBox* height_ = nullptr;
    QDoubleSpinBox* rotation_ = nullptr;
    QToolButton* flip_h_ = nullptr;
    QToolButton* flip_v_ = nullptr;
    QPushButton* reset_ = nullptr;
    QCheckBox* show_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QCheckBox* clipped_ = nullptr;
    QCheckBox* auto_hide_ = nullptr;
    QCheckBox* invert_ = nullptr;
    QComboBox* blend_ = nullptr;
    bool quiet_ = false;  // (setting the fields from the sources: no change sent back)
};

}  // namespace genko::app
