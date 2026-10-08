#pragma once

#include <QWidget>

#include <map>
#include <variant>
#include <vector>

class QAction;
class QLabel;
class QPushButton;
class QVBoxLayout;

// ツールの設定 (Python's genko/app/tool_settings.py): what the tool in hand can do, right next to it — the brush for the
// pen, the size for the eraser, the panel commands for the frame tool, the selection's ways for the marquee, and so on.
// Its title and hint name the tool; its page changes with the tool.

namespace genko::app {

// A command as a quiet row (its picture and words, left-aligned), or a switch as a check box.
QWidget* action_button(QAction* action);
// One button that opens a menu of actions (groups split by lines): a long list kept short.
QPushButton* menu_button(const QString& label, const std::vector<std::vector<QAction*>>& groups);
// A tool's page: sections (a string starts one) of command rows and switches; nullptr is a gap.
using PageItem = std::variant<QString, QAction*, QWidget*, std::nullptr_t>;
QWidget* action_page(const std::vector<PageItem>& items);

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

private:
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QPushButton* fold_ = nullptr;
    QVBoxLayout* stack_ = nullptr;
    QWidget* stack_widget_ = nullptr;
    std::map<QString, QWidget*> pages_;
    QWidget* current_ = nullptr;
    QString tool_ = QStringLiteral("select");
    bool folded_ = false;
};

}  // namespace genko::app
