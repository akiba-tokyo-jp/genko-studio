#pragma once

#include <string>

#include "core/json.hpp"

// The studio's own data files (Python's genko/studio/layouts.json, vocab.json and guide/*.md), embedded as they are.

namespace genko::service::data {

// layout.templates(): the page layout templates, parsed once ({name: {description, tiers, bleed?, slant?, title?}}).
const core::Json& layouts();
// genreq.vocab(): the words of the art requests (shots, angles, …), parsed once.
const core::Json& vocab();
// guide/manga_rules.md and guide/drawing.md, as text.
const std::string& rules();
const std::string& drawing();
// guide/SKILL.md, as text (the MCP resource genko://guide/skill).
const std::string& skill();

}  // namespace genko::service::data
