// 編集 → キーボードショートカット… and メニュー… (PC5 H-d; PhotoCraft v0.2.0's prefs.rs keyboard_shortcuts and menus, MIT).

#include "app/shortcuts_dialog.hpp"

#include <QAction>
#include <QComboBox>
#include <QScopedValueRollback>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <set>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/key_sets.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/menu_look.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/workspace.hpp"

namespace genko::app {

namespace {

constexpr int kName = Qt::UserRole;  // (a row's command name)

const QString kOther = QStringLiteral("そのほか（パネル・メニューに無いコマンド）");

QString key_words(const QKeySequence& key) { return key.toString(QKeySequence::NativeText); }

QIcon swatch(const QColor& colour) {
    QPixmap picture(12, 12);
    picture.fill(Qt::transparent);
    if (colour.isValid()) {
        QPainter painter(&picture);
        painter.fillRect(QRect(1, 1, 10, 10), colour);
    }
    return QIcon(picture);
}

QString colour_label(const QString& id) {
    for (const menu_look::Colour& one : menu_look::colours()) {
        if (one.id == id) return one.label;
    }
    return menu_look::colours().front().label;
}

}  // namespace

ShortcutsDialog::ShortcutsDialog(MainWindow* window, int tab) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("キーボードショートカットとメニュー"));
    setObjectName(QStringLiteral("shortcutsDialog"));
    setMinimumSize(620, 520);
    tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("shortcuts_tabs"));
    fill_keys();
    fill_menus();
    tabs->setCurrentIndex(std::clamp(tab, 0, tabs->count() - 1));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ShortcutsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* body = new QWidget;
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(tabs, 1);
    look::frame(this, look::header(QStringLiteral("キーボードショートカットとメニュー"), QStringLiteral("このパソコンでのキーとメニューです。原稿には残りません。")),
                body, nullptr, look::footer(buttons));
    resize(820, 660);
}

// --- キーボードショートカット ------------------------------------------------------------------------------------------------

void ShortcutsDialog::fill_keys() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    // the set: Photoshop's, Genko's, the person's own
    key_set = new QComboBox;
    key_set->setObjectName(QStringLiteral("shortcuts_key_set"));
    for (const key_sets::Set& one : key_sets::sets()) key_set->addItem(one.label, one.id);
    const QStringList own = key_sets::own_sets();
    if (!own.isEmpty()) key_set->insertSeparator(key_set->count());
    for (const QString& name : own) key_set->addItem(name, QStringLiteral("own:") + name);
    key_set->setCurrentIndex(std::max(0, key_set->findData(key_sets::chosen())));
    key_set->setToolTip(QStringLiteral("「Photoshop の初期設定」は Photoshop と同じキー、「Genko」は PC5 より前の Genko のキー、その下は自分で保存した組です"));
    save_set_button = new QPushButton(QStringLiteral("組を保存…"));
    save_set_button->setToolTip(QStringLiteral("今のキーを、名前を付けた組として残します"));
    connect(save_set_button, &QPushButton::clicked, this, [this] { save_set(); });
    delete_set_button = new QPushButton(QStringLiteral("組を削除"));
    delete_set_button->setToolTip(QStringLiteral("自分で保存した組を消します（Photoshop・Genko の組は消せません）"));
    connect(delete_set_button, &QPushButton::clicked, this, [this] { delete_set(); });
    auto* set_row = new QHBoxLayout;
    set_row->addWidget(new QLabel(QStringLiteral("キーの組")));
    set_row->addWidget(key_set, 1);
    set_row->addWidget(save_set_button);
    set_row->addWidget(delete_set_button);
    column->addLayout(set_row);
    filter = new QLineEdit;
    filter->setObjectName(QStringLiteral("shortcuts_filter"));
    filter->setPlaceholderText(QStringLiteral("コマンドの名前かキーで絞り込む"));
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, this, &ShortcutsDialog::apply_filter);
    column->addWidget(filter);
    // every command by its menu (in the menus' order), the ones in no menu after them
    tree = new QTreeWidget;
    tree->setObjectName(QStringLiteral("shortcuts_tree"));
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("コマンド"), QStringLiteral("ショートカット")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setStretchLastSection(false);
    tree->header()->resizeSection(1, 220);
    tree->setUniformRowHeights(true);
    const auto names = command_names(window_);
    const auto places = menu_places(window_);
    std::map<QString, QTreeWidgetItem*> tops;
    for (QAction* top : window_->menuBar()->actions()) {
        if (top->menu() == nullptr || tops.contains(top->text())) continue;
        auto* item = new QTreeWidgetItem(tree, {QString(top->text()).remove(QLatin1Char('&'))});
        item->setFlags(Qt::ItemIsEnabled);
        tops[top->text()] = item;
    }
    std::vector<QAction*> order = commands(window_);  // (the menus' order)
    const std::vector<QAction*> all = preferences::commands(window_);
    {
        std::set<QString> listed;
        for (QAction* action : order) listed.insert(command_name(names, action));
        for (QAction* action : all) {
            if (!listed.contains(command_name(names, action))) order.push_back(action);
        }
        std::set<QString> wanted;
        for (QAction* action : all) wanted.insert(command_name(names, action));
        std::set<QString> seen;
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&](QAction* action) {
                                       const QString name = command_name(names, action);
                                       return !wanted.contains(name) || !seen.insert(name).second;
                                   }),
                    order.end());
    }
    QTreeWidgetItem* other = nullptr;
    for (QAction* action : order) {
        const QString name = command_name(names, action);
        const auto place = places.find(action);
        const QString path = place != places.end() ? place->second : QString();
        QTreeWidgetItem* parent = nullptr;
        if (!path.isEmpty()) {
            const auto top = tops.find(path.section(QStringLiteral(" → "), 0, 0));
            if (top != tops.end()) parent = top->second;
        }
        if (parent == nullptr) {
            if (other == nullptr) {
                other = new QTreeWidgetItem(tree, {kOther});
                other->setFlags(Qt::ItemIsEnabled);
            }
            parent = other;
        }
        auto* item = new QTreeWidgetItem(parent, {QString(name).remove(QLatin1Char('&'))});
        item->setData(0, kName, name);
        item->setToolTip(0, path.isEmpty() ? name : path + QStringLiteral(" → ") + QString(action->text()).remove(QLatin1Char('&')));
        auto* edit = new QKeySequenceEdit(action->shortcut());
        edit->setMaximumSequenceLength(1);
        if (auto* field = edit->findChild<QLineEdit*>()) field->setPlaceholderText(QStringLiteral("キーを押す"));
        edit->setAccessibleName(QStringLiteral("%1 のショートカット").arg(name));
        tree->setItemWidget(item, 1, edit);
        editors.emplace_back(name, edit);
        shown_[name] = action->shortcut();
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, name, edit] { key_changed(name, edit); });
    }
    for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {  // (a menu with no command of its own: not shown)
        if (tree->topLevelItem(i)->childCount() == 0) delete tree->takeTopLevelItem(i);
    }
    column->addWidget(tree, 1);
    // a key another command has: taken from it, or given up
    clash = new QLabel;
    clash->setObjectName(QStringLiteral("shortcuts_clash"));
    theme::role(clash, "error");
    clash->setWordWrap(true);
    take_button = new QPushButton(QStringLiteral("前のコマンドから外して付ける"));
    connect(take_button, &QPushButton::clicked, this, &ShortcutsDialog::take_from_previous);
    keep_button = new QPushButton(QStringLiteral("やめる"));
    keep_button->setObjectName(QStringLiteral("shortcuts_keep"));
    connect(keep_button, &QPushButton::clicked, this, &ShortcutsDialog::keep_previous);
    take_button->hide();
    keep_button->hide();
    auto* clash_row = new QHBoxLayout;
    clash_row->addWidget(clash, 1);
    clash_row->addWidget(take_button);
    clash_row->addWidget(keep_button);
    column->addLayout(clash_row);
    reset_one_button = new QPushButton(QStringLiteral("初期設定に戻す"));
    reset_one_button->setToolTip(QStringLiteral("選んだコマンドのキーを、キーの組のキーに戻します"));
    connect(reset_one_button, &QPushButton::clicked, this, [this] { reset_one(); });
    reset_all_button = new QPushButton(QStringLiteral("すべて最初のキーに戻す"));
    connect(reset_all_button, &QPushButton::clicked, this, &ShortcutsDialog::reset_keys);
    summary_button = new QPushButton(QStringLiteral("概要…"));
    summary_button->setToolTip(QStringLiteral("すべてのコマンドとキーの一覧を、ブラウザで開けるページ（HTML）に書き出します"));
    connect(summary_button, &QPushButton::clicked, this, [this] { write_summary(); });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(reset_one_button);
    buttons->addWidget(reset_all_button);
    buttons->addStretch(1);
    buttons->addWidget(summary_button);
    column->addLayout(buttons);
    choose_key_set();
    connect(key_set, &QComboBox::currentIndexChanged, this, [this] { choose_key_set(); });
    tabs->addTab(page, QStringLiteral("キーボードショートカット"));
}

QKeySequenceEdit* ShortcutsDialog::editor(const QString& command) const {
    for (const auto& [name, edit] : editors) {
        if (name == command) return edit;
    }
    return nullptr;
}

QTreeWidgetItem* ShortcutsDialog::row(const QString& command) const {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        for (int j = 0; j < top->childCount(); ++j) {
            if (top->child(j)->data(0, kName).toString() == command) return top->child(j);
        }
    }
    return nullptr;
}

void ShortcutsDialog::choose_key_set() {
    // (each command's keys in the set now chosen; a command whose key is the last set's own takes the new set's — the
    // person's own stay)
    const QString set = key_set->currentData().toString();
    std::map<QString, QList<QKeySequence>> next = key_sets::keys_by_name(window_, set);
    if (!set_keys.empty()) {
        const QScopedValueRollback<bool> quiet(quiet_, true);
        for (const auto& [name, edit] : editors) {
            const auto was = set_keys.find(name);
            const QKeySequence old = was != set_keys.end() && !was->second.isEmpty() ? was->second.front() : QKeySequence();
            if (edit->keySequence() != old) continue;
            const auto now = next.find(name);
            edit->setKeySequence(now != next.end() && !now->second.isEmpty() ? now->second.front() : QKeySequence());
            shown_[name] = edit->keySequence();
        }
        pending_.reset();
        clash->clear();
        take_button->hide();
        keep_button->hide();
    }
    set_keys = std::move(next);
    delete_set_button->setEnabled(set.startsWith(QStringLiteral("own:")));
}

void ShortcutsDialog::key_changed(const QString& name, QKeySequenceEdit* edit) {
    if (quiet_) return;
    const QKeySequence key = edit->keySequence();
    const QKeySequence before = shown_[name];
    pending_.reset();
    take_button->hide();
    keep_button->hide();
    clash->clear();
    if (!key.isEmpty()) {
        for (const auto& [other, its] : editors) {
            if (other == name || its->keySequence() != key) continue;
            // (Photoshop's: the key is another's — taken from it, or this change given up)
            pending_ = Pending{name, other, before};
            clash->setText(QStringLiteral("「%1」は「%2」に付いています。").arg(key_words(key), other));
            take_button->show();
            keep_button->show();
            return;
        }
    }
    shown_[name] = key;
}

void ShortcutsDialog::take_from_previous() {
    if (!pending_) return;
    const Pending was = *pending_;
    QKeySequenceEdit* other = editor(was.other);
    QKeySequenceEdit* mine = editor(was.name);
    if (other == nullptr || mine == nullptr) return;
    {
        const QScopedValueRollback<bool> quiet(quiet_, true);
        other->setKeySequence(QKeySequence());
    }
    shown_[was.other] = QKeySequence();
    shown_[was.name] = mine->keySequence();
    pending_.reset();
    take_button->hide();
    keep_button->hide();
    clash->setText(QStringLiteral("「%1」を「%2」から外して、「%3」に付けました。").arg(key_words(mine->keySequence()), was.other, was.name));
}

void ShortcutsDialog::keep_previous() {
    if (!pending_) return;
    const Pending was = *pending_;
    if (QKeySequenceEdit* mine = editor(was.name)) {
        const QScopedValueRollback<bool> quiet(quiet_, true);
        mine->setKeySequence(was.before);
    }
    pending_.reset();
    take_button->hide();
    keep_button->hide();
    clash->clear();
}

void ShortcutsDialog::reset_one(const QString& command) {
    QString name = command;
    if (name.isEmpty() && tree->currentItem() != nullptr) name = tree->currentItem()->data(0, kName).toString();
    QKeySequenceEdit* edit = editor(name);
    if (edit == nullptr) return;
    const auto found = set_keys.find(name);
    edit->setKeySequence(found != set_keys.end() && !found->second.isEmpty() ? found->second.front() : QKeySequence());  // (a clash said)
}

void ShortcutsDialog::reset_keys() {
    const QScopedValueRollback<bool> quiet(quiet_, true);
    for (const auto& [name, edit] : editors) {
        const auto found = set_keys.find(name);
        edit->setKeySequence(found != set_keys.end() && !found->second.isEmpty() ? found->second.front() : QKeySequence());
        shown_[name] = edit->keySequence();
    }
    pending_.reset();
    take_button->hide();
    keep_button->hide();
    clash->clear();
}

void ShortcutsDialog::apply_filter() {
    const QString words = filter->text().trimmed().toLower();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        int shown = 0;
        for (int j = 0; j < top->childCount(); ++j) {
            QTreeWidgetItem* item = top->child(j);
            const QString name = item->data(0, kName).toString();
            QString keys;
            if (QKeySequenceEdit* edit = editor(name)) keys = edit->keySequence().toString(QKeySequence::PortableText) + QLatin1Char(' ') + key_words(edit->keySequence());
            const bool match = words.isEmpty() || name.toLower().contains(words) || keys.toLower().contains(words) ||
                               item->toolTip(0).toLower().contains(words);
            item->setHidden(!match);
            if (match) ++shown;
        }
        top->setHidden(shown == 0);
        top->setExpanded(!words.isEmpty() && shown > 0);
    }
}

bool ShortcutsDialog::save_set(std::optional<QString> name) {
    if (!name) {
        const auto typed = ask::get_text(this, QStringLiteral("組を保存"), QStringLiteral("組の名前"));
        if (!typed) return false;
        name = *typed;
    }
    const QString kept = name->trimmed();
    if (kept.isEmpty()) return false;
    if (key_sets::reserved(kept)) {
        clash->setText(QStringLiteral("「%1」は Genko にある組の名前です。別の名前にします。").arg(kept));
        return false;
    }
    if (kept.contains(QLatin1Char('/')) || kept.contains(QLatin1Char('\\'))) {
        clash->setText(QStringLiteral("組の名前に「/」「\\」は使えません。"));
        return false;
    }
    if (key_sets::own_sets().contains(kept) &&
        !ask::question(this, QStringLiteral("組を保存"), QStringLiteral("「%1」はもうあります。今のキーで置き換えますか？").arg(kept))) {
        return false;
    }
    // (the built-in set under it, and where the keys shown differ from it)
    const QString chosen = key_set->currentData().toString();
    const QString base = chosen.startsWith(QStringLiteral("own:")) ? key_sets::own_base(chosen.mid(4)) : chosen;
    const auto base_keys = key_sets::keys_by_name(window_, base);
    std::map<QString, QList<QKeySequence>> over;
    for (const auto& [command, edit] : editors) {
        const auto in_set = set_keys.find(command);
        const QList<QKeySequence> set_list = in_set != set_keys.end() ? in_set->second : QList<QKeySequence>();
        // (the key shown is the set's first: the set's whole list — Ctrl+K and Ctrl+, — else the one shown)
        const QList<QKeySequence> now = (set_list.isEmpty() ? edit->keySequence().isEmpty() : edit->keySequence() == set_list.front())
                                            ? set_list
                                            : (edit->keySequence().isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{edit->keySequence()});
        const auto in_base = base_keys.find(command);
        if (in_base == base_keys.end() || in_base->second != now) over[command] = now;
    }
    key_sets::save_own(kept, base, over);
    const QString id = QStringLiteral("own:") + kept;
    if (key_set->findData(id) < 0) {
        if (key_sets::own_sets().size() == 1) key_set->insertSeparator(key_set->count());
        key_set->addItem(kept, id);
    }
    {
        const QSignalBlocker quiet(key_set);
        key_set->setCurrentIndex(key_set->findData(id));
    }
    set_keys = key_sets::keys_by_name(window_, id);
    delete_set_button->setEnabled(true);
    clash->setText(QStringLiteral("キーの組「%1」を保存しました。").arg(kept));
    return true;
}

bool ShortcutsDialog::delete_set() {
    const QString chosen = key_set->currentData().toString();
    if (!chosen.startsWith(QStringLiteral("own:"))) {
        clash->setText(QStringLiteral("「%1」は Genko にある組なので削除できません。").arg(key_sets::label(chosen)));
        return false;
    }
    const QString name = chosen.mid(4);
    if (!ask::question(this, QStringLiteral("組を削除"), QStringLiteral("キーの組「%1」を削除しますか？").arg(name))) return false;
    const QString base = key_sets::own_base(name);
    key_sets::delete_own(name);
    const int at = key_set->findData(chosen);
    key_set->setCurrentIndex(std::max(0, key_set->findData(base)));  // (the keys shown: the built-in set's, as the set chosen)
    key_set->removeItem(at);
    if (key_sets::own_sets().isEmpty()) {
        for (int i = key_set->count() - 1; i >= 0; --i) {
            if (key_set->itemData(i).isNull() && key_set->itemText(i).isEmpty()) key_set->removeItem(i);  // (the line)
        }
    }
    clash->setText(QStringLiteral("キーの組「%1」を削除しました。").arg(name));
    return true;
}

QString ShortcutsDialog::summary_html() const {
    QString html = QStringLiteral("<!DOCTYPE html>\n<html lang=\"ja\"><head><meta charset=\"utf-8\"><title>キーボードショートカット — %1</title>\n"
                                  "<style>body{font-family:sans-serif;margin:2em}table{border-collapse:collapse;margin-bottom:1.5em}"
                                  "td,th{border:1px solid #bbb;padding:3px 10px;text-align:left}th{background:#eee}</style></head><body>\n"
                                  "<h1>キーボードショートカット</h1>\n<p>キーの組: %1</p>\n")
                       .arg(key_sets::label(key_set->currentData().toString()).toHtmlEscaped());
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = tree->topLevelItem(i);
        html += QStringLiteral("<h2>%1</h2>\n<table><tr><th>コマンド</th><th>ショートカット</th></tr>\n").arg(top->text(0).toHtmlEscaped());
        for (int j = 0; j < top->childCount(); ++j) {
            const QString name = top->child(j)->data(0, kName).toString();
            QStringList keys;
            if (QKeySequenceEdit* edit = editor(name); edit != nullptr && !edit->keySequence().isEmpty()) {
                keys << key_words(edit->keySequence());
                // (the set's other keys go with its first: 一般… Ctrl+K and Ctrl+,)
                const auto found = set_keys.find(name);
                if (found != set_keys.end() && !found->second.isEmpty() && found->second.front() == edit->keySequence()) {
                    for (qsizetype k = 1; k < found->second.size(); ++k) keys << key_words(found->second[k]);
                }
            }
            html += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>\n").arg(top->child(j)->toolTip(0).toHtmlEscaped(), keys.join(QStringLiteral("、")).toHtmlEscaped());
        }
        html += QStringLiteral("</table>\n");
    }
    return html + QStringLiteral("</body></html>\n");
}

bool ShortcutsDialog::write_summary(const QString& path) {
    QString where = path;
    if (where.isEmpty()) where = ask::save_path(this, QStringLiteral("キーボードショートカットの概要"), QStringLiteral("キーボードショートカット.html"), QStringLiteral("HTML (*.html)"));
    if (where.isEmpty()) return false;
    QSaveFile file(where);
    if (!file.open(QIODevice::WriteOnly) || file.write(summary_html().toUtf8()) < 0 || !file.commit()) {
        clash->setText(QStringLiteral("概要を書き出せませんでした: %1").arg(where));
        return false;
    }
    clash->setText(QStringLiteral("概要を書き出しました: %1").arg(where));
    return true;
}

std::vector<std::pair<QString, QStringList>> ShortcutsDialog::mapping() const {
    std::vector<std::pair<QString, QStringList>> out;
    for (const auto& [name, edit] : editors) {
        out.emplace_back(name, edit->keySequence().isEmpty() ? QStringList() : QStringList{edit->keySequence().toString()});
    }
    return out;
}

// --- メニュー -------------------------------------------------------------------------------------------------------------

void ShortcutsDialog::fill_menus() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    auto* hint = new QLabel(QStringLiteral("使わない項目を隠し、よく使う項目に色を付けます。隠した項目もキーと 検索 で使えます。承認・ロック・AI の接続は、"
                                           "隠しても確かめの窓が出ます。メニューの終わりの「すべてのメニュー項目を表示」で、隠した項目も出せます。"));
    hint->setWordWrap(true);
    theme::role(hint, "hint");
    column->addWidget(hint);
    menu_tree = new QTreeWidget;
    menu_tree->setObjectName(QStringLiteral("menus_tree"));
    menu_tree->setColumnCount(3);
    menu_tree->setHeaderLabels({QStringLiteral("メニューの項目"), QStringLiteral("表示"), QStringLiteral("カラー")});
    menu_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    menu_tree->header()->setStretchLastSection(false);
    menu_tree->header()->resizeSection(1, 56);
    menu_tree->header()->resizeSection(2, 110);
    menu_tree->setUniformRowHeights(true);
    menu_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const auto names = command_names(window_);
    const QStringList hid = menu_look::hidden();
    const auto colours = menu_look::coloured();
    std::function<void(QMenu*, QTreeWidgetItem*)> walk = [&](QMenu* menu, QTreeWidgetItem* parent) {
        for (QAction* item : menu->actions()) {
            if (item->isSeparator()) continue;
            if (item->menu() != nullptr) {
                auto* node = new QTreeWidgetItem(parent, {item->menu()->title().remove(QLatin1Char('&'))});
                node->setFlags(Qt::ItemIsEnabled);
                walk(item->menu(), node);
                if (node->childCount() == 0) delete node;
                continue;
            }
            if (item->text().isEmpty() || item->text().startsWith(QStringLiteral("（")) || remade(item)) continue;
            const QString name = command_name(names, item);
            auto* row = new QTreeWidgetItem(parent, {QString(item->text()).remove(QLatin1Char('&'))});
            row->setData(0, kName, name);
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            hidden_[name] = hid.contains(name);
            row->setCheckState(1, hidden_[name] ? Qt::Unchecked : Qt::Checked);
            const auto colour = colours.find(name);
            colours_[name] = colour != colours.end() ? colour->second : QStringLiteral("none");
            row->setText(2, colour_label(colours_[name]));
            row->setIcon(2, swatch(menu_look::colour(colours_[name])));
            menu_rows_[name].push_back(row);
        }
    };
    for (QAction* top : window_->menuBar()->actions()) {
        if (top->menu() == nullptr) continue;
        auto* node = new QTreeWidgetItem(menu_tree, {QString(top->text()).remove(QLatin1Char('&'))});
        node->setFlags(Qt::ItemIsEnabled);
        walk(top->menu(), node);
        if (node->childCount() == 0) delete node;
    }
    connect(menu_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        if (col != 1 || quiet_) return;
        const QString name = item->data(0, kName).toString();
        if (!name.isEmpty()) set_hidden(name, item->checkState(1) == Qt::Unchecked);
    });
    connect(menu_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { show_menu_row(item); });
    column->addWidget(menu_tree, 1);
    menu_colour = new QComboBox;
    menu_colour->setObjectName(QStringLiteral("menus_colour"));
    for (const menu_look::Colour& one : menu_look::colours()) menu_colour->addItem(swatch(one.colour), one.label, one.id);
    menu_colour->setToolTip(QStringLiteral("選んだ項目の色"));
    connect(menu_colour, &QComboBox::activated, this, [this] {
        for (QTreeWidgetItem* item : menu_tree->selectedItems()) {
            const QString name = item->data(0, kName).toString();
            if (!name.isEmpty()) set_colour(name, menu_colour->currentData().toString());
        }
    });
    menus_reset = new QPushButton(QStringLiteral("初期設定に戻す"));
    menus_reset->setToolTip(QStringLiteral("すべての項目を表示し、色を外します"));
    connect(menus_reset, &QPushButton::clicked, this, &ShortcutsDialog::reset_menus);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("カラー")));
    row->addWidget(menu_colour);
    row->addStretch(1);
    row->addWidget(menus_reset);
    column->addLayout(row);
    tabs->addTab(page, QStringLiteral("メニュー"));
}

void ShortcutsDialog::show_menu_row(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    const QString name = item->data(0, kName).toString();
    menu_colour->setEnabled(!name.isEmpty());
    if (!name.isEmpty()) menu_colour->setCurrentIndex(std::max(0, menu_colour->findData(colour_of(name))));
}

void ShortcutsDialog::set_hidden(const QString& command, bool hidden) {
    hidden_[command] = hidden;
    const QScopedValueRollback<bool> quiet(quiet_, true);
    for (QTreeWidgetItem* row : menu_rows_[command]) row->setCheckState(1, hidden ? Qt::Unchecked : Qt::Checked);
}

void ShortcutsDialog::set_colour(const QString& command, const QString& colour) {
    const QString id = menu_look::colour(colour).isValid() ? colour : QStringLiteral("none");
    colours_[command] = id;
    for (QTreeWidgetItem* row : menu_rows_[command]) {
        row->setText(2, colour_label(id));
        row->setIcon(2, swatch(menu_look::colour(id)));
    }
}

bool ShortcutsDialog::is_hidden(const QString& command) const {
    const auto found = hidden_.find(command);
    return found != hidden_.end() && found->second;
}

QString ShortcutsDialog::colour_of(const QString& command) const {
    const auto found = colours_.find(command);
    return found != colours_.end() ? found->second : QStringLiteral("none");
}

void ShortcutsDialog::reset_menus() {
    for (const auto& [name, rows] : menu_rows_) {
        set_hidden(name, false);
        set_colour(name, QStringLiteral("none"));
    }
}

// --- 決める ----------------------------------------------------------------------------------------------------------------

void ShortcutsDialog::save() {
    const auto keys = mapping();
    if (const auto clashes = preferences::conflicts(keys); !clashes.empty()) {
        const preferences::Clash& first = clashes.front();
        clash->setText(QStringLiteral("「%1」が「%2」と「%3」の両方に付いています。どちらかを変えます。").arg(first.keys, first.first, first.second));
        tabs->setCurrentIndex(0);
        return;
    }
    // the set (written only when chosen: the default stays the default), the person's own over it
    const QString chosen = key_set->currentData().toString();
    const bool own = chosen.startsWith(QStringLiteral("own:"));
    const QString base = own ? key_sets::own_base(chosen.mid(4)) : chosen;
    if (base != key_sets::current()) key_sets::set_current(base);
    if (own) {
        settings()->setValue(QStringLiteral("ui/key_set_own"), chosen.mid(4));
    } else {
        settings()->remove(QStringLiteral("ui/key_set_own"));
    }
    preferences::keep_keys(window_, keys, set_keys);
    // the menus' items
    QStringList hid;
    std::map<QString, QString> colours;
    for (const auto& [name, rows] : menu_rows_) {
        if (is_hidden(name)) hid << name;
        if (colour_of(name) != QLatin1String("none")) colours[name] = colour_of(name);
    }
    menu_look::set_hidden(hid);
    menu_look::set_coloured(colours);
    for (MainWindow* window : documents::windows()) preferences::apply_all(window);  // (this computer's: every window)
    accept();
}

}  // namespace genko::app
