// PC4 E-0 (描く道具の土台, design section E 5.2): the drawing tools' groups — Photoshop's: ブラシ, 消しゴム, 塗り, ぼかし, 覆い焼き,
// スポイト — in the ツール menu and on the palette (a button's long press or right click offers its group's other tools),
// and the frames of the drawing tools' new panels (ブラシ設定 F5, ブラシ, パターン, スタイル, ツールプリセット), each filled by its
// group (E2: ブラシ設定・ブラシ・ツールプリセット, E3: パターン, E6: スタイル); カラー gets F6 (main_window_paint.cpp).

#include <QContextMenuEvent>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/colours.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"

namespace genko::app {

const std::vector<MainWindow::ToolGroup>& MainWindow::paint_tool_groups() {
    // (each group's tools by their ids, the first the one the palette shows; a tool not made yet — its group's: E1 鉛筆ツール,
    // E5 the brushes that change pixels — is left out until it is)
    static const std::vector<ToolGroup> groups{
        {"ブラシ", {"act_pen", "act_pencil", "act_color_replacement", "act_mixer_brush"}},
        {"消しゴム", {"act_eraser", "act_background_eraser", "act_magic_eraser"}},
        {"塗り", {"act_fill", "act_lassofill", "act_gradient"}},
        // (Genko's 色混ぜ beside Photoshop's ぼかし・シャープ・指先, which change pixels: E5)
        {"ぼかし", {"act_blur_tool", "act_sharpen_tool", "act_smudge_tool", "act_blend"}},
        {"覆い焼き", {"act_dodge_tool", "act_burn_tool", "act_sponge_tool"}},
        {"スポイト", {"act_picker"}},
    };
    return groups;
}

std::vector<QAction*> MainWindow::tool_group_of(const QAction* tool) const {
    for (const ToolGroup& group : paint_tool_groups()) {
        std::vector<QAction*> made;
        bool in = false;
        for (const char* name : group.tools) {
            QAction* act = action(QString::fromLatin1(name));
            if (act == nullptr) continue;
            made.push_back(act);
            in = in || act == tool;
        }
        if (in) return made;
    }
    return {};
}

void MainWindow::build_paint_tool_groups(QMenu* tools) {
    // (the tools the menu does not have yet, by their groups: none until a group makes one)
    const QList<QAction*> listed = tools->actions();
    bool added = false;
    for (const ToolGroup& group : paint_tool_groups()) {
        for (const char* name : group.tools) {
            QAction* act = action(QString::fromLatin1(name));
            if (act == nullptr || listed.contains(act)) continue;
            tools->addAction(act);
            added = true;
        }
    }
    if (added) tools->addSeparator();
}

void MainWindow::attach_tool_groups(QToolBar* palette) {
    for (QAction* act : palette->actions()) {
        const std::vector<QAction*> group = tool_group_of(act);
        if (group.size() < 2) continue;  // (a tool alone: nothing to offer)
        auto* button = qobject_cast<QToolButton*>(palette->widgetForAction(act));
        if (button == nullptr) continue;
        auto* menu = new QMenu(button);
        menu->setObjectName(QStringLiteral("tool_group:") + act->objectName());
        for (QAction* tool : group) menu->addAction(tool);
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);  // (a long press; a click chooses the tool shown)
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QToolButton::customContextMenuRequested, button, [button, menu](const QPoint& at) { menu->popup(button->mapToGlobal(at)); });
    }
}

QDockWidget* MainWindow::paint_dock(const QString& title, const char* name, const QString& about) {
    auto* body = new QWidget;
    body->setObjectName(QString::fromLatin1(name));
    auto* layout = new QVBoxLayout(body);
    auto* label = new QLabel(about);
    label->setWordWrap(true);
    theme::hint(label);
    layout->addWidget(label);
    layout->addStretch(1);
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(title);
    dock->setWidget(body);
    dock->setMinimumWidth(200);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(layers_dock_, dock);
    dock->hide();  // (brought by ウィンドウ, as ヒストグラム)
    view_menu_->addAction(dock->toggleViewAction());
    return dock;
}

void MainWindow::build_paint_docks() {
    const QString soon = QStringLiteral("（準備中）");
    QDockWidget* settings_dock = paint_dock(QStringLiteral("ブラシ設定"), "brush_settings_panel",
                                            QStringLiteral("ブラシ先端のシェイプ・散布・テクスチャ・デュアルブラシなど、ブラシの細かい設定") + soon);
    settings_dock->toggleViewAction()->setShortcut(QKeySequence(QStringLiteral("F5")));  // (Photoshop's ブラシ設定)
    paint_dock(QStringLiteral("ブラシ"), "brushes_panel", QStringLiteral("グループに分けたブラシの一覧と、ブラシの読み込み・書き出し") + soon);
    // (グラデーション: E4's — its name is the gradient tool's old one, which preferences::renamed() reads as グラデーションツール)
    paint_dock(QStringLiteral("パターン"), "patterns_panel", QStringLiteral("パターンのライブラリと、この原稿のパターン") + soon);
    paint_dock(QStringLiteral("スタイル"), "styles_panel", QStringLiteral("レイヤースタイルのプリセット") + soon);
    paint_dock(QStringLiteral("ツールプリセット"), "tool_presets_panel", QStringLiteral("道具と設定をまとめたプリセット") + soon);
    layers_dock_->raise();
    // スウォッチ: the カラー panel's tab (colorsets.json), opened from ウィンドウ as Photoshop's panel
    view_menu_->addAction(make("act_swatches", QStringLiteral("スウォッチ"), [this] {
        if (colours_ == nullptr) return;
        for (QWidget* up = colours_->parentWidget(); up != nullptr; up = up->parentWidget()) {
            if (auto* dock = qobject_cast<QDockWidget*>(up)) {
                dock->show();
                dock->raise();
                break;
            }
        }
        colours_->tabs->setCurrentIndex(0);
    }, {}, QStringLiteral("カラー パネルのスウォッチを開きます")));
}

}  // namespace genko::app
