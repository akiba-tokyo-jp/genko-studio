#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"

// The effect words a name plan's panels may carry (fx), and what each becomes (Python's genko/studio/fxwords.py):
// "effect" (lines Genko draws over the art at the finish), "mark" (a 漫符 stamped by the first reported face),
// "rain" (rain streaks at the finish) or "art" (drawn by the image tool: the words go into the art request).

namespace genko::service::fxwords {

using core::Json;

// fxwords.KNOWN: the Japanese words, sorted (for the agent).
const std::vector<std::string>& known();

// fxwords.resolve(word): {kind, key, en} for a known word (str(word).strip().lower()), nothing for an unknown one.
std::optional<Json> resolve(const Json& word);

// fxwords.art_words(words): (Japanese, English) for the art request: every word of the panel, known or not.
std::pair<std::vector<std::string>, std::vector<std::string>> art_words(const Json& words);

// fxwords.emphasis_words(emphasis): the panel's weight in the story (0..1) as words for the art request; nothing for
// an ordinary panel (or a value that is not a number).
std::optional<std::pair<std::string, std::string>> emphasis_words(const Json& emphasis);

}  // namespace genko::service::fxwords
