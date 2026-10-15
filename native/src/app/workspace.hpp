#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <map>
#include <utility>
#include <vector>

class QAction;
class QGridLayout;
class QLineEdit;
class QListWidget;
class QMainWindow;
class QPushButton;
class QToolBar;

// The screen made one's own (Python's genko/app/workspace.py, J1): a search over every command, a quick-access panel
// of chosen commands, a command bar that can be changed, and named workspaces (where the panels are). Everything is
// kept in the settings, so it stays for every book.

namespace genko::app {

// The commands, as Python names them: every command in the menus (each once, by its words), in menu order.
std::vector<QAction*> commands(QMainWindow* window);
std::map<QString, QAction*> by_text(QMainWindow* window);
// The menu path of a command ("ページ → コマ"; empty when it is in no menu).
QString where(QMainWindow* window, const QAction* action);
// An item a menu makes again each time it opens (recent books, オートアクション, ワークスペース, ストック: the menus with
// the property "remade"), deleted then: not a command to keep in the quick access, the command bar or the keys, nor to
// find.
bool remade(const QAction* action);
// The commands whose name, menu or explanation holds every word (in any order, any case).
std::vector<QAction*> search(QMainWindow* window, const QString& words);

// コマンドを探す (Ctrl+Shift+F): type part of a name, Enter does it.
class CommandSearch : public QDialog {
    Q_OBJECT

public:
    explicit CommandSearch(QMainWindow* window);

    void refresh();
    // The command chosen, done (the dialog closed first).
    void run();
    const std::vector<QAction*>& found() const { return found_; }

    QLineEdit* query = nullptr;
    QListWidget* results = nullptr;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QMainWindow* window_;
    std::vector<QAction*> found_;
    // (the same, held so that one a menu has since deleted — its items made again as it opens — is not run: the window
    // stays usable while this is open)
    std::vector<QPointer<QAction>> alive_;
};

// The command names kept under a setting ("|" for a line between), and keep them. A command renamed since keeps its
// place (preferences' RENAMED).
QStringList chosen(const QString& key, const QStringList& defaults);
void keep(const QString& key, const QStringList& names);
extern const QStringList kDefaultCommandBar;
extern const QStringList kDefaultQuick;

// Pick and order commands: every command on the left, the chosen ones on the right.
class ChooseCommands : public QDialog {
    Q_OBJECT

public:
    ChooseCommands(QMainWindow* window, const QString& title, const QStringList& current, bool separators = false);

    QStringList names() const;

    QLineEdit* filter = nullptr;
    QListWidget* all = nullptr;
    QListWidget* mine = nullptr;
    QPushButton* add = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* up = nullptr;
    QPushButton* down = nullptr;
    QPushButton* line = nullptr;     // (区切りを足す: the command bar only)
    QPushButton* restore = nullptr;  // はじめの並びに戻す

private:
    void fill_all();
    void add_chosen();
    void move_chosen(int delta);

    QMainWindow* window_;
};

// クイックアクセス: the commands one uses most, as buttons in one place.
class QuickAccess : public QWidget {
    Q_OBJECT

public:
    explicit QuickAccess(QMainWindow* window);

    QStringList names() const;
    void refresh();
    // 並べるものを選ぶ… (the dialog is exec'ed; true when it was shown).
    bool edit();

    std::vector<QPushButton*> buttons;

private:
    QMainWindow* window_;
    QGridLayout* grid_ = nullptr;
};

// The command bar (under the menus) with the chosen commands: what is marked "keep" (the books' tabs, the room after
// them) stays, the commands are put again.
void fill_commandbar(QMainWindow* window, QToolBar* bar);
void edit_commandbar(QMainWindow* window, QToolBar* bar);

// --- the cursor and the keys held (環境設定) -------------------------------------------------------------------------

using Choices = std::vector<std::pair<QString, QString>>;  // (the words shown, the value kept)
// The pen's pointer: circle | circle_cross | cross | dot (default circle).
const Choices& cursors();
QString cursor_kind();
// What a held Alt or Ctrl switches to ("": nothing): Alt スポイト and Ctrl 選択 unless chosen otherwise.
const Choices& modifier_tool_choices();
QString modifier_tool(const QString& key);
// 道具ごとの修飾キー: the tools that can have their own Alt / Ctrl / Shift, and what those can switch to ("": the
// common setting, "none": nothing).
const Choices& keyed_tools();
const Choices& held_tools();
std::map<QString, std::map<QString, QString>> tool_modifiers();
void set_tool_modifier(const QString& tool, const QString& key, const QString& value);
// キーを押している間だけ持ち替え: a tool's key held down is that tool only while held (on unless turned off).
bool hold_swap();

// Workspaces: where the panels and the window are, kept under a name.
QStringList workspaces();
void save_workspace(QMainWindow* window, const QString& name);
bool load_workspace(QMainWindow* window, const QString& name);
void delete_workspace(const QString& name);

}  // namespace genko::app
