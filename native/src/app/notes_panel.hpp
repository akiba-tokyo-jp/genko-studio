#pragma once

#include <QWidget>

#include <string>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QToolButton;

// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): Photoshop's 注釈 パネル for the page shown — its notes listed (each
// row its author, date and first line; ★ before an agent's), the chosen one's author and date above its text, which is
// edited there (set_annotation when the text is left: another note chosen, the panel left), and the buttons 前の注釈・
// 次の注釈・注釈を削除. The note chosen is the one the canvas draws chosen (MainWindow::choose_note). Each change an op
// through the window.

namespace genko::app {

class MainWindow;

class NotesPanel : public QWidget {
    Q_OBJECT
public:
    explicit NotesPanel(MainWindow* window);
    // The page's notes again (after a change, another page), the chosen one kept while the page has it.
    void refresh();
    // The note shown ("": none).
    QString chosen() const { return chosen_; }
    void choose(const QString& id);
    // 前の注釈・次の注釈 (-1, +1, round), 注釈を削除, the text edited put in the book (none when it is as it was).
    void step(int by);
    void remove_chosen();
    void commit_text();
    // Tests: the list, the text, the head (author and date), a button (note_previous, note_next, note_delete).
    QListWidget* list() const { return list_; }
    QPlainTextEdit* text() const { return text_; }
    QLabel* head() const { return head_; }
    QToolButton* button(const QString& name) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void show_chosen();

    MainWindow* window_;
    QListWidget* list_ = nullptr;
    QLabel* head_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QLabel* count_ = nullptr;
    QString chosen_;
    std::string shown_text_;  // (the chosen note's text as the book has it: an edit is committed when it differs)
    bool loading_ = false;
};

}  // namespace genko::app
