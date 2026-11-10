#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// The timelapse (タイムラプス, Python's genko/timelapse.py): while it is on (set_timelapse), every save leaves a small
// picture of each page it changed in studio/timelapse/ (JPEG, quality 85, the long side 720 px; index.jsonl lists them
// in order: {"n", "page", "at", "file"}); the export plays them back as a moving picture (render/movie.hpp), for one
// page or the whole book in the order the work was done.
//
// A page this build cannot draw whole yet (render::NotYetPorted) is left out of the recording, as Python leaves out a
// picture that cannot be made: a save never fails for the timelapse.

namespace genko::render::timelapse {

inline constexpr int kLongSide = 720;
inline constexpr std::size_t kMaxFrames = 50'000;
inline constexpr std::array<std::string_view, 4> kFormats{"webp", "gif", "png", "mp4"};

// bool((episode.extra.get("timelapse") or {}).get("on"))
bool is_on(const core::Document& doc);

// <project>/studio/timelapse
std::filesystem::path folder(const std::filesystem::path& project);

// The recorded pictures in order ({n, page, at, file}; lines that are not JSON skipped), of one page or all.
std::vector<core::Json> frames(const std::filesystem::path& project, std::optional<core::Json> page = std::nullopt);

// The page numbers whose content (or the words on them) differ between two project.json payloads (before: none for a
// new book), in the order of `after`'s pages.
std::vector<core::Json> changed_pages(const core::Json& before, const core::Json& after);

// A picture of each of these pages as they are now; the files written (none past kMaxFrames pictures).
std::vector<std::filesystem::path> record(const std::filesystem::path& project, const core::Document& doc,
                                          const std::vector<core::Json>& pages);

// After a save of `doc` into `project` (io.save_episode's ending): when the timelapse is on, a picture of each page the
// save changed (`before`: project.json as it was, null for a new book), or of every page when it was just turned on
// (`before` not on). Never throws: a picture that cannot be made is left out.
void after_save(const std::filesystem::path& project, const core::Document& doc, const core::Json& before);

// Everything recorded, gone.
void clear(const std::filesystem::path& project);

// timelapse.export: the recorded pictures (one page's, or all) as a moving picture at dest (its suffix, or fmt: webp |
// gif | png (apng) | mp4), each on a grey board of the largest size. `seconds` fits the whole recording into that time
// (pictures dropped evenly); `hold` keeps the finished picture on screen that long at the end. `frames`: the pictures in
// the file. PyValueError "nothing has been recorded yet (turn the timelapse on and work for a while)", "format must be
// one of webp, gif, png, mp4", "fps is 1 to 60", "the recorded pictures are missing"; the moving picture's own errors.
std::filesystem::path export_timelapse(const std::filesystem::path& project, const std::filesystem::path& dest,
                                       std::optional<core::Json> page = std::nullopt, double fps = 12,
                                       std::optional<double> seconds = std::nullopt, std::optional<std::string> fmt = std::nullopt,
                                       double hold = 2.0, int* frames = nullptr);

}  // namespace genko::render::timelapse
