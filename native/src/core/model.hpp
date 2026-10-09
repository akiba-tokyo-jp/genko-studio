#pragma once

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

#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"

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
enum class LayerKind { Raster, Strokes, Fill, Tone, Folder, Placed, Adjust };

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
    bool color_prints = false;
    std::optional<Json> screen;

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
    std::optional<std::vector<Point>> path;    // a balloon drawn by hand: its outline
    std::vector<std::string> emphasis_runs;    // 傍点
    std::vector<std::pair<std::string, Json>> style_runs;  // [[words, {scale, bold, rgb}]]
    Json style = Json::object();
    std::vector<Json> tails;
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
    Json nombre = Json::object();
    Json extra = Json::object();  // top-level keys this build does not know; written back unchanged
    std::int64_t revision = 0;
    std::optional<std::string> start_side;
    bool strict_gates = false;
    Json studio = Json::object();
    // v4 (docs/cpp-migration/schema-v4.md §2)
    std::string book_id;
    std::vector<std::string> features;
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
