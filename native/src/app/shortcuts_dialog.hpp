#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

#include <map>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace genko::app {
class MainWindow;
}

// 編集 → キーボードショートカット… and メニュー… (PC5 H-d, edit.keyboardShortcuts and edit.menus; PhotoCraft v0.2.0's prefs.rs
// keyboard_shortcuts and menus, MIT; SPEC VIEW-01「キーボードショートカットとメニュー」): Photoshop's one window with two tabs —
// moved here from 環境設定's ショートカット page.
// - キーボードショートカット: the key set (Photoshop's, Genko's — the user's decision 8 — and the person's own sets saved under a
//   name, never a built-in set's), every command by its menu (the commands in no menu after them), found by a part of its
//   name or its key; a command's keys back to the set's one by one or all at once; a key another command has said, and
//   either taken from it (前のコマンドから外して付ける, Photoshop's) or given up; the list written out as HTML (概要…).
// - メニュー: each menu's items shown or hidden, and coloured (menu_look).
// Nothing is kept until 決める (as 環境設定): the keys (shortcuts/<name> over the set chosen), the set (ui/key_set, and the
// person's own chosen: ui/key_set_own), the menus' (menus/hidden, menus/colors/<name>) — every window applied.

namespace genko::app {

class ShortcutsDialog : public QDialog {
    Q_OBJECT

public:
    // tab: 0 キーボードショートカット, 1 メニュー
    ShortcutsDialog(MainWindow* window, int tab = 0);

    QTabWidget* tabs = nullptr;
    // --- キーボードショートカット ---
    QComboBox* key_set = nullptr;  // (data: a built-in set's id, or "own:<name>")
    QPushButton* save_set_button = nullptr;
    QPushButton* delete_set_button = nullptr;
    QLineEdit* filter = nullptr;
    QTreeWidget* tree = nullptr;
    std::vector<std::pair<QString, QKeySequenceEdit*>> editors;  // (the command's name, its key) in the tree's order
    QKeySequenceEdit* editor(const QString& command) const;
    QTreeWidgetItem* row(const QString& command) const;
    QLabel* clash = nullptr;
    QPushButton* take_button = nullptr;  // 前のコマンドから外して付ける
    QPushButton* keep_button = nullptr;  // やめる
    QPushButton* reset_one_button = nullptr;
    QPushButton* reset_all_button = nullptr;
    QPushButton* summary_button = nullptr;
    // each command's keys in the set chosen (by its name): what 初期設定に戻す gives it
    std::map<QString, QList<QKeySequence>> set_keys;
    void choose_key_set();
    // The command's key back to the set's (none: the row chosen in the tree).
    void reset_one(const QString& command = {});
    void reset_keys();
    // The key just given to `command` that another command has: said, until taken (the other's taken off) or kept (the
    // key given back to what it was).
    void take_from_previous();
    void keep_previous();
    // 組を保存… under a name (none: asked) — false, said, for a built-in set's name or none; 組を削除 (a built-in one refused).
    bool save_set(std::optional<QString> name = std::nullopt);
    bool delete_set();
    // 概要… (Photoshop's summary): every command, its menu and its keys as a page — written where asked (or `path`).
    QString summary_html() const;
    bool write_summary(const QString& path = {});
    std::vector<std::pair<QString, QStringList>> mapping() const;
    // --- メニュー ---
    QTreeWidget* menu_tree = nullptr;
    QComboBox* menu_colour = nullptr;  // (the colour of the rows chosen)
    QPushButton* menus_reset = nullptr;
    void set_hidden(const QString& command, bool hidden);
    void set_colour(const QString& command, const QString& colour);
    bool is_hidden(const QString& command) const;
    QString colour_of(const QString& command) const;
    void reset_menus();

    // 決める: kept and applied (every window), the dialog accepted — or, with two commands on one key, the clash said and
    // nothing kept.
    void save();

private:
    void fill_keys();
    void fill_menus();
    void apply_filter();
    void key_changed(const QString& name, QKeySequenceEdit* edit);
    void show_menu_row(QTreeWidgetItem* item);

    MainWindow* window_;
    std::map<QString, QKeySequence> shown_;  // (each editor's key before its last change: やめる gives it back)
    struct Pending {
        QString name;
        QString other;
        QKeySequence before;
    };
    std::optional<Pending> pending_;
    bool quiet_ = false;  // (keys put by the dialog itself: not a person's change)
    std::map<QString, std::vector<QTreeWidgetItem*>> menu_rows_;  // (a command's rows in the メニュー tab)
    std::map<QString, bool> hidden_;
    std::map<QString, QString> colours_;
};

}  // namespace genko::app
