#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <tuple>
#include <vector>

class QMainWindow;
class QTextBrowser;

// ヘルプ (Python's genko/app/help.py): a one-page guide from the name to the export, answers for when something seems
// stuck, the shortcut list made from the menus as they are now, and about Genko. The guide and the answers name the
// menus and keys through the commands themselves (SPEC UX-03, I10): {menu:act_x} is written 「ファイル → 新しい原稿…」 as
// the menus hold it now, {key:act_x} its keys now, {submenu:title} a menu inside a menu by its path, {top:title} a menu
// of the menu bar, {items:title} the commands of a menu inside a menu (「…」「…」) — so a menu moved or a key changed
// changes the help with it, and a command that is not in the menus is found (render's `missing`) instead of being
// described (the text never shows a command's inner name). The words taken from the menus are HTML-escaped.

namespace genko::app::help {

extern const char* const kGuide;
extern const char* const kFaq;

struct Rendered {
    QString html;
    QStringList missing;  // the references that name no command in the menus (none: every one found)
    QStringList keyless;  // the {key:} references to a command without keys now (written キーなし)
};
Rendered render(QMainWindow* window, const QString& text);

QString guide(QMainWindow* window);
QString faq(QMainWindow* window);
QString about();
// (menu, command, keys) for every command that has keys, in menu order.
std::vector<std::tuple<QString, QString, QString>> shortcut_rows(QMainWindow* window);
QString shortcut_html(QMainWindow* window);

class HelpDialog : public QDialog {
    Q_OBJECT

public:
    HelpDialog(QWidget* parent, const QString& title, const QString& html);
    QTextBrowser* text = nullptr;
};

}  // namespace genko::app::help
