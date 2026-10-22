// PC4 節 E (SPEC.md COLOR-08, patterns@1): the ops of the book's patterns — registered here (E-0); E3 writes their work in
// this file: a pattern the book keeps (core::Pattern, Document::patterns and pattern_files) added from its picture,
// renamed, and deleted when nothing uses it.

#include "render/ops_paint_tools.hpp"

namespace genko::render {

void register_pattern_ops(core::OpRegistry& registry) {
    // E3: {op: add_pattern, name, png | gkcr | gkir (base64) | asset, mode, depth, dpi} → {"pattern": id};
    // {op: rename_pattern, id, name}; {op: delete_pattern, id} (refused while a fill uses it)
    for (const char* op : {"add_pattern", "rename_pattern", "delete_pattern"}) registry.add(op, not_yet_pc4(op, "E3"));
}

}  // namespace genko::render
