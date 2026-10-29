// ウィンドウ → ワークスペース ▸ and 編集 → キーボードショートカット…・メニュー…・ツールバー… (PC5 H-d, 節 H 5.2・5.4; SPEC VIEW-01
// 「ワークスペース」「キーボードショートカットとメニュー」「ツールバー」): Photoshop's workspaces built in (PhotoCraft v0.2.0's
// workspace_ui.rs and menus.rs apply_workspace, MIT) and the person's own — kept with their keys, menus and toolbar when
// chosen, refused a built-in one's name —, the one in use checked, reset, deleted (never the one in use) and locked;
// a workspace left as it was moved, and chosen again so (環境設定 → ワークスペース → ワークスペースの変更を記憶). The window's
// arrangement of the books (アレンジ ▸) is not a workspace's: Photoshop's workspaces keep the panels only.

#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include "app/ask.hpp"
#include "app/comfort.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/shortcuts_dialog.hpp"
#include "app/toolbar_dialog.hpp"
#include "app/workspace.hpp"

namespace genko::app {

namespace {

const QString kChanges = QStringLiteral("workspace_changes");  // (a workspace as it was left: workspace_changes/<name>)

bool setting_on(const char* key, bool fallback) {
    const QString value = settings()->value(QString::fromLatin1(key), fallback ? QStringLiteral("true") : QStringLiteral("false")).toString().toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") || value == QLatin1String("yes");
}

bool remember_changes() { return setting_on("ui/workspace_remember", true); }
bool locked() { return setting_on("ui/workspace_locked", false); }

QString attribute_of(const Preset& preset) {
    // (graphicAndWeb → act_workspace_graphic_and_web)
    QString out = QStringLiteral("act_workspace_");
    for (const QChar c : preset.id) {
        if (c.isUpper()) out += QLatin1Char('_');
        out += c.toLower();
    }
    return out;
}

// The approval's panels: a workspace never hides nor brings them (the approval's way is the book's, not the screen's).
bool approval_panel(const QDockWidget* dock) {
    for (const char* title : {"承認箱", "コマの詳細", "資料"}) {
        if (dock->objectName() == QString::fromUtf8(title)) return true;
    }
    return false;
}

}  // namespace

void MainWindow::build_workspace_actions() {
    for (const Preset& one : presets()) {
        const QString label = one.label;
        make(attribute_of(one), label, [this, label] { choose_workspace(label); }, {},
             one.show.isEmpty() ? QStringLiteral("パネルを、窓を開いたときの並びにします")
                                : QStringLiteral("Photoshop の「%1」のパネルの並びにします").arg(label),
             true);
    }
    make("act_workspace_reset", QStringLiteral("ワークスペースをリセット"), [this] { reset_workspace(); }, {},
         QStringLiteral("今のワークスペースを、残したときのパネルの並びに戻します"));
    make("act_delete_workspace", QStringLiteral("ワークスペースを削除…"), [this] { delete_workspace_dialog(); }, {},
         QStringLiteral("自分で残したワークスペースを消します（今のワークスペースは消せません）"));
    QAction* lock = make("act_lock_workspace", QStringLiteral("ワークスペースをロック"), [] {}, {},
                         QStringLiteral("パネルを動かしたり、窓から外したりできないようにします"), true);
    connect(lock, &QAction::triggered, this, [this](bool on) { lock_workspace(on); });
}

void MainWindow::fill_workspaces() {
    for (QMenu* sub : workspace_menu_->findChildren<QMenu*>(Qt::FindDirectChildrenOnly)) sub->deleteLater();  // (clear() leaves them)
    workspace_menu_->clear();
    const QString now = current_workspace();
    for (const Preset& one : presets()) {
        QAction* act = action(attribute_of(one));
        const QSignalBlocker quiet(act);
        act->setChecked(one.label == now);
        workspace_menu_->addAction(act);
    }
    const QStringList names = workspaces();
    if (!names.isEmpty()) workspace_menu_->addSeparator();
    for (const QString& name : names) {
        QAction* own = workspace_menu_->addAction(name, this, [this, name] { choose_workspace(name); });
        own->setCheckable(true);
        own->setChecked(name == now);
    }
    workspace_menu_->addSeparator();
    QAction* reset = action(QStringLiteral("act_workspace_reset"));
    reset->setStatusTip(QStringLiteral("「%1」を、残したときのパネルの並びに戻します").arg(now));
    workspace_menu_->addAction(reset);
    workspace_menu_->addAction(action(QStringLiteral("act_save_workspace")));
    workspace_menu_->addAction(action(QStringLiteral("act_delete_workspace")));
    {
        QAction* lock = action(QStringLiteral("act_lock_workspace"));
        const QSignalBlocker quiet(lock);
        lock->setChecked(locked());
        workspace_menu_->addAction(lock);
    }
    workspace_menu_->addSeparator();
    workspace_menu_->addAction(action(QStringLiteral("act_keyboard_shortcuts")));  // (Photoshop's キーボードショートカットとメニュー…)
    if (stage_menu_ != nullptr) {
        workspace_menu_->addSeparator();
        workspace_menu_->addMenu(stage_menu_);
    }
}

QString MainWindow::current_workspace() const {
    const QString kept = settings()->value(QStringLiteral("ui/workspace")).toString();
    if (preset(kept) != nullptr || (!kept.isEmpty() && workspaces().contains(kept))) return kept;
    return presets().front().label;  // (初期設定)
}

void MainWindow::lay_out_workspace(const QString& name, bool as_left) {
    const QString left = settings()->value(kChanges + QLatin1Char('/') + name).toString();
    if (as_left && !left.isEmpty() && restoreState(QByteArray::fromBase64(left.toLatin1()))) return;
    if (const Preset* one = preset(name)) {
        if (!default_state_.isEmpty()) restoreState(default_state_);
        if (!one->show.isEmpty()) {
            const auto docks = findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly);
            const auto named = [](const QDockWidget* dock, const QString& title) { return dock->windowTitle() == title || dock->objectName() == title; };
            for (QDockWidget* dock : docks) {
                if (approval_panel(dock)) continue;
                dock->setVisible(std::any_of(one->show.begin(), one->show.end(), [&](const QString& title) { return named(dock, title); }));
            }
            for (auto front = one->front.rbegin(); front != one->front.rend(); ++front) {
                for (QDockWidget* dock : docks) {
                    if (named(dock, *front) && dock->isVisible()) dock->raise();
                }
            }
        }
        if (one->id == QLatin1String("pixelArt")) {  // (ピクセルアート: the pixels' grid and their own squares on)
            for (const char* switch_on : {"act_pixel_grid", "act_pixel_art"}) {
                if (QAction* act = action(QString::fromLatin1(switch_on)); act != nullptr && !act->isChecked()) act->setChecked(true);
            }
        }
        return;
    }
    load_workspace(this, name);
}

void MainWindow::choose_workspace(const QString& name) {
    if (preset(name) == nullptr && !workspaces().contains(name)) {
        flash(QStringLiteral("ワークスペース「%1」はありません").arg(name), 5000, true);
        return;
    }
    // (the one left kept as it is now: chosen again, it comes back so — Photoshop's ワークスペースの変更を記憶)
    if (remember_changes() && settled_) {
        settings()->setValue(kChanges + QLatin1Char('/') + current_workspace(), QString::fromLatin1(saveState().toBase64()));
    }
    settings()->setValue(QStringLiteral("ui/workspace"), name);
    lay_out_workspace(name, remember_changes());
    if (preset(name) == nullptr && load_workspace_extras(name)) {
        for (MainWindow* window : documents::windows()) preferences::apply_all(window);  // (its keys, menus, toolbar: this computer's)
    }
    for (const Preset& one : presets()) {
        QAction* act = action(attribute_of(one));
        const QSignalBlocker quiet(act);
        act->setChecked(one.label == name);
    }
    flash(QStringLiteral("ワークスペースを「%1」にしました（ウィンドウ → ワークスペース）").arg(name), 3000);
}

void MainWindow::reset_workspace() {
    const QString name = current_workspace();
    settings()->remove(kChanges + QLatin1Char('/') + name);
    lay_out_workspace(name, false);
    if (preset(name) == nullptr && load_workspace_extras(name)) {
        for (MainWindow* window : documents::windows()) preferences::apply_all(window);
    }
    flash(QStringLiteral("ワークスペース「%1」を、残したときの並びに戻しました").arg(name), 3000);
}

bool MainWindow::save_workspace(std::optional<QString> name) {
    if (!name) return new_workspace();
    const QString kept = name->trimmed();
    if (kept.isEmpty()) return false;
    if (reserved_workspace(kept)) {
        flash(QStringLiteral("「%1」は Genko にあるワークスペースの名前です。別の名前にします").arg(kept), 6000, true);
        return false;
    }
    if (kept.contains(QLatin1Char('/')) || kept.contains(QLatin1Char('\\'))) {
        // (the settings keep a workspace under its name: "/" and "\" would make it a group inside another)
        flash(QStringLiteral("ワークスペースの名前に「/」「\\」は使えません"), 5000, true);
        return false;
    }
    genko::app::save_workspace(this, kept);
    settings()->remove(kChanges + QLatin1Char('/') + kept);
    settings()->setValue(QStringLiteral("ui/workspace"), kept);
    flash(QStringLiteral("ワークスペース「%1」を残しました（ウィンドウ → ワークスペース）").arg(kept), 3000);
    return true;
}

bool MainWindow::new_workspace() {
    NewWorkspaceDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return false;
    const QString name = dialog.name->text().trimmed();
    if (name.isEmpty()) {
        flash(QStringLiteral("ワークスペースの名前を入れます"), 5000, true);
        return false;
    }
    if (reserved_workspace(name)) {
        flash(QStringLiteral("「%1」は Genko にあるワークスペースの名前です。別の名前にします").arg(name), 6000, true);
        return false;
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        flash(QStringLiteral("ワークスペースの名前に「/」「\\」は使えません"), 5000, true);
        return false;
    }
    if (workspaces().contains(name) &&
        !ask::question(this, QStringLiteral("新規ワークスペース"), QStringLiteral("ワークスペース「%1」はもうあります。今の並びで置き換えますか？").arg(name))) {
        return false;
    }
    genko::app::save_workspace(this, name, dialog.keys->isChecked(), dialog.menus->isChecked(), dialog.toolbar->isChecked());
    settings()->remove(kChanges + QLatin1Char('/') + name);
    settings()->setValue(QStringLiteral("ui/workspace"), name);
    flash(QStringLiteral("ワークスペース「%1」を残しました（ウィンドウ → ワークスペース）").arg(name), 3000);
    return true;
}

bool MainWindow::delete_workspace_dialog() {
    const QStringList names = workspaces();
    if (names.isEmpty()) {
        flash(QStringLiteral("自分で残したワークスペースはありません（Genko にあるワークスペースは消せません）"), 5000);
        return false;
    }
    const auto chosen = ask::get_item(this, QStringLiteral("ワークスペースを削除"), QStringLiteral("削除するワークスペース"), names, 0, false);
    if (!chosen || chosen->isEmpty()) return false;
    if (*chosen == current_workspace()) {  // (Photoshop's: another chosen first)
        flash(QStringLiteral("「%1」は今のワークスペースなので削除できません。ほかのワークスペースにしてから削除します").arg(*chosen), 6000, true);
        return false;
    }
    if (!ask::question(this, QStringLiteral("ワークスペースを削除"), QStringLiteral("ワークスペース「%1」を削除しますか？").arg(*chosen))) return false;
    delete_workspace(*chosen);
    flash(QStringLiteral("ワークスペース「%1」を削除しました").arg(*chosen), 3000);
    return true;
}

void MainWindow::lock_workspace(bool on) {
    settings()->setValue(QStringLiteral("ui/workspace_locked"), on ? QStringLiteral("true") : QStringLiteral("false"));
    for (MainWindow* window : documents::windows()) window->apply_lock(on);
    apply_lock(on);  // (a window not yet in the list: tests)
    flash(on ? QStringLiteral("ワークスペースをロックしました（パネルは動かせません）") : QStringLiteral("ワークスペースのロックを外しました"), 3000);
}

void MainWindow::apply_lock(bool on) {
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        const QVariant own = dock->property("genko_features");  // (the panel's own: given back when unlocked)
        if (on) {
            if (!own.isValid()) dock->setProperty("genko_features", dock->features().toInt());
            const auto features = QDockWidget::DockWidgetFeatures::fromInt(dock->property("genko_features").toInt());
            dock->setFeatures(features & ~(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable));
        } else if (own.isValid()) {
            dock->setFeatures(QDockWidget::DockWidgetFeatures::fromInt(own.toInt()));
            dock->setProperty("genko_features", QVariant());
        }
    }
    if (QAction* lock = action(QStringLiteral("act_lock_workspace")); lock != nullptr && lock->isChecked() != on) {
        const QSignalBlocker quiet(lock);
        lock->setChecked(on);
    }
}

void MainWindow::restore_workspace_at_start() {
    // (the workspace in use when Genko was last used, as Photoshop's: the window's own first layout is 初期設定)
    const QString now = current_workspace();
    if (now != presets().front().label) lay_out_workspace(now, remember_changes());
}

void MainWindow::open_keyboard_shortcuts(int tab) {
    ShortcutsDialog dialog(this, tab);
    ask::exec(&dialog);
}

void MainWindow::open_toolbar_dialog() {
    ToolbarDialog dialog(this);
    ask::exec(&dialog);
}

void MainWindow::fill_palette() {
    const QStringList order = toolbar::order();
    const QStringList away = toolbar::hidden();
    const QString laid = order.join(QLatin1Char('\t')) + QLatin1Char('\n') + away.join(QLatin1Char('\t'));
    if (palette_ == nullptr || laid == palette_laid_) return;
    palette_laid_ = laid;
    palette_->clear();  // (its buttons and their groups' menus go with them; the tools are the window's)
    if (extra_tools_ != nullptr) {
        if (QMenu* menu = extra_tools_->menu()) menu->deleteLater();
        extra_tools_->deleteLater();
        extra_tools_ = nullptr;
    }
    bool line = false;
    bool any = false;
    for (const QString& entry : order) {
        if (entry == QLatin1String("|")) {
            line = any;
            continue;
        }
        QAction* tool = action(entry);
        if (tool == nullptr || away.contains(entry)) continue;
        if (line) palette_->addSeparator();
        line = false;
        palette_->addAction(tool);
        any = true;
    }
    attach_tool_groups(palette_);  // (PC4 E-0: a tool's group on a long press or a right click: paint_panels.cpp)
    // (PC5 H-a review P3: 手のひらツール with ズームツール in its group — one button, so the palette still fits a short window)
    QAction* hand = action(QStringLiteral("act_hand_tool"));
    if (auto* button = hand != nullptr ? qobject_cast<QToolButton*>(palette_->widgetForAction(hand)) : nullptr) {
        auto* menu = new QMenu(button);
        menu->setObjectName(QStringLiteral("tool_group:") + hand->objectName());
        menu->addAction(hand);
        menu->addAction(action(QStringLiteral("act_zoom_tool")));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QToolButton::customContextMenuRequested, button, [button, menu](const QPoint& at) { menu->popup(button->mapToGlobal(at)); });
    }
    // 余分なツール: the tools moved away, under the palette's last button (Photoshop's …)
    QStringList extra;
    for (const QString& entry : away) {
        if (action(entry) != nullptr) extra << entry;
    }
    if (!extra.isEmpty()) {
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("extra_tools_menu"));
        for (const QString& entry : extra) menu->addAction(action(entry));
        extra_tools_ = new QAction(QStringLiteral("…"), this);
        extra_tools_->setObjectName(QStringLiteral("extra_tools"));  // (not a command: never named nor given keys)
        extra_tools_->setToolTip(QStringLiteral("余分なツール（編集 → ツールバー… で並べ替え）"));
        extra_tools_->setMenu(menu);
        palette_->addAction(extra_tools_);
        if (auto* button = qobject_cast<QToolButton*>(palette_->widgetForAction(extra_tools_))) button->setPopupMode(QToolButton::InstantPopup);
    }
    QTimer::singleShot(0, this, &MainWindow::fit_tools);
}

void MainWindow::apply_workspace_prefs() {
    fill_palette();
    apply_lock(locked());
    // 大きいタブ: the books' tabs larger
    if (doc_tabs_ != nullptr) {
        const bool large = setting_on("ui/large_tabs", false);
        if (large != doc_tabs_->property("large").toBool()) {
            doc_tabs_->setProperty("large", large);
            if (large) {
                QFont font = QApplication::font();
                if (font.pointSizeF() > 0) {
                    font.setPointSizeF(font.pointSizeF() * 1.25);
                } else {
                    font.setPixelSize(static_cast<int>(font.pixelSize() * 1.25));
                }
                doc_tabs_->setFont(font);
            } else {
                doc_tabs_->setFont(QFont());  // (the window's again)
            }
        }
    }
    if (canvas_only_ != nullptr) canvas_only_->auto_show = setting_on("ui/auto_show_panels", true);
}

}  // namespace genko::app
