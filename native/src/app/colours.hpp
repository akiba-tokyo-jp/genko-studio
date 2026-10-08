#pragma once

#include <QPushButton>
#include <QWidget>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLineEdit;
class QSpinBox;
class QTabWidget;
class QVBoxLayout;

// カラー: the colour panel (Python's genko/app/colours.py, J4) — a square of saturation and brightness with a hue bar,
// RGB and hex, the main and sub colours (X swaps them) and the transparent colour, colour sets, the colours used
// lately, colours between four chosen ones (中間色) and colours near the current one (近似色).

namespace genko::app {

class BrushPanel;

using Rgb = std::array<int, 3>;

namespace colours {

inline constexpr int kHistory = 24;

using ColourSet = std::pair<std::string, std::vector<Rgb>>;

// Python's colorsys (the same sums, so the same colours come out).
std::array<double, 3> rgb_to_hsv(double r, double g, double b);
std::array<double, 3> hsv_to_rgb(double h, double s, double v);

// マンガのグレー, 基本の色, 肌・髪.
const std::vector<ColourSet>& built_in_sets();
bool is_built_in(const std::string& name);
// The built-in sets and the person's own (the settings folder's colorsets.json), in that order (one's own set of a
// built-in name takes the built-in's place). A file that cannot be read adds nothing; a set that cannot be read
// stops the reading there, as in Python.
std::vector<ColourSet> load_sets(const std::filesystem::path& config_dir);
// One set kept (or, nullopt, forgotten) in colorsets.json, the others there left as they are. Throws core::Error.
void save_set(const std::filesystem::path& config_dir, const std::string& name, const std::optional<std::vector<Rgb>>& colours);

Rgb mix(const Rgb& a, const Rgb& b, double t);
// 中間色: an n×n grid mixed from four corners (top-left, top-right, bottom-left, bottom-right).
std::vector<std::vector<Rgb>> between(const std::array<Rgb, 4>& corners, int n = 5);
// 近似色: rows of the colour with its hue turned a little, and lighter / darker, more / less vivid.
std::vector<std::vector<Rgb>> nearby(const Rgb& rgb, int n = 5);  // (not "near": a macro on Windows)

}  // namespace colours

// A colour to click (22 px, 2 px apart: a 24 px target pitch, WCAG 2.5.8).
class Swatch : public QPushButton {
    Q_OBJECT
public:
    explicit Swatch(const Rgb& rgb = {0, 0, 0}, int size = 22, QWidget* parent = nullptr);
    void set_rgb(const Rgb& rgb);
    const Rgb& rgb() const { return rgb_; }

signals:
    void picked(const genko::app::Rgb& rgb);

private:
    Rgb rgb_{0, 0, 0};
};

// Saturation across, brightness up and down, for the hue chosen on the bar.
class SVSquare : public QWidget {
    Q_OBJECT
public:
    explicit SVSquare(QWidget* parent = nullptr);
    double hue = 0.0, sat = 1.0, val = 1.0;
    // A point picked (the widget's coordinates).
    void pick(QPointF pos);

signals:
    void changed(double sat, double val);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
};

class HueBar : public QWidget {
    Q_OBJECT
public:
    explicit HueBar(QWidget* parent = nullptr);
    double hue = 0.0;
    void pick(QPointF pos);

signals:
    void changed(double hue);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
};

// The pen's colour, chosen every way a painter chooses one. The colour itself is the brush panel's (brush/rgb).
class ColourPanel : public QWidget {
    Q_OBJECT
public:
    ColourPanel(BrushPanel* brush, QWidget* parent = nullptr);

    Rgb rgb() const;
    const Rgb& sub_rgb() const { return sub_rgb_; }
    void choose(const Rgb& rgb);
    void show_colour(const Rgb& rgb);
    // メインとサブを入れ替える (X).
    void swap();
    // A colour just used goes to the front of 履歴.
    void remember(const Rgb& rgb);
    // The current colour added to the chosen set (a built-in set: to one's own copy of it).
    void add_to_set();
    // A new set of the current colour (no name: asked).
    void new_set(const std::optional<QString>& name = std::nullopt);
    // The current colour as a corner of 中間色 (0 左上, 1 右上, 2 左下, 3 右下).
    void set_corner(int i);

    const std::vector<Rgb>& history() const { return history_; }
    const std::array<Rgb, 4>& corners() const { return corners_; }
    const std::vector<colours::ColourSet>& sets() const { return sets_; }

    // The fields (the window and the tests use them as Python's attributes).
    Swatch* main = nullptr;
    Swatch* sub = nullptr;
    QPushButton* swap_button = nullptr;
    QCheckBox* transparent = nullptr;
    SVSquare* square = nullptr;
    HueBar* bar = nullptr;
    std::array<QSpinBox*, 3> spins{};
    QLineEdit* hex = nullptr;
    QTabWidget* tabs = nullptr;
    QComboBox* set_choice = nullptr;
    QWidget* set_box = nullptr;
    QWidget* history_page = nullptr;
    QWidget* between_page = nullptr;
    QWidget* near_page = nullptr;
    QComboBox* pick_source = nullptr;

signals:
    // スポイト: where the picker takes its colour ("view" | "layer").
    void pick_source_chosen(const QString& source);

private:
    void from_square();
    void from_hue(double h);
    void from_numbers();
    void from_hex();
    void fill_sets();
    void fill_set();
    void fill_history();
    void fill_between();
    void fill_near(const Rgb& rgb);
    // A set kept in colorsets.json (a warning when it cannot be).
    bool keep_set(const std::string& name);

    BrushPanel* brush_ = nullptr;
    bool loading_ = false;
    Rgb sub_rgb_{255, 255, 255};
    std::vector<Rgb> history_;
    std::array<Rgb, 4> corners_{};
    std::vector<colours::ColourSet> sets_;
    QGridLayout* set_grid_ = nullptr;
    QGridLayout* history_grid_ = nullptr;
    QVBoxLayout* between_layout_ = nullptr;
    QVBoxLayout* near_layout_ = nullptr;
};

}  // namespace genko::app
