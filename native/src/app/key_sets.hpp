#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

#include <map>
#include <utility>
#include <vector>

class QAction;
class QMainWindow;

// キーの組 (PC5 H-0, the user's decision 8; SPEC VIEW-01): the commands' keys come in sets — 「Photoshop」, the keys
// Photoshop gives the same commands (the default), and 「Genko」, the keys Genko had before PC5 (a person used to them
// chooses it back in キーボードショートカット…). A command has its Genko keys where it is made (MainWindow::make); the
// Photoshop set changes only where Photoshop's differ (photoshop_keys: a command Photoshop has keys it gives another, a
// Genko command without its key where Photoshop's command has it). The person's own keys (環境設定's shortcuts/<name>)
// lie over either set.

namespace genko::app::key_sets {

struct Set {
    QString id;     // kept in settings.ini: ui/key_set (core/prefs_table: keyboardShortcuts.keySet)
    QString label;  // the words shown
};

// The sets, the default first.
const std::vector<Set>& sets();
QString current();
void set_current(const QString& id);

// The Photoshop set where it is not Genko's: a command (by its attribute "act_x", or a panel's switch by its words) and
// its keys there (none: no key). Each part of PC5 that makes a command Photoshop gives a key adds its row here.
const std::vector<std::pair<QString, QList<QKeySequence>>>& photoshop_keys();

// The keys of a command in a set (its Genko keys kept the first time a set is applied to its window).
QList<QKeySequence> keys_of(const QAction* action, const QString& set);
// Every command of the window with the set's keys (the person's own laid over them after: preferences::apply_shortcuts).
void apply(QMainWindow* window, const QString& set);

}  // namespace genko::app::key_sets
