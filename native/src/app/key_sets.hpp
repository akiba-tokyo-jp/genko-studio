#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

#include <map>
#include <utility>
#include <vector>

class QAction;
class QMainWindow;

namespace genko::app {
class MainWindow;
}

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

// Photoshop's own keys (its tools' letters and their groups' Shift+letter, its menus' keys: PortableText), and the
// Genko-only tools that take a key Photoshop leaves free in the Photoshop set (the user's decision 2026-10-29: never one of
// photoshop_defaults — test_gui_actions' theKeySets).
const QStringList& photoshop_defaults();
const std::vector<QString>& genko_tools();

// The keys of a command in a set (its Genko keys kept the first time a set is applied to its window).
QList<QKeySequence> keys_of(const QAction* action, const QString& set);
// The default became the Photoshop set with PC5 (the coordinator's decision with the user's decision 8): a person whose
// settings are from before — settings.ini with keys, no key set chosen — is told once, on the app's start, and may go back
// to the Genko set there. from_before_pc5 looks before the app writes anything (marking it told: ui/key_set_told);
// tell_new_default says it in the window and puts 「Genko の組に戻す」 in its status bar (gone when chosen, or after a while).
bool from_before_pc5();
void tell_new_default(MainWindow* window);

// Every command of the window with the set's keys (the person's own laid over them after: preferences::apply_shortcuts).
// `set`: a built-in set's id, or "own:<name>" — the person's own set (PC5 H-d).
void apply(QMainWindow* window, const QString& set);

// PC5 H-d (キーボードショートカット…'s 組を保存・組を削除, Photoshop's): the person's own sets, each a built-in set and the keys laid
// over it by command name (key_sets/<name>/base, key_sets/<name>/keys: {"<name>": "Ctrl+X|Ctrl+Y" — "" none}), kept on this
// computer. The set in use: ui/key_set the built-in one, ui/key_set_own the person's own over it (none: the built-in).
QStringList own_sets();
bool reserved(const QString& name);  // (a built-in set's id or words: never an own set's name)
QString own_base(const QString& name);
std::map<QString, QList<QKeySequence>> own_keys(const QString& name);
void save_own(const QString& name, const QString& base, const std::map<QString, QList<QKeySequence>>& keys);
void delete_own(const QString& name);
QString current_own();  // (none: no own set in use, or it is gone)
QString chosen();       // ("own:<name>" or current())
// The keys of each command in a set (by its name, as preferences::commands names them).
std::map<QString, QList<QKeySequence>> keys_by_name(QMainWindow* window, const QString& set);
QString label(const QString& set);  // (the words of a set)

}  // namespace genko::app::key_sets
