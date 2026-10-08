#pragma once

#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QListWidget;
class QPixmap;
class QSpinBox;
class QTabWidget;
class QTimer;

// The 素材 panel (Python's genko/app/material_panel.py), three pages so none needs a long scroll: 素材 — the materials,
// built-in and the person's own, by folder and found by name, tag or kind, put on the page (貼る, a double click) and
// kept (画像を追加, 範囲を登録, 名前, タグ, 消す, ＋フォルダ, 素材パック); トーン — the tone layer drawn on: its pattern,
// size, lines, density, angle, dot shape, the screen's shift and gradient, and how the eraser scrapes it; 効果線 — the
// page's effect lines, each one's settings, made pen lines or deleted. Every change to a book is an op through the
// window; the library is the person's (render/materials).

namespace genko::core {
struct Layer;
}

namespace genko::app {

class MainWindow;
class MaterialBrowser;

// "集中線", "流線", … (effects.LABELS); the kind itself for one it does not know.
QString effect_label(const std::string& kind);

// fields.effect_picture: a small picture of an effect (the same drawing the page gets, 60×44 at twice the pixels), for
// the effect tool's choices; fields.effect_tiles: the kinds as such pictures, two to a row, their names under them.
// The pictures are drawn when first asked for (set_effect_pictures), not when the window opens.
QPixmap effect_picture(const std::string& kind);
QWidget* effect_tiles(const std::vector<QAction*>& actions);
void set_effect_pictures(const std::vector<QAction*>& actions, const std::vector<std::string>& kinds);

class MaterialPanel : public QWidget {
    Q_OBJECT
public:
    explicit MaterialPanel(MainWindow* window);

    // The tone and the page's effect lines shown again (the layer drawn on, the page in front).
    void refresh();
    // The material chosen in the list (none: nothing chosen, or it is gone).
    std::optional<core::Json> current_material() const;
    void select_material(const QString& material_id);
    void select_effect(const std::string& effect_id);

    // 素材: the buttons.
    void use();
    void import_image();
    void register_selection();
    void rename();
    void edit_tags();
    void pack_menu();
    void import_pack(bool zip_file);
    void export_pack();
    void remove();
    void new_folder();
    // 効果線: the chosen one as pen lines on the layer drawn on; deleted.
    void effect_to_layer();
    void delete_effect();

    // The fields (tests use them as Python's attributes).
    MaterialBrowser* browser = nullptr;
    QTabWidget* tabs = nullptr;
    QGroupBox* tone_box = nullptr;
    QLabel* tone_label = nullptr;
    QComboBox* pattern = nullptr;
    QDoubleSpinBox* scale = nullptr;
    QDoubleSpinBox* lpi = nullptr;
    QSpinBox* density = nullptr;
    QDoubleSpinBox* angle = nullptr;
    QComboBox* dot_shape = nullptr;
    QDoubleSpinBox* off_x = nullptr;
    QDoubleSpinBox* off_y = nullptr;
    QComboBox* gradient = nullptr;
    QDoubleSpinBox* g_angle = nullptr;
    QSpinBox* g_start = nullptr;
    QSpinBox* g_end = nullptr;
    QCheckBox* soft = nullptr;
    QGroupBox* effect_box = nullptr;
    QListWidget* effects = nullptr;
    QFormLayout* effect_form = nullptr;
    std::map<std::string, QWidget*> effect_fields;

private:
    void reload_library(const std::optional<QString>& select = std::nullopt);
    const core::Layer* tone_layer() const;
    void pattern_chosen();
    void tone(const core::Json& change);
    void offset_typed();
    void offset_now();
    void gradient_changed();
    void fill_effects();
    const core::Json* effect() const;
    void effect_picked();
    void show_effect();
    void effect_set(const std::string& key, const core::Json& value);

    MainWindow* window_ = nullptr;
    bool loading_ = false;
    QTimer* offset_timer_ = nullptr;
    bool offset_typed_ = false;                // (a shift was typed: offset_layer_ is the tone layer it was typed for)
    std::optional<std::string> offset_layer_;
};

}  // namespace genko::app
