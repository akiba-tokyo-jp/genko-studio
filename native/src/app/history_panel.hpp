#pragma once

#include <QString>
#include <QWidget>

#include <optional>

#include "core/json.hpp"

class QListWidget;
class QListWidgetItem;

// 履歴 (Python's genko/app/history.py): every change in the order it was made, named in words ("ペンで描いた",
// "コマを割った" …), with the ones undone greyed below the present. Clicking one goes back (or forward) to just after it —
// plain undo and redo through the book's session, one step at a time. The changes come from the book's journal on disk
// (saved before this session) and from the session (Session::history).

namespace genko::app {

class MainWindow;

namespace history {

// history.describe(ops): a change in words — the first thing it did (and how many more).
QString describe(const core::Json& ops);
// history._when(at): the time of a saved change ("　HH:MM" today, "　MM/DD HH:MM" before); nothing for unsaved ones.
QString when(std::optional<double> at);

}  // namespace history

class HistoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPanel(MainWindow* window);

    // The list again from the session: （はじめ）, the changes done, the present (▶, bold, chosen), the ones undone.
    void refresh();
    // Undo or redo until `target` changes are done (a refusal said in the status line).
    void go_to(int target);
    // How many changes are done now (the present's row).
    int done() const { return done_; }
    QListWidget* list() const { return list_; }

private:
    MainWindow* window_;
    QListWidget* list_ = nullptr;
    int done_ = 0;
};

}  // namespace genko::app
