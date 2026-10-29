#pragma once

#include <functional>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// PC5 節 J (SPEC.md PROD-01, FILE-01「.pcraft」): a .pcraft document's keys the page's production copies — "layer_comps"
// (J-a), "slices" (J-b), "notes" (J-c), "measurement" (J-d1), "timeline" (J-e1) — and 節 I's "variables" (I-d), where PC4
// F-f said it. PC4 F-f left them out and said so; each group's row reads its key onto the page (render/pcraft_read) and
// writes it from the page (formats/pcraft_write). A key whose row has no reader yet is said as before ("…は Genko に無いため
// 外しました", only when it holds something, in the table's order: PC4 F-f's). The contexts' `layers` (the document's layer
// ids and the book's) are not filled by J-0: J-a, the first row that names layers, fills them where pcraft_read and
// pcraft_write make the layers. (A layer's "video" — its frames — is the layer's: J-e2, render/pcraft_read.)

namespace genko::render::pcraft_production {

struct ReadContext {
    core::Page* page = nullptr;
    double mm = 25.4 / 72;                              // mm a pixel of the document
    std::map<std::string, std::string> layers;          // a document layer's id → the layer made of it
    std::function<void(std::string)> say;               // a note of the reading
    const core::Json* document = nullptr;               // (PC5 J-a) the whole document (a row's keys beside its own)
};

struct WriteContext {
    double px = 72 / 25.4;                              // the document's pixels a mm
    std::map<std::string, std::string> layers;          // a layer's id → its id in the document
    std::function<void(std::string)> say;               // a note of the writing
    core::Json* document = nullptr;                     // (PC5 J-a) the whole document (a row's keys beside its own)
};

struct Row {
    std::string_view key;
    std::string_view what;  // its name in a note
    // Is the value worth a note when it is left out (holds something)?
    bool (*worth_saying)(const core::Json& value) = nullptr;
    // The value onto the page (nullptr: not read yet — left out, said).
    void (*read)(const core::Json& value, ReadContext& context) = nullptr;
    // The page's own as the key's value (none: the page has none of it; nullptr: not written yet).
    std::optional<core::Json> (*write)(const core::Page& page, WriteContext& context) = nullptr;
};

// The table (each group fills its row: render/pcraft_production.cpp).
std::span<const Row> rows();
// The document's keys onto the page.
void read(const core::Json& document, ReadContext& context);
// The page's own keys into the document.
void write(const core::Page& page, core::Json& document, WriteContext& context);

}  // namespace genko::render::pcraft_production
