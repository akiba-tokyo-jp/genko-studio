#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/color_model.hpp"
#include "core/geometry.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"
#include "core/vector.hpp"

// The manuscript model (docs/cpp-migration/ARCHITECTURE.md §2): Python's genko/models.py in C++.
//
// A Document is a value. Its pages are shared between copies (copy-on-write through Document::edit_page), the
// strokes of a layer are an immutable list shared between copies of the layer, and picture bytes are immutable
// shared strings. Fields Python keeps as it read them are Nums (an int or a float) or Json values, so a book is
// written back exactly as it was read; fields Python converts on reading (float(), bool(), str()) have the C++
// type of the result.

namespace genko::core {

enum class Binding { Right, Left };
enum class LayerRole { Name, Draft, Ink, Bg, Finish, Tone, Effect, Frames, Text, User };
enum class LayerKind { Raster, Strokes, Fill, Tone, Folder, Placed, Adjust, Smart };

std::string_view to_string(Binding binding);
std::string_view to_string(LayerRole role);
std::string_view to_string(LayerKind kind);
std::optional<Binding> binding_from(std::string_view text);
std::optional<LayerRole> layer_role_from(std::string_view text);
std::optional<LayerKind> layer_kind_from(std::string_view text);

using NumList = std::vector<Num>;

// One page's paper, its finished size (仕上がり, centred on the paper), the bleed around it (裁ち落とし) and the
// basic frame inside it (基本枠). Old books had no trim or margins: their trim is the paper less the bleed all
// round and their margins are inner_margin_mm on every side.
struct PageSpec {
    Num width_mm;
    Num height_mm;
    Num dpi;
    Num bleed_mm;
    Num inner_margin_mm;
    std::string expression = "mono";
    std::optional<std::string> preset;
    std::optional<Num> trim_w_mm;
    std::optional<Num> trim_h_mm;
    std::optional<NumList> margins_mm;  // top, bottom, inner (のど), outer (小口)
    // PC3 (page-spec@1, core/vector.hpp): where the trim's top left is on a page of its own paper whose trim is not in the
    // paper's middle (none: in the middle)
    std::optional<std::pair<double, double>> trim_origin_mm;

    struct Margins {
        double top = 0, bottom = 0, inner = 0, outer = 0;
    };

    std::pair<Num, Num> trim_size() const;
    std::pair<double, double> trim_origin() const;
    // Throws core::Error("format") when margins_mm does not hold four numbers (Python fails there too).
    Margins margins() const;
    std::pair<double, double> frame_size() const;
    // "用紙 257×364 mm ・ 仕上がり 220×310 mm ・ 裁ち落とし 5 mm ・ 基本枠 180×270 mm ・ 600 dpi"
    std::string describe() const;

    // The usual sizes (publishers and printers differ, so every number can be changed).
    static PageSpec b4_comic();
    static PageSpec b5_doujin();
    static PageSpec a5_doujin();
    static PageSpec a4_mono();
    static PageSpec webtoon();
    // Throws core::Error("value") with Python's message when the sizes do not fit.
    static PageSpec custom(double paper_w, double paper_h, double trim_w, double trim_h, double bleed, double top,
                           double bottom, double inner, double outer, std::int64_t dpi = 600,
                           std::string expression = "mono");
    static PageSpec publisher(std::string_view name);
};

struct PaperPreset {
    std::string_view key;
    std::string_view label;
    PageSpec (*make)();
};

// PAPER_PRESETS: b4, b5, a5, a4, webtoon (in this order).
std::span<const PaperPreset> paper_presets();
const PaperPreset* find_paper_preset(std::string_view key);

struct Stroke {
    std::string id;
    std::vector<PointF> points;
    std::vector<double> pressure;
    double width_mm = 0.35;
    std::string kind = "gpen";
    std::optional<std::vector<std::int64_t>> rgb;  // none: the layer's ink colour
    std::optional<Json> color_rgb;  // native.color_stroke_v1: precise straight sRGB, not a preview
    double opacity = 1.0;
    std::vector<double> rotation;  // the pen's barrel turn at each point (degrees; アートペン)
    double pressure_opacity = 0.0;  // 0..1: how much a light touch also lightens the line
    // brushes@1 (PC4, core/brush_dabs.hpp): the pen's inputs at each point a dab brush reads — its tilt (x and y,
    // degrees, ±90: two values a point), the time (ms from the first point) and the airbrush wheel (0..1) — and the
    // flow the window drew it with (none: the brush's). Empty: none (every line before PC4).
    std::vector<double> tilt;
    std::vector<double> time_ms;
    std::vector<double> wheel;
    std::optional<double> flow;
};

using StrokePtr = std::shared_ptr<const Stroke>;

// The strokes of a layer. Never changed once made: an edit makes a new list (sharing the strokes that stay).
struct StrokeList {
    std::vector<StrokePtr> items;
    // The .strokes.json asset this list was read from ("" for a list made in memory): saving the same list again
    // reuses it (Python's blobcache).
    std::string blob_ref;
};

using StrokeListPtr = std::shared_ptr<const StrokeList>;

// The shared empty list (a layer's strokes are never null).
const StrokeListPtr& empty_strokes();
StrokeListPtr make_strokes(std::vector<StrokePtr> items, std::string blob_ref = {});

using Bytes = std::shared_ptr<const std::string>;

// The asset ref of some picture bytes, remembered while those same bytes are kept, so an unchanged picture is not
// hashed again on every save.
struct BlobMemo {
    std::string ref;
    std::weak_ptr<const std::string> of;

    const std::string* ref_for(const Bytes& bytes) const;
    void remember(const Bytes& bytes, std::string ref_value);
};

// A layer mask: an L image over the whole page (white shows, black hides).
struct Mask {
    Bytes png;
    bool enabled = true;
    BlobMemo memo;
    // masks@1 (PC3, core/masks.hpp): moves with the layer's pixels (none: fixed to the page, as every mask before it);
    // its density (0..1: the mask's value m drawn as 1 − density·(1 − m)); its feather (mm: a Gaussian of σ = feather/2)
    bool linked = false;
    double density = 1.0;
    double feather_mm = 0.0;
};

// A fill or pasted picture kept at its own resolution over a box: {"id", "box", "mode", "rgb", "opacity", …}.
struct Patch {
    Json attrs = Json::object();  // its keys in order, without "png" and "asset"
    Bytes png;                    // null: the picture was missing (the patch is not written back, as in Python)
    std::optional<std::string> asset;  // the ref it was read from
    BlobMemo memo;
    // Keys set after the patch was read that it did not have (duplicate_layer's new "id"): Python's dict keeps them
    // after "asset", which it added when it read the patch, and writes them there.
    Json after_asset = Json::object();
};

struct Layer {
    std::string id;
    LayerRole role = LayerRole::User;
    LayerKind kind = LayerKind::Strokes;
    bool visible = true;
    bool exportable = true;
    StrokeListPtr strokes = empty_strokes();
    std::optional<std::string> raster_relpath;
    std::optional<NumList> fill_rgb;
    Bytes raster_png;
    BlobMemo raster_memo;
    std::optional<Num> lpi;
    std::optional<Num> density;
    std::optional<std::vector<Point>> region;
    double opacity = 1.0;
    std::optional<std::string> material_id;
    double angle = 45.0;
    std::string title;
    std::string blend = "normal";
    bool clip = false;
    bool lock_alpha = false;
    bool locked = false;       // nothing can be drawn on or erased from a locked layer
    bool panel_clip = true;    // lines stay inside the panels; false lets them run out (はみ出し)
    bool panel_each = false;   // each line stays in the panel it begins in
    std::vector<Patch> patches;
    std::optional<Json> tone;  // tone layers: {pattern, gradient}
    // The folder it is in (null: none). As Python keeps it: a str when read (the reader repairs other values), and
    // set_layer's "parent" as it is given.
    Json parent_id = nullptr;
    std::optional<std::string> asset;     // placed: "sha256:…" in assets/
    std::optional<std::string> frame_id;  // placed: the panel it belongs to
    std::optional<Rect> placement_mm;     // placed: where the whole image lands on the page
    std::string fit = "cover";            // cover | contain | stretch
    std::string clip_to = "frame";        // frame | bleed | none
    std::optional<Json> source;           // placed: {"candidate": …, "request": …}
    std::optional<Json> finish;           // placed: mono finishing override
    std::optional<Mask> mask;
    std::optional<std::vector<std::int64_t>> color;  // shown in this colour on screen; never printed
    bool reference = false;
    std::optional<Json> fill;
    std::optional<Json> adjust;
    std::optional<Json> effect;
    Bytes color_raster;  // immutable native RGBA16/32 asset; never an 8-bit preview
    // color-mode@1: the pixels of a paint layer on a CMYK or Lab page (GKIR, core/ink.hpp), in the page's space and
    // depth, stretched over the page as raster_png is. A layer has at most one of raster_png, color_raster, ink_raster.
    Bytes ink_raster;
    BlobMemo ink_memo;
    bool color_prints = false;
    std::optional<Json> screen;
    // smart-objects@1 (core/smart.hpp): a smart object's own settings — {"contents", "matrix", "interp", …} — on a layer
    // of kind "smart" (which holds no pixels, lines or fill of its own; frame_id, clip_to, source and finish as a placed
    // picture's).
    std::optional<Json> smart;
    // PC3 (pen-shapes@1, core/vector.hpp): a pen layer's shape parts (決定 7; never null: the shared empty list)
    vector::ShapeListPtr shapes = vector::empty_shapes();
    // PC3 (vector@1): its vector mask
    std::optional<vector::VectorMask> vector_mask;
    // PC3 (transform@1): its link group (the layers of one link move together) and its position lock
    std::optional<std::string> link;
    bool lock_position = false;

    std::size_t stroke_count() const { return strokes ? strokes->items.size() : 0; }
};

struct Frame {
    std::string id;
    Rect rect;
    std::vector<Frame> children;
    std::optional<std::string> split_axis;
    bool clip = true;
    bool bleed = false;
    double border_mm = 0.8;
    std::optional<Json> panel;              // leaf only: the panel brief, candidates and adoption
    std::optional<std::vector<Point>> poly;  // a slanted or free-form panel: its corners; rect is their box
    std::optional<Json> split;              // a split node's cut: {"a", "b"} in its box's 0..1 coordinates, "gutter_mm"
    bool custom = false;                    // a person shaped this panel by hand
    std::optional<std::vector<double>> curves;  // how far each edge bows out (mm, outward +)
    std::optional<Json> line;               // the border's look
    double corner_mm = 0.0;                 // 角の丸み
};

// One ruby of a line held by its place in the text (ruby-layout@1, core/ruby.hpp): the reading over the text's
// extended grapheme clusters [start, end). `base` is the text of that range when the ruby was set. A ruby that needs
// review (要確認: an edit reached into its base, or where it went cannot be told) keeps the base and reading it had, its
// range only where it was last seen; it is not drawn, and `review` says why ("edited", "ambiguous" or "unplaced";
// empty: drawn). Each ruby has its own settings (none given: the line's look, Python's; core/ruby.hpp has their bounds):
// its face, its size against the text (scale), where it sits over its base (align), its distance from the text
// (gap_mm) and the space between its letters (tracking). An edit carries them with the ruby.
struct RubySpan {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string ruby;
    std::string base;
    std::string review;
    std::optional<std::string> font;   // a bundled face's key (gothic, mincho, …) or a font file's path
    std::optional<double> scale;       // 0.25..0.8 of the text's size
    std::optional<std::string> align;  // head | center | tail
    std::optional<double> gap_mm;      // -1..10 mm
    std::optional<double> tracking;    // -0.5..2 of the ruby's size

    bool operator==(const RubySpan&) const = default;
};

struct StoryLine {
    std::string id;
    Num page_index;
    std::string text;
    std::string speaker;
    std::optional<std::string> frame_id;
    std::string ruby;
    Num x_mm = 0;
    Num y_mm = 0;
    Num w_mm = 40;
    Num h_mm = 20;
    std::string balloon = "speech";
    std::optional<Point> tail;
    std::string wrap = "horizontal";
    std::vector<Json> ruby_runs;               // each a list (Python's tuple(item)), e.g. ["東京", "とうきょう"]
    // ruby-layout@1: the rubies by place, in the order of their starts (none: the line holds its ruby as Python's
    // ruby_runs, found by its words). With spans, ruby_runs is their mirror: [base, reading] of each drawn one.
    std::optional<std::vector<RubySpan>> ruby_spans;
    std::optional<std::vector<Point>> path;    // a balloon drawn by hand: its outline
    std::vector<std::string> emphasis_runs;    // 傍点
    std::vector<std::pair<std::string, Json>> style_runs;  // [[words, {scale, bold, rgb}]]
    Json style = Json::object();
    std::vector<Json> tails;
};

// channels@1 (core/channels.hpp, docs/cpp-migration/schema-v4.md「チャンネル」): a page's alpha or spot channel — one plane
// of density (0: not selected, no ink; 1: selected, full ink) over the whole page, kept as a GKIR Mask raster at the
// page's depth (null: all 0), with how it shows (an alpha channel's overlay colour, opacity and what it marks; a spot
// channel's ink colour and solidity).
enum class ChannelKind { Alpha, Spot };

struct Channel {
    std::string id;    // "ch_" and 12 hex digits, one per page
    std::string name;  // 1..64 characters, one per page
    ChannelKind kind = ChannelKind::Alpha;
    Bytes raster;      // GKIR Mask (null: all 0)
    BlobMemo memo;
    std::array<int, 3> color{255, 0, 0};  // alpha: the overlay's colour; spot: the ink as it shows (sRGB)
    double opacity = 0.5;                 // alpha: 0..1
    std::string indicates = "masked";     // alpha: masked | selected
    double solidity = 0.0;                // spot: 0..1
    bool quick_mask = false;              // masks@1 (PC3): the page's Quick Mask (an alpha channel; one at most)
};

struct Bible {
    std::string plot;
    Json characters = Json::array();
    Json constraints = Json::array();
};

struct Page {
    Num index;
    PageSpec spec;
    std::vector<Frame> frames;
    Binding binding = Binding::Right;
    std::string note;
    bool name_ok = false;
    std::string stage = "name";
    std::vector<std::pair<LayerRole, NumList>> fills;  // in the order they were set (Python's dict)
    std::vector<Layer> layers;
    std::optional<Num> spread_with;
    Json selected_frame_id = nullptr;  // (not saved) select_frame's "frame_id" as it is given; null: none
    Json effects = Json::array();
    std::optional<Json> ruler;  // the old single perspective ruler (kept for old books)
    Json rulers = Json::array();
    Json prims = Json::array();
    bool numero = true;
    std::optional<Num> onion_from;
    std::optional<Num> lt_threshold;
    Json extra = Json::object();  // keys this build does not know; written back unchanged
    std::string id;
    bool art_ok = false;
    std::optional<Json> plan;
    // color-mode@1: the page's colour mode (none: an RGB page, as Python's).
    std::optional<PageColor> color;
    // color-model@1: an RGB page's colour model past Python's (none: 8 bits, sRGB). Never with `color`: both are the
    // page's `color` in the book.
    std::optional<RgbMode> rgb;
    // channels@1: its alpha and spot channels, in the order the channel panel lists them.
    std::vector<Channel> channels;
    // PC3 (vector@1, core/vector.hpp): its saved paths (the Paths panel's order), its work path, its clipping path.
    std::vector<vector::NamedPath> paths;
    std::optional<vector::Path> work_path;
    std::optional<vector::ClippingPath> clipping_path;
    // PC3 (page-spec@1): `spec` is this page's own paper (the book's is doc.spec), written as the page's native_spec.
    bool own_spec = false;

    Rect paper_rect_mm() const;
    Rect trim_rect_mm() const;
    Rect bleed_rect_mm() const;
    Num spread_step_mm() const;
    // "left" or "right"; an empty start_side is Python's None (the binding decides where page 1 sits).
    std::string side(std::string_view start_side = {}) const;
    std::string binding_edge(std::string_view start_side = {}) const;
    Rect inner_rect_mm(std::string_view start_side = {}) const;
    bool is_recto() const;

    // The panels in reading order (models.Page._walk_leaves). The pointers stay valid until the page changes.
    std::vector<const Frame*> leaf_frames() const;
    const Frame* find_frame(std::string_view frame_id) const;
    Frame* find_frame(std::string_view frame_id);
    const Frame* frame_at(const Num& x_mm, const Num& y_mm) const;
    const Frame* parent_of(std::string_view frame_id) const;
    Frame* parent_of(std::string_view frame_id);
    // Python's split_frame/merge_frame/resize_frame, with the same errors: core::Error("key") for an unknown
    // frame, core::Error("value") otherwise.
    std::pair<Frame*, Frame*> split_frame(std::string_view frame_id, std::string_view axis, const Num& ratio,
                                          const Num& gutter_mm);
    Frame& merge_frame(std::string_view frame_id);
    Frame& resize_frame(std::string_view frame_id, const Rect& rect);
    void paint(LayerRole role, NumList rgb);

    // The first layer with this role (null when there is none).
    const Layer* first_layer(LayerRole role) const;
    // Python's Page._layer: the first layer with this role, made (and appended) when there is none.
    Layer& layer_for(LayerRole role);
    const NumList* fill_of(LayerRole role) const;
};

using PagePtr = std::shared_ptr<Page>;

// smart-objects@1 (core/smart.hpp, docs/cpp-migration/schema-v4.md「スマートオブジェクト」): what a smart object shows —
// a page of its own (kind "page": the layers it wraps, its own spec and colour mode; index 1, no lines) or a picture
// file kept as the bytes it was read from (kind "file": an asset of its own suffix, never changed). Shared by every
// layer that shows it (in any page of the book) and never changed once made: Document::edit_contents copies it first
// when another Document still shares it.
struct SmartContents {
    std::string kind = "page";  // page | file
    std::string name;           // the layer's title, or the file's name (what exporting it names it)
    PagePtr page;               // kind page: its page
    // kind file: the file's bytes ("sha256:…", in assets/ with its suffix, or in Document::smart_files) and what its
    // header said when it was placed (the drawing reads the file again)
    std::string asset;
    std::string suffix;         // ".png", ".jpg", ".tif", ".webp", ".bmp", ".gif", ".psd", ".psb"
    std::string format;         // png | jpeg | tiff | webp | bmp | gif | psd
    std::int64_t width_px = 0;
    std::int64_t height_px = 0;
    double dpi = 0;             // its resolution (the page's when the file has none): its size in mm
    int depth = 8;              // bits per sample (1, 8, 16, 32)
    std::string mode = "rgb";   // gray | rgb | cmyk | lab (with an alpha or not)
    // How its colours come onto a page (the colour settings' answer, kept: the pixels are never converted when placed):
    // {"use": "keep" | "embedded" | "assign", "icc": "sha256:…" (assign), "intent", "bpc"}
    Json profile = Json::object({{"use", "keep"}});
    std::optional<Json> link;  // a linked file's place (PC3 A1): the contents are its last copy
    std::optional<Json> psd;   // what a PSD's placed layer kept (PC3 A3)
};

// patterns@1 (PC4, core/patterns.hpp, schema-v4.md「パターン」): a pattern the book keeps — the copy of one its fills and
// overlays use (the library of patterns is the machine's: render/preset_store). Its picture is an asset of the book (an
// 8-bit RGB or grey one a PNG, RGBA or LA; a 16- or 32-bit RGB one GKCR; a grey, CMYK or Lab one of more bits, and CMYK
// and Lab of any, GKIR version 2), held in Document::pattern_files by its ref. At 100 % one pixel of it is 1/dpi inch.
struct Pattern {
    std::string id;           // "pat-1"…: 1..64 of A-Z a-z 0-9 - _
    std::string name;         // 1..255 characters
    std::string asset;        // "sha256:…"
    std::string mode = "rgb";  // rgb | gray | cmyk | lab
    int depth = 8;            // 8 | 16 | 32
    std::int64_t width = 0;   // 1..8192, and at most 64 M pixels
    std::int64_t height = 0;
    double dpi = 300.0;       // 50..2400
    friend bool operator==(const Pattern&, const Pattern&) = default;
};

// Python's Episode (without its undo stack, pending journal and asset folder, which belong to the session).
struct Document {
    std::string title;
    Num episode = 1;
    PageSpec spec;
    Binding binding = Binding::Right;
    std::vector<PagePtr> pages;  // shared between copies: change a page only through edit_page()
    std::vector<StoryLine> story;
    Bible bible;
    Json tickets = Json::array();
    bool autosave = false;
    std::string font_path;
    Json page_locks = Json::object();
    std::vector<std::int64_t> brush_rgb{20, 20, 20};
    double brush_width_mm = 0.35;
    std::int64_t brush_stabilize = 0;
    bool brush_taper = false;
    std::string brush_curve = "linear";
    Json brush_custom = Json::object();
    // BRUSH-01 (paper-texture@1): the grey pictures of the papers its brushes keep (brush_custom[key]["paper"]["asset"]),
    // by their asset ref. Written as assets/<ab>/<64 hex>.png; only those a brush keeps are written and read.
    std::map<std::string, Bytes> papers;
    // brushes@1 (PC4, core/brush_dabs.hpp): the brush state's kind (the brush add_stroke draws with when it names none;
    // none: the G pen) and background colour (背景色: what the ops' "background" means), none when not set; and the grey
    // pictures its dab brushes' tips, dual tips and texture tiles are, by their asset ref (assets/<ab>/<64 hex>.png;
    // only those a brush names are written and read).
    std::optional<std::string> brush_kind;
    std::optional<std::vector<std::int64_t>> brush_background_rgb;
    std::map<std::string, Bytes> brush_tips;
    // patterns@1 (PC4, core/patterns.hpp): the patterns it keeps (at most 512, in their order) and their pictures, by
    // their asset ref (written with the suffix their kind takes: core::pattern_suffix).
    std::vector<Pattern> patterns;
    std::map<std::string, Bytes> pattern_files;
    // PC2a (adjustments@1): the 3D lookup tables its color_lookup corrections use (their "lut"), by their asset ref
    // (core/lut3d: GKLU bytes). Written as assets/<ab>/<64 hex>.gklut; only those a correction uses are written and read.
    std::map<std::string, Bytes> luts;
    // smart-objects@1 (core/smart.hpp): the contents of its smart objects, by id ("sc_" and 12 hex digits). Only those a
    // layer shows (on a page, or inside other contents) are written.
    std::map<std::string, std::shared_ptr<const SmartContents>> smart_contents;
    // smart-objects@1: the bytes of the files ops were given (their "data") that are not in assets/ yet, by their asset ref;
    // the writer writes those its contents use (with their suffix), as it writes luts.
    std::map<std::string, Bytes> smart_files;
    Json nombre = Json::object();
    Json extra = Json::object();  // top-level keys this build does not know; written back unchanged
    std::int64_t revision = 0;
    std::optional<std::string> start_side;
    bool strict_gates = false;
    Json studio = Json::object();
    // v4 (docs/cpp-migration/schema-v4.md §2)
    std::string book_id;
    std::vector<std::string> features;
    // color-mode@1: the colour mode of the pages it makes (none: RGB).
    std::optional<PageColor> color;
    // color-model@1: the RGB colour model of the pages it makes (none: 8 bits, sRGB). Never with `color`.
    std::optional<RgbMode> rgb;
    // color-model@1 (SPEC COLOR-03, PC1b): カラー設定's 文字の色をガンマで混ぜる — the gamma (over 1, at most 2.2) the
    // letters of its lines are mixed with what lies under them in (PhotoCraft's blendTextGamma), kept in the book so it
    // draws the same everywhere. None: mixed as Python's (Pillow's paste). Written under the book's "color".
    std::optional<double> text_gamma;
    // Why this book must not be saved (unknown features, missing assets, repairs); empty when it may be.
    std::string read_only_reason;
    // (not saved) The pages whose strokes and pictures are not read yet (storage::LoadOptions::assets_of_page: the
    // first page is shown while the others are read). Such a book is never written (storage::project_payload_v4
    // refuses it), and an op batch may not reach these pages (CommandBus refuses it with "page_not_loaded").
    std::vector<PagePtr> deferred;
    // (not saved) The book's folder, as Python's Episode.asset_dir: where the book was read from
    // (storage::load_document), none for a book not on disk. import_psd reads a relative path from there.
    std::optional<std::filesystem::path> asset_dir;

    const Page& page(std::size_t i) const { return *pages.at(i); }
    // pages[i] is one of `deferred` (its strokes and pictures are not read yet).
    bool is_deferred(std::size_t i) const;
    // The page to change: copied first when another Document still shares it.
    Page& edit_page(std::size_t i);
    // smart-objects@1: the contents to change (core::Error("key") for none), copied first when another Document still
    // shares them; and the page of contents of kind page, copied first as edit_page copies one.
    SmartContents& edit_contents(const std::string& id);
    Page& edit_contents_page(const std::string& id);

    // Python's Episode.story_for_page: the lines whose page_index is page_index.
    std::vector<const StoryLine*> story_for_page(const Num& page_index) const;
    StoryLine& add_line(const Num& page_index, std::string text, std::string speaker = "",
                        std::optional<std::string> frame_id = std::nullopt, std::string ruby = "", Num x_mm = 0,
                        Num y_mm = 0, Num w_mm = 40, Num h_mm = 20, std::string balloon = "speech",
                        std::optional<Point> tail = std::nullopt);
};

// BG (fill), NAME (strokes, not exported), INK (strokes, each line in its panel), FINISH (strokes).
std::vector<Layer> default_layers();

// A page as Python's Page(...) makes it: a new id and the default layers.
Page make_page(const Num& index, const PageSpec& spec, Binding binding);

// Python's new_episode: page_count pages, each with one root panel over the basic frame, and a new book id.
Document new_episode(std::string title, const Num& episode, int page_count, const PageSpec& spec,
                     Binding binding = Binding::Right);

}  // namespace genko::core
