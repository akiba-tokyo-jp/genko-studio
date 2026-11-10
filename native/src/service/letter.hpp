#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "service/blocking.hpp"
#include "service/layout.hpp"
#include "studio/schemas.hpp"

// Lettering v1 (Python's genko/studio/letter.py): size vertical balloons and place them inside their panels. Sizes
// follow the renderer (render._draw_balloon): the em is capped at 5 mm, text columns run right to left and the
// balloon adds a quarter-em pad. The result becomes add_line ops with explicit coordinates, so a replay never depends
// on fonts.

namespace genko::service::letter {

using core::Json;

inline constexpr double kEmMm = 5.0;
inline constexpr double kPadMm = kEmMm / 4;
inline constexpr double kMarginMm = 2.0;
inline constexpr double kGapMm = 1.0;
inline constexpr double kStepMm = 1.5;
inline constexpr double kTailMm = 7.0;
inline constexpr double kTailRoomMm = 3.0;
inline constexpr double kTailMaxMm = 30.0;
inline constexpr double kOverMm = 6.0;
inline constexpr double kLeading = 0.4;
inline constexpr double kSfxEmMm = 12.0;
inline constexpr double kAsideTilt = 6.0;
inline constexpr double kTagEmMm = 3.2;
inline constexpr double kTitleMaxMm = 18.0;

struct BalloonPlacement {
    std::string slot;
    std::int64_t line_index = 0;
    Json beat_id = "";
    std::string text;
    std::string balloon;
    std::string speaker;
    double x_mm = 0;
    double y_mm = 0;
    double w_mm = 0;
    double h_mm = 0;
    std::optional<std::array<double, 2>> tail;
    std::string speaker_id;
    std::optional<Json> style;
    std::string wrap = "vertical";
};

// letter.looks(bible): each kind of text's font, size (times the usual) and weight: the defaults, then the bible's.
Json looks(const Json& bible);
// letter.look_style(look, balloon): the line style that carries a look (size_mm only when the size changes).
Json look_style(const Json& look, const std::string& balloon);
// letter.measure(breaks, balloon, em): the balloon's box (w, h) as the renderer draws it.
std::array<double, 2> measure(const std::vector<std::string>& breaks, const std::string& balloon, double em = kEmMm);
// letter.fits(rect, balloon, others, em): how much text the panel takes in one balloon of this kind, in words.
std::string fits(const Box& rect, const std::string& balloon, std::size_t others = 0, double em = kEmMm);
// letter.place_page(plan, layout, bible, speakers): the balloons of every panel (and the title, the 名札) and the
// issues; `speakers`: beat id → the speaking character's id (from the script).
std::pair<std::vector<BalloonPlacement>, std::vector<studio::Issue>> place_page(const Json& plan, const layout::CompiledLayout& compiled,
                                                                                 const Json& bible, const Json& speakers);
// letter.placements_to_ops(page_index, placements, layout): the add_line ops.
Json placements_to_ops(const Json& page_index, const std::vector<BalloonPlacement>& placements, const layout::CompiledLayout& compiled);
// letter.tail_to(box, head, balloon): where a balloon's tail ends (toward the speaker's mouth, short of the face).
std::optional<std::array<double, 2>> tail_to(const Box& box, const Box& head, const std::string& balloon = "speech");

// The geometry the finish shares (letter._find_spot, _gap, _overlap): where a balloon of `size` goes in a panel.
struct Room {
    std::optional<std::array<double, 4>> page;  // the panels' outer edge (x0, y0, x1, y1)
    std::vector<Box> neighbours;
    const std::vector<Box>* taken = nullptr;  // the balloons placed on the page so far (Python's list, shared)
    std::size_t taken_count = static_cast<std::size_t>(-1);  // (only the first so many of them: a slice)
    bool allows(const Box& box) const;
};
std::optional<std::pair<Box, bool>> find_spot(const Box& rect, std::array<double, 2> size, const std::vector<Box>& placed,
                                              const std::vector<Figure>& figs, const Json& speaker_id = Json(),
                                              const std::optional<std::array<double, 2>>& near = std::nullopt, const Room* room = nullptr);
double gap(const Box& a, const Box& b);
double overlap(const Box& a, const Box& b);

// balloons.ellipse_around(points, power): the smallest upright oval holding every point (width, height, dx, dy), as
// numpy computes it.
std::array<double, 4> ellipse_around(const std::vector<std::array<double, 2>>& points, double power);

}  // namespace genko::service::letter
