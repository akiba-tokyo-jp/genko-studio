#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace genko::app {
class MainWindow;
}

// 編集 → ツールバー… (PC5 H-d, edit.toolbar; PhotoCraft v0.2.0's prefs.rs toolbar, MIT; SPEC VIEW-01「ツールバー」): the tool
// palette's buttons in the person's order, lines between where wanted, the tools one does not use moved to 余分なツール (the
// palette's last button, its menu holding them) — Photoshop's. Each button keeps its group (a long press or a right click:
// MainWindow::attach_tool_groups — スポイト's ものさし・注釈・カウント), the ツール panel's switch (ウィンドウ → ツール) stays.
// Kept on this computer (toolbar/order, toolbar/hidden: the tools by their attribute, "|" a line), never in a book.

namespace genko::app {

namespace toolbar {
// The palette as Genko makes it (attributes, "|" for a line).
const QStringList& default_order();
// The order kept (a tool made since, not in it, after the rest in its default place) and the tools moved away.
QStringList order();
QStringList hidden();
void keep(const QStringList& order, const QStringList& hidden);
}  // namespace toolbar

class ToolbarDialog : public QDialog {
    Q_OBJECT

public:
    explicit ToolbarDialog(MainWindow* window);

    QListWidget* shown = nullptr;  // ツールバー (the tools and the lines, in order)
    QListWidget* extra = nullptr;  // 余分なツール
    QPushButton* up = nullptr;
    QPushButton* down = nullptr;
    QPushButton* to_extra = nullptr;
    QPushButton* to_bar = nullptr;
    QPushButton* line = nullptr;
    QPushButton* restore = nullptr;

    QStringList order() const;
    QStringList hidden() const;
    // (the row chosen in ツールバー, or the tool by its attribute)
    void move(int delta);
    void hide_tool(const QString& attribute = {});
    void show_tool(const QString& attribute = {});
    void add_line();
    void restore_defaults();
    // 決める: kept and every window's palette laid out again.
    void save();

private:
    void fill(const QStringList& order, const QStringList& hidden);
    MainWindow* window_;
};

}  // namespace genko::app
