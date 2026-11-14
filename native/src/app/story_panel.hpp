#pragma once

#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
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
    // fukidashi@1: the tails' kinds and their names (しっぽの形, here and in a balloon's own menu); a line's tails all
    // given a kind (one turned inward has its tip moved inside the balloon, one turned back out moved outside again);
    // and how far a line's outline trembles as the field shows it (style.wave_mm, or a line of before's wobble as the
    // wave it is drawn with).
    static const std::vector<std::pair<QString, QString>>& tail_kinds();
    static core::Json tails_as(const core::StoryLine& l, const std::string& kind);
    static double shown_wave(const core::StoryLine& l);

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
    // The chosen line's lettering and balloon (style_box: shown beside the select tool) — PC5 G-d: its face, size, tracking,
    // rows' room, alignment, colour, weight, slant, width, 縦中横 and 約物の詰め are the 文字 and 段落 panels' (app/type_panels)
    QWidget* style_box = nullptr;
    QLabel* style_title = nullptr;
    QWidget* style_body = nullptr;
    QDoubleSpinBox* outline = nullptr;
    QDoubleSpinBox* border = nullptr;
    QComboBox* fill = nullptr;
    QDoubleSpinBox* rotate = nullptr;
    QDoubleSpinBox* skew = nullptr;
    QPushButton* gradient = nullptr;
    QDoubleSpinBox* arc = nullptr;
    QComboBox* latin = nullptr;
    QComboBox* mark = nullptr;
    QPushButton* outline_colour = nullptr;
    QCheckBox* double_line = nullptr;
    QSpinBox* spikes = nullptr;
    QDoubleSpinBox* spike_depth = nullptr;
    QPushButton* line_colour = nullptr;
    QPushButton* fill_colour = nullptr;
    QSpinBox* fill_cover = nullptr;
    QDoubleSpinBox* text_dx = nullptr;
    QDoubleSpinBox* text_dy = nullptr;
    QDoubleSpinBox* tail_width = nullptr;
    // fukidashi@1 (SPEC BALLOON-01): the balloon's shape, each shown only for the kinds it changes — the corners' radius
    // (自動: the kind's own; 0: square corners), the fuller rounded box, a polygon's sides, the trembling outline, a
    // burst's every other point shorter, the outline dashed; and its tails' kind (inward: a notch) and how far their
    // join with the balloon is rounded (自動: the renderer's own; 0: a corner). Each change is one edit_line of that key.
    // (The hand-drawn look's 線の揺れ is gone: a line of before that holds style.wobble shows it as the trembling it is
    // drawn with.)
    QDoubleSpinBox* corner = nullptr;
    QCheckBox* squircle = nullptr;
    QSpinBox* sides = nullptr;
    QDoubleSpinBox* wave = nullptr;
    QDoubleSpinBox* spike_alt = nullptr;
    QDoubleSpinBox* dash = nullptr;
    QComboBox* tail_kind = nullptr;
    QDoubleSpinBox* tail_fillet = nullptr;
    QFormLayout* style_form = nullptr;
    QCheckBox* path_curve = nullptr;
    QDoubleSpinBox* ruby_scale = nullptr;
    QCheckBox* mono_ruby = nullptr;
    // TEXT-01: the chosen line's rubies (those that need review marked 要確認), reassigned or cleared, and under them the
    // chosen ruby's own settings (ruby_settings: its face, size against the text, alignment, distance from the text and
    // letter spacing; each change an edit_line of the line's ruby_spans)
    QWidget* ruby_box = nullptr;
    QListWidget* rubies = nullptr;
    QPushButton* ruby_reassign = nullptr;
    QPushButton* ruby_clear = nullptr;
    QWidget* ruby_settings = nullptr;
    QComboBox* ruby_font = nullptr;
    QDoubleSpinBox* ruby_ratio = nullptr;
    QComboBox* ruby_align = nullptr;
    QDoubleSpinBox* ruby_gap = nullptr;
    QDoubleSpinBox* ruby_tracking = nullptr;
    QComboBox* layer_order = nullptr;
    QPushButton* reset = nullptr;

    // 書体 (fonts@1): the chosen ruby's face set from the 書体 palette (an edit_line of the line's rubies). What came of
    // it: no ruby of the chosen line is chosen in the list; the ruby is in that face already (nothing done); the change
    // was not made (refused, the book read-only — said where it was refused); done.
    enum class RubyFace { NoRuby, Same, Refused, Done };
    RubyFace set_chosen_ruby_font(const QString& key);

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
    // fukidashi@1: one key of the chosen line's style set (null: back to the kind's own); its trembling; its tails' kind
    // and their join's rounding; and which of these rows the line's kind has.
    // (`field` and what it shows now: a field left as the panel showed it makes no edit — left_as_shown)
    void set_shape_key(const QWidget* field, double shown, double half_step, const char* key, const core::Json& value);
    bool left_as_shown(const QWidget* field, double value, double half_step);
    void set_wave();
    void set_tail_kind();
    void set_tail_fillet();
    void show_shape_rows(const core::StoryLine& l);
    void pick_gradient();
    void pick_outline_colour();
    void reset_style();
    void show_rubies(const core::StoryLine* l);
    // The chosen ruby's settings shown (none chosen: nothing to set); one of them changed (`key` a ruby setting of
    // core/ruby.hpp, null: the line's look).
    void show_ruby_settings();
    void set_ruby(const char* key, const core::Json& value);
    // 振り直す…: the chosen ruby's base asked (found in the text; the place chosen when it is there more than once) and
    // its reading (the old ones offered). 外す: the chosen ruby taken off (Undo puts it back).
    void reassign_ruby();
    void clear_ruby();

    MainWindow* window_ = nullptr;
    std::vector<std::string> line_ids_;
    std::map<const QWidget*, double> shown_;  // (the shape fields and the tails' as last shown: show_picked)
    bool loading_ = false;
};

}  // namespace genko::app
