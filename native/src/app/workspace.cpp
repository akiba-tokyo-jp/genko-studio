#include "app/workspace.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"

namespace genko::app {

const QStringList kDefaultCommandBar{QStringLiteral("元に戻す"), QStringLiteral("やり直す"), QStringLiteral("|"),
                                     QStringLiteral("全体を表示"), QStringLiteral("縮小"), QStringLiteral("拡大"), QStringLiteral("|"),
                                     QStringLiteral("◀ 前のページ"), QStringLiteral("次のページ ▶"), QStringLiteral("|"),
                                     QStringLiteral("原稿を書き出す…")};  // (PC4 F-0: 書き出し…'s new name)
const QStringList kDefaultQuick{QStringLiteral("ペン"), QStringLiteral("消しゴム"), QStringLiteral("塗りつぶし"), QStringLiteral("テキスト"),
                                QStringLiteral("コマ割り"), QStringLiteral("選択範囲・選んだコマにトーンを貼る"), QStringLiteral("集中線"),
                                QStringLiteral("左右反転して見る")};

namespace {

void walk(QMenu* menu, const std::function<void(QAction*)>& each) {
    for (QAction* action : menu->actions()) {
        if (action->isSeparator()) continue;
        if (action->menu() != nullptr) {
            walk(action->menu(), each);
        } else if (!remade(action)) {
            each(action);
        }
    }
}

bool find_in(QMenu* menu, const QAction* action, QStringList& trail) {
    for (QAction* item : menu->actions()) {
        if (item == action) return true;
        if (item->menu() != nullptr) {
            trail << item->text();
            if (find_in(item->menu(), action, trail)) return true;
            trail.removeLast();
        }
    }
    return false;
}

// Every item of a menu with its menu path, as find_in finds it (the menus walked in the same order, an item before its
// own menu: its first place), the ones already known kept.
void places_in(QMenu* menu, QStringList& trail, std::unordered_map<const QAction*, QString>& out) {
    for (QAction* item : menu->actions()) {
        if (!out.contains(item)) out.emplace(item, trail.join(QStringLiteral(" → ")));
        if (item->menu() != nullptr) {
            trail << item->text();
            places_in(item->menu(), trail, out);
            trail.removeLast();
        }
    }
}

QString place_of(const std::unordered_map<const QAction*, QString>& known, const QAction* action) {
    const auto found = known.find(action);
    return found != known.end() ? found->second : QString();
}

const QString kLine = QStringLiteral("――（区切り）");

}  // namespace

bool remade(const QAction* action) {
    for (const QObject* up = action->parent(); up != nullptr; up = up->parent()) {
        if (qobject_cast<const QMenu*>(up) == nullptr) return false;
        if (up->property("remade").toBool()) return true;
    }
    return false;
}

namespace {

// Every command in the menus once (one in two menus — the layer panel's in レイヤー too — once), in menu order.
std::vector<QAction*> menu_commands(QMainWindow* window) {
    std::vector<QAction*> out;
    QSet<const QAction*> seen;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr) continue;
        walk(top->menu(), [&](QAction* action) {
            const QString text = action->text();
            if (text.isEmpty() || seen.contains(action) || text.startsWith(QStringLiteral("（"))) return;
            seen.insert(action);
            out.push_back(action);
        });
    }
    return out;
}

}  // namespace

namespace {

// (known: the menu paths, when the caller has them already — else walked here once, when a name needs one)
std::map<const QAction*, QString> command_names(const std::vector<QAction*>& all, QMainWindow* window,
                                                const std::unordered_map<const QAction*, QString>* known = nullptr) {
    std::map<QString, int> count;
    for (const QAction* action : all) ++count[action->text()];
    std::map<const QAction*, QString> out;
    std::optional<std::unordered_map<const QAction*, QString>> walked;
    for (const QAction* action : all) {
        QString place;
        if (count[action->text()] > 1) {
            if (known == nullptr) known = &walked.emplace(menu_places(window));
            place = place_of(*known, action);
        }
        out[action] = place.isEmpty() ? action->text() : QStringLiteral("%1（%2）").arg(action->text(), place);
    }
    return out;
}

std::vector<QAction*> commands(const std::vector<QAction*>& all, const std::map<const QAction*, QString>& names) {
    std::vector<QAction*> out;
    QSet<QString> seen;
    for (QAction* action : all) {
        const QString name = command_name(names, action);
        if (seen.contains(name)) continue;
        seen.insert(name);
        out.push_back(action);
    }
    return out;
}

}  // namespace

std::map<const QAction*, QString> command_names(QMainWindow* window) { return command_names(menu_commands(window), window); }

QString command_name(const std::map<const QAction*, QString>& names, const QAction* action) {
    const auto found = names.find(action);
    return found != names.end() ? found->second : action->text();
}

std::vector<QAction*> commands(QMainWindow* window) {
    const std::vector<QAction*> all = menu_commands(window);
    return commands(all, command_names(all, window));
}

std::map<QString, QAction*> by_text(QMainWindow* window) {
    const std::vector<QAction*> all = menu_commands(window);
    const auto names = command_names(all, window);
    const std::vector<QAction*> listed = commands(all, names);
    std::map<QString, QAction*> out;
    for (QAction* action : listed) out[command_name(names, action)] = action;
    // (a name kept before others had the same words — the words alone: the first of them in the menus, the one that had
    // them then, as the layer panel's 色調補正のレイヤー's before レイヤー → 色調補正)
    for (QAction* action : listed) out.emplace(action->text(), action);
    return out;
}

QString where(QMainWindow* window, const QAction* action) {
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr) continue;
        QStringList trail{top->text()};
        if (find_in(top->menu(), action, trail)) return trail.join(QStringLiteral(" → "));
    }
    return {};
}

std::unordered_map<const QAction*, QString> menu_places(QMainWindow* window) {
    std::unordered_map<const QAction*, QString> out;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr) continue;
        QStringList trail{top->text()};
        places_in(top->menu(), trail, out);
    }
    return out;
}

std::vector<QAction*> search(QMainWindow* window, const QString& words) {
    static const QRegularExpression space(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    const QStringList parts = words.toLower().split(space, Qt::SkipEmptyParts);
    std::vector<QAction*> out;
    const std::vector<QAction*> all = menu_commands(window);
    const auto known = menu_places(window);  // (the menus walked once: for the names and the places both)
    for (QAction* action : commands(all, command_names(all, window, &known))) {
        const QString text = (action->text() + QLatin1Char(' ') + place_of(known, action) + QLatin1Char(' ') + action->statusTip()).toLower();
        if (std::all_of(parts.begin(), parts.end(), [&text](const QString& part) { return text.contains(part); })) out.push_back(action);
    }
    return out;
}

CommandSearch::CommandSearch(QMainWindow* window) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("コマンドを探す"));
    setProperty("glass_wanted", true);  // (the system's frosted glass where there is one)
    resize(520, 420);
    query = new QLineEdit;
    query->setObjectName(QStringLiteral("query"));
    query->setPlaceholderText(QStringLiteral("例: トーン、見開き、太く、反転…"));
    results = new QListWidget;
    results->setObjectName(QStringLiteral("results"));
    connect(query, &QLineEdit::textChanged, this, &CommandSearch::refresh);
    connect(query, &QLineEdit::returnPressed, this, &CommandSearch::run);
    connect(results, &QListWidget::itemActivated, this, [this] { run(); });
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(query);
    layout->addWidget(results, 1);
    refresh();
}

void CommandSearch::refresh() {
    results->clear();
    found_.clear();
    alive_.clear();
    for (QAction* action : search(window_, query->text())) {
        if (!action->isEnabled()) continue;
        found_.push_back(action);
        alive_.emplace_back(action);
        if (found_.size() == 60) break;
    }
    for (QAction* action : found_) {
        const QString keys = action->shortcut().toString(QKeySequence::NativeText);
        auto* item = new QListWidgetItem(QStringLiteral("%1　（%2）").arg(action->text(), where(window_, action)) +
                                         (keys.isEmpty() ? QString() : QStringLiteral("　") + keys));
        item->setToolTip(action->statusTip());
        results->addItem(item);
    }
    if (!found_.empty()) results->setCurrentRow(0);
}

void CommandSearch::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) && results->count() > 0) {
        const int row = results->currentRow() + (event->key() == Qt::Key_Down ? 1 : -1);
        results->setCurrentRow(std::max(0, std::min(results->count() - 1, row)));
        return;
    }
    QDialog::keyPressEvent(event);
}

void CommandSearch::run() {
    const int row = results->currentRow();
    if (row < 0 || row >= static_cast<int>(alive_.size())) return;
    const QPointer<QAction> action = alive_[static_cast<std::size_t>(row)];
    if (action.isNull()) {
        refresh();  // (its menu made its items again: the list found again, nothing run)
        return;
    }
    accept();
    action->trigger();
}

QStringList chosen(const QString& key, const QStringList& defaults) {
    const QVariant value = settings()->value(key);
    if (!value.isValid() || value.toString().isEmpty()) return defaults;
    std::map<QString, QString> old_to_new;
    for (const auto& [now, before] : preferences::renamed()) old_to_new[before] = now;  // (a command renamed later keeps its place)
    QStringList out;
    for (const QString& name : value.toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts)) {
        const auto found = old_to_new.find(name);
        out << (found != old_to_new.end() ? found->second : name);
    }
    return out;
}

void keep(const QString& key, const QStringList& names) { settings()->setValue(key, names.join(QLatin1Char('\t'))); }

ChooseCommands::ChooseCommands(QMainWindow* window, const QString& title, const QStringList& current, bool separators)
    : QDialog(window), window_(window) {
    setWindowTitle(title);
    resize(640, 460);
    all = new QListWidget;
    all->setObjectName(QStringLiteral("all"));
    mine = new QListWidget;
    mine->setObjectName(QStringLiteral("mine"));
    filter = new QLineEdit;
    filter->setPlaceholderText(QStringLiteral("探す"));
    connect(filter, &QLineEdit::textChanged, this, &ChooseCommands::fill_all);
    for (const QString& name : current) mine->addItem(name == QLatin1String("|") ? kLine : name);
    add = new QPushButton(QStringLiteral("→ 足す"));
    connect(add, &QPushButton::clicked, this, &ChooseCommands::add_chosen);
    remove = new QPushButton(QStringLiteral("← 外す"));
    connect(remove, &QPushButton::clicked, this, [this] { delete mine->takeItem(mine->currentRow()); });
    up = new QPushButton(QStringLiteral("↑"));
    connect(up, &QPushButton::clicked, this, [this] { move_chosen(-1); });
    down = new QPushButton(QStringLiteral("↓"));
    connect(down, &QPushButton::clicked, this, [this] { move_chosen(1); });
    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(QStringLiteral("コマンド")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("並べるもの")), 0, 2);
    grid->addWidget(filter, 1, 0);
    grid->addWidget(all, 2, 0, 5, 1);
    grid->addWidget(add, 2, 1);
    grid->addWidget(remove, 3, 1);
    grid->addWidget(up, 4, 1);
    grid->addWidget(down, 5, 1);
    if (separators) {
        line = new QPushButton(QStringLiteral("区切りを足す"));
        connect(line, &QPushButton::clicked, this, [this] { mine->addItem(kLine); });
        grid->addWidget(line, 6, 1);
    }
    grid->addWidget(mine, 1, 2, 6, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    restore = buttons->button(QDialogButtonBox::RestoreDefaults);
    restore->setText(QStringLiteral("はじめの並びに戻す"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(grid, 1);
    layout->addWidget(buttons);
    fill_all();
}

void ChooseCommands::fill_all() {
    all->clear();
    const auto names = command_names(window_);
    for (QAction* action : search(window_, filter->text())) all->addItem(command_name(names, action));
}

void ChooseCommands::add_chosen() {
    QListWidgetItem* item = all->currentItem();
    if (item != nullptr && mine->findItems(item->text(), Qt::MatchExactly).isEmpty()) mine->addItem(item->text());
}

void ChooseCommands::move_chosen(int delta) {
    const int row = mine->currentRow();
    if (row < 0 || row + delta < 0 || row + delta >= mine->count()) return;
    QListWidgetItem* item = mine->takeItem(row);
    mine->insertItem(row + delta, item);
    mine->setCurrentRow(row + delta);
}

QStringList ChooseCommands::names() const {
    QStringList out;
    for (int i = 0; i < mine->count(); ++i) {
        const QString text = mine->item(i)->text();
        out << (text.startsWith(QStringLiteral("――")) ? QStringLiteral("|") : text);
    }
    return out;
}

QuickAccess::QuickAccess(QMainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("quickAccess"));
    grid_ = new QGridLayout;
    grid_->setSpacing(3);
    auto* edit_button = new QPushButton(QStringLiteral("並べるものを選ぶ…"));
    edit_button->setObjectName(QStringLiteral("quickEdit"));
    edit_button->setProperty("row", true);
    theme::role_prop(edit_button, "quiet", true);
    connect(edit_button, &QPushButton::clicked, this, [this] { edit(); });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(grid_);
    layout->addStretch(1);
    layout->addWidget(edit_button);
}

QStringList QuickAccess::names() const { return chosen(QStringLiteral("ui/quick"), kDefaultQuick); }

void QuickAccess::refresh() {
    for (QPushButton* button : buttons) {
        grid_->removeWidget(button);
        button->deleteLater();
    }
    buttons.clear();
    const std::map<QString, QAction*> actions = by_text(window_);
    const auto command_names_now = command_names(window_);
    for (const QString& name : names()) {
        const auto found = actions.find(name);
        if (found == actions.end()) continue;
        QAction* action = found->second;
        // (one of the same words as another: its place after them, the user's decision 8)
        const QString called = command_name(command_names_now, action);
        auto* button = new QPushButton(called == action->text() ? short_words(action->text()) : called);
        button->setProperty("row", true);  // (quiet rows like the tool settings)
        button->setToolTip(action->statusTip().isEmpty() ? called : action->statusTip());
        button->setAccessibleName(called);
        button->setIcon(icon_or_blank(action->icon()));
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
        button->setMinimumWidth(60);
        buttons.push_back(button);
    }
    // two to a row; a long name takes a whole row (so it is not cut)
    int row = 0;
    int col = 0;
    for (QPushButton* button : buttons) {
        const bool wide = button->text().size() > 7;
        if (wide && col != 0) {
            ++row;
            col = 0;
        }
        grid_->addWidget(button, row, col, 1, wide ? 2 : 1);
        col = wide ? 0 : col + 1;
        if (col == 2 || wide) {
            ++row;
            col = 0;
        }
    }
}

bool QuickAccess::edit() {
    ChooseCommands dialog(window_, QStringLiteral("クイックアクセスに並べるもの"), names());
    connect(dialog.restore, &QPushButton::clicked, &dialog, [&dialog] {
        keep(QStringLiteral("ui/quick"), kDefaultQuick);
        dialog.reject();
    });
    if (ask::exec(&dialog) == QDialog::Accepted) keep(QStringLiteral("ui/quick"), dialog.names());
    refresh();
    return true;
}

void fill_commandbar(QMainWindow* window, QToolBar* bar) {
    for (QAction* action : bar->actions()) {
        if (action->property("keep").toBool()) continue;
        bar->removeAction(action);
        if (action->isSeparator() && action->parent() == bar) action->deleteLater();
    }
    const std::map<QString, QAction*> actions = by_text(window);
    for (const QString& name : chosen(QStringLiteral("ui/commandbar"), kDefaultCommandBar)) {
        if (name == QLatin1String("|")) {
            bar->addSeparator();
        } else if (const auto found = actions.find(name); found != actions.end()) {
            bar->addAction(found->second);
        }
    }
}

void edit_commandbar(QMainWindow* window, QToolBar* bar) {
    ChooseCommands dialog(window, QStringLiteral("コマンドバーに並べるもの"), chosen(QStringLiteral("ui/commandbar"), kDefaultCommandBar), true);
    QObject::connect(dialog.restore, &QPushButton::clicked, &dialog, [&dialog] {
        keep(QStringLiteral("ui/commandbar"), kDefaultCommandBar);
        dialog.reject();
    });
    if (ask::exec(&dialog) == QDialog::Accepted) keep(QStringLiteral("ui/commandbar"), dialog.names());
    fill_commandbar(window, bar);
}

QStringList workspaces() {
    auto store = settings();
    store->beginGroup(QStringLiteral("workspaces"));
    QStringList names = store->childGroups();
    store->endGroup();
    names.sort();
    return names;
}

void save_workspace(QMainWindow* window, const QString& name) {
    auto store = settings();
    store->setValue(QStringLiteral("workspaces/%1/state").arg(name), QString::fromLatin1(window->saveState().toBase64()));
    store->setValue(QStringLiteral("workspaces/%1/geometry").arg(name), QString::fromLatin1(window->saveGeometry().toBase64()));
}

bool load_workspace(QMainWindow* window, const QString& name) {
    auto store = settings();
    const QString state = store->value(QStringLiteral("workspaces/%1/state").arg(name)).toString();
    if (state.isEmpty()) return false;
    window->restoreGeometry(QByteArray::fromBase64(store->value(QStringLiteral("workspaces/%1/geometry").arg(name)).toString().toLatin1()));
    const bool ok = window->restoreState(QByteArray::fromBase64(state.toLatin1()));
    store->setValue(QStringLiteral("ui/workspace"), name);
    return ok;
}

void delete_workspace(const QString& name) { settings()->remove(QStringLiteral("workspaces/%1").arg(name)); }

// --- the cursor and the keys held ----------------------------------------------------------------------------------

namespace {

bool known(const Choices& choices, const QString& value) {
    return std::any_of(choices.begin(), choices.end(), [&value](const auto& choice) { return choice.second == value; });
}

}  // namespace

const Choices& cursors() {
    static const Choices all{{QStringLiteral("ブラシの大きさの円"), QStringLiteral("circle")}, {QStringLiteral("円と十字"), QStringLiteral("circle_cross")},
                             {QStringLiteral("十字"), QStringLiteral("cross")}, {QStringLiteral("点"), QStringLiteral("dot")}};
    return all;
}

QString cursor_kind() {
    const QString value = settings()->value(QStringLiteral("ui/cursor"), QStringLiteral("circle")).toString();
    return known(cursors(), value) ? value : QStringLiteral("circle");
}

const Choices& modifier_tool_choices() {
    static const Choices all{{QStringLiteral("何もしない"), QString()}, {QStringLiteral("スポイト"), QStringLiteral("picker")},
                             {QStringLiteral("選択"), QStringLiteral("select")}, {QStringLiteral("移動ツール"), QStringLiteral("move")},
                             {QStringLiteral("消しゴム"), QStringLiteral("eraser")}};
    return all;
}

QString modifier_tool(const QString& key) {
    const QString fallback = key == QLatin1String("alt") ? QStringLiteral("picker") : key == QLatin1String("ctrl") ? QStringLiteral("select") : QString();
    const QVariant kept = settings()->value(QStringLiteral("keys/%1_tool").arg(key));
    const QString value = kept.isValid() ? kept.toString() : fallback;
    return known(modifier_tool_choices(), value) ? value : fallback;
}

const Choices& keyed_tools() {
    static const Choices all{{QStringLiteral("ペン"), QStringLiteral("pen")},          {QStringLiteral("消しゴム"), QStringLiteral("eraser")},
                             {QStringLiteral("塗りつぶし"), QStringLiteral("fill")},    {QStringLiteral("選択範囲"), QStringLiteral("marquee")},
                             {QStringLiteral("選択"), QStringLiteral("select")},        {QStringLiteral("テキスト"), QStringLiteral("text")},
                             {QStringLiteral("ベクター線の直し"), QStringLiteral("vector")}, {QStringLiteral("色混ぜ"), QStringLiteral("blend")},
                             {QStringLiteral("グラデーション"), QStringLiteral("gradient")}, {QStringLiteral("コマ"), QStringLiteral("frame")}};
    return all;
}

const Choices& held_tools() {
    static const Choices all{{QStringLiteral("共通の設定のまま"), QString()}, {QStringLiteral("何もしない"), QStringLiteral("none")},
                             {QStringLiteral("スポイト"), QStringLiteral("picker")},   {QStringLiteral("選択"), QStringLiteral("select")},
                             {QStringLiteral("移動ツール"), QStringLiteral("move")},  {QStringLiteral("消しゴム"), QStringLiteral("eraser")},
                             {QStringLiteral("ペン"), QStringLiteral("pen")},          {QStringLiteral("塗りつぶし"), QStringLiteral("fill")}};
    return all;
}

std::map<QString, std::map<QString, QString>> tool_modifiers() {
    std::map<QString, std::map<QString, QString>> out;
    for (const auto& [label, tool] : keyed_tools()) {
        for (const char* key : {"alt", "ctrl", "shift"}) {
            const QString value = settings()->value(QStringLiteral("keys/%1/%2").arg(tool, QLatin1String(key)), QString()).toString();
            if (!value.isEmpty() && known(held_tools(), value)) out[tool][QString::fromLatin1(key)] = value;
        }
    }
    return out;
}

void set_tool_modifier(const QString& tool, const QString& key, const QString& value) {
    settings()->setValue(QStringLiteral("keys/%1/%2").arg(tool, key), value);
}

bool hold_swap() {
    const QString value = settings()->value(QStringLiteral("keys/hold_swap"), QStringLiteral("true")).toString().toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") || value == QLatin1String("yes");
}

}  // namespace genko::app
