#pragma once

#include <filesystem>
#include <string>

// review.html for the person who approves (Python's genko/studio/review.py: review_html): the book's open requests,
// the proposals read from hand-drawn names, the character sheets, every page (a name with its reading-order badges,
// else the proof) with its candidates, and what stops the final export. A page to look at: the approving is done in
// the app (or with the commands folded away in it). Pictures are embedded as PNG (base64).

namespace genko::service::review {

// review.review_html(project, max_px=…)
std::string review_html(const std::filesystem::path& project, int max_px = 900);

}  // namespace genko::service::review
