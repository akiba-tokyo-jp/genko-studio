#pragma once

#include <QPointF>
#include <QWidget>

#include <array>
#include <cstdint>
#include <optional>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

class QComboBox;
class QLabel;
class QTimer;

// ヒストグラム パネルと情報パネル (PC2d: window.panel.histogram, window.panel.info; PhotoCraft v0.2.0's
// ui-egui/src/tone.rs:213-275 and panels.rs:831-889, MIT). Display only: they never change the book.
//
// ヒストグラム: the page as it is drawn (proof, at a small resolution: a picture of 384 pixels or so across, as upstream's
// cache) or the layer drawn on (its own pixels at their own size: a paint layer's raster, precise or of inks as the
// screen shows them; else its picture drawn), each pixel that is not wholly transparent counted in 256 levels of RGB (the
// mean of the three), red, green, blue and luminosity (0.299 R + 0.587 G + 0.114 B), with the mean, the standard
// deviation, the median and the pixels counted. Followed a moment after the page changes, while the panel is shown.
//
// 情報: under the pointer on the page, its place (mm, and pixels at the book's resolution), the colour the screen shows
// there (sRGB), the page's values there (render::pixel_at, as the status bar and `genko inspect --pixel` give them),
// the colour in the proof profile's values (色の校正's: its CMYK ink, or its own channels), and the selection's size;
// (PC5 J-d1) the ものさしツール's line: its angle, lengths, size and start (Photoshop's A・L1・L2・W・H・X・Y).

namespace genko::app {

class MainWindow;

// What a histogram counts: its channels (RGB, red, green, blue, luminosity), 256 levels each, and the pixels counted.
struct Histogram {
    static constexpr int kRgb = 0, kRed = 1, kGreen = 2, kBlue = 3, kLuminosity = 4;
    std::array<std::array<std::uint64_t, 256>, 5> bins{};
    std::uint64_t pixels = 0;
};
// The histogram of a picture ("RGBA", "RGB", "L"…): every pixel whose alpha is not 0.
Histogram histogram_of(const render::Image& picture);
// A channel's numbers: the mean and the standard deviation of its levels, the median level, the pixels counted.
struct HistogramStats {
    double mean = 0;
    double deviation = 0;
    int median = 0;
    std::uint64_t pixels = 0;
};
HistogramStats stats_of(const std::array<std::uint64_t, 256>& bins);

class HistogramPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistogramPanel(MainWindow* window);
    // Made again now from the page or the layer as they are (the panel shown or not: tests and the window).
    void refresh();
    const Histogram& histogram() const { return histogram_; }
    // 0 the page, 1 the layer drawn on; the channel shown: 0 RGB, 1 red, 2 green, 3 blue, 4 luminosity, 5 colours (red,
    // green and blue together; its numbers RGB's).
    QComboBox* source() const { return source_; }
    QComboBox* channel() const { return channel_; }
    // The numbers shown (tests): mean, deviation, median, pixels.
    QLabel* figure(int i) const { return figures_[static_cast<std::size_t>(i)]; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void follow();  // (made again when what it counts has changed)
    void show_figures();

    MainWindow* window_;
    QComboBox* source_ = nullptr;
    QComboBox* channel_ = nullptr;
    QWidget* graph_ = nullptr;
    std::array<QLabel*, 4> figures_{};
    QTimer* watch_ = nullptr;
    Histogram histogram_;
    const void* counted_ = nullptr;  // (the page or layer counted last, by its block, and the choice: what made it)
    int counted_source_ = -1;
    friend class HistogramGraph;
};

class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(MainWindow* window);
    // The pointer at a place on the page (mm; none: off it).
    void show_at(std::optional<QPointF> mm);
    // The selection's size again.
    void show_selection();
    // (PC5 J-d1) The ものさしツール's readout (core::to_json(RulerReadout); none: no line).
    void show_ruler(const std::optional<core::Json>& readout);
    QLabel* ruler() const { return ruler_; }
    // What is shown (tests): the place, the screen's colour, the page's values, the proof's values, the selection.
    QLabel* place() const { return place_; }
    QLabel* screen() const { return screen_; }
    QLabel* values() const { return values_; }
    QLabel* proof() const { return proof_; }
    QLabel* selection() const { return selection_; }
    // pixel_at's answer for the place shown last (none: nothing under the pointer).
    const std::optional<core::Json>& at() const { return at_; }

private:
    MainWindow* window_;
    QLabel* place_ = nullptr;
    QLabel* screen_ = nullptr;
    QLabel* values_ = nullptr;
    QLabel* proof_ = nullptr;
    QLabel* selection_ = nullptr;
    QLabel* ruler_ = nullptr;  // (PC5 J-d1)
    std::optional<core::Json> at_;
};

}  // namespace genko::app
