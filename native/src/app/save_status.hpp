#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "app/session.hpp"

// The save state shown to the person (SPEC SAVE-01: 保存済み・未保存・保存中・保存失敗・復旧用コピーのみ), the notice
// when a save fails (why, where, when the book was last saved; 再試行, 別の場所に保存, 復旧用コピーの作成), and the
// question before a book with unsaved changes is closed (キャンセル is the default: Enter never throws work away).

namespace genko::app {

class PathLabel;

// The state in words: "保存済み・14:32（世代 5）", "未保存", "保存中…", "保存失敗", "復旧用コピーのみ".
QString state_words(const SaveStatus& status);
// Why a save failed, in the person's words.
QString failure_reason(const SaveStatus& status);

// The state in the status bar, and where the book is kept (the whole path one click away).
class SaveStatusLabel : public QWidget {
    Q_OBJECT

public:
    explicit SaveStatusLabel(QWidget* parent = nullptr);
    void show_status(const SaveStatus& status);
    QLabel* state() const { return state_; }
    PathLabel* where() const { return where_; }

private:
    QLabel* state_ = nullptr;
    PathLabel* where_ = nullptr;
};

// The notice over the page while a save has failed.
class SaveFailureBar : public QWidget {
    Q_OBJECT

public:
    explicit SaveFailureBar(QWidget* parent = nullptr);
    void show_status(const SaveStatus& status);
    QLabel* words() const { return words_; }
    PathLabel* where() const { return where_; }
    QPushButton* retry_button() const { return retry_; }
    QPushButton* save_as_button() const { return save_as_; }
    QPushButton* recovery_button() const { return recovery_; }

signals:
    void retry();
    void saveAs();
    void recoveryCopy();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void refresh_words_height();
    QLabel* words_ = nullptr;
    PathLabel* where_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* save_as_ = nullptr;
    QPushButton* recovery_ = nullptr;
};

// 保存されていない変更があります: what to do with them before the book is closed (or the window is, or Genko).
class CloseGuard : public QDialog {
    Q_OBJECT

public:
    enum class Choice { Cancel, Save, SaveAs, Discard };
    // verb: 閉じる (a tab, a window, Genko) or 切り替える (to another book)
    CloseGuard(QWidget* parent, const SaveStatus& status, const QString& title, const QString& verb = QStringLiteral("閉じる"));
    Choice choice() const { return choice_; }
    QPushButton* save_button() const { return save_; }
    QPushButton* save_as_button() const { return save_as_; }
    QPushButton* discard_button() const { return discard_; }
    QPushButton* cancel_button() const { return cancel_; }
    // The second question before throwing changes away (its default is to go back). Tests answer it.
    static bool confirm_discard(QWidget* parent, const QString& title);

private:
    Choice choice_ = Choice::Cancel;
    QPushButton* save_ = nullptr;
    QPushButton* save_as_ = nullptr;
    QPushButton* discard_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

}  // namespace genko::app
