#pragma once

#include <QString>
#include <QWidget>

// The way in for typed words on the canvas (M2: only the entrance; typing lines into balloons comes with M4): a
// widget that takes the input method's text — the words still being converted (preedit) shown underlined, and the
// words the person settled on (commit) handed on whole. Keys typed without an input method count as settled at once.

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

}  // namespace genko::app
