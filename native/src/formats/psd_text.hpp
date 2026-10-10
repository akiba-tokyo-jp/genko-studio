#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PSD-01: the editable PSD (not Python's: the improvement I05). The PSD export has two clearly separate ways with the
// lines' text: as pictures (見た目優先で画像化 — Python's PSD, byte for byte, and the default) or kept editable
// (文字を編集可能なレイヤーとして保持): each placed line a group of its own (「台詞 …」) holding its balloon as a picture
// (「フキダシ」), its text as a Photoshop text layer (「本文 #<line id>」: the characters as set, a line for each column or
// row, each character's font, size, weight, slant and colour, the spacing, the alignment, where it lies and how it is
// moved, turned, narrowed or leant, an outline as the layer's stroke) and each ruby as a text layer of its own
// (「ルビ「<reading>」 #<line id>-r<n>」, n its place in the line's rubies, `.k` for each part of a reading broken over
// columns or rows). A ruby is not tied to its text in other apps: changing the text there does not move it.
//
// What PSD text cannot keep (the table below, docs/PSD_TEXT_SUPPORT.md) is said before anything is written: the strict
// way stops the export (PsdTextRefused); only when the person chooses to (`rasterize`) is such a line drawn as a
// picture, as the image way draws it. Fonts are named, never put in the file: a font the line asks for that is not
// there (the default used instead) and a character drawn in another font than the line's are warned about.
// Nothing about this is kept in the book.

namespace genko::formats {

struct PsdText {
    bool editable = false;   // false: the lines as pictures (Python's PSD)
    bool rasterize = false;  // (editable) a line using what PSD text cannot keep is drawn as a picture instead of stopping
};

// What the first editable PSD does not keep as text (each with its key, its name and what becomes of it).
struct PsdTextFeature {
    std::string_view key;
    std::string_view label;
    std::string_view impact;
};
const std::vector<PsdTextFeature>& psd_text_unsupported();

// A placed line using some of them: its page, id, its first words and the features' keys.
struct PsdTextIssue {
    std::int64_t page = 0;
    std::string line;
    std::string text;
    std::vector<std::string> features;
};
// Every placed line of the book's pages that the editable PSD cannot keep as text, page by page, at `dpi`.
std::vector<PsdTextIssue> psd_text_issues(const core::Document& episode, std::int64_t dpi);
core::Json issues_json(const std::vector<PsdTextIssue>& issues);

// The fonts the book's placed lines are set in (their PostScript names, as the PSD names them) and what about them is
// warned of: {"page", "line", "kind": "font_missing" (the style's font not found: the default used) | "font_fallback"
// (characters drawn in another font than the line's own), "detail"}.
struct PsdTextFonts {
    std::vector<std::string> fonts;
    std::vector<core::Json> warnings;
};
PsdTextFonts psd_text_fonts(const core::Document& episode);

// The strict refusal: core::Error("psd_text_unsupported"), "the editable PSD stops: N line(s) use what PSD text cannot
// keep (page P 「words」: feature, …)".
class PsdTextRefused : public core::Error {
public:
    explicit PsdTextRefused(std::vector<PsdTextIssue> issues);
    const std::vector<PsdTextIssue>& issues() const noexcept { return issues_; }

private:
    std::vector<PsdTextIssue> issues_;
};

// What an editable export did: the text layers and ruby layers written, the lines drawn as pictures, the fonts named
// and the warnings.
struct PsdTextReport {
    std::int64_t text_layers = 0;
    std::int64_t ruby_layers = 0;
    std::vector<PsdTextIssue> rasterized;
    std::vector<std::string> fonts;
    std::vector<core::Json> warnings;
    core::Json to_json() const;
};

}  // namespace genko::formats
