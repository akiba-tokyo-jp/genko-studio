#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/vector.hpp"

// PC5 節 J (SPEC.md PROD-01, FILE-01「PSD・PSB」): the PSD's image resources and global blocks a page's layer comps, slices,
// notes and measurement scale are copied from and written to — J-a 1065 (and the layers' cmls in their shmd), J-b 1050,
// J-c the global Anno, J-d1 1074. Until a group writes its row, the file's block is kept as it was (psd-keep@1, PC4 F-d)
// and written back; once a row reads it onto the page it is not kept (the page's own is written in its place).
//
// Reading (render/import_psd): after the file's layers are made, read() offers each resource and global block of a row
// to its reader; those taken are left out of what the page keeps (page_keep). Writing (formats/psd_export): write() gives
// the resources and global blocks the page's own make; a kept one of the same id or key is not written beside it.

namespace genko::render::psd_production {

struct ReadContext {
    const core::vector::Affine* onto = nullptr;             // the file's canvas (fractions of it) on the page (mm)
    std::map<std::int64_t, std::string> layers;            // a file layer's id (lyid) → the layer made of it
    double width_px = 0, height_px = 0;                    // the file's canvas
    core::Json* report = nullptr;                          // the import's report (a group says what it read)
};

struct Taken {
    std::set<int> resources;          // the image resources' ids read onto the page
    std::set<std::string> globals;    // the global blocks' keys

    bool empty() const { return resources.empty() && globals.empty(); }
};

struct Written {
    std::vector<std::pair<int, std::string>> resources;          // an image resource's id and its data
    std::vector<std::pair<std::string, std::string>> globals;    // a global block's key and its data (signature 8BIM)

    bool empty() const { return resources.empty() && globals.empty(); }
};

struct WriteContext {
    std::map<std::string, std::int64_t> lyids;  // a layer's id → the lyid written for it
    double width_px = 0, height_px = 0;         // the file's canvas
};

// A row of the table: an image resource (its id) or a global block (its key) one group copies.
struct Row {
    int resource = -1;            // (a global block: -1)
    std::string_view global;      // (an image resource: "")
    std::string_view group;       // "J-a" …
    // The block's data onto the page (false: not read — then it is kept as it was).
    bool (*read)(core::Page& page, std::string_view data, const ReadContext& context) = nullptr;
    // The page's own as the block's data (none: the page has none of it).
    std::optional<std::string> (*write)(const core::Page& page, const WriteContext& context) = nullptr;
};

// The table (each group adds its row: render/psd_production.cpp).
std::span<const Row> rows();
bool copies_resource(int id);
bool copies_global(std::string_view key);

// The file's resources and global blocks the table reads, onto the page.
Taken read(core::Page& page, const std::vector<std::pair<int, std::string_view>>& resources,
           const std::vector<std::pair<std::string, std::string_view>>& globals, const ReadContext& context);
// The page's own resources and global blocks.
Written write(const core::Page& page, const WriteContext& context);

}  // namespace genko::render::psd_production
