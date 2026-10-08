#pragma once

#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>

// The way in for typed words on the canvas: a widget that takes the input method's text — the words still being
// converted (preedit) shown underlined, and the words the person settled on (commit) handed on whole. Keys typed
// without an input method count as settled at once. And the text tool's editor (Python's canvas.InlineEditor): a line
// typed where it goes, the input method's words converted in place.

namespace genko::app {

class ImeEntry : public QWidget {
    Q_OBJECT

public:
    explicit ImeEntry(QWidget* parent = nullptr);

    const QString& preedit() const { return preedit_; }
    // Everything settled so far.
    const QString& text() const { return text_; }
    void clear();

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void committed(const QString& words);
    void preeditChanged(const QString& words);

protected:
    void inputMethodEvent(QInputMethodEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override { return QSize(240, 32); }

private:
    QString preedit_;
    QString text_;
};

// Typing a line where it goes (Python's canvas.InlineEditor): Ctrl+Enter (or clicking elsewhere) keeps it, Esc drops
// it; Enter starts the next column. The input method's words being converted stay in the editor until they are settled
// (Ctrl+Enter while converting does not end the line). The right-click menu styles the characters chosen
// (text_style.hpp). Finished, it hides and goes (deleted later), then hands on the words — stripped — or nothing.
class InlineEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    InlineEditor(QWidget* parent, const QString& text, std::function<void(std::optional<QString>)> on_done);
    ~InlineEditor() override;

    // Shown at this point of the parent (260 × 110 px), its hint above it, with the focus and the cursor at the end.
    void place(double x, double y);
    void finish(bool keep);
    bool finished() const { return finished_; }
    const QString& preedit() const { return preedit_; }
    QLabel* hint() const { return hint_; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

private:
    std::function<void(std::optional<QString>)> on_done_;
    QPointer<QLabel> hint_;
    QString preedit_;
    bool finished_ = false;
};

}  // namespace genko::app
