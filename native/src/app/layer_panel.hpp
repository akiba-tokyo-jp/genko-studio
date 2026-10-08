#pragma once

#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QSlider;

// The layer panel (Python's LayerPanel in genko/app/main.py): the page's layers, front first, the chosen one where
// the pen and the eraser work. Adding (pen, paint, folder, fill, gradient, correction), deleting, duplicating, merging,
// moving; each layer's settings (name, opacity, blend, clipping, protection, lock, panels, draft, reference, tint);
// its mask and effects; several layers at once; a filter on the layer, with its result shown on the page while the
// numbers change. Every change is an op through the window (apply_ops): Undo, saving and the journal as for any edit.

namespace genko::app {

class MainWindow;

class LayerPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(MainWindow* window);
    // The page's layers again (after a change, another page).
    void refresh();
    // The filters again (the plugins chosen to run, after their settings changed).
    void reload_filters();
    // The layer drawn on chosen in the list (the list as it is: the other layers chosen with it stay chosen).
    void show_target();
    // The layers chosen in the list (Ctrl / Shift+click), bottom first.
    std::vector<std::string> selected_ids() const;
    // Tests and the window: the list, the filter chooser, the menus.
    QListWidget* list() const { return list_; }
    QComboBox* filter_box() const { return filter_; }
    QMenu* special_menu() const { return special_; }
    QMenu* many_menu() const { return many_; }
    QMenu* mask_menu() const { return mask_menu_; }
    QMenu* effects_menu() const { return effects_; }
    const std::vector<std::string>& ids() const { return ids_; }
    // Alt+クリック: this layer alone shown; again: the others back.
    void solo(const std::string& layer_id);
    // The buttons' work (tests call them as the buttons do).
    void add(const std::string& kind, const QString& title);
    void add_adjust(const std::string& kind);
    void add_fill();
    void add_gradient();
    void edit_special();
    void apply_filter();
    void remove();
    void duplicate();
    void merge_down();
    void move(int delta);
    void set(const std::string& key, const core::Json& value);
    void mask(const core::Json& change);

private:
    std::pair<const core::Page*, const core::Layer*> layer() const;
    void selected(bool from_list = true);
    void visibility(QListWidgetItem* item);
    void search(const QString& text);
    std::optional<std::vector<std::string>> several(std::size_t least = 2);
    void set_selected(const core::Json& fields);
    void merge_selected();
    void group_selected();
    void merge_visible(bool copy);
    void convert(const std::string& to);
    void drafts(const core::Json& fields);
    void paper();
    void add_special(const std::string& kind, const QString& title, const core::Json& fields);
    std::optional<core::Json> adjust_fields(const std::string& kind, const core::Json& now = core::Json::object());
    void border();
    void water_edge();
    void screen();
    void mask_from_selection();
    std::optional<std::vector<int>> histogram(const core::Page& page, const core::Layer& layer) const;
    QIcon thumbnail(const core::Page& page, const core::Layer& layer);
    void show_details(bool on, bool save = true);

    MainWindow* window_;
    std::vector<std::string> ids_;
    bool loading_ = false;
    QLabel* target_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* name_ = nullptr;
    QSlider* opacity_ = nullptr;
    QComboBox* blend_ = nullptr;
    QCheckBox* clip_ = nullptr;
    QCheckBox* protect_ = nullptr;
    QCheckBox* locked_ = nullptr;
    QCheckBox* overhang_ = nullptr;
    QCheckBox* each_panel_ = nullptr;
    QCheckBox* draft_ = nullptr;
    QCheckBox* reference_ = nullptr;
    QComboBox* tint_ = nullptr;
    QPushButton* mask_button_ = nullptr;
    QPushButton* effect_button_ = nullptr;
    QMenu* special_ = nullptr;
    QMenu* many_ = nullptr;
    QMenu* mask_menu_ = nullptr;
    QMenu* effects_ = nullptr;
    QAction* mask_off_ = nullptr;
    QAction* color_prints_ = nullptr;
    QComboBox* filter_ = nullptr;
    QPushButton* details_toggle_ = nullptr;
    QWidget* details_ = nullptr;
    std::map<std::string, std::vector<std::string>> solo_hidden_;  // page id → the layers hidden by solo (this session)
    // page id/layer id → its small picture and what it was made of (the lines' and pixels' own blocks held, so that
    // one freed cannot come back at the same address and pass for it)
    struct Thumb {
        core::StrokeListPtr strokes;
        core::Bytes raster;
        core::Bytes color;
        core::Bytes mask;
        std::vector<core::Bytes> patches;
        std::string state;
        QIcon icon;
    };
    std::map<std::string, Thumb> thumbs_;
};

}  // namespace genko::app
