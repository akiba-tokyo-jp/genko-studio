#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

// Generated images brought in as candidates of a generation request (Python's genko/studio/importer.py). Accepted
// sources: a file inside <project>/studio/inbox/ (same machine), or an asset already uploaded to <project>/assets/.
// Nothing else is read. Each image is checked (a real image, size limits), stored as PNG, and measured (aspect match,
// how busy the balloon areas are, brightness, how well the edges follow the pose guide) so weak candidates sort last.

namespace genko::service::importer {

using core::Json;

inline constexpr std::uintmax_t kMaxBytes = 30ULL * 1024 * 1024;
inline constexpr std::int64_t kMaxPixels = 64'000'000;
inline constexpr std::size_t kMaxImages = 16;
inline constexpr int kThumb = 256;

// importer.ImportError_ (a ValueError): the message and where.
class ImportError : public std::runtime_error {
public:
    ImportError(const std::string& message, std::string path = "/") : std::runtime_error(message), path_(std::move(path)) {}
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// importer.inbox(project): <project>/studio/inbox
std::filesystem::path inbox(const std::filesystem::path& project);
// importer.read_source(project, item, index): the bytes of {file} (in the inbox) or {asset}.
std::string read_source(const std::filesystem::path& project, const Json& item, std::size_t index);
// importer.normalize(blob, index): PNG bytes (a picture of another format saved as PNG), its size and the picture.
std::tuple<std::string, std::pair<int, int>, render::Image> normalize(const std::string& blob, std::size_t index = 0);
// importer.check_origin(origin, index)
Json check_origin(const Json& origin, std::size_t index);
// importer.metrics(image, request): deterministic hints only.
Json metrics(const render::Image& image, const Json& request);
// importer.rank_key(metrics): lower is better.
double rank_key(const Json& metrics);
// importer.candidate_id(request_id, asset)
std::string candidate_id(const std::string& request_id, const std::string& asset);
// importer.prepare(project, request, images): the import_candidates items (stored, not yet in project.json) and the
// first picture.
std::pair<Json, std::optional<render::Image>> prepare(const std::filesystem::path& project, const Json& request, const Json& images);

}  // namespace genko::service::importer
