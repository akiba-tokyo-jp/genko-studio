#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QTimer>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QFormLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QWidget;

// PC4 E6 (layer.layerStyle.*, SPEC.md COLOR-09「レイヤースタイル」): Photoshop's レイヤースタイル window — the list of the
// blending options (混合オプション: the layer's 描画モード and 不透明度, its 塗りの不透明度 and ブレンド条件) and the effects
// (ベベルとエンボス … ドロップシャドウ, each switched on by its box; ＋ adds another of the kinds Photoshop takes several
// of, − takes one away), the chosen one's settings beside it (lengths in mm, as Genko's pages) and a swatch of the style
// (made off the window's thread). Its result goes to the book as one set_layer_style (with set_layer's opacity and
// set_global_light when they changed): one Undo takes it back. PhotoCraft v0.2.0's ui-egui layer style window is the shape
// it follows (no code taken).

namespace genko::app {

class LayerStyleDialog : public QDialog {
    Q_OBJECT
public:
    // `open_at`: the kind whose settings are shown first (switched on when the layer has none), "" the blending options.
    LayerStyleDialog(std::shared_ptr<const core::Document> book, const core::Page& page, const core::Layer& layer, const std::string& open_at,
                     QWidget* parent = nullptr);
    ~LayerStyleDialog() override;

    // The style as set (set_layer_style's "style": a library pattern given as its picture), the layer's mode and
    // opacity, the global light (changed with an effect that uses it).
    core::Json style() const;
    std::string blend() const { return blend_; }
    double opacity() const { return opacity_; }
    std::array<double, 2> light() const { return light_; }
    // PC5 I-a (blend-channels@1): 混合オプション → チャンネル — the layer's channels as set (empty: every one blends).
    std::vector<bool> channels() const;
    void set_channel(std::size_t channel, bool on);

    // (tests and the window) the list's rows, a row chosen (its settings shown), an effect's settings as they are now
    // (the book's form; a pattern {"choice", …}) and changed.
    QListWidget* list() const { return list_; }
    void choose(const std::string& kind);
    core::Json effect(const std::string& kind) const;
    void set(const std::string& kind, const std::string& key, const core::Json& value);
    void set_fill_opacity(double fill);
    void set_blend_if(const core::Json& blend_if);

private:
    struct Row {
        std::string kind;
        core::Json effect;  // (null: the kind's placeholder row, off)
    };
    void fill_list();
    void show_row(int row);
    void build_blending(QFormLayout* form);
    void build_effect(QFormLayout* form, Row& row);
    void changed();
    void preview();
    int row_of(const std::string& kind) const;
    core::Json& switched_on(int row);

    std::shared_ptr<const core::Document> book_;
    std::string page_space_;  // (rgb | gray | cmyk | lab: Blend If's channels)
    std::vector<Row> rows_;
    core::Json fill_ = 1.0;
    core::Json blend_if_ = core::Json::array();
    bool shown_ = true;
    std::vector<bool> channels_;  // (PC5 I-a: one for each colour channel of the page; none: its mode has none to leave out)
    std::string blend_ = "normal";
    double opacity_ = 1.0;
    std::array<double, 2> light_{120.0, 30.0};
    QListWidget* list_ = nullptr;
    QScrollArea* page_ = nullptr;
    QLabel* swatch_ = nullptr;
    QPushButton* more_ = nullptr;
    QPushButton* less_ = nullptr;
    QTimer later_;
    QFutureWatcher<QImage>* worker_ = nullptr;
    bool filling_ = false;
};

// The kinds' names as Photoshop's window shows them (ドロップシャドウ …).
QString effect_title(const std::string& kind);

}  // namespace genko::app
