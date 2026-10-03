#include "app/path_label.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "app/look.hpp"
#include "app/theme.hpp"

namespace genko::app {

PathDialog::PathDialog(QWidget* parent, const QString& title, const QString& path) : QDialog(parent), path_(path) {
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(title);
    theme::role(heading, "heading");
    layout->addWidget(heading);
    // the whole path, wrapped and selectable (a long Japanese name never runs out of sight)
    text_ = new QPlainTextEdit(QDir::toNativeSeparators(path));
    text_->setObjectName(QStringLiteral("fullPath"));
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    text_->setWordWrapMode(QTextOption::WrapAnywhere);
    text_->setMinimumHeight(80);
    layout->addWidget(text_, 1);
    auto* buttons = new QDialogButtonBox;
    copy_ = buttons->addButton(QStringLiteral("コピー"), QDialogButtonBox::ActionRole);
    copy_->setToolTip(QStringLiteral("この場所をクリップボードに写します"));
    open_ = buttons->addButton(QStringLiteral("フォルダーを開く"), QDialogButtonBox::ActionRole);
    open_->setToolTip(QStringLiteral("原稿の入っているフォルダーを開きます"));
    QPushButton* close = buttons->addButton(QStringLiteral("閉じる"), QDialogButtonBox::RejectRole);
    close->setDefault(true);
    connect(copy_, &QPushButton::clicked, this, [this] {
        PathLabel::copy(path_);
        copy_->setText(QStringLiteral("コピーしました"));
    });
    connect(open_, &QPushButton::clicked, this, [this] { PathLabel::open_folder(path_); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    look::fit_to_screen(this, QSize(560, 220));
}

QString PathDialog::shown() const { return text_->toPlainText(); }

PathLabel::PathLabel(QWidget* parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setObjectName(QStringLiteral("pathLabel"));
}

void PathLabel::set_path(const QString& path, const QString& lead) {
    path_ = path;
    lead_ = lead;
    const QString native = QDir::toNativeSeparators(path);
    setToolTip(path.isEmpty() ? QString() : native + QStringLiteral("\nクリックで全文・コピー・フォルダーを開く"));
    setAccessibleName(lead + native);
    elide();
}

QSize PathLabel::minimumSizeHint() const { return QSize(80, QLabel::minimumSizeHint().height()); }

void PathLabel::elide() {
    if (path_.isEmpty()) {
        setText(lead_);
        return;
    }
    const QString native = QDir::toNativeSeparators(path_);
    const int room = std::max(40, width() - fontMetrics().horizontalAdvance(lead_) - 4);
    // (cut in the middle: the drive or home at the start and the book's own name at the end stay)
    setText(lead_ + fontMetrics().elidedText(native, Qt::ElideMiddle, room));
}

void PathLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    elide();
}

void PathLabel::mousePressEvent(QMouseEvent* event) {
    QLabel::mousePressEvent(event);
    if (!path_.isEmpty()) show_full();
}

void PathLabel::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) && !path_.isEmpty()) {
        show_full();
        return;
    }
    QLabel::keyPressEvent(event);
}

PathDialog* PathLabel::show_full() {
    auto* dialog = new PathDialog(window(), QStringLiteral("保存先"), path_);
    dialog->show();
    emit opened(dialog);
    return dialog;
}

QString PathLabel::folder_of(const QString& path) {
    const QFileInfo info(path);
    // a book is a folder (.genko): its folder is the one it is in
    return info.absolutePath();
}

void PathLabel::copy(const QString& path) { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); }

bool PathLabel::open_folder(const QString& path) { return QDesktopServices::openUrl(QUrl::fromLocalFile(folder_of(path))); }

}  // namespace genko::app
