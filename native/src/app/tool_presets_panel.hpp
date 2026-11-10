#pragma once

#include <QString>
#include <QWidget>

#include <optional>

#include "core/json.hpp"

class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The Tool Presets panel (PC4 E2, window.panel.toolPresets, tool.presets.*; SPEC.md COLOR-09「ツールプリセット」; Photoshop's
// ツールプリセット パネル): the tool presets of this machine (render/tool_presets: the settings folder's tool_presets.json),
// 現在のツールのみ shown or all. A click selects one (tool.presets.select: its tool in hand, its brush and its settings on
// the tool's page, its 描画色 when it keeps one). The buttons: 新規ツールプリセット… (the tool in hand and its page's settings,
// 描画色を含める asked), 名前を変更…, 削除, and the menu's 初期設定に戻す. The library is read again before each change and
// written whole (what another window or `genko tool-presets` wrote is kept).

namespace genko::app {

class MainWindow;

class ToolPresetsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolPresetsPanel(MainWindow* window);
    void refresh();
    QTreeWidget* tree() const { return tree_; }
    QCheckBox* current_only = nullptr;
    QPushButton* button(const QString& name) const;
    // The buttons' work (the tests call them as they do).
    void new_preset(std::optional<QString> name = std::nullopt, std::optional<bool> include_colour = std::nullopt);
    void rename(std::optional<QString> name = std::nullopt);
    void remove();
    // 初期設定に戻す (`append`: the built-in ones added after one's own); in place of one's own asked first unless `sure`.
    void reset(bool append = false, std::optional<bool> sure = std::nullopt);
    // The preset named made the tool in hand with its settings (false: none of that name).
    bool select(const QString& name);

private:
    void chosen(QTreeWidgetItem* item);

    MainWindow* window_;
    QTreeWidget* tree_ = nullptr;
    bool loading_ = false;
};

}  // namespace genko::app
