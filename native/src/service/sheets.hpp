#pragma once

#include <filesystem>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// Character sheets as people approve them (Python's service.face_crop and sheet_approval_op).

namespace genko::service {

using core::Json;

// face_crop(image, box01): the face close-up of a character sheet: where the agent (or the person) said it is (x, y,
// w, h in 0..1); without that, a square at the top centre (only a guess: people see it before approving).
render::Image face_crop(const render::Image& image, const Json& box01);

// Image.open(io.BytesIO(data)) of a picture in the book (PyUncaught UnidentifiedImageError / DecompressionBombError
// where Pillow raises them).
render::Image open_picture(const std::string& data);

// sheet_approval_op(project, episode, character_id, candidate_id, face_box01): the approve op for a character sheet,
// with the face cut out of it and stored as an asset (written before the op is applied, as Python's).
Json sheet_approval_op(const std::filesystem::path& project, const core::Document& doc, const Json& character_id,
                       const Json& candidate_id, const Json& face_box01);

}  // namespace genko::service
