#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QRadioButton;

// The windows of 編集 → レイヤーを自動整列… and レイヤーを自動合成… (PC5 I-g, SPEC.md AUTO-01; Photoshop's): each gives the op
// (auto_align_layers, auto_blend_layers) for the layers chosen; app/main_window_photomerge.cpp sends it on a worker. Their
// widgets are public for the GUI tests.

namespace genko::app {

class AutoAlignDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutoAlignDialog(QWidget* parent);
    std::vector<std::pair<std::string, QRadioButton*>> projections;  // 投影法: auto, perspective, collage, cylindrical, spherical, reposition
    QCheckBox* geometric = nullptr;                                  // レンズ補正: 幾何学的歪曲
    core::Json op(const core::Json& page, const std::vector<std::string>& ids) const;
};

class AutoBlendDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutoBlendDialog(QWidget* parent);
    QRadioButton* panorama = nullptr;  // 合成方法: パノラマ
    QRadioButton* stack = nullptr;     // 画像をスタック
    QCheckBox* seamless = nullptr;     // シームレスなトーンとカラー
    core::Json op(const core::Json& page, const std::vector<std::string>& ids) const;
};

}  // namespace genko::app
