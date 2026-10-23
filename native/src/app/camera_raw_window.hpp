#pragma once

#include <QImage>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "app/filter_menu.hpp"
#include "core/json.hpp"

// PC4 節 D5 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → Camera Raw フィルター…'s window — Adobe Camera Raw's: on the
// left the picture (what shows of the layer, the frame the filter works) with the result laid on it a moment after a change
// (worked on a thread at the window's resolution with the op's own kernel: render/fx_camera_raw), 補正前 (Y) showing it as it
// was; on the right the tabs 基本補正・トーンカーブ・ディテール・カラーミキサー・カラーグレーディング・効果 with Camera Raw's sliders
// in its words, 初期化 setting every one back. OK sends the settings (filter_raster's camera_raw; on a smart object a smart
// filter: one step to undo), キャンセル nothing.
//
// The panel (CameraRawPanel) is a part of its own, so 節 F's RAW development (F-e: a camera RAW file opened through Camera
// Raw's window) shows it with a picture of its own (Picture: floats at the window's resolution).

namespace genko::app::camera_raw_window {

// A picture the panel works: straight RGBA floats, a pixel's colours as a page holds them (display-encoded; past 1 kept
// when `floats`: a 32-bit page's, a RAW file's), at the window's resolution — how many of its pixels a pixel of the working
// 200 dpi spans (`s`: Camera Raw's lengths, ディテール's radii and 粒子's cells), where its (0, 0) lies on a grid of its pixels
// over the page (`x0`, `y0`: 粒子's place, as the op's) and where it lies on the page (`box_mm`: x0, y0, x1, y1). Its frame is
// what shows in it (its pixels not clear).
struct Picture {
    int width = 0, height = 0;
    std::vector<float> rgba;
    bool floats = false;
    double s = 1;
    int x0 = 0, y0 = 0;
    std::array<double, 4> box_mm{0, 0, 1, 1};
};

// The picture as it is, and with camera_raw's settings (their normal form) worked on it as the op's kernel works a layer
// (its frame what shows in it; on a thread of its own: it stops with the thread's StopScope, render::Cancelled).
QImage shown(const Picture& picture);
QImage developed(const Picture& picture, const core::Json& settings);

// Camera Raw's tabs and preview. Its settings (camera_raw's normal form) start at their defaults; `changed` is called when the
// person changes one. The preview is worked out on a thread 150 ms after the last change (the last one asked for wins); its
// properties "previews" (how many shown) and "previewing" say so (tests).
class CameraRawPanel : public QWidget {
public:
    explicit CameraRawPanel(QWidget* parent = nullptr);
    ~CameraRawPanel() override;
    CameraRawPanel(const CameraRawPanel&) = delete;
    CameraRawPanel& operator=(const CameraRawPanel&) = delete;

    // The picture to work (none: 読み込んでいます… until it is given).
    void set_picture(std::shared_ptr<const Picture> picture);
    // The settings shown (any of camera_raw's: read by its table; a key not given at its default), and as the panel holds
    // them: camera_raw's normal form (core::OpError for what its table refuses).
    void set_settings(const core::Json& settings);
    core::Json settings() const;
    // 初期化: every setting back to its default.
    void reset();
    std::function<void()> changed;

private:
    struct Parts;
    std::unique_ptr<Parts> parts_;
};

// The window from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open(const FilterWindowCall& call);

}  // namespace genko::app::camera_raw_window
