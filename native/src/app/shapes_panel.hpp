#pragma once

#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

#include "render/shape_presets.hpp"

class QComboBox;
class QListWidget;
class QPushButton;

// The Shapes panel (pen-shapes@1, PC3 C-b, SPEC.md COLOR-07「シェイプのプリセット」; Photoshop's シェイプ パネル): the shape
// presets' groups (a machine's setting: render/shape_presets, the settings folder's shapes.json) and each group's shapes
// as small pictures. A click chooses the shape for the カスタムシェイプツール (and takes the tool), a double click (or
// 配置) puts it on the page as a shape part (add_shape_part through the window); 削除 takes a shape of the person's out of
// the presets, 初期設定に戻す brings the built-in ones back.

namespace genko::app {

class MainWindow;

class ShapesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ShapesPanel(MainWindow* window);
    // The presets again (read from the settings folder).
    void refresh();
    // Tests and the window: the groups, the shapes of the group shown, a button by its name, the presets as read.
    QComboBox* groups() const { return groups_; }
    QListWidget* list() const { return list_; }
    QPushButton* button(const QString& name) const;
    const std::vector<render::shape_presets::Group>& presets() const { return presets_; }
    // The shape chosen ({group, name}; empty names when none).
    std::pair<QString, QString> chosen() const;
    // The buttons' work (tests call them as the buttons do).
    bool place();
    bool remove();
    void reset();

private:
    void show_group();

    MainWindow* window_;
    QComboBox* groups_ = nullptr;
    QListWidget* list_ = nullptr;
    std::vector<render::shape_presets::Group> presets_;
    bool loading_ = false;
};

}  // namespace genko::app
