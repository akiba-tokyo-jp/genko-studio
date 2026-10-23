// PC4 E2 (window.panel.toolPresets, tool.presets.new・.list・.edit・.reset・.select; SPEC.md COLOR-09「ツールプリセット」): the
// Tool Presets panel — the machine's tool presets (render/tool_presets), the tool in hand's or all.

#include "app/tool_presets_panel.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "core/error.hpp"
#include "render/preset_store.hpp"
#include "render/tool_presets.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace store = render::preset_store;

namespace {

constexpr int kNameRole = Qt::UserRole + 1;

}  // namespace

ToolPresetsPanel::ToolPresetsPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("tool_presets_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    current_only = new QCheckBox(QStringLiteral("現在のツールのみ"));
    current_only->setObjectName(QStringLiteral("tool_presets_current_only"));
    current_only->setChecked(settings()->value(QStringLiteral("tool_presets/current_only"), QStringLiteral("true")).toString() == QLatin1String("true"));
    connect(current_only, &QCheckBox::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("tool_presets/current_only"), on ? QStringLiteral("true") : QStringLiteral("false"));
        refresh();
    });
    layout->addWidget(current_only);
    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("tool_presets_tree"));
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { chosen(item); });
    auto* row = new QHBoxLayout;
    const auto add = [&](const char* name, const QString& text, const QString& tip, const std::function<void()>& work) {
        auto* b = new QPushButton(text);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, work);
        row->addWidget(b);
    };
    add("tool_preset_new", QStringLiteral("＋"), QStringLiteral("新規ツールプリセット…: 今のツールとその設定を名前を付けて残します"), [this] { new_preset(); });
    add("tool_preset_rename", QStringLiteral("名前…"), QStringLiteral("選んだツールプリセットの名前を変えます"), [this] { rename(); });
    add("tool_preset_delete", QStringLiteral("削除"), QStringLiteral("選んだツールプリセットを消します"), [this] { remove(); });
    auto* more = new QToolButton;
    more->setObjectName(QStringLiteral("tool_preset_more"));
    more->setText(QStringLiteral("…"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->setObjectName(QStringLiteral("tool_preset_menu"));
    menu->addAction(QStringLiteral("ツールプリセットを初期設定に戻す"), this, [this] { reset(); });
    more->setMenu(menu);
    row->addWidget(more);
    layout->addLayout(row);
}

QPushButton* ToolPresetsPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

void ToolPresetsPanel::refresh() {
    loading_ = true;
    tree_->clear();
    const store::Library lib = store::load(config_dir(), render::tool_presets::kind());
    const QString tool = window_->tool_in_hand();
    for (const store::Group& g : lib.groups) {
        for (const Json& item : g.items) {
            const QString of = QString::fromStdString(item.value("tool", std::string()));
            if (current_only->isChecked() && of != tool) continue;
            const QString name = QString::fromStdString(item.value("name", std::string()));
            auto* row = new QTreeWidgetItem(tree_, {name});
            row->setData(0, kNameRole, name);
            row->setToolTip(0, QStringLiteral("%1（%2）").arg(name, window_->tool_title(of)));
        }
    }
    if (!lib.problem.empty()) {
        auto* note = new QTreeWidgetItem(tree_, {QStringLiteral("（ツールプリセットのファイルを読めないので、初期設定を見せています）")});
        note->setFlags(Qt::ItemIsEnabled);
    }
    loading_ = false;
}

void ToolPresetsPanel::chosen(QTreeWidgetItem* item) {
    if (loading_ || item == nullptr) return;
    const QString name = item->data(0, kNameRole).toString();
    if (!name.isEmpty()) select(name);
}

bool ToolPresetsPanel::select(const QString& name) {
    const store::Library lib = store::load(config_dir(), render::tool_presets::kind());
    const Json* item = store::find(lib.groups, name.toStdString());
    if (item == nullptr) return false;
    window_->use_tool_preset(*item);
    return true;
}

void ToolPresetsPanel::new_preset(std::optional<QString> name, std::optional<bool> include_colour) {
    const QString tool = window_->tool_in_hand();
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("新規ツールプリセット"), QStringLiteral("名前"), window_->tool_title(tool));
        if (!asked) return;
        name = asked->trimmed();
    }
    if (!include_colour) include_colour = ask::question(this, QStringLiteral("新規ツールプリセット"), QStringLiteral("描画色も含めますか？"));
    Json options = window_->tool_preset_options(tool, *include_colour);
    QString error;
    try {
        store::Library lib = store::load(config_dir(), render::tool_presets::kind());  // (read again: another window's kept)
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        const Json item = render::tool_presets::item_of(tool.toStdString(), name->isEmpty() ? std::nullopt : std::optional<std::string>(name->toStdString()), options);
        const Json out = store::add(lib.groups, render::tool_presets::kind(), item, std::nullopt, tool.toStdString());
        store::save(config_dir(), render::tool_presets::kind(), lib.groups);
        window_->flash(QStringLiteral("ツールプリセット「%1」を作りました").arg(QString::fromStdString(out.value("name", std::string()))), 3000);
    } catch (const std::exception& e) {
        ask::warning(this, QStringLiteral("新規ツールプリセット"), QStringLiteral("作れませんでした: %1").arg(wording::error(QString::fromUtf8(e.what()))));
    }
    refresh();
}

void ToolPresetsPanel::rename(std::optional<QString> name) {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return;
    const QString was = item->data(0, kNameRole).toString();
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("名前を変更"), QStringLiteral("名前"), was);
        if (!asked) return;
        name = asked->trimmed();
    }
    try {
        store::Library lib = store::load(config_dir(), render::tool_presets::kind());
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        store::edit(lib.groups, render::tool_presets::kind(), Json{{"action", "rename"}, {"preset", was.toStdString()}, {"name", name->toStdString()}});
        store::save(config_dir(), render::tool_presets::kind(), lib.groups);
    } catch (const std::exception& e) {
        ask::warning(this, QStringLiteral("名前を変更"), QStringLiteral("変えられませんでした: %1").arg(wording::error(QString::fromUtf8(e.what()))));
    }
    refresh();
}

void ToolPresetsPanel::remove() {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return;
    const QString was = item->data(0, kNameRole).toString();
    try {
        store::Library lib = store::load(config_dir(), render::tool_presets::kind());
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        store::edit(lib.groups, render::tool_presets::kind(), Json{{"action", "delete"}, {"preset", was.toStdString()}});
        store::save(config_dir(), render::tool_presets::kind(), lib.groups);
    } catch (const std::exception& e) {
        ask::warning(this, QStringLiteral("削除"), QStringLiteral("消せませんでした: %1").arg(wording::error(QString::fromUtf8(e.what()))));
    }
    refresh();
}

void ToolPresetsPanel::reset() {
    try {
        store::Library lib = store::load(config_dir(), render::tool_presets::kind());
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        lib.groups = store::reset(lib.groups, render::tool_presets::kind(), false);
        store::save(config_dir(), render::tool_presets::kind(), lib.groups);
    } catch (const std::exception& e) {
        ask::warning(this, QStringLiteral("初期設定に戻す"), QStringLiteral("戻せませんでした: %1").arg(wording::error(QString::fromUtf8(e.what()))));
    }
    refresh();
}

}  // namespace genko::app
