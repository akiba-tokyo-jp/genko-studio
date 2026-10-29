#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"

// PC5 節 J (SPEC.md PROD-01, FILE-01「PSD・PSB」): the PSD's image resources and global blocks a page's layer comps, slices,
// notes and measurement scale are copied from and written to — J-a 1065 (and the layers' cmls in their shmd), J-b 1050,
// J-c the global Anno, J-d1 1074 (and 1080, the count, kept but not written while the page has its own). Until a group
// writes its row, the file's block is kept as it was (psd-keep@1, PC4 F-d) and written back.
//
// Reading (render/import_psd): after the file's layers are made, read() offers each resource and global block of a row
// to its reader. A block read onto the page is still kept (psd-keep@1) with a mark — the page's native_psd `copied`: the
// block and the fingerprint of what the page held right after reading it (the row's fingerprint) — so the file's own
// bytes are written again while the page's is as it was read (as PhotoCraft does), and the parts the page does not hold
// (Anno's sound notes) are there for the row's writer (WriteContext::kept).
// Writing (formats/psd_export): write() is called once a file, and gives the resources and global blocks in place of the
// kept ones it handles: for a block marked copied, the kept bytes while the page's fingerprint is the mark's, else the row's
// writer's (none: the page holds none any more — not written); a row that names layers (1065, 1050) writes nothing into a
// file without layers. A kept block of an id or key handled is not written beside it; the others are written as before.
//
// (PC5 J-a) A row may also write an item of its layers' metadata block (shmd) — the layer comps' cmls: each layer's state in
// each comp — in their records: while its block is the page's own (its writer's), every layer record's kept shmd loses its
// items of that key and takes the layer's own (Written::layers; a layer without a lyid kept gets one then); while it is the
// kept bytes, the kept shmd is written as it was; the page holding none any more, the key's items are taken out.

namespace genko::render::psd_production {

struct ReadContext {
    const core::vector::Affine* onto = nullptr;   // the file's canvas (fractions of it) on the page (mm)
    std::map<std::int64_t, std::string> layers;  // a file layer's id (lyid) → the layer made of it
    double width_px = 0, height_px = 0;          // the file's canvas
    bool merged_only = false;                    // only the merged picture was read (no layer made: `layers` empty)
    bool page_had = false;                       // the page keeps an earlier PSD's blocks (this file's are not kept)
    core::Json* report = nullptr;                // the import's report (a group says what it read)
    const core::Document* doc = nullptr;         // (PC5 J-a) the book being read into (its kept blocks: native_files)
};

struct Taken {
    std::set<int> resources;                                   // the image resources' ids read onto the page
    std::set<std::string> globals;                             // the global blocks' keys
    std::vector<core::PsdCopied> copied;                       // … and the marks they are kept with (fingerprints)

    bool empty() const { return resources.empty() && globals.empty(); }
};

struct WriteContext {
    bool layers_written = true;                 // the file has the page's layers (false: a flat PSD, a bitmap, an indexed one)
    // a layer's id → the lyid it is written with when a row names it: its kept one (the first layer of it), else one after
    // the largest kept (PC5 J-a). Written in the layer's record only when a row names the layer — name_layer (or
    // LayerItems::lyids) —, and only for a layer the file writes a record of (a hidden layer of a page without comps, a
    // folder have none): a row names only those it finds here and in the records' layers.
    std::map<std::string, std::int64_t> lyids;
    double width_px = 0, height_px = 0;         // the file's canvas
    // The page's kept block's bytes (an image resource by its id, a global block by its key; none: not kept).
    std::function<std::optional<std::string>(int resource)> kept_resource;
    std::function<std::optional<std::string>(std::string_view global)> kept_global;
    const core::Document* doc = nullptr;  // (PC5 J-a) the book written
    // (PC5 J-a, the review's P2-4) A row naming a layer (1050's layer-based slices, J-b): its lyid (none: the file has none
    // for it), now written in its record. Set by write() for the rows.
    std::function<std::optional<std::int64_t>(const std::string& layer)> name_layer;
};

// (PC5 J-a) The layers' metadata items (shmd) the rows write in the records: the keys whose kept items every record loses,
// each layer's own items (key, data), and the lyids written for the layers that hold some.
struct LayerItems {
    std::set<std::string> keys;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> items;
    std::map<std::string, std::int64_t> lyids;

    bool empty() const { return keys.empty() && items.empty() && lyids.empty(); }
};

struct Written {
    std::vector<std::pair<int, std::string>> resources;          // an image resource's id and its data
    std::vector<std::pair<std::string, std::string>> globals;    // a global block's key and its data (signature 8BIM)
    std::set<int> handled_resources;                             // the kept ones of these ids not written beside them
    std::set<std::string> handled_globals;
    LayerItems layers;                                           // (PC5 J-a) the layers' shmd items

    bool empty() const {
        return resources.empty() && globals.empty() && handled_resources.empty() && handled_globals.empty() && layers.empty();
    }
};

// A row of the table: an image resource (its id) or a global block (its key) one group copies.
struct Row {
    int resource = -1;            // (a global block: -1)
    std::string_view global;      // (an image resource: "")
    std::string_view group;       // "J-a" …
    bool names_layers = false;    // (1065, 1050: by the layers' lyids — never into a file without layers)
    // The block's data onto the page (false: not read — then it is kept as it was, unmarked).
    bool (*read)(core::Page& page, std::string_view data, const ReadContext& context) = nullptr;
    // The page's own as the block's data (none: the page has none of it).
    std::optional<std::string> (*write)(const core::Page& page, const WriteContext& context) = nullptr;
    // What the page holds of it, as a fingerprint ("" none): the kept bytes are written while it is the one read. (`doc`:
    // the book, when the fingerprint takes what its layers show — the comps' places —; none: the page alone.)
    std::string (*fingerprint)(const core::Page& page, const core::Document* doc) = nullptr;
    // A row that copies nothing (read and write nullptr) but whose kept block the page's own replaces (1080 while the page
    // has its count marks: J-d1): true → the kept block is not written.
    bool (*supersedes)(const core::Page& page) = nullptr;
    // (PC5 J-a) The key of the item of its layers' shmd the row writes ("cmls"; "": none), and its layers' items when the
    // block is the page's own (into `out`: each layer's (key, data) and the lyids they name).
    std::string_view layer_item;
    void (*write_layers)(const core::Page& page, const WriteContext& context, LayerItems& out) = nullptr;
};

// The table (each group adds its row: render/psd_production.cpp; tests may set their own while a TableForTest lives).
std::span<const Row> rows();
struct TableForTest {
    explicit TableForTest(std::span<const Row> rows);
    ~TableForTest();
    TableForTest(const TableForTest&) = delete;
    TableForTest& operator=(const TableForTest&) = delete;
};
bool copies_resource(int id);
bool copies_global(std::string_view key);
// A fingerprint of a JSON value (its canonical form's sha256): for the rows' fingerprint.
std::string fingerprint_of(const core::Json& value);

// The file's resources and global blocks the table reads, onto the page.
Taken read(core::Page& page, const std::vector<std::pair<int, std::string_view>>& resources,
           const std::vector<std::pair<std::string, std::string_view>>& globals, const ReadContext& context);
// The page's own resources and global blocks for one file (called once a file).
Written write(const core::Page& page, const WriteContext& context);

// (PC5 J-a) A layers' metadata block (shmd): its items (Adobe's "Metadata setting": a count, then each item's signature,
// key, copy-on-sheet-duplication byte, 3 bytes, length and data — Photoshop pads the data to an even length). None for
// bytes that are not one (PhotoCraft's psd/src/metadata.rs, MIT).
struct MetaItem {
    std::string signature = "8BIM";
    std::string key;
    bool copy = false;
    std::string reserved = std::string(3, '\0');
    std::string data;
};
std::optional<std::vector<MetaItem>> shmd_items(std::string_view bytes);
std::string shmd_bytes(const std::vector<MetaItem>& items);
// A layer record's shmd: `kept` (its kept bytes; none: it keeps none) with the items of the keys the page writes
// (own.layers.keys) taken out and the layer's own put in their place (a key it had not: first). None: no shmd for it.
// The kept bytes as they were when the page writes none of its items (or they are not a shmd).
std::optional<std::string> layer_shmd(const Written& own, const std::string& layer, const std::optional<std::string>& kept);

}  // namespace genko::render::psd_production
