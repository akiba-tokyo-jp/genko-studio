#pragma once

#include <QDialog>
#include <QLabel>
#include <QString>

class QPlainTextEdit;
class QPushButton;

// A place on the disk shown in little room (SPEC UX-02, I12): the path is cut in the middle to fit, never only shown
// in part — a click (or Enter) opens the whole of it, with コピー and フォルダーを開く. The status bar and the save
// failure notice use it, so a long Japanese folder name cannot be mistaken for another.

namespace genko::app {

class PathDialog : public QDialog {
    Q_OBJECT

public:
    PathDialog(QWidget* parent, const QString& title, const QString& path);

    QString shown() const;
    QPushButton* copy_button() const { return copy_; }
    QPushButton* open_button() const { return open_; }

private:
    QString path_;
    QPlainTextEdit* text_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* open_ = nullptr;
};

class PathLabel : public QLabel {
    Q_OBJECT

public:
    explicit PathLabel(QWidget* parent = nullptr);

    // lead: the words before the path ("保存先: "); none for no path.
    void set_path(const QString& path, const QString& lead = {});
    QString path() const { return path_; }
    // The text shown now (cut to the room there is).
    QString shown() const { return text(); }
    // The whole path in a window (modeless); the window is returned (tests).
    PathDialog* show_full();
    // The folder that holds the book (what フォルダーを開く opens).
    static QString folder_of(const QString& path);
    // Put the path on the clipboard.
    static void copy(const QString& path);
    static bool open_folder(const QString& path);

signals:
    void opened(genko::app::PathDialog* dialog);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    void elide();

    QString path_;
    QString lead_;
};

}  // namespace genko::app
