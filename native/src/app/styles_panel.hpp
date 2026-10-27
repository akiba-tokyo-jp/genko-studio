#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

#include <optional>
#include <string>

#include "core/json.hpp"

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The Styles panel (PC4 E6, window.panel.styles and style.presets.*, SPEC.md COLOR-09「スタイル」; Photoshop's スタイル
// パネル): the layer styles of this machine by their groups (render/style_presets: the settings folder's styles.json),
// each with its swatch — made off the window's thread, small, kept by the preset's settings, so a refresh reads only the
// small styles.json again and never the patterns' pictures it has drawn already. A click applies the style to the chosen
// layers (Shift+click: its effects added to theirs); the buttons: 新規スタイル… (the target layer's effects and blending
// options), 削除, and the panel's menu: 名前を変更…, 新規スタイルグループ…, スタイルを読み込み… (.asl), スタイルを初期設定に戻す,
// 初期設定のスタイルを追加. Every change reads the file again first (what another window or `genko styles` wrote is
// kept) and writes it at once; the window never keeps its own copy of the library.

namespace genko::app {

class MainWindow;

class StylesPanel : public QWidget {
    Q_OBJECT
public:
    explicit StylesPanel(MainWindow* window);
    // The library read again and listed (swatches not made yet asked for in the background).
    void refresh();
    QTreeWidget* tree() const { return tree_; }
    QPushButton* button(const QString& name) const;
    // The chosen preset's name, or "" (a group, nothing).
    std::string chosen() const;
    // The work of the clicks, the buttons and the menu (tests call them as they do).
    bool apply(const std::string& name, bool add);
    void rename(std::optional<QString> name = std::nullopt);
    void remove();
    void new_group(std::optional<QString> name = std::nullopt);
    bool import_asl(const QString& path);
    void reset(bool append);
    // Swatches asked for and not made yet (tests wait for none).
    int pending() const { return pending_; }

signals:
    void swatches_done();

private:
    void ask_swatch(QTreeWidgetItem* item, const core::Json& preset);
    void put_swatches();

    MainWindow* window_;
    QTreeWidget* tree_ = nullptr;
    int pending_ = 0;
    bool loading_ = false;
};

}  // namespace genko::app
