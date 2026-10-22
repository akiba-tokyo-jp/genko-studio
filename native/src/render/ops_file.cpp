// PC4 節 F (SPEC.md FILE-01): the ops of the files' group — registered here; each group writes its op's work in this file
// (F-b File Info, F-e a RAW smart object's development).

#include "render/ops_file.hpp"

#include <string>

#include "core/pyops.hpp"

namespace genko::render {

namespace {

// A frame for an op whose work is not written yet: refused with not_yet_ported ("<op> is not in the C++ build yet (PC4
// <group>)"), nothing changed.
core::OpFunction later(const char* op, const char* group) {
    const std::string message = std::string(op) + " is not in the C++ build yet (PC4 " + group + ")";
    return [message](core::OpContext&) { core::not_yet_ported(message); };
}

}  // namespace

void register_file_ops(core::OpRegistry& registry) {
    // F-b: ファイル情報… ({"title"?, "author"?, "author_title"?, "description"?, "keywords"?: [str] | "a; b", "copyright"?,
    // "copyright_status"?: "unknown" | "copyrighted" | "public_domain", "copyright_url"?} → {"file_info"}: the book's
    // file_info, file-info@1; the title is set_meta's)
    registry.add("set_file_info", later("set_file_info", "F-b"));
    // F-e: a RAW smart object's development ({"contents", "develop"?, "adjust"?} → {"contents", "raw"}: camera-raw@1)
    registry.add("set_camera_raw", later("set_camera_raw", "F-e"));
}

}  // namespace genko::render
