#pragma once

#include <QIcon>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The Brushes panel (PC4 E2, window.panel.brushes, SPEC.md COLOR-09「ブラシのプリセット」; Photoshop's ブラシ パネル): the
// brushes of this machine by their groups (render/brush_presets: Genko's, PhotoCraft's 36 dab brushes in 一般・ドライ
// メディア・ウェットメディア・特殊効果, and the person's from brushes.json), each with a small picture of its tip. A click
// makes it the brush in hand (the ブラシツール's, ツールの設定's list follows). The buttons: 新規ブラシプリセット… (the brush in
// hand kept as one's own), 名前を変更…, 削除 (one's own), and the panel's menu: ブラシを読み込み… (.abr: the window's thread
// stays free while it is read; .genkobrush), ブラシを書き出し… (.genkobrush), 新規グループ…, グループ名を変更…, グループを
// 削除, グループに移動. The library is read again for every change and every refresh, the pictures of the tips are made
// once (by their ref, small) off the window's thread and kept for the next refresh.

namespace genko::app {

class MainWindow;

class BrushesPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrushesPanel(MainWindow* window);
    // The library again (after a change; the pictures already made are kept).
    void refresh();
    QTreeWidget* tree() const { return tree_; }
    QPushButton* button(const QString& name) const;
    // The buttons' and the menu's work (the tests call them as they do).
    void new_preset(std::optional<QString> name = std::nullopt);
    void rename(std::optional<QString> name = std::nullopt);
    void remove();
    // A Photoshop .abr (read off the window's thread) or a .genkobrush into the library; true when something was read.
    bool import_file(const QString& path, std::optional<bool> replace = std::nullopt);
    bool export_file(const QString& path);
    void new_group(std::optional<QString> name = std::nullopt);
    void rename_group(std::optional<QString> name = std::nullopt);
    void remove_group();
    void move_to_group(const QString& group);
    // The key of the item chosen (a brush), or "".
    std::string chosen_key() const;
    // The tips' pictures still to make (the tests wait for none).
    std::size_t pictures_pending() const { return pending_.size(); }
    // The warnings of the last .abr read (the tests).
    std::vector<std::string> last_warnings;

private:
    void chosen(QTreeWidgetItem* item);
    void make_pictures();
    QString group_of_choice() const;

    MainWindow* window_;
    QTreeWidget* tree_ = nullptr;
    bool loading_ = false;
    // (the tips' pictures to make: the item's key and what draws it — a ref, or a round tip's hardness and roundness)
    struct Pending {
        std::string key;
        std::string ref;
        double hardness = 1.0, roundness = 1.0;
    };
    std::vector<Pending> pending_;
    QTimer picture_timer_;
    bool making_ = false;
};

// A brush's small picture: its sampled tip (by its ref: the library's or this process's picture) or a round tip of its
// hardness and roundness, `side` pixels square, black paint on clear; kept by what draws it.
QIcon brush_tip_icon(const std::string& ref, double hardness, double roundness, int side = 32);

}  // namespace genko::app
