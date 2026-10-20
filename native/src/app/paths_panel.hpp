#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

#include "core/vector.hpp"

class QListWidget;
class QListWidgetItem;
class QPushButton;

// The Paths panel (vector@1, PC3 C-a, SPEC.md COLOR-07; Photoshop's パス パネル): the page's work path, its saved paths
// and the vector mask of the layer drawn on (and the shape parts of a pen layer, PC3 C-b), each with a small picture of
// its shape and its name. A click on a row makes it the target path (what the path tools draw on and show, what the
// buttons work on); a double click renames a saved path (or a shape part), or saves the work path under a name. The
// buttons, as Photoshop's: パスを塗りつぶす, ブラシでパスの境界線を描く,
// パスを選択範囲として読み込む, 選択範囲から作業用パスを作成, マスクを追加 (the vector mask of the layer drawn on),
// 新規パスを作成, パスを削除. Every change of the book is an op through the window (apply_ops).

namespace genko::app {

class MainWindow;

class PathsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PathsPanel(MainWindow* window);
    // The page's rows again (after a change, another page).
    void refresh();
    // Tests and the window: the list, a button by its name, each row's target ("work", "saved:<name>", "mask:<layer id>",
    // "shape:<layer id>/<part id>": PC3 C-b).
    QListWidget* list() const { return list_; }
    QPushButton* button(const QString& name) const;
    const std::vector<QString>& keys() const { return keys_; }
    // The buttons' work (tests call them as the buttons do).
    void add_new(std::optional<QString> name = std::nullopt);
    void remove();
    void rename(std::optional<QString> name = std::nullopt);

private:
    void clicked(QListWidgetItem* item);
    QIcon thumbnail(const core::vector::Path& path, double width_mm, double height_mm);

    MainWindow* window_;
    QListWidget* list_ = nullptr;
    std::vector<QString> keys_;
    bool loading_ = false;
};

}  // namespace genko::app
