// オートアクション (Python's genko/app/actions.py).

#include "app/actions.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <utility>

#include "app/ask.hpp"
#include "app/history_panel.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "service/actions.hpp"

namespace genko::app::actions {

using core::Json;

// (PC5 I-0: kept and played back by service/actions.cpp — the same file, form and bytes)
std::filesystem::path path() { return service::actions::path(); }

Json load() { return service::actions::load(); }

std::optional<QString> save(const Json& all) {
    if (const auto failed = service::actions::save(all)) return QString::fromStdString(*failed);
    return std::nullopt;
}

std::optional<QString> store(const QString& name, const Json& ops) {
    if (const auto failed = service::actions::store(name.toStdString(), ops)) return QString::fromStdString(*failed);
    return std::nullopt;
}

std::optional<QString> remove(const QString& name) {
    if (const auto failed = service::actions::remove(name.toStdString())) return QString::fromStdString(*failed);
    return std::nullopt;
}

Json recordable(const Json& ops) { return service::actions::recordable(ops); }

std::vector<bool> page_bound(const Json& ops) { return service::actions::page_bound(ops); }

Json playable(const Json& ops) { return service::actions::playable(ops); }

Json replay(const Json& recorded, std::optional<Json> page, const std::optional<std::string>& layer_id, const std::optional<std::string>& frame_id,
            const std::function<std::string()>& new_id) {
    return service::actions::replay(recorded, std::move(page), layer_id, frame_id, new_id);
}

QString describe(const Json& ops) { return history::describe(ops); }

QString step_label(const Json& op) { return history::describe(Json::array({op})); }

// --- the dialog --------------------------------------------------------------------------------------------------------

ActionsDialog::ActionsDialog(MainWindow* window) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("オートアクションの管理"));
    resize(560, 420);
    names = new QListWidget;
    steps = new QListWidget;
    auto* note = new QLabel(QStringLiteral("灰色の手順は、記録したページの決まった物（台詞・線など）を変えるもので、ほかのページでは飛ばします。"));
    note->setWordWrap(true);
    theme::hint(note);
    data_ = load();
    connect(names, &QListWidget::currentRowChanged, this, [this](int) { fill_steps(); });
    auto* buttons = new QHBoxLayout;
    const std::vector<std::pair<QString, std::function<void()>>> list{{QStringLiteral("↑"), [this] { move_step(-1); }},
                                                                      {QStringLiteral("↓"), [this] { move_step(1); }},
                                                                      {QStringLiteral("手順を外す"), [this] { drop_step(); }},
                                                                      {QStringLiteral("名前を変える"), [this] { rename(); }},
                                                                      {QStringLiteral("消す"), [this] { remove_current(); }},
                                                                      {QStringLiteral("このページで実行"), [this] { play(); }}};
    for (const auto& [label, slot] : list) {
        auto* button = new QPushButton(label);
        connect(button, &QPushButton::clicked, this, slot);
        buttons->addWidget(button);
    }
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* row = new QHBoxLayout;
    row->addWidget(names, 1);
    row->addWidget(steps, 2);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(row, 1);
    layout->addWidget(note);
    layout->addLayout(buttons);
    layout->addWidget(close);
    fill_names();
}

std::optional<QString> ActionsDialog::current() const {
    QListWidgetItem* item = names->currentItem();
    return item != nullptr ? std::optional<QString>(item->text()) : std::nullopt;
}

void ActionsDialog::fill_names(const QString& select) {
    QSignalBlocker quiet(names);
    names->clear();
    for (const auto& [name, value] : data_.items()) names->addItem(QString::fromStdString(name));
    if (names->count() > 0) {
        const auto found = select.isEmpty() ? QList<QListWidgetItem*>() : names->findItems(select, Qt::MatchExactly);
        names->setCurrentItem(found.isEmpty() ? names->item(0) : found.front());
    }
    fill_steps();
}

void ActionsDialog::fill_steps() {
    steps->clear();
    const auto name = current();
    if (!name) return;
    const Json& ops = data_[name->toStdString()]["ops"];
    const std::vector<bool> bound = page_bound(ops);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        auto* item = new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(step_label(ops[i])) +
                                         (bound[i] ? QStringLiteral("（このページだけ）") : QString()));
        if (bound[i]) item->setForeground(Qt::gray);
        steps->addItem(item);
    }
}

void ActionsDialog::keep() {
    if (const auto failed = save(data_)) ask::warning(this, QStringLiteral("オートアクション"), *failed);
    fill_steps();
}

void ActionsDialog::move_step(int delta) {
    const auto name = current();
    const int row = steps->currentRow();
    if (!name) return;
    Json& ops = data_[name->toStdString()]["ops"];
    const int count = static_cast<int>(ops.size());
    if (!(0 <= row && row < count) || !(0 <= row + delta && row + delta < count)) return;
    std::swap(ops[static_cast<std::size_t>(row)], ops[static_cast<std::size_t>(row + delta)]);
    keep();
    steps->setCurrentRow(row + delta);
}

void ActionsDialog::drop_step() {
    const auto name = current();
    const int row = steps->currentRow();
    if (!name) return;
    Json& ops = data_[name->toStdString()]["ops"];
    if (0 <= row && row < static_cast<int>(ops.size())) {
        ops.erase(ops.begin() + row);
        keep();
    }
}

void ActionsDialog::rename() {
    const auto name = current();
    if (!name) return;
    const auto given = ask::get_text(this, QStringLiteral("名前を変える"), QStringLiteral("新しい名前"), *name);
    if (!given || given->trimmed().isEmpty() || given->trimmed() == *name) return;
    const QString renamed = given->trimmed();
    Json again = Json::object();
    for (const auto& [key, value] : data_.items()) again[key == name->toStdString() ? renamed.toStdString() : key] = value;
    data_ = again;
    if (const auto failed = save(data_)) ask::warning(this, QStringLiteral("オートアクション"), *failed);
    fill_names(renamed);
}

void ActionsDialog::remove_current() {
    const auto name = current();
    if (name && ask::question(this, QStringLiteral("オートアクション"), QStringLiteral("「%1」を消しますか？").arg(*name))) {
        data_.erase(name->toStdString());
        if (const auto failed = save(data_)) ask::warning(this, QStringLiteral("オートアクション"), *failed);
        fill_names();
    }
}

void ActionsDialog::play() {
    if (const auto name = current()) window_->play_action(*name);
}

}  // namespace genko::app::actions
