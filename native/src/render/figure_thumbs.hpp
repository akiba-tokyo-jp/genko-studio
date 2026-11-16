#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <array>
#include <vector>

#include "core/json.hpp"

// Small pictures of the pose library's poses (SPEC FIGURE-02, G4): the pose gallery's icons (ポーズ集…), the contact
// sheets the figure_poses tool and `genko studio figure-poses --sheet` give, and the sheets test_figure_poses writes for
// review. A pose is put on a figure (core::pose_checks::figure_of, with the body given: the selected figure's in the
// gallery), turned about its upright axis (正面 0, 斜め 0.785, 横 1.571), drawn with the core raster (no camera: the
// figure's own perspective) at `supersample`× and box-averaged in integers: the same bytes on every run and thread.
// The frame is 13 u tall with the floor near the foot (as G3's thumbnails), the figure centred across; a body wider or
// taller than that (lying, arms spread, a pair) is drawn smaller so it fits, and one lower than 0.4 of it (lying down)
// larger, as large as it fits but at most twice (G5).

namespace genko::render::figure_thumbs {

using core::Json;

inline constexpr double kFront = 0.0;
inline constexpr double kThreeQuarter = 0.785;
inline constexpr double kSide = 1.571;

// Where the picture is on the page (the figure's pos 0): its top left (mm) and its pixels per mm, and the floor's y.
struct Frame {
    double left = 0;
    double top = 0;
    double px_per_mm = 1;
    double floor = 0;
    double unit = 0;  // (u, mm)
};

// The prim a pose makes (pose_figure's joints and hands, and what the hands hold), with `body` (null: the usual one) and
// the height in mm; a pair pose's two (the first at 0, the second where it stands from it: G5).
Json figure_for(const Json& pose, const Json& body = Json(), double height_mm = 120.0);
std::vector<Json> figures_for(const Json& pose, const Json& body = Json(), double height_mm = 120.0);

// The figure `prim` (or figures, turned together about the first one's upright axis) seen turned `turn`: its frame for
// a picture of width × height.
Frame frame_of(const Json& prim, double turn, int width, int height);
Frame frame_of(const std::vector<Json>& prims, double turn, int width, int height);

// A grey picture of `prim` (or of the figures, as frame_of turns them) (one byte a pixel, 255 where there is no
// surface), width × height.
std::string grey(const Json& prim, double turn, int width, int height, int supersample = 2, Frame* frame = nullptr);
std::string grey(const std::vector<Json>& prims, double turn, int width, int height, int supersample = 2, Frame* frame = nullptr);

// The pictures of poses, remembered by (pose id with ~m, view, the body's digest, size): what the gallery and the tool
// draw again and again. Safe from many threads; at most `capacity` kept (the oldest let go first).
class Cache {
public:
    explicit Cache(std::size_t capacity = 2048);
    // The picture of pose `id` (as pose_library::resolve takes it: "<id>" or "<id>~m") on `body`; empty when there is no
    // such pose.
    std::string get(std::string_view id, double turn, const Json& body, int width, int height, int supersample = 2);
    std::size_t size() const;
    void clear();

private:
    struct Entry {
        std::string key;
        std::string grey;
    };
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::vector<Entry> entries_;  // (oldest first)
};

// The body's digest: what of the body changes its picture (the key of the cache).
std::string body_digest(const Json& body);

// A contact sheet of poses (RGB PNG bytes): one row per pose, a cell per turn, `cell_w` × `cell_h` each, a thin
// border round each cell (no words: the tool's reply says the poses in the sheet's order).
std::string sheet_png(const std::vector<Json>& poses, const std::vector<double>& turns, const Json& body, int cell_w = 120, int cell_h = 180,
                      int columns = 0);

}  // namespace genko::render::figure_thumbs
