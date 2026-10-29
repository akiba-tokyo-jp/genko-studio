#pragma once

#include <QWidget>

#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "core/json.hpp"

class QAction;
class QIcon;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

// ツールの設定 (Python's genko/app/tool_settings.py): what the tool in hand can do, right next to it — the brush for the
// pen, the size for the eraser, the panel commands for the frame tool, the selection's ways for the marquee, and so on.
// Its title and hint name the tool; its page changes with the tool.

namespace genko::app {

// A command's words on a narrow page (Python's SHORT: the words themselves when they are short already).
QString short_words(const QString& text);
// Rows without a picture keep its room, so every row's words start at the same place.
QIcon icon_or_blank(const QIcon& icon);
// A command as a quiet row (its picture and words, left-aligned), or a switch as a check box.
QWidget* action_button(QAction* action);
// One button that opens a menu of actions (groups split by lines): a long list kept short.
QPushButton* menu_button(const QString& label, const std::vector<std::vector<QAction*>>& groups);
// A tool's page: sections (a string starts one) of command rows and switches; nullptr is a gap.
using PageItem = std::variant<QString, QAction*, QWidget*, std::nullptr_t>;
QWidget* action_page(const std::vector<PageItem>& items);
// tool_settings.fit_narrow: a side panel let shrink to the side's width — lists of choices show their start and open
// wide, long labels wrap, form rows put the field under its name, a box's note goes under its title, and buttons are
// only as wide as their words — instead of pushing the panel past the edge where the rest is cut off.
void fit_narrow(QWidget* root);

// PC5 G-a (type@1): the anti-alias methods by Photoshop's names (書式 → アンチエイリアス ▸), in its order: {key, name}.
const std::vector<std::pair<QString, QString>>& antialias_names();

// How a new line typed with the text tool starts (Python's TextToolSettings): its balloon, direction, face and size,
// and whether the balloon is drawn by hand. PC5 G-a: what the text tools make (作るもの: 台詞 — lines, the default — or
// テキストレイヤー) and a type layer's anti-alias.
class TextToolSettings : public QWidget {
    Q_OBJECT
public:
    explicit TextToolSettings(QWidget* parent = nullptr);
    // line_fields(): {balloon, vertical, style: {font?, size_mm?}}; and (PC5 G-a) whether type layers are made, and their
    // anti-alias
    struct LineFields {
        std::string balloon;
        bool vertical = true;
        core::Json style = core::Json::object();
        bool type_layer = false;
        std::string antialias = "smooth";
    };
    LineFields line_fields() const;

    QComboBox* make = nullptr;
    QComboBox* antialias = nullptr;
    QComboBox* balloon = nullptr;
    QCheckBox* vertical = nullptr;
    QCheckBox* draw_balloon = nullptr;
    QComboBox* font = nullptr;
    QDoubleSpinBox* size = nullptr;
};

class ToolSettings : public QWidget {
    Q_OBJECT
public:
    explicit ToolSettings(QWidget* parent = nullptr);
    // The page shown for these tools (the canvas's tool names: pen, eraser, marquee, …).
    void add(const std::vector<QString>& tools, QWidget* page);
    // The tool in hand: its name and hint, and its page (none: only the name).
    void show_tool(const QString& tool);
    // Only the tool's name, or everything.
    void set_folded(bool on);
    const QString& tool() const { return tool_; }
    QWidget* page_for(const QString& tool) const;
    QWidget* current_page() const { return current_; }
    QLabel* title() const { return title_; }
    QLabel* hint() const { return hint_; }
    // (PC4 E1) A short word beside the tool's name — the blend a brush draws with when it is not 通常 — or none (empty).
    void set_badge(const QString& text);
    QLabel* badge() const { return badge_; }
    // Folded to the tool's name (the fold button and the pages under it).
    bool folded() const { return folded_; }
    QPushButton* fold_button() const { return fold_; }
    QWidget* stack() const { return stack_widget_; }
    // PC5 H-0: a tool's keys as the title says them after its name ("（K）"; none: nothing) — the window's commands' keys now
    std::function<QString(const QString& tool)> keys_of;

private:
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* badge_ = nullptr;
    QPushButton* fold_ = nullptr;
    QVBoxLayout* stack_ = nullptr;
    QWidget* stack_widget_ = nullptr;
    std::map<QString, QWidget*> pages_;
    QWidget* current_ = nullptr;
    QString tool_ = QStringLiteral("select");
    bool folded_ = false;
};

}  // namespace genko::app
