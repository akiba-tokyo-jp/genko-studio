#pragma once

#include <QDialog>
#include <QPixmap>
#include <QWidget>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "app/pen.hpp"
#include "core/brushes.hpp"
#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;

// The brush panel (Python's genko/app/brush_panel.py): the pen's kind, size, opacity, steadiness, taper, pressure,
// colour; the fill and eraser settings. The settings live in the app's settings and travel with every line as op
// fields, so a line keeps the look it was drawn with. And the dialog that makes one's own brush from another (or
// opens one of one's own again): its size and pressure, smoothness, colour and texture, tip and pattern, with a sample
// line drawn as it changes; and (this build) the paper under its lines (紙質: BRUSH-01).

namespace genko::app {

class LineSample;

// A short line drawn with this brush (pressed lightly, hard, lightly), for the brush list.
QPixmap stroke_preview(const std::string& kind, const std::array<int, 3>& ink, QSize size = {96, 20});

// The sizes offered as chips: the person's own (brush/sizes), or the usual nine.
std::vector<double> size_presets();

class BrushPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrushPanel(QWidget* parent = nullptr);

    // The brush chosen (its key).
    std::string kind() const;
    // The pen as set now (what a new line carries: PenSettings::stroke_fields).
    PenSettings pen() const;
    core::Json stroke_fields() const { return pen().stroke_fields(); }
    // How a fill looks for its area (fill / fill_enclosed fields).
    core::Json fill_fields() const;
    const std::array<int, 3>& rgb() const { return rgb_; }
    void set_colour(const std::array<int, 3>& rgb);
    // The list again (a new brush, or a book that brought its own); `select`: choose that one.
    void reload_kinds(const std::optional<std::string>& select = std::nullopt);
    std::vector<double> sizes() const { return size_presets(); }
    void set_sizes(const std::vector<double>& values);
    // The size one preset up or down ([ ]).
    double nudge_size(int step);

    // The fields (the window and the tests use them as Python's attributes).
    QListWidget* kinds = nullptr;
    QPushButton* make = nullptr;
    QPushButton* edit = nullptr;
    QPushButton* forget = nullptr;
    QPushButton* files = nullptr;
    QDoubleSpinBox* size = nullptr;
    QSlider* opacity = nullptr;
    QSpinBox* steady = nullptr;
    QCheckBox* taper = nullptr;
    QDoubleSpinBox* taper_in = nullptr;
    QDoubleSpinBox* taper_out = nullptr;
    QSpinBox* ink_pressure = nullptr;
    QCheckBox* speed_steady = nullptr;
    QDoubleSpinBox* snap_lines = nullptr;
    QDoubleSpinBox* post_fit = nullptr;
    QComboBox* pressure = nullptr;
    QPushButton* swatch = nullptr;
    QDoubleSpinBox* gap = nullptr;
    QComboBox* reference = nullptr;
    QSpinBox* tolerance = nullptr;
    QDoubleSpinBox* expand = nullptr;
    QCheckBox* skip_draft = nullptr;
    QCheckBox* skip_text = nullptr;
    QComboBox* lasso_mode = nullptr;
    QDoubleSpinBox* gap_size = nullptr;
    QCheckBox* crossing = nullptr;
    LineSample* sample = nullptr;

signals:
    void changed();

private:
    void fill_kinds();
    void fill_sizes();
    void size_menu(double value, QPushButton* button);
    void load();
    void apply_kind_defaults(const std::string& kind, bool stored);
    void kind_changed();
    void save();
    void follow_taper();
    void pick();

    std::array<int, 3> rgb_{20, 20, 20};
    QGridLayout* size_grid_ = nullptr;
    bool loading_ = true;
};

// Duplicate a brush and adjust it, or (editing) one of one's own opened again.
class BrushDialog : public QDialog {
    Q_OBJECT
public:
    BrushDialog(QWidget* parent, const std::string& base, bool editing = false);
    // The brush as set (brushes.from_dict's settings).
    core::Json data() const;
    // Draw the sample now (tests).
    void draw_sample();

    QLineEdit* name = nullptr;
    QDoubleSpinBox* width = nullptr;
    QSpinBox* thin = nullptr;
    QDoubleSpinBox* curve = nullptr;
    QSpinBox* opacity = nullptr;
    QSpinBox* steady = nullptr;
    QCheckBox* taper = nullptr;
    QComboBox* texture = nullptr;
    QCheckBox* fixed = nullptr;
    QCheckBox* white = nullptr;
    QLabel* sample = nullptr;
    QComboBox* tip = nullptr;
    QSpinBox* tip_angle = nullptr;
    QSpinBox* tip_ratio = nullptr;
    QCheckBox* tip_follow = nullptr;
    QCheckBox* tip_rotation = nullptr;
    QPushButton* tip_picture = nullptr;
    QComboBox* pattern = nullptr;
    QSpinBox* spacing = nullptr;
    QSpinBox* scatter = nullptr;
    QSpinBox* stamp = nullptr;
    QSpinBox* jitter = nullptr;
    QCheckBox* turn = nullptr;
    QSpinBox* count = nullptr;
    QSpinBox* speed = nullptr;
    QSpinBox* post = nullptr;
    QSpinBox* mix = nullptr;
    QSpinBox* stretch = nullptr;
    QComboBox* aa = nullptr;
    std::string tip_png;
    // 紙質 (BRUSH-01): the paper under the lines, its picture (taken in: paper_asset, the grey picture's ref) and settings,
    // and a patch of fully inked paper as the brush lays it (paper_preview)
    QCheckBox* paper_on = nullptr;
    QPushButton* paper_picture = nullptr;
    QLabel* paper_preview = nullptr;
    QLabel* paper_about = nullptr;
    QSpinBox* paper_density = nullptr;
    QSpinBox* paper_scale = nullptr;
    QSpinBox* paper_rotation = nullptr;
    QCheckBox* paper_flip_x = nullptr;
    QCheckBox* paper_flip_y = nullptr;
    QCheckBox* paper_invert = nullptr;
    QComboBox* paper_blend = nullptr;
    QComboBox* paper_coords = nullptr;
    QComboBox* paper_seam = nullptr;
    QSpinBox* paper_seed = nullptr;
    std::string paper_asset;
    // The paper as set (its picture: paper_asset). A number the person did not change keeps the brush's own value
    // (the boxes hold whole percent and degrees; a brush made through an op may have finer ones).
    core::Paper paper() const;
    // Take a picture file in as the paper (true when it could be; otherwise why not, in Japanese: paper_error).
    bool take_paper(const QString& path);
    QString paper_error;

private:
    void pick_tip();
    void pick_paper();
    void show_paper();
    std::string base_;
    core::Paper started_paper_;  // the base's paper as the dialog opened (paper())
};

}  // namespace genko::app
