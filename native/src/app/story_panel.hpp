#pragma once

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// 台詞 (Python's main.py StoryPanel): the page's lines in reading order. Pick one to edit its words, speaker, balloon
// and lettering style; add one to the chosen panel; reorder; delete. Ruby is typed as ｜約束《やくそく》. The chosen line's
// lettering and balloon settings (style_box) sit beside the tool (ツールの設定 of the select tool), where there is room;
// this panel keeps the list and the words. Every change is an op through the window.

namespace genko::app {

class MainWindow;

class StoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit StoryPanel(MainWindow* window);

    // The page's lines listed again (the one chosen kept chosen).
    void refresh();
    std::optional<std::string> current_id() const;
    void select(const std::optional<std::string>& line_id);
    // The buttons' work: コマに追加, 台詞を直す, 消す, ↑ 前へ / ↓ 後へ.
    void add();
    void apply_edit();
    void remove();
    void move(int delta);
    // The line chosen in the list (none: no line, or one not on this page).
    const core::StoryLine* line() const;

    // The list and the words
    QListWidget* list = nullptr;
    QLabel* empty_note = nullptr;
    QPushButton* up_button = nullptr;
    QPushButton* down_button = nullptr;
    QPushButton* delete_button = nullptr;
    QLineEdit* speaker = nullptr;
    QPlainTextEdit* text = nullptr;
    QComboBox* kind = nullptr;
    QCheckBox* vertical = nullptr;
    QPushButton* add_button = nullptr;
    QPushButton* apply_button = nullptr;
    // The chosen line's lettering and balloon (style_box: shown beside the select tool)
    QWidget* style_box = nullptr;
    QLabel* style_title = nullptr;
    QWidget* style_body = nullptr;
    QComboBox* font = nullptr;
    QDoubleSpinBox* size = nullptr;
    QDoubleSpinBox* tracking = nullptr;
    QDoubleSpinBox* leading = nullptr;
    QDoubleSpinBox* outline = nullptr;
    QDoubleSpinBox* border = nullptr;
    QComboBox* align = nullptr;
    QComboBox* fill = nullptr;
    QCheckBox* tcy = nullptr;
    QDoubleSpinBox* rotate = nullptr;
    QDoubleSpinBox* skew = nullptr;
    QDoubleSpinBox* scale_x = nullptr;
    QPushButton* gradient = nullptr;
    QCheckBox* yakumono = nullptr;
    QDoubleSpinBox* arc = nullptr;
    QComboBox* latin = nullptr;
    QComboBox* mark = nullptr;
    QComboBox* weight = nullptr;
    QCheckBox* italic = nullptr;
    QPushButton* outline_colour = nullptr;
    QDoubleSpinBox* wobble = nullptr;
    QCheckBox* double_line = nullptr;
    QSpinBox* spikes = nullptr;
    QDoubleSpinBox* spike_depth = nullptr;
    QPushButton* color = nullptr;
    QPushButton* line_colour = nullptr;
    QPushButton* fill_colour = nullptr;
    QSpinBox* fill_cover = nullptr;
    QDoubleSpinBox* text_dx = nullptr;
    QDoubleSpinBox* text_dy = nullptr;
    QDoubleSpinBox* tail_width = nullptr;
    QCheckBox* path_curve = nullptr;
    QDoubleSpinBox* ruby_scale = nullptr;
    QCheckBox* mono_ruby = nullptr;
    QComboBox* layer_order = nullptr;
    QPushButton* reset = nullptr;

private:
    std::vector<const core::StoryLine*> lines() const;
    // The chosen line's words and lettering shown (never throws: what cannot be shown is logged, the rest is shown).
    void picked();
    void show_picked();
    void main_button();
    void style(const core::Json& change);
    void style_changed();
    void pick_style_colour(const char* key, const QString& title, const QColor& fallback);
    void set_tail_width();
    void font_changed();
    std::optional<std::string> pick_system_font();
    void pick_gradient();
    void pick_color();
    void pick_outline_colour();
    void reset_style();

    MainWindow* window_ = nullptr;
    std::vector<std::string> line_ids_;
    bool loading_ = false;
};

}  // namespace genko::app
