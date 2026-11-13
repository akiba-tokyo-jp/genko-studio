#pragma once

#include <cstdint>
#include <string_view>

#include "core/model.hpp"

// fukidashi@1 (SPEC.md BALLOON-01, docs/cpp-migration/schema-v4.md「フキダシ」): the balloons drawn clean. Since 2026-11-13
// every balloon is drawn by one renderer (render/text/balloon_clean) — true ellipses, even lines, tails that run out
// of the outline — and the hand-drawn look (the egg-shaped oval, the pen's swelling line, the wobble) is gone, in
// books of before too. What this header holds:
//
//   - the feature: the book names fukidashi@1 while a line (or a type layer's record) uses what a build of before
//     cannot draw at all — the kinds capsule and polygon, the style keys corner_mm, squircle, sides, wave_mm,
//     spike_alt and dash_mm, a tail of kind inward or with fillet_mm;
//   - the geometry's generation (project.json's "balloon_geometry", a plain key a build of before keeps as it is):
//     a book without it was lettered for the hand-drawn oval — 0.955 of its box, fuller than an ellipse — so, read,
//     each such oval's box is made 1.035 times as big about its middle (the true ellipse of that box holds the oval
//     the letters were fitted in, and the letters are still fitted in that oval: they are set exactly as they were).
//     Once, in memory; the book says 2 from its next save.

namespace genko::core {

inline constexpr std::string_view kFukidashiFeature = "fukidashi@1";

// The generation of the balloons' geometry this build draws (1: the hand-drawn ovals of before, never written).
inline constexpr std::int64_t kBalloonGeometry = 2;
// How much bigger than its box the true ellipse is that holds the hand-drawn oval of that box (the superellipse of
// power 2.6 at 0.955 of the box reaches 2^(1/2 − 1/2.6) × 0.955 = 1.0345 of the ellipse's diagonal).
inline constexpr double kOvalGrowth = 1.035;
// The share of an oval's box the letters' oval takes: the letters are kept in the oval they always were (the
// superellipse of power 2.6 — render/text/lettering's hug_power), 0.955 of the box before it grew, so of the box now
// 0.955 / kOvalGrowth. The letters of a book of before are set exactly as they were; a balloon of now is sized so that
// its true ellipse holds that oval.
inline constexpr double kOvalRoom = 0.955 / kOvalGrowth;

// Whether a line's balloon is one of the ovals (speech, thought, whisper).
bool is_oval(const StoryLine& line);
// Whether a line of a book of before was drawn as the hand-drawn oval: an oval kind, not hand: false, no wobble (a
// balloon drawn by hand — a path — keeps its box: its letters are fitted as before, lettering.cpp).
bool legacy_oval(const StoryLine& line);
// A line of a book of before read: a hand-drawn oval's box made kOvalGrowth times as big about its middle (to 3
// decimals of a mm). True when it was.
bool grow_legacy_oval(StoryLine& line);

// Whether a line uses what names the feature.
bool uses_fukidashi(const StoryLine& line);
// Whether a line or a type layer's record of a page the book writes does.
bool uses_fukidashi(const Document& doc);
// Whether the book has an oval (a line's or a type layer's record's): it then says its balloons' generation.
bool has_ovals(const Document& doc);

}  // namespace genko::core
