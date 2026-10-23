// PC4 E2 (edit.presets.presetManager; SPEC.md COLOR-09「プリセットマネージャー」): 編集 → プリセット → プリセットマネージャー…

#include "app/preset_manager.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "render/preset_files.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kNameRole = Qt::UserRole + 1;   // (a preset's name)
constexpr int kGroupRole = Qt::UserRole + 2;  // (its group's name)

const std::vector<std::pair<std::string, QString>>& kind_names() {
    static const std::vector<std::pair<std::string, QString>> k{{"brushes", QStringLiteral("ブラシ")},
                                                                {"gradients", QStringLiteral("グラデーション")},
                                                                {"patterns", QStringLiteral("パターン")},
                                                                {"shapes", QStringLiteral("シェイプ")},
                                                                {"tool_presets", QStringLiteral("ツールプリセット")}};
    return k;
}

}  // namespace

PresetManager::PresetManager(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("preset_manager"));
    setWindowTitle(QStringLiteral("プリセットマネージャー"));
    auto* layout = new QVBoxLayout(this);
    kinds = new QComboBox;
    kinds->setObjectName(QStringLiteral("preset_manager_kind"));
    for (const auto& [k, label] : kind_names()) kinds->addItem(label, QString::fromStdString(k));
    connect(kinds, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    layout->addWidget(kinds);
    problem = new QLabel;
    problem->setObjectName(QStringLiteral("preset_manager_problem"));
    problem->setWordWrap(true);
    problem->hide();
    layout->addWidget(problem);
    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("preset_manager_tree"));
    tree_->setHeaderHidden(true);
    tree_->setMinimumSize(280, 320);
    layout->addWidget(tree_, 1);
    auto* grid = new QGridLayout;
    int n = 0;
    const auto add = [&](const char* name, const QString& text, const std::function<void()>& work) {
        auto* b = new QPushButton(text);
        b->setObjectName(QString::fromLatin1(name));
        connect(b, &QPushButton::clicked, this, work);
        grid->addWidget(b, n / 3, n % 3);
        ++n;
    };
    add("preset_rename", QStringLiteral("名前を変更…"), [this] { rename(); });
    add("preset_delete", QStringLiteral("削除"), [this] { remove(); });
    add("preset_up", QStringLiteral("上へ"), [this] { move(-1); });
    add("preset_down", QStringLiteral("下へ"), [this] { move(1); });
    add("preset_to_group", QStringLiteral("グループに移動…"), [this] { to_group(); });
    add("preset_new_group", QStringLiteral("新規グループ…"), [this] { new_group(); });
    add("preset_rename_group", QStringLiteral("グループ名を変更…"), [this] { rename_group(); });
    add("preset_delete_group", QStringLiteral("グループを削除"), [this] { remove_group(); });
    layout->addLayout(grid);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
    refresh();
}

QPushButton* PresetManager::button(const QString& name) const { return findChild<QPushButton*>(name); }

void PresetManager::show_kind(const std::string& kind) {
    const int at = kinds->findData(QString::fromStdString(kind));
    if (at >= 0) kinds->setCurrentIndex(at);
    refresh();
}

std::string PresetManager::kind() const { return kinds->currentData().toString().toStdString(); }

void PresetManager::refresh() {
    const QTreeWidgetItem* was = tree_->currentItem();
    const QString was_name = was != nullptr ? was->data(0, kNameRole).toString() : QString();
    tree_->clear();
    Json list;
    try {
        list = render::preset_files::manager_list(config_dir(), {kind()})[kind()];
    } catch (const std::exception& error) {
        problem->setText(wording::error(QString::fromUtf8(error.what())));
        problem->show();
        return;
    }
    problem->setVisible(list.contains("problem"));
    if (list.contains("problem")) problem->setText(QStringLiteral("このプリセットのファイルを読めないので、変えられません（ファイルはそのまま）"));
    for (const Json& g : list.value("groups", Json::array())) {
        const QString group = QString::fromStdString(g.value("name", std::string()));
        auto* row = new QTreeWidgetItem(tree_, {group});
        row->setData(0, kGroupRole, group);
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (g.value("builtin", false)) row->setToolTip(0, QStringLiteral("入っているブラシ（変えられません）"));
        for (const Json& p : g.value("presets", Json::array())) {
            const QString name = QString::fromStdString(p.get<std::string>());
            auto* item = new QTreeWidgetItem(row, {name});
            item->setData(0, kNameRole, name);
            item->setData(0, kGroupRole, group);
            if (name == was_name) tree_->setCurrentItem(item);
        }
        row->setExpanded(true);
    }
}

std::optional<std::pair<std::string, std::string>> PresetManager::chosen() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || item->data(0, kNameRole).toString().isEmpty()) return std::nullopt;
    return std::pair{item->data(0, kNameRole).toString().toStdString(), item->data(0, kGroupRole).toString().toStdString()};
}

std::optional<std::string> PresetManager::chosen_group() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return std::nullopt;
    return item->data(0, kGroupRole).toString().toStdString();
}

bool PresetManager::apply(const Json& params) {
    try {
        render::preset_files::manager_edit(config_dir(), kind(), params);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("プリセットマネージャー"), QStringLiteral("できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        return false;
    }
    refresh();
    window_->presets_changed();
    return true;
}

void PresetManager::rename() {
    const auto c = chosen();
    if (!c) return;
    const auto name = ask::get_text(this, QStringLiteral("名前を変更"), QStringLiteral("名前"), QString::fromStdString(c->first));
    if (!name || name->trimmed().isEmpty()) return;
    apply(Json{{"action", "rename"}, {"preset", c->first}, {"group", c->second}, {"name", name->trimmed().toStdString()}});
}

void PresetManager::remove() {
    const auto c = chosen();
    if (!c) return;
    apply(Json{{"action", "delete"}, {"preset", c->first}, {"group", c->second}});
}

void PresetManager::move(int by) {
    const auto c = chosen();
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!c || item == nullptr || item->parent() == nullptr) return;
    const int at = item->parent()->indexOfChild(const_cast<QTreeWidgetItem*>(item));
    const int to = at + by;
    if (to < 0 || to >= item->parent()->childCount()) return;
    apply(Json{{"action", "move"}, {"preset", c->first}, {"group", c->second}, {"to", c->second}, {"index", to}});
}

void PresetManager::to_group() {
    const auto c = chosen();
    if (!c) return;
    QStringList groups;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) groups << tree_->topLevelItem(i)->data(0, kGroupRole).toString();
    const auto picked = ask::get_item(this, QStringLiteral("グループに移動"), QStringLiteral("グループ"), groups);
    if (!picked) return;
    apply(Json{{"action", "move"}, {"preset", c->first}, {"group", c->second}, {"to", picked->toStdString()}});
}

void PresetManager::new_group() {
    const auto name = ask::get_text(this, QStringLiteral("新規グループ"), QStringLiteral("グループ名"), QStringLiteral("グループ 1"));
    if (!name || name->trimmed().isEmpty()) return;
    apply(Json{{"action", "new_group"}, {"name", name->trimmed().toStdString()}});
}

void PresetManager::rename_group() {
    const auto g = chosen_group();
    if (!g) return;
    const auto name = ask::get_text(this, QStringLiteral("グループ名を変更"), QStringLiteral("グループ名"), QString::fromStdString(*g));
    if (!name || name->trimmed().isEmpty()) return;
    apply(Json{{"action", "rename_group"}, {"group", *g}, {"name", name->trimmed().toStdString()}});
}

void PresetManager::remove_group() {
    const auto g = chosen_group();
    if (!g) return;
    if (!ask::question(this, QStringLiteral("グループを削除"), QStringLiteral("グループ「%1」とその中のプリセットを消しますか？").arg(QString::fromStdString(*g)))) return;
    apply(Json{{"action", "delete_group"}, {"group", *g}});
}

}  // namespace genko::app
