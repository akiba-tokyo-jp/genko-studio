#pragma once

#include "core/command_bus.hpp"

// PC4 節 F (形式・コーデック・RAW・.pcraft; SPEC.md FILE-01): the ops that change what a book keeps of its files — the
// C++ build's own (the public list, `genko schema`, and the MCP tools stay as they are; MCP's apply takes them). Opening
// and writing files are not ops (formats/open_file, formats/copy: an op reads and writes no path). F-b's set_file_info is
// in render/ops_formats.cpp, F-e's set_camera_raw in render/ops_camera_raw.cpp.

namespace genko::render {

// render/ops_formats.cpp: F-b set_file_info (ファイル情報…), F-e set_camera_raw (a RAW smart object's development)
void register_format_ops(core::OpRegistry& registry);

// render/ops_camera_raw.cpp (PC4 F-e): a RAW smart object's development ({"contents", "develop"?, "adjust"?} → {"contents",
// "raw", "size_px", "depth"}: camera-raw@1).
void set_camera_raw(core::OpContext& c);

}  // namespace genko::render
