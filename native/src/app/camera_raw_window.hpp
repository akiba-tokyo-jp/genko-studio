#pragma once

#include <QImage>
#include <QWidget>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/filter_menu.hpp"
#include "core/json.hpp"

// PC4 節 D5 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → Camera Raw フィルター…'s window — Adobe Camera Raw's: on the
// left the picture (what shows of the layer, the frame the filter works) with the result laid on it a moment after a change
// (worked on a thread at the window's resolution with the op's own kernel: render/fx_camera_raw), 補正前 (Y) showing it as it
// was; on the right the tabs 基本補正・トーンカーブ・ディテール・カラーミキサー・カラーグレーディング・効果 with Camera Raw's sliders
// in its words, 初期化 setting every one back. OK sends the settings (filter_raster's camera_raw; on a smart object a smart
// filter: one step to undo) — what the person did not touch as it came —, キャンセル nothing.
//
// The panel (CameraRawPanel) is a part of its own, so 節 F's RAW development (F-e: a camera RAW file opened through Camera
// Raw's window) shows it with a picture of its own (Picture: floats at the window's resolution, how they are shown), puts the
// RAW's own controls at the top of 基本補正 (add_to_basic) and hides the relative ones it replaces (set_hidden).

namespace genko::app::camera_raw_window {

// A picture the panel works: width × height pixels of straight RGBA floats (`rgba`: 4 a pixel), a pixel's colours as a page
// holds them (past 1 kept when `floats`: a 32-bit page's, a RAW file's), at the window's resolution — how many of its pixels
// a pixel of the working 200 dpi spans (`s` > 0: Camera Raw's lengths, ディテール's radii and 粒子's cells), where its (0, 0)
// lies on a grid of its pixels over the page (`x0`, `y0`: 粒子's place, as the op's) and where it lies on the page (`box_mm`:
// x0, y0, x1, y1). Its frame is what shows in it (its pixels not clear). Its colours are shown on the screen as sRGB's
// display values, or through `to_screen` (none: they are sRGB's; F-e's development in another space — linear ProPhoto —
// gives its own: the RGBA of `pixels` pixels in place, on the window's and the preview's threads).
struct Picture {
    int width = 0, height = 0;
    std::vector<float> rgba;
    bool floats = false;
    double s = 1;
    int x0 = 0, y0 = 0;
    std::array<double, 4> box_mm{0, 0, 1, 1};
    std::function<void(float* rgba, std::size_t pixels)> to_screen;
};
// Is it one the panel can work (a positive size, width × height × 4 values, a finite positive scale)?
bool well_formed(const Picture& picture);

// The picture as it is, and with camera_raw's settings (their normal form) worked on it as the op's kernel works a layer
// (its frame what shows in it; on a thread of its own: it stops with the thread's StopScope, render::Cancelled), as the
// screen shows them. An empty image for a picture not well formed.
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

    // The picture to work (none — or one not well formed: it says so —: 読み込んでいます… until one is given).
    void set_picture(std::shared_ptr<const Picture> picture);
    // The settings it starts from (any of camera_raw's: read by its table; a key not given at its default, ones it refuses at
    // the defaults), and as it holds them: camera_raw's normal form of those, each setting the person touched as its field
    // shows it (one not touched as it came: its fields' rounding never changes it) — core::OpError for what the table refuses.
    void set_settings(const core::Json& settings);
    core::Json settings() const;
    // 初期化: every setting back to its default (粒子's seed kept).
    void reset();
    // (F-e) A widget of its own at the top of 基本補正 (the RAW's white balance: 撮影時の設定・自動・カスタム, K), and the rows of
    // settings hidden or shown again by their keys (temperature and tint, which the RAW's replace; their values kept).
    void add_to_basic(QWidget* widget);
    void set_hidden(const std::vector<std::string>& keys, bool hidden = true);
    std::function<void()> changed;

private:
    struct Parts;
    std::unique_ptr<Parts> parts_;
};

// The window from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows). From the
// command it uses the command's seed (FilterWindowCall::seed: a new grain each time it is applied, its preview's the same)
// and leaves it to the command; a smart filter set again keeps its own.
void open(const FilterWindowCall& call);

}  // namespace genko::app::camera_raw_window
