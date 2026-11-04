// ウィンドウ → アクション (PC5 J-g, window.panel.actions; SPEC.md PROD-01「アクション」).

#include "app/actions_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/actions.hpp"
#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"

namespace genko::app {

using core::Json;

ActionsPanel::ActionsPanel(MainWindow* window) : QWidget(window), window_(window) {
    names = new QListWidget;
    names->setObjectName(QStringLiteral("actionNames"));
    steps = new QListWidget;
    steps->setObjectName(QStringLiteral("actionSteps"));
    state = new QLabel;
    state->setWordWrap(true);
    theme::hint(state);
    const auto button = [this](const QString& text, const QString& tip, auto slot) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, slot);
        return b;
    };
    stop = button(QStringLiteral("■ 停止"), QStringLiteral("記録を止めて、記録した手順をアクションに残します"), [this] { finish(); });
    record = button(QStringLiteral("● 記録"), QStringLiteral("新規アクション: 名前を決めて、これからの操作を記録します"), [this] { start(); });
    play = button(QStringLiteral("▶ 再生"), QStringLiteral("選んだアクションを、前のページ（描いているレイヤー・選んだコマ）で再生します"), [this] {
        if (const auto name = current()) window_->play_action(*name);
    });
    remove = button(QStringLiteral("削除"), QStringLiteral("選んだアクションを削除します"), [this] {
        if (const auto name = current()) {
            window_->remove_action(*name);
            refresh();
        }
    });
    manage = button(QStringLiteral("管理…"), QStringLiteral("手順の並べ替え・外す・名前の変更"), [this] {
        QDialog* dialog = window_->manage_actions();
        connect(dialog, &QObject::destroyed, this, [this] { refresh(); });
    });
    connect(names, &QListWidget::currentRowChanged, this, [this](int) {
        fill_steps();
        play->setEnabled(current().has_value() && !window_->recording());
        remove->setEnabled(current().has_value() && !window_->recording());
    });
    connect(names, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (const auto name = current(); name && !window_->recording()) window_->play_action(*name);
    });
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    for (QToolButton* b : {stop, record, play, remove, manage}) row->addWidget(b);
    row->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(names, 2);
    layout->addWidget(steps, 3);
    layout->addWidget(state);
    layout->addLayout(row);
    refresh();
}

std::optional<QString> ActionsPanel::current() const {
    QListWidgetItem* item = names->currentItem();
    return item != nullptr ? std::optional<QString>(item->text()) : std::nullopt;
}

void ActionsPanel::refresh() {
    const std::optional<QString> chosen = current();
    {
        const QSignalBlocker quiet(names);
        names->clear();
        for (const auto& [name, value] : actions::load().items()) names->addItem(QString::fromStdString(name));
        QListWidgetItem* again = nullptr;
        if (chosen) {
            const auto found = names->findItems(*chosen, Qt::MatchExactly);
            if (!found.isEmpty()) again = found.front();
        }
        if (again == nullptr && names->count() > 0) again = names->item(0);
        names->setCurrentItem(again);
    }
    fill_steps();
    const bool recording = window_->recording().has_value();
    if (!recording) recording_name_.reset();
    record->setEnabled(!recording);
    stop->setEnabled(recording);
    play->setEnabled(!recording && current().has_value());
    remove->setEnabled(!recording && current().has_value());
    if (recording) {
        state->setText(QStringLiteral("● 記録中（%1 手）%2").arg(window_->recording()->size()).arg(recording_name_ ? QStringLiteral("：「%1」").arg(*recording_name_) : QString()));
    } else if (names->count() == 0) {
        state->setText(QStringLiteral("記録したアクションはまだありません。● 記録 で始めます。"));
    } else {
        state->setText(QStringLiteral("灰色の手順は、記録したページの決まった物を変えるもので、ほかのページでは飛ばします。"));
    }
}

void ActionsPanel::fill_steps() {
    steps->clear();
    const auto name = current();
    if (!name) return;
    const Json all = actions::load();
    const auto found = all.find(name->toStdString());
    if (found == all.end()) return;
    const Json& ops = (*found)["ops"];
    const std::vector<bool> bound = actions::page_bound(ops);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        auto* item = new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(actions::step_label(ops[i])) +
                                         (bound[i] ? QStringLiteral("（このページだけ）") : QString()));
        if (bound[i]) item->setForeground(Qt::gray);
        steps->addItem(item);
    }
}

void ActionsPanel::start(std::optional<QString> name) {
    if (window_->recording()) return;
    if (!name) {
        name = ask::get_text(this, QStringLiteral("新規アクション"), QStringLiteral("名前"));
        if (!name || name->trimmed().isEmpty()) return;
    }
    recording_name_ = name->trimmed();
    window_->start_recording();
    refresh();
}

void ActionsPanel::finish() {
    if (!window_->recording()) return;
    const std::optional<QString> name = recording_name_;
    recording_name_.reset();
    window_->stop_recording(name);
    refresh();
    if (name) {
        const auto found = names->findItems(*name, Qt::MatchExactly);
        if (!found.isEmpty()) names->setCurrentItem(found.front());
    }
}

}  // namespace genko::app
