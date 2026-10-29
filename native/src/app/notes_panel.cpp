#include "app/notes_panel.hpp"

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/main_window.hpp"
#include "core/annotations.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kId = Qt::UserRole + 11;  // (a note's id)

QString when_of(const core::Annotation& note) {
    if (note.modified.empty()) return {};
    return QDateTime::fromString(QString::fromStdString(note.modified), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

const core::Annotation* note_of(const core::Page* page, const QString& id) {
    if (page == nullptr) return nullptr;
    for (const core::Annotation& a : page->annotations)
        if (QString::fromStdString(a.id) == id) return &a;
    return nullptr;
}

}  // namespace

NotesPanel::NotesPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("notes_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("notes_list"));
    list_->setToolTip(QStringLiteral("このページの注釈（注釈ツールでページをクリックすると足せます）"));
    layout->addWidget(list_, 1);
    head_ = new QLabel;
    head_->setObjectName(QStringLiteral("note_head"));
    head_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(head_);
    text_ = new QPlainTextEdit;
    text_->setObjectName(QStringLiteral("note_text"));
    text_->setPlaceholderText(QStringLiteral("注釈の本文"));
    text_->installEventFilter(this);
    layout->addWidget(text_, 2);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    const auto add = [&](const QString& text, const QString& tip, const QString& name, auto slot) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        connect(b, &QToolButton::clicked, this, slot);
        row->addWidget(b);
        return b;
    };
    add(QStringLiteral("◀"), QStringLiteral("前の注釈"), QStringLiteral("note_previous"), [this] { step(-1); });
    add(QStringLiteral("▶"), QStringLiteral("次の注釈"), QStringLiteral("note_next"), [this] { step(1); });
    count_ = new QLabel;
    count_->setObjectName(QStringLiteral("note_count"));
    row->addWidget(count_);
    row->addStretch(1);
    add(QStringLiteral("削除"), QStringLiteral("注釈を削除"), QStringLiteral("note_delete"), [this] { remove_chosen(); });
    layout->addLayout(row);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (loading_ || item == nullptr) return;
        window_->choose_note(item->data(kId).toString());
    });
}

NotesPanel::~NotesPanel() {
    if (text_ != nullptr) text_->removeEventFilter(this);  // (its focus lost as the window goes: no op then)
}

QToolButton* NotesPanel::button(const QString& name) const { return findChild<QToolButton*>(name); }

void NotesPanel::refresh() {
    loading_ = true;
    const core::Page* page = window_->current_page();
    list_->clear();
    int at = -1;
    if (page != nullptr) {
        for (const core::Annotation& a : page->annotations) {
            QString first = QString::fromStdString(a.text).section(QLatin1Char('\n'), 0, 0);
            if (first.isEmpty()) first = QStringLiteral("（本文なし）");
            QString who = QString::fromStdString(a.author);
            const QString when = when_of(a);
            QString text = (core::annotation_by_person(a.actor) ? QString() : QStringLiteral("★ ")) + first;
            if (!who.isEmpty() || !when.isEmpty()) text += QStringLiteral(" — ") + QStringList{who, when}.join(QLatin1Char(' ')).trimmed();
            auto* item = new QListWidgetItem(text);
            item->setData(kId, QString::fromStdString(a.id));
            item->setToolTip(core::annotation_by_person(a.actor) ? QString::fromStdString(a.text)
                                                                : QStringLiteral("エージェント（%1）の注釈\n").arg(QString::fromStdString(a.actor)) +
                                                                      QString::fromStdString(a.text));
            list_->addItem(item);
            if (QString::fromStdString(a.id) == chosen_) at = list_->count() - 1;
        }
    }
    if (at < 0) chosen_.clear();
    if (at >= 0) list_->setCurrentRow(at);
    loading_ = false;
    show_chosen();
}

void NotesPanel::show_chosen() {
    const core::Page* page = window_->current_page();
    const core::Annotation* note = note_of(page, chosen_);
    const QSignalBlocker quiet(text_);
    if (note == nullptr) {
        head_->setText(page != nullptr && !page->annotations.empty() ? QStringLiteral("注釈を選んでください") : QStringLiteral("このページには注釈がありません"));
        text_->setPlainText(QString());
        text_->setEnabled(false);
        shown_text_.clear();
    } else {
        QString head = QString::fromStdString(note->author);
        if (head.isEmpty()) head = QStringLiteral("（作成者なし）");
        if (const QString when = when_of(*note); !when.isEmpty()) head += QStringLiteral("　") + when;
        head_->setText(head);
        shown_text_ = note->text;
        text_->setPlainText(QString::fromStdString(note->text));
        text_->setEnabled(true);
    }
    const int n = page != nullptr ? static_cast<int>(page->annotations.size()) : 0;
    count_->setText(n == 0 ? QString() : QStringLiteral("%1 / %2").arg(std::max(0, list_->currentRow()) + 1).arg(n));
    for (const char* name : {"note_previous", "note_next"}) {
        if (QToolButton* b = button(QString::fromLatin1(name))) b->setEnabled(n > 1 || (n == 1 && note == nullptr));
    }
    if (QToolButton* b = button(QStringLiteral("note_delete"))) b->setEnabled(note != nullptr);
}

void NotesPanel::choose(const QString& id) {
    if (id != chosen_) commit_text();
    chosen_ = id;
    loading_ = true;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(kId).toString() == id) list_->setCurrentRow(i);
    }
    if (id.isEmpty()) list_->setCurrentRow(-1);
    loading_ = false;
    show_chosen();
}

void NotesPanel::step(int by) {
    const int n = list_->count();
    if (n == 0) return;
    const int now = list_->currentRow();
    const int next = now < 0 ? (by > 0 ? 0 : n - 1) : ((now + by) % n + n) % n;
    window_->choose_note(list_->item(next)->data(kId).toString());
}

void NotesPanel::remove_chosen() {
    if (chosen_.isEmpty()) return;
    const QString id = chosen_;
    shown_text_.clear();
    chosen_.clear();
    window_->note_op(Json{{"op", "delete_annotations"}, {"ids", Json::array({id.toStdString()})}});
}

void NotesPanel::commit_text() {
    if (chosen_.isEmpty() || !text_->isEnabled()) return;
    if (note_of(window_->current_page(), chosen_) == nullptr) return;  // (another page shown now: its note is not here)
    const std::string now = text_->toPlainText().toStdString();
    if (now == shown_text_) return;
    shown_text_ = now;
    window_->note_op(Json{{"op", "set_annotation"}, {"id", chosen_.toStdString()}, {"text", now}});
}

bool NotesPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == text_ && event->type() == QEvent::FocusOut) commit_text();
    return QWidget::eventFilter(watched, event);
}

}  // namespace genko::app
