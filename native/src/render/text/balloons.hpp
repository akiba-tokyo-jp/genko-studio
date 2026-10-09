#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"

// Balloons (Python's genko/balloons.py, the balloon side): each line's shape (speech, rounded, box, cloud, thought,
// shout, electric, flash, whisper, narration and the 飾り枠 frames; drawn by hand: uneven or wobbled; a hand-drawn
// outline, smoothed or not) drawn as a mask with its tails (wedge, straight, zigzag, fade, bubbles, bent through
// points), joined balloons as one mask, the outline its inner edge (a pen's swelling line, a double line, dashes,
// dots, corner marks, radiating lines), cut where the balloon eraser went, filled; a turned balloon drawn upright and
// laid turned; a picture balloon; then the letters (render/text/lettering.hpp). The same pixels as Python's.
//
// What is drawn on is a part of a page (a render region): the shapes are worked out for the whole balloon and only
// the part is painted, so the pixels are those of the whole page cut there.

namespace genko::render::text {

// The part of a page an image holds: where its top left lies on the page, and the page's size (Python's image.width
// and image.height, which bound a balloon's working area).
struct PagePart {
    Image* image = nullptr;
    Point origin;
    Size page;
};
inline PagePart whole_page(Image& image) { return PagePart{&image, Point{0, 0}, image.size()}; }

// {frame id: (x, y, w, h) mm} of a page's panels (render_page's `panels`), in the order the dict was made (a later
// frame with the same id wins, as in a dict).
using Panels = std::vector<std::pair<std::string, std::array<double, 4>>>;

// Called with what a group could not draw (render::NotYetPorted); rethrows it when what is not ported must stop the
// drawing. Without one, it is rethrown. Letters this build cannot draw are left out line by line (the balloon and the
// group's other lines drawn): a font it does not read, and OpenType features across, where Pillow without raqm (the
// BASIC layout the reference is held to) raises KeyError — "text_features" here.
using Unported = std::function<void(const NotYetPorted&)>;

class SpeakerFonts;

// draw_lines(image, lines, dpi, font_path, show_speaker, panels): every placed line of a page, joined balloons
// (style.group) drawn together in the reading order of their first line.
void draw_lines(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi,
                const std::optional<std::string>& font_path = std::nullopt, bool show_speaker = true,
                const Panels* panels = nullptr, std::stop_token stop = {}, const Unported* unported = nullptr);
// draw_group(image, lines, dpi, show_speaker, font_path, panels): one balloon (or several joined ones) with their
// tails and text. (speakers: the names' font shared by the groups drawn on the same part)
void draw_group(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, bool show_speaker = true,
                const std::optional<std::string>& font_path = std::nullopt, const Panels* panels = nullptr,
                std::stop_token stop = {}, const Unported* unported = nullptr, SpeakerFonts* speakers = nullptr);

// The balloons' pictures remembered for drawing a page again in parts (the letters, the shapes' masks, a turned
// balloon's sheet) are forgotten.
void clear_balloon_cache();

// The geometry of balloons.py, for the contract tests (points as Python's (x, y) floats).
namespace balloon {

using Pt = std::array<double, 2>;
using Box4 = std::array<double, 4>;

// tails_of(line): its tails ({"to", "via", "vias", "width_mm", "kind"}) with a target, or its old single tail.
std::vector<core::Json> tails_of(const core::StoryLine& line);
// _ellipse_point(cx, cy, rx, ry, t)
Pt ellipse_point(double cx, double cy, double rx, double ry, double t);
// _wobbly(points, amount, size, seed): pushed in and out smoothly (a balloon drawn by hand).
std::vector<Pt> wobbly(const std::vector<Pt>& points, double amount, double size, std::string_view seed);
// _uneven(box, seed, n): a letterer's ellipse, a little egg-shaped, inside its box.
std::vector<Pt> uneven(const Box4& box, std::string_view seed, int n = 120);
// _outline(kind, box, n): an ellipse's or a box's outline as points, or nothing for the other shapes.
std::optional<std::vector<Pt>> outline(std::string_view kind, const Box4& box, int n = 96);
// _electric(box, st): the 電子音 edge.
std::vector<Pt> electric(const Box4& box, const core::Json& st);
// _edge_point(kind, box, toward, spread): two points on the edge, `spread` apart, facing `toward`.
std::pair<Pt, Pt> edge_point(std::string_view kind, const Box4& box, Pt toward, double spread);
// _smooth_closed(points, per): a closed Catmull–Rom curve through the points.
std::vector<Pt> smooth_closed(const std::vector<Pt>& points, int per = 6);
// _polyline_tail(kind, box, tip, vias, base): a tail bent at each of `vias`.
std::vector<Pt> polyline_tail(std::string_view kind, const Box4& box, Pt tip, const std::vector<Pt>& vias, double base);
// _tail_polygon(kind, box, tip, via, base, style)
std::vector<Pt> tail_polygon(std::string_view kind, const Box4& box, Pt tip, const std::optional<Pt>& via, double base,
                             std::string_view style = "wedge");
// _turned(point, centre, degrees): turned about `centre` (clockwise on the page).
Pt turned(Pt point, Pt centre, double degrees);
// _thought_trail(line, panels): where a thought's bubbles trail to when nobody is named.
Pt thought_trail(const core::StoryLine& line, const Panels* panels);

}  // namespace balloon

}  // namespace genko::render::text
