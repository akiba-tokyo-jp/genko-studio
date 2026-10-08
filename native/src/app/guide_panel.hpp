#pragma once

#include <QWidget>

#include <optional>
#include <string>

#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QSpinBox;

// 定規・3D (Python's genko/app/guide_panel.py): the page's rulers — on or off, angle, ellipse, copies, the panel they
// work in — and its 3D figures and boxes — pose, turn, size, perspective, the figure's body and hands, the camera and
// the light, traced as lines or drawn with their surfaces. Every change is an op through the window.

namespace genko::app {

class MainWindow;

// "直線定規", "パース定規（2 点）", "対称定規（6 方向）", "… ・ コマの中だけ".
QString ruler_label(const core::Json& ruler);

class GuidePanel : public QWidget {
    Q_OBJECT
public:
    explicit GuidePanel(MainWindow* window);

    // The page's rulers and 3D listed again (the canvas's chosen ones kept chosen).
    void refresh();
    void select_prim(const std::string& prim_id);
    void delete_ruler();
    void delete_prim();
    void keep_pose(const std::optional<QString>& name = std::nullopt);
    void body_dialog();
    void camera_dialog();
    void render_dialog();

    // The fields (tests use them as Python's attributes).
    QListWidget* rulers = nullptr;
    QDoubleSpinBox* angle = nullptr;
    QDoubleSpinBox* ratio = nullptr;
    QSpinBox* copies = nullptr;
    QCheckBox* mirror = nullptr;
    QCheckBox* in_panel = nullptr;
    QLabel* ruler_hint = nullptr;
    QListWidget* prims = nullptr;
    QComboBox* preset = nullptr;
    QSlider* turn = nullptr;
    QSlider* tip = nullptr;
    QSlider* lean = nullptr;
    QDoubleSpinBox* size = nullptr;
    QSlider* focal = nullptr;
    QPushButton* body_button = nullptr;
    QPushButton* save_pose = nullptr;
    QCheckBox* ik = nullptr;

private:
    QSlider* slider(int lo, int hi, int index);
    const core::Json* ruler() const;
    const core::Json* prim() const;
    void show_ruler();
    void show_prim();
    void ruler_picked();
    void ruler_toggled(QListWidgetItem* item);
    void ruler_set(const core::Json& change);
    void panel_only(bool on);
    void prim_picked();
    void prim_set(const core::Json& change);
    void rot(int index, int degrees);
    void resize_prim();
    void choose_preset();

    MainWindow* window_ = nullptr;
    bool loading_ = false;
};

}  // namespace genko::app
