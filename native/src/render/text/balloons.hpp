#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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

// Balloons (fukidashi@1, SPEC BALLOON-01): each line's shape (speech, rounded, box, cloud, thought, shout, electric,
// flash, whisper, narration, capsule, polygon and the 飾り枠 frames; a hand-drawn outline, smoothed or not) as an outline
// of real pixels with its tails (wedge, straight, zigzag, fade, bubbles, bent through points, inward), joined balloons
// as one union, the line what lies within its width of the union's edge (a double line, even dashes, dots, corner marks,
// spokes, a trembling outline), cut where the balloon eraser went, filled; a turned balloon drawn upright and laid
// turned; a picture balloon; then the letters (render/text/lettering.hpp). Every balloon is drawn clean (the geometry:
// render/text/balloon_clean.hpp): the hand-drawn look is gone, in books of before too.
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
struct LetterParts;
struct SetLayout;

// draw_lines(image, lines, dpi, font_path, show_speaker, panels): every placed line of a page, joined balloons
// (style.group) drawn together in the reading order of their first line. `parts` (PSD-01, the editable PSD): only
// these of each line (render::text::LetterParts; none: all of it).
void draw_lines(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi,
                const std::optional<std::string>& font_path = std::nullopt, bool show_speaker = true,
                const Panels* panels = nullptr, std::stop_token stop = {}, const Unported* unported = nullptr,
                const LetterParts* parts = nullptr);
// draw_group(image, lines, dpi, show_speaker, font_path, panels): one balloon (or several joined ones) with their
// tails and text. (speakers: the names' font shared by the groups drawn on the same part)
void draw_group(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, bool show_speaker = true,
                const std::optional<std::string>& font_path = std::nullopt, const Panels* panels = nullptr,
                std::stop_token stop = {}, const Unported* unported = nullptr, SpeakerFonts* speakers = nullptr,
                const LetterParts* parts = nullptr);

// PSD-01 (internal, not an op): a placed line drawn alone (as render::page_layers draws each) as the editable PSD lays
// it: its lettering as set (render::text::set_layout, its picture with nothing drawn) and the map from the set
// letters' pixels to the page's — where paint_text puts the lettering and, for a line turned by rotate_deg, how its
// sheet is turned and laid (x' = a·x + c·y + e, y' = b·x + d·y + f).
struct EditableLine {
    std::shared_ptr<const SetLayout> lettering;
    std::array<double, 6> to_page{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
};
EditableLine editable_line(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt);

// The balloons' pictures remembered for drawing a page again in parts (the letters, the shapes' masks, a turned
// balloon's sheet) are forgotten.
void clear_balloon_cache();
// (PC5 H-f, メモリをクリア) The bytes they hold now (the letters' too: layout_cache_bytes).
std::int64_t balloon_cache_bytes();

// What the drawing of a balloon reads of a line, for the tests.
namespace balloon {

using Pt = std::array<double, 2>;

// How far a line's old style.wobble (0..1) makes its outline tremble, as a share of its box's narrow side: the wave
// that stands for the hand-drawn wobble of before (style.wave_mm says it in mm).
inline constexpr double kWobbleWave = 0.03;

// tails_of(line): its tails ({"to", "via", "vias", "width_mm", "kind", "fillet_mm"}) with a target, or its old single tail.
std::vector<core::Json> tails_of(const core::StoryLine& line);
// _turned(point, centre, degrees): turned about `centre` (clockwise on the page).
Pt turned(Pt point, Pt centre, double degrees);
// _thought_trail(line, panels): where a thought's bubbles trail to when nobody is named.
Pt thought_trail(const core::StoryLine& line, const Panels* panels);

}  // namespace balloon

}  // namespace genko::render::text
