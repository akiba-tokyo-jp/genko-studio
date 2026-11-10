#pragma once

#include <QTimer>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "core/brush_dabs.hpp"
#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;

// The Brush Settings panel (PC4 E2, window.panel.brushSettings, F5; SPEC.md COLOR-09「ブラシ設定パネル」; Photoshop's ブラシ設定
// パネル): the brush in hand's dab settings (brushes@1, core/brush_dabs.hpp) in Photoshop's sections — ブラシ先端のシェイプ,
// シェイプ, 散布, テクスチャ, デュアルブラシ, カラー, 伝達, ブラシポーズ, ノイズ, ウェットエッジ, ビルドアップ, テクスチャを保護 —
// with a line drawn with them below. A change applies to the brush in hand: one of the person's own that no open book
// holds is changed in its place (this process at once, the library a moment later); a built-in one, or one the book
// already draws lines with, is first copied as one's own (「…のコピー」, chosen), so no line drawn already changes. A
// brush of Genko's own way of drawing shows its 詳細 (先端・模様・質感・紙質) and makes a dab brush of itself on asking
// (「Photoshop 型（ダブ）にする」).

namespace genko::app {

class LineSample;
class MainWindow;

class BrushSettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrushSettingsPanel(MainWindow* window);
    // The brush in hand shown again.
    void refresh();
    // The settings as the fields have them (the brush's own beneath what the panel does not show).
    core::DabSettings settings() const;
    // A field by its name (objectName "bs_…": the tests).
    QWidget* field(const QString& name) const;
    // A change waiting to be kept in the library, kept now (the tests; the panel does it a moment after the last change).
    void keep_now();
    // The brush the panel changes (its key), after a copy was made.
    std::string key() const { return key_; }
    LineSample* sample = nullptr;
    QStackedWidget* pages = nullptr;  // (0: a dab brush's sections, 1: a brush of Genko's own way of drawing)
    QPushButton* to_dabs = nullptr;
    QPushButton* details = nullptr;
    QLabel* about = nullptr;

private:
    void build_dab_page(QWidget* page);
    void changed();
    void show_settings(const core::DabSettings& s);
    void make_dab_copy();
    void pick_tip();

    MainWindow* window_;
    std::string key_;          // (the brush shown)
    core::DabSettings shown_;  // (its settings as read: what the fields do not show is kept from them)
    std::string tip_;          // (a sampled tip's ref chosen here; "" round)
    std::string texture_asset_;
    bool loading_ = false;
    QTimer keep_timer_;
    bool keep_waiting_ = false;
    std::map<QString, std::function<void(core::DabSettings&)>> readers_;
    std::map<QString, std::function<void(const core::DabSettings&)>> writers_;
};

}  // namespace genko::app
