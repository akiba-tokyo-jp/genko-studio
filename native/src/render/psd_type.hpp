#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/psd_text.hpp"

// PC5 G-e (type@1, SPEC.md TEXT-02「PSD・.pcraft の文字」, design section G 2.8–2.9): Photoshop's text layer — a PSD's TySh
// block as render/psd_text reads and writes it, and PhotoCraft's TextLayer of a .pcraft (render/pcraft_read,
// formats/pcraft_write) put in the same terms — and Genko's type layer (its record, core/type_text.hpp) each other's.
// After PhotoCraft v0.2.0's text/psd.rs and psd_styles.rs (the keys and their meaning, MIT), in Genko's own model: lengths
// in mm, ranges in graphemes, the attributes resolved in core::type_text's order — only what differs from what the styles
// and defaults give written as a text's own.
//
// Reading: the text (its UTF-16 as code points, a lone surrogate U+FFFD, a paragraph break "\r" and Photoshop's forced
// line break U+0003 a line break, another control character a space), its style runs and paragraph runs as ranges and
// paragraphs of graphemes, the fonts by their PostScript names (a bundled font's name found: that font; else the name kept
// — `postscript` — and the default drawn, said), point or paragraph (box) text, across or down, the matrix, the warp
// (style.native_warp), the anti-alias and the named sheets as the book's styles. What Genko's typesetting cannot hold — a
// text past type_ops::kMaxTypeText graphemes, a matrix of no area, a place past a metre from the page, attributes no record
// takes — leaves the layer as its pixels, and why is said.

namespace genko::render::psd_type {

// Where the file's pixels lie on the page: a pixel (x, y) at (x0_mm + x · mm_x, y0_mm + y · mm_y).
struct Frame {
    double mm_x = 25.4 / 72.0;
    double mm_y = 25.4 / 72.0;
    double x0_mm = 0.0;
    double y0_mm = 0.0;
};

// EngineData's keys of a run's style (every one) or of a paragraph's properties, as a NamedSheet's data holds them (Font
// by its PostScript name, FillColor as RGB or {"cmyk"}).
core::Json engine_keys(const psd::TextStyle& style);
core::Json engine_keys(const psd::ParagraphRun& paragraph);
// The character (paragraph) attributes those keys give (only the keys there), lengths `mm_per_px` a pixel, each kept within
// its bounds: Font a bundled font's name (bundled_font) or `postscript` (the name added to `missing` once).
core::Json char_attrs(const core::Json& keys, double mm_per_px, std::vector<std::string>* missing = nullptr);
core::Json para_attrs(const core::Json& keys, double mm_per_px, bool vertical);
// The other way: EngineData's keys of the attributes given (only those), lengths `px_per_mm`; a font named by `font_name`
// (a font file Genko draws with → its PostScript name). A key no attribute gives is not written.
core::Json char_keys(const core::Json& attrs, double px_per_mm, const std::function<std::string(const std::string&)>& font_name);
core::Json para_keys(const core::Json& attrs, double px_per_mm, bool vertical);

// The bundled font (its name: "gothic", "mincho", …) whose PostScript name this is; none for another.
std::optional<std::string> bundled_font(std::string_view postscript);

// Photoshop's anti-alias (the TxLr descriptor's AntA value; EngineData's AntiAlias where that one is not known) as
// native_type.antialias, and back (AntA, EngineData's AntiAlias).
std::string antialias_of(std::string_view anta, int engine);
std::pair<std::string, int> anta_of(std::string_view antialias);

// The file's named sheets taken into the book (one import: a sheet met again — the same name and keys — is the same
// style): its index among a text layer's sheets → the book's style id.
struct Sheets {
    std::map<std::string, std::string> taken;  // (kind, name and keys → the book's id)
    std::vector<std::string> added;            // (the ids made, in order)
    bool full = false;                         // (a kind past its 512: not taken, said)
    bool full_char = false, full_para = false; // (which: the rest of that kind not worked out — the review's P3-6)
};

// What a text layer became: its record (none: the layer is read as its pixels, `why` in words) and what is said of it.
struct Read {
    std::optional<core::StoryLine> record;
    std::string why;
    std::vector<std::string> missing_fonts;  // (the PostScript names not found, each once)
    std::vector<std::string> notes;
};
// A text layer of the file as a type layer's record on the page `frame` lays the file on: its sheets taken into
// `doc.type_styles` (core::type_styles' ids and names: a name trimmed, made NFC and of printable characters, unique among
// the kind's). The point text's box is not fitted round its letters (the caller's: type_ops::fit_point_text).
Read read_text(const psd::TextLayer& text, const Frame& frame, core::Document& doc, Sheets& sheets);
// The named sheets of a text layer taken into the book alone (a .pcraft's styles that no text layer read), each one's id
// (none: past the kind's 512) when asked. core::Error for a sheet whose attributes are not a style's.
void take_sheets(const psd::TextLayer& text, const Frame& frame, core::Document& doc, Sheets& sheets,
                 std::vector<std::optional<std::string>>* char_ids = nullptr, std::vector<std::optional<std::string>>* para_ids = nullptr,
                 std::vector<std::string>* missing = nullptr);

// The text layer of a record (the other way, for a writer): its text, runs and paragraphs resolved, its shape, matrix and
// anti-alias, the book's styles as its named sheets (all of them, each run naming the one it takes), lengths `px_per_mm` a
// pixel of a page `origin_mm` at the page's top left. `font_name`: a font file → its PostScript name. Fonts of a composite
// face (アンチック: kana in Mincho) named letter by letter. None when the record has no shape (point or box): it is set as a
// line is (PSD-01's way).
std::optional<psd::TextLayer> text_of(const core::StoryLine& record, const core::Document& book, double px_per_mm,
                                      const std::function<std::string(const std::string&)>& font_name);

// PC5 G-e (design G 2.9): PhotoCraft's TextLayer of a .pcraft (its manifest's layer content of kind "text": text, runs and
// paragraphs by UTF-8 bytes, CharStyle and ParagraphStyle in points, shape, orientation, antialias, warp, transform) and its
// document's TextStyles as a text layer in the terms above — each run's and paragraph's Genko attributes given as they
// are (TextStyle::attrs), its styles the named sheets (their ids by their places) —, and the other way. `dpi`: the
// document's (points to pixels). None for content that is not one (nothing to read: as its pixels).
std::optional<psd::TextLayer> from_pcraft(const core::Json& content, const core::Json* text_styles);
// The content of a text layer (without its cache and psd_raw) of a type layer's text layer (text_of's) at `dpi`; `family`:
// a font file → its family and style names (PhotoCraft finds a face by them).
core::Json to_pcraft(const psd::TextLayer& text, double dpi, const std::function<std::pair<std::string, std::string>(const std::string&)>& family);
// The book's styles as PhotoCraft's TextStyles (ids from 1 in the order of each kind; Basic Paragraph its basic), and the
// map from the book's ids to them (to_pcraft names a run's sheet by its place: the same order).
core::Json pcraft_text_styles(const core::Document& book);

// (tests) While one lives, import_psd reads a PSD's text layers as their pixels, as a build before G-e did (a book opened
// and saved then).
class PixelsForTest {
public:
    PixelsForTest();
    ~PixelsForTest();
    PixelsForTest(const PixelsForTest&) = delete;
    PixelsForTest& operator=(const PixelsForTest&) = delete;
};
bool read_as_pixels();

// What a type layer read from a PSD is written back as while it is the same (psd-keep@1's text): its record, at this dpi
// on a page of these pixels — "sha256:…".
std::string fingerprint(const core::StoryLine& record, std::int64_t dpi, std::int64_t width_px, std::int64_t height_px);
// The same with the styles the record takes in `book` (Basic Paragraph and those it names, as they are: a style redefined
// since, its look is not the block's) and the colour mode of its page (an RGB document's FillColor is not a CMYK one's:
// the review's P3-3) — what psd-keep@1's `text` holds.
std::string fingerprint(const core::StoryLine& record, const core::Document& book, const core::Page& page, std::int64_t dpi, std::int64_t width_px,
                        std::int64_t height_px);

}  // namespace genko::render::psd_type
