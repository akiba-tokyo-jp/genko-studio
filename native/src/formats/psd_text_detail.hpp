#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PSD-01 inside formats/ (psd_text.cpp and psd_export.cpp): the pieces both use.

namespace genko::render::text {
class Fonts;
}

namespace genko::formats::detail {

// A line render::page_layers draws (a place on the page or a balloon).
bool placed_line(const core::StoryLine& line);
// The first `count` characters of a line's text without its line breaks (its layer's name, its issue's words).
std::string words_of(const std::string& text, std::size_t count);
// What a line uses that PSD text cannot keep (psd_text_unsupported's keys, in its order). With `dpi`, also a line of
// Python's (ruby_runs) whose rubies, held by place as the editable PSD sets them, would set it otherwise than its own
// lettering does ("ruby_runs": e.g. a reading wider than its text, which Python's way cuts and by place widens the
// lettering for).
std::vector<std::string> line_features(const core::StoryLine& line, std::optional<std::int64_t> dpi = std::nullopt,
                                       const std::optional<std::string>& font_path = std::nullopt);
// The line as the editable PSD sets it: a line of Python's holding its ruby_runs by place where the explicit rule finds
// them (core::ruby::from_runs), as render::text::ruby_places tells them apart.
core::StoryLine by_place(const core::StoryLine& line);
// A PostScript name as the PSD names a font: only A–Z, a–z, 0–9 and . _ + - kept (the text engine's tokens).
std::string font_token(std::string_view name);
// The PostScript name of a font file Fonts opens (a bundled font's name or a file's path), read from its name table
// (its file name without the extension when it has none).
std::string font_name(render::text::Fonts& fonts, const std::string& path);
// The fonts a line's letters and rubies are drawn in (their PostScript names, added to `used` once each) and what is
// warned of about them (`warnings`: font_missing, font_fallback).
void font_warnings(const core::StoryLine& line, std::int64_t page, const std::optional<std::string>& font_path, render::text::Fonts& fonts,
                   std::vector<std::string>* used, std::vector<core::Json>& warnings);

}  // namespace genko::formats::detail
