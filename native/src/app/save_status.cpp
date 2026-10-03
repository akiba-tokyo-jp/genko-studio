#include "app/save_status.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/look.hpp"
#include "app/path_label.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/paths.hpp"

namespace genko::app {

namespace {

QString when(const QDateTime& at) {
    if (!at.isValid()) return {};
    return at.date() == QDate::currentDate() ? at.toString(QStringLiteral("HH:mm")) : at.toString(QStringLiteral("M月d日 HH:mm"));
}

QString target_of(const SaveStatus& status) {
    return status.target ? QString::fromStdString(core::path_to_utf8(*status.target)) : QString();
}

}  // namespace

QString state_words(const SaveStatus& status) {
    switch (status.kind) {
    case SaveKind::Saved: {
        QString words = QStringLiteral("保存済み");
        if (status.last_saved.isValid()) {
            words += QStringLiteral("・%1（世代 %2）").arg(when(status.last_saved)).arg(status.last_revision);
        } else if (status.target) {
            words += QStringLiteral("（世代 %1）").arg(status.last_revision);  // (as it was opened: nothing saved since)
        }
        return words;
    }
    case SaveKind::Dirty:
        return status.target ? QStringLiteral("未保存（まもなく自動で保存）") : QStringLiteral("未保存（ファイル → 別の場所に保存）");
    case SaveKind::Saving:
        return status.newer_waiting ? QStringLiteral("保存中…（その後の変更は次に保存）") : QStringLiteral("保存中…");
    case SaveKind::Failed:
        return QStringLiteral("保存失敗");
    case SaveKind::RecoveryOnly:
        return QStringLiteral("復旧用コピーのみ（原稿には未保存）");
    }
    return {};
}

QString failure_reason(const SaveStatus& status) {
    const QString& m = status.reason;
    if (status.code == QLatin1String("locked")) return QStringLiteral("この原稿は、ほかの Genko か AI が作業中です（しばらくして自動でやり直します）");
    if (m.contains(QStringLiteral("No space left"))) return QStringLiteral("ディスクの空きがありません");
    if (m.contains(QStringLiteral("Permission denied")) || m.contains(QStringLiteral("Read-only file system")) ||
        m.contains(QStringLiteral("Access is denied"))) {
        return QStringLiteral("この場所には書き込めません（読み取り専用か、書き込みが許されていません）");
    }
    if (status.code == QLatin1String("read_only")) return QStringLiteral("この原稿は読み取り専用で開いています");
    if (m.isEmpty()) return QStringLiteral("原稿に書き込めませんでした");
    return wording::error(m);
}

SaveStatusLabel::SaveStatusLabel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    state_ = new QLabel;
    state_->setObjectName(QStringLiteral("saveState"));
    where_ = new PathLabel;
    where_->setMaximumWidth(360);
    layout->addWidget(state_);
    layout->addWidget(where_, 1);
}

void SaveStatusLabel::show_status(const SaveStatus& status) {
    state_->setText(state_words(status));
    const bool bad = status.kind == SaveKind::Failed || status.kind == SaveKind::RecoveryOnly;
    state_->setStyleSheet(bad ? QStringLiteral("color: %1; font-weight: 600;").arg(theme::tokens().danger) : QString());
    QString tip = state_words(status);
    if (bad) tip += QStringLiteral("\n") + failure_reason(status);
    state_->setToolTip(tip);
    where_->set_path(target_of(status), status.target ? QStringLiteral("保存先: ") : QString());
}

SaveFailureBar::SaveFailureBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("saveFailure"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(4);
    words_ = new QLabel;
    words_->setWordWrap(true);
    words_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    words_->setTextFormat(Qt::PlainText);
    where_ = new PathLabel;
    auto* row = new QHBoxLayout;
    retry_ = new QPushButton(QStringLiteral("再試行"));
    retry_->setToolTip(QStringLiteral("もう一度、原稿に保存します"));
    save_as_ = new QPushButton(QStringLiteral("別の場所に保存…"));
    save_as_->setToolTip(QStringLiteral("書き込める別の場所に、この原稿を保存します"));
    recovery_ = new QPushButton(QStringLiteral("復旧用コピーを作る"));
    recovery_->setToolTip(QStringLiteral("設定フォルダーの復旧用の場所に、今の原稿を書きます（次に開くときに戻せます）"));
    theme::primary(retry_);
    row->addWidget(where_, 1);
    row->addWidget(retry_);
    row->addWidget(save_as_);
    row->addWidget(recovery_);
    layout->addWidget(words_);
    layout->addLayout(row);
    connect(retry_, &QPushButton::clicked, this, &SaveFailureBar::retry);
    connect(save_as_, &QPushButton::clicked, this, &SaveFailureBar::saveAs);
    connect(recovery_, &QPushButton::clicked, this, &SaveFailureBar::recoveryCopy);
    hide();
}

void SaveFailureBar::show_status(const SaveStatus& status) {
    const bool bad = status.kind == SaveKind::Failed || status.kind == SaveKind::RecoveryOnly;
    if (!bad) {
        hide();
        return;
    }
    QString text = QStringLiteral("⚠ 原稿に保存できませんでした: %1。").arg(failure_reason(status));
    text += status.last_saved.isValid() ? QStringLiteral("最後に保存できたのは %1（世代 %2）です。").arg(when(status.last_saved)).arg(status.last_revision)
                                        : QStringLiteral("この原稿を開いてからは、まだ保存できていません。");
    if (status.kind == SaveKind::RecoveryOnly) {
        text += QStringLiteral("変更は復旧用のコピーに書きました（%1）。原稿そのものには、まだ入っていません。").arg(when(status.recovery_time));
    } else if (status.nowhere) {
        text += QStringLiteral("原稿にも復旧用の場所にも書き込めません。この変更は、まだどこにも保存されていません。");
    } else if (status.recovery_time.isValid()) {
        text += QStringLiteral("復旧用のコピー（%1）には、その後の変更は入っていません。").arg(when(status.recovery_time));
    }
    words_->setText(text);
    words_->setToolTip(status.reason);
    where_->set_path(target_of(status), QStringLiteral("保存先: "));
    refresh_words_height();
    show();  // present the new text and its wrap height together, not an old one-line hint
}

void SaveFailureBar::refresh_words_height() {
    if (words_ == nullptr || layout() == nullptr) return;
    const QMargins margins = layout()->contentsMargins();
    const int available = qMax(1, contentsRect().width() - margins.left() - margins.right());
    const QMargins text_margins = words_->contentsMargins();
    const int inset = 2 * words_->margin();
    const int text_width = qMax(1, available - text_margins.left() - text_margins.right() - inset);
    // QLabel::heightForWidth is clamped by our previous minimumHeight. Measure the plain
    // wrapped text independently so narrower/longer messages cannot keep a stale minimum.
    const QRect bounds = words_->fontMetrics().boundingRect(QRect(0, 0, text_width, 0),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, words_->text());
    const int needed = bounds.height() + text_margins.top() + text_margins.bottom() + inset;
    if (needed > 0 && words_->minimumHeight() != needed) words_->setMinimumHeight(needed);
}

void SaveFailureBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh_words_height();
}

CloseGuard::CloseGuard(QWidget* parent, const SaveStatus& status, const QString& title, const QString& verb) : QDialog(parent) {
    setWindowTitle(QStringLiteral("保存されていない変更"));
    auto* heading = new QLabel(QStringLiteral("「%1」には、原稿に保存されていない変更があります").arg(title));
    heading->setWordWrap(true);
    theme::role(heading, "heading");
    QString text = QStringLiteral("今の状態: %1").arg(state_words(status));
    if (status.kind == SaveKind::Failed || status.kind == SaveKind::RecoveryOnly) {
        text += QStringLiteral("\n理由: %1").arg(failure_reason(status));
        if (status.last_saved.isValid()) text += QStringLiteral("\n最後に保存できた時刻: %1").arg(when(status.last_saved));
        if (status.kind == SaveKind::RecoveryOnly) text += QStringLiteral("\n変更は復旧用のコピーにあります（原稿そのものには入っていません）。");
        if (status.nowhere) text += QStringLiteral("\n⚠ 原稿にも復旧用の場所にも書き込めていません。");
    }
    auto* words = new QLabel(text);
    words->setWordWrap(true);
    words->setTextFormat(Qt::PlainText);
    auto* where = new PathLabel;
    where->set_path(target_of(status), status.target ? QStringLiteral("保存先: ") : QString());
    save_ = new QPushButton(QStringLiteral("保存して%1").arg(verb));
    save_->setToolTip(QStringLiteral("原稿に保存できたときだけ%1ます").arg(verb.left(verb.size() - 1)));
    save_as_ = new QPushButton(QStringLiteral("別の場所に保存して%1").arg(verb));
    discard_ = new QPushButton(QStringLiteral("保存せずに%1…").arg(verb));
    discard_->setToolTip(QStringLiteral("保存していない変更を捨てます（もう一度確かめます）"));
    cancel_ = new QPushButton(QStringLiteral("キャンセル"));
    // the safe answer is the default one: Enter (or Esc) keeps everything as it is
    for (QPushButton* button : {save_, save_as_, discard_}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    cancel_->setAutoDefault(true);
    cancel_->setDefault(true);
    theme::primary(cancel_);  // the safe default must be visible, not merely an internal Qt flag
    if (!status.target) save_->setEnabled(false);  // (a book never given a folder: only 別の場所に保存)
    connect(save_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::Save;
        accept();
    });
    connect(save_as_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::SaveAs;
        accept();
    });
    connect(discard_, &QPushButton::clicked, this, [this, title] {
        if (!confirm_discard(this, title)) return;
        choice_ = Choice::Discard;
        accept();
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        choice_ = Choice::Cancel;
        reject();
    });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(discard_);
    buttons->addStretch(1);
    buttons->addWidget(cancel_);
    buttons->addWidget(save_as_);
    buttons->addWidget(save_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(words);
    layout->addWidget(where);
    layout->addSpacing(8);
    layout->addLayout(buttons);
    look::fit_to_screen(this, QSize(620, 240));
    cancel_->setFocus();
}

bool CloseGuard::confirm_discard(QWidget* parent, const QString& title) {
    if (const auto r = ask::responder(); r && r->question) {
        return r->question(QStringLiteral("変更を捨てる"), QStringLiteral("「%1」の保存していない変更を捨てて閉じますか？").arg(title));
    }
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("変更を捨てる"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("「%1」の保存していない変更を捨てて閉じますか？\nこの変更は元に戻せません。").arg(title));
    QPushButton* drop = box.addButton(QStringLiteral("捨てて閉じる"), QMessageBox::DestructiveRole);
    QPushButton* back = box.addButton(QStringLiteral("戻る"), QMessageBox::RejectRole);
    box.setDefaultButton(back);
    box.setEscapeButton(back);
    box.exec();
    return box.clickedButton() == drop;
}

}  // namespace genko::app
