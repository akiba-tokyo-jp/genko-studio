#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// smart-objects@1 (docs/cpp-migration/SPEC.md「スマートオブジェクト」, schema-v4.md「スマートオブジェクト」, PC3 A0): the
// model's rules for smart objects — a layer of kind "smart" that shows contents of the book (Document::smart_contents:
// a page of its own, or a picture file kept as its bytes) through its own matrix, never changing them. Here are the
// contents' JSON, the layer's settings, their checks, the matrix, which contents a book uses and the stand-in a smart
// object draws exactly as. The drawing is render/smart.hpp; the ops are render/ops_smart.cpp.
//
// A layer's "smart" (in this order): {"contents": "sc_…", "matrix": [a, b, c, d, e, f], "interp": …}, its stack mode (PC3
// A4: "stack", one of kStackModes), its smart filters (PC3 A2: "filters", "filters_on", "filter_mask") and what later steps
// of PC3 add to it — "perspective" (the contents' four corners on the page), "warp", "psd" — which this build keeps as they
// are and does not draw yet (NotYetPorted names them: render/smart.cpp).

namespace genko::core {

inline constexpr std::string_view kSmartObjectsFeature = "smart-objects@1";

namespace smart {

inline constexpr int kDepthMax = 8;          // contents inside contents, at most
inline constexpr double kDetMin = 1e-9;      // mm²: a matrix squashing the contents flatter than this is refused
inline constexpr std::size_t kNameMax = 255;  // characters of a contents' name
inline constexpr std::int64_t kSideMax = 1 << 20;  // pixels of a file's side

// The interpolations a smart object is drawn with (a picture's default lanczos — as a placed picture is —, a page's
// bicubic) and its stack modes (PC3 A4: the contents' top layers combined pixel by pixel).
inline constexpr std::array<std::string_view, 4> kInterps{"lanczos", "bicubic", "bilinear", "nearest"};
inline constexpr std::array<std::string_view, 11> kStackModes{"entropy", "kurtosis", "maximum", "mean", "median", "minimum",
                                                              "range", "skewness", "standard_deviation", "summation", "variance"};

// The files a smart object's contents can be: their suffix (as kept in assets/) and format.
struct FileFormat {
    std::string_view suffix;
    std::string_view format;
};
inline constexpr std::array<FileFormat, 8> kFileFormats{{{".png", "png"},
                                                         {".jpg", "jpeg"},
                                                         {".tif", "tiff"},
                                                         {".webp", "webp"},
                                                         {".bmp", "bmp"},
                                                         {".gif", "gif"},
                                                         {".psd", "psd"},
                                                         {".psb", "psd"}}};
// The suffix a file's name gives it (its extension in lower case; .jpeg as .jpg, .tiff as .tif), or none for a kind of
// file a smart object cannot hold.
std::optional<std::string> suffix_of_name(std::string_view name);
// The format of a suffix (kFileFormats), or none.
std::optional<std::string> format_of_suffix(std::string_view suffix);

// A smart object's matrix: its contents' mm → the page's mm (x' = a x + c y + e, y' = b x + d y + f: transform_area's
// "matrix" in the same order).
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    std::pair<double, double> apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
    double det() const { return a * d - b * c; }
    bool operator==(const Matrix&) const = default;
};
// `later` after `first`.
Matrix multiply(const Matrix& later, const Matrix& first);
std::optional<Matrix> inverse(const Matrix& m);
// Each value within 1e-9 of a whole multiple of 1e-9 taken to it (PhotoCraft's snap_affine: a transform undone gives the
// matrix it had back exactly, and a picture's box the place it was given).
Matrix snapped(const Matrix& m);
bool is_identity(const Matrix& m);
// Only scaled and moved (no turn, no shear, no mirror).
bool axis_aligned(const Matrix& m);
// How much it enlarges at most (its larger singular value).
double largest_scale(const Matrix& m);
Json to_json(const Matrix& m);
// Six finite numbers (core::Error("value") otherwise).
Matrix matrix_from(const Json& value);

// "sc_" and 12 hex digits.
std::string new_contents_id();
bool is_contents_id(std::string_view text);

// Smart filters (PC3 A2, PhotoCraft's smart filters): a smart object's "filters" — [{"kind", "params", "blend", "opacity",
// "visible"}, …], the bottom one first: a filter_raster kind and its settings, laid over what is below it in one of the 20
// blend modes at an opacity of 0..1 —, "filters_on" (none: on) and "filter_mask" ({"asset": "sha256:…", "enabled"}: a
// PNG "L" of the page at 150 dpi, as a layer's mask, through which the filtered picture shows over the unfiltered one).
inline constexpr std::size_t kFiltersMax = 64;
inline constexpr std::array<std::string_view, 5> kFilterKeys{"kind", "params", "blend", "opacity", "visible"};
// Can a filter_raster kind be a smart filter (SO に掛けられる)? Python's filters (core::kAdjustments, kShapeFilters: what
// they make is their settings' and the layer's pixels' — noise and rain by their seed) and the C++ build's own kinds
// registered so (AdjustKind::smart). Never a plugin (a person's program: what it makes is the machine's).
bool filter_kind(std::string_view kind);
// Are the smart object's filters drawn: on, and one of them shown?
bool filters_drawn(const Layer& layer);

// A layer of kind smart, its contents' id (nothing when its settings name none), its matrix (the identity when none).
bool is_smart(const Layer& layer);
std::optional<std::string> contents_id(const Layer& layer);
Matrix matrix_of(const Layer& layer);
void set_matrix(Layer& layer, const Matrix& m);
// A smart object's setting put in its place among the others (the order the book keeps them in: contents, matrix,
// perspective, warp, interp, stack, the smart filters, psd), or (null) taken out (PC3 A4: its stack mode).
void put_setting(Layer& layer, std::string_view key, Json value);
// A new smart object's settings: {"contents", "matrix", "interp"}.
Json settings(const std::string& contents, const Matrix& m, std::string_view interp);
// The contents a smart object layer shows (null when none).
const SmartContents* contents_of(const Document& doc, const Layer& layer);
// The contents' own size in mm: a page's paper, a file's pixels at its dpi.
std::pair<double, double> size_mm(const SmartContents& contents);

// The JSON of a file's contents ({"kind": "file", "name", "asset", "suffix", "format", "size_px", "dpi", "depth", "mode",
// "profile", "link"?, "psd"?}) and back, checked (core::Error("value")); a page's contents ({"kind": "page", "name",
// "spec", "page"}) are written and read by storage with the page's own functions — here only their name is read.
Json file_json(const SmartContents& contents);
SmartContents contents_from_json(const Json& value);

// PC3 A1: a linked file's record (SmartContents::link; the contents are its last copy): {"path": where it is (absolute,
// as the machine names it), "relative"?: from the book's folder ("/" between names), "sha256": its bytes' when last read
// (64 lowercase hex digits), "bytes": its size}, in this order. Paths of at most kPathMax bytes. core::Error("value").
inline constexpr std::size_t kPathMax = 4096;
void check_link(const Json& link);
Json link_json(const std::string& path, const std::optional<std::string>& relative, const std::string& sha256, std::int64_t bytes);

// Where a book shows contents: the first smart object layer showing them, on a page of the book (`inside` none) or on the
// page of other contents (`inside` their id: an op reaches that layer with "contents"); none when nothing shows them.
struct Showing {
    std::optional<std::string> inside;
    Num page;
    std::string layer;
};
std::optional<Showing> showing(const Document& doc, const std::string& id);

// The contents the book's pages show (directly or inside other contents), in id order: what the writer writes.
std::vector<std::string> used_contents(const Document& doc);
// Does the book hold a smart object (a layer of kind smart, or contents)? It then names smart-objects@1.
bool uses_smart(const Document& doc);
// The positions of the book's pages that show these contents (directly or inside other contents).
std::vector<std::size_t> pages_using(const Document& doc, const std::string& id);
// The ids of the contents a page shows directly (its smart object layers').
std::set<std::string> shown_on(const Page& page);
// Contents changed (PC3 A1): every other contents showing them — directly or further in — made a new value too
// (Document::edit_contents), so that what is drawn of those, remembered by their identity, is drawn again.
void renew_containers(Document& doc, const std::string& id);

// The book's smart objects as they must be (core::Error("value") says what is not): a smart layer's settings (known
// keys, its contents held, a finite matrix that does not squash them flat, convex corners, a known interpolation and
// stack mode, its smart filters — at most kFiltersMax, each of a kind filter_kind takes, its settings read as that kind
// reads them where this build's core reads them, a known blend, an opacity of 0..1 — and their mask), no pixels, lines or fill of its own on it (an op that drew on a smart object is
// refused so: rasterize it first), the contents (a page of index 1; a file of a known format, its size, depth, mode and
// colour use), and contents inside contents a tree no deeper than kDepthMax. Pages not read yet are not looked at.
void validate_smart(const Document& doc);

// The layer a smart object draws exactly as, when there is one — contents of one layer (not a folder or correction),
// normal and not clipped, on a page of the same paper, panels and colour model, shown untouched (the identity, no smart
// filter drawn, nothing of PC3's later steps on it), and nothing of the smart object's own drawn on it (mask, effect, screen, colour): its
// contents' layer with the smart object's id, title, folder, visibility, opacity, blend and clipping on it (the layer it
// was made of, as it was, until either changes). Nothing otherwise.
std::optional<Layer> plain_layer(const Document* doc, const Page& page, const Layer& layer);
// The page with each smart object that draws exactly as a layer of its own (plain_layer) as that layer; nothing when it
// has none. What every drawing of a page draws (render/page.cpp): the same pixels as the layer was before.
std::optional<Page> with_plain_layers(const Page& page, const Document* doc);

// A page of another book coming into `doc` (import_pages, a layer pasted from another book): the contents its smart objects
// show (and those inside them) and the files ops gave them come along, each under an id `doc` does not hold yet (the page's
// smart objects then name it). The files of `from`'s assets are the caller's to copy first (storage::copy_assets).
void bring_contents(Document& doc, const Document& from, Page& page);

// The same panels as a page's drawing takes them (their boxes, shapes, clipping and borders, and the panels in them).
bool same_panels(const std::vector<Frame>& a, const std::vector<Frame>& b);

// A smart object moved with its page (a page-wide transform: the paper's size, a turn): its matrix after `m` (page mm →
// page mm), and its corners when it has them.
void transform_layer(Layer& layer, const Matrix& m);

}  // namespace smart

}  // namespace genko::core
