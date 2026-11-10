#pragma once

#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"

// Writing project.json v4 (docs/cpp-migration/schema-v4.md §2): the keys, order and values of Python's v3 writer
// (genko/io.py _payload, _layer_to_v3, _layer_to_dict, _frame_to_dict, _line_to_dict) with version 4 and, right after
// it, min_reader, writer, book_id and features.

namespace genko::storage {

// {"app": "genko-native", "version": "<this build>"}
core::Json writer_info();

// The paper as project.json and the snapshot write it.
core::Json spec_to_json(const core::PageSpec& spec);

// project.json v4 as a Json value. Every asset it refers to is in `store` when it returns: pictures are stored by
// their hash, strokes as .strokes.json blobs (a list read from a blob that `store` already holds is not written
// again). Throws core::Error("value") for a document without a valid book_id, and core::Error("partial") for one
// whose pages are not all read (Document::deferred).
core::Json project_payload_v4(const core::Document& doc, AssetStore& store);

// The same, as the text of project.json: UTF-8, LF, indent 2, no trailing newline (Python's
// json.dumps(payload, ensure_ascii=False, indent=2)).
std::string project_json_v4(const core::Document& doc, AssetStore& store);

// (Books are written only through storage::Saver — transaction.hpp —, which uses project_payload_v4 for its
// project.json step: there is no other way to write a book.)

}  // namespace genko::storage
