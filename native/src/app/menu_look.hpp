#pragma once

#include <QColor>
#include <QRect>
#include <QString>
#include <QStringList>

#include <map>
#include <utility>
#include <vector>

class QAction;
class QMenu;

namespace genko::app {
class MainWindow;
}

// 編集 → メニュー… (PC5 H-d, edit.menus; PhotoCraft v0.2.0's prefs.rs menus, MIT; SPEC VIEW-01「キーボードショートカットとメニュー」):
// the menus' items one does not use hidden, the ones one looks for coloured — Photoshop's. Both by the command's name (as
// the keys are: workspace's command_names), kept in this computer's settings (menus/hidden, menus/colors/<name>), never in
// a book. A hidden item is taken out of its menu only while the menu is open: the command stays the window's — its keys
// work, 検索 finds it, the help names its place, the command bar and the palette keep it — and a menu with hidden items
// ends with すべてのメニュー項目を表示 (the menu opened again with them all). Hiding never changes what a command does: the
// approval, the lock and the AI's connection ask as before when they run.

namespace genko::app::menu_look {

struct Colour {
    QString id;     // kept: none | red | orange | yellow | green | blue | violet | gray
    QString label;  // the words shown
    QColor colour;
};
// Photoshop's colours, なし first.
const std::vector<Colour>& colours();
QColor colour(const QString& id);  // (invalid for none or an unknown one)

// What is kept: the commands hidden (their names), and each coloured command's colour.
QStringList hidden();
void set_hidden(const QStringList& names);
std::map<QString, QString> coloured();
void set_coloured(const std::map<QString, QString>& colours);

// The window's menus follow what is kept (called by preferences::apply_all): every menu of the menu bar, and the menus in
// them, hides the hidden as it opens and paints the colours.
void apply(MainWindow* window);

// (tests) The words of the item that shows a menu's hidden items, and the colours painted over a menu's items now.
extern const char* const kShowAll;
std::vector<std::pair<QRect, QColor>> painted(QMenu* menu);

}  // namespace genko::app::menu_look
