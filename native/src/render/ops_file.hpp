#pragma once

#include "core/command_bus.hpp"

// PC4 節 F (形式・コーデック・RAW・.pcraft; SPEC.md FILE-01): the ops that change what a book keeps of its files — the
// C++ build's own (the public list, `genko schema`, and the MCP tools stay as they are; MCP's apply takes them). Opening
// and writing files are not ops (formats/open_file, formats/copy: an op reads and writes no path). Each group writes its
// op's work in render/ops_file.cpp; until then the op is registered and refused with not_yet_ported naming it and its
// group, so a book or a script that names it is told it is not in this build yet rather than that it is unknown.

namespace genko::render {

// render/ops_file.cpp: F-b set_file_info (ファイル情報…), F-e set_camera_raw (a RAW smart object's development)
void register_file_ops(core::OpRegistry& registry);

}  // namespace genko::render
