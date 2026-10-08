#include "render/timelapse.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/movie.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/transaction.hpp"

namespace genko::render::timelapse {

namespace {

using core::Json;

std::filesystem::path index_file(const std::filesystem::path& project) { return folder(project) / "index.jsonl"; }

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// (pages a recording names by a plain file name in the folder only)
bool plain_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

// The size of a JPEG this recorded (its SOF), without decoding it; nothing when it cannot be read so.
std::optional<Size> jpeg_size(const std::string& bytes) {
    std::size_t at = 2;
    if (bytes.size() < 4 || static_cast<unsigned char>(bytes[0]) != 0xff || static_cast<unsigned char>(bytes[1]) != 0xd8) return std::nullopt;
    while (at + 4 <= bytes.size()) {
        if (static_cast<unsigned char>(bytes[at]) != 0xff) return std::nullopt;
        const int marker = static_cast<unsigned char>(bytes[at + 1]);
        const std::size_t length = (static_cast<std::size_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
                                   static_cast<unsigned char>(bytes[at + 3]);
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (at + 9 > bytes.size()) return std::nullopt;
            const int h = (static_cast<unsigned char>(bytes[at + 5]) << 8) | static_cast<unsigned char>(bytes[at + 6]);
            const int w = (static_cast<unsigned char>(bytes[at + 7]) << 8) | static_cast<unsigned char>(bytes[at + 8]);
            return Size{w, h};
        }
        at += 2 + length;
    }
    return std::nullopt;
}

std::optional<Image> open_recorded(const std::filesystem::path& path) {
    const std::string bytes = read_text(path);
    if (bytes.empty()) return std::nullopt;
    try {
        return open_image(bytes, kPillowOpenLimits).convert("RGB");
    } catch (const std::exception&) {  // (OSError: a picture that is not there or cannot be read)
        return std::nullopt;
    }
}

}  // namespace

bool is_on(const core::Document& doc) {
    if (!doc.extra.is_object()) return false;
    const auto found = doc.extra.find("timelapse");
    if (found == doc.extra.end() || !core::py_truthy(*found)) return false;
    return found->is_object() && core::py_truthy(core::py_get(*found, "on"));
}

std::filesystem::path folder(const std::filesystem::path& project) { return project / "studio" / "timelapse"; }

std::vector<Json> frames(const std::filesystem::path& project, std::optional<Json> page) {
    std::vector<Json> out;
    std::istringstream lines(read_text(index_file(project)));
    std::string line;
    while (std::getline(lines, line)) {
        Json item;
        try {
            item = core::parse_python_json(line);
        } catch (const core::Error&) {
            continue;
        }
        if (!item.is_object()) continue;
        if (!page || core::py_equals(core::py_get(item, "page"), *page)) out.push_back(std::move(item));
    }
    return out;
}

std::vector<Json> changed_pages(const Json& before, const Json& after) {
    const auto pages_of = [](const Json& payload) {
        return payload.is_object() ? core::py_get(payload, "pages", Json::array()) : Json::array();
    };
    const auto lines_of = [](const Json& payload, const Json& index) {
        Json out = Json::array();
        if (!payload.is_object()) return out;
        for (const Json& line : core::iterate(core::py_or(core::py_get(payload, "story", Json::array()), Json::array()))) {
            if (core::py_equals(core::py_get(line, "page_index"), index)) out.push_back(line);
        }
        return out;
    };
    const Json old_pages = pages_of(before);
    std::vector<Json> out;
    for (const Json& page : core::iterate(pages_of(after))) {
        const Json index = core::py_get(page, "index");
        Json old;  // (the last page of that index, as Python's dict keeps it)
        for (const Json& p : core::iterate(old_pages)) {
            if (core::py_equals(core::py_get(p, "index"), index)) old = p;
        }
        if (old.is_null() || !core::py_equals(old, page) || !core::py_equals(lines_of(before, index), lines_of(after, index))) {
            out.push_back(index);
        }
    }
    return out;
}

std::vector<std::filesystem::path> record(const std::filesystem::path& project, const core::Document& doc, const std::vector<Json>& pages) {
    std::vector<std::filesystem::path> written;
    if (pages.empty()) return written;
    const std::filesystem::path dir = folder(project);
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) return written;
    const std::vector<Json> known = frames(project);
    if (known.size() >= kMaxFrames) return written;
    std::int64_t n = 1;
    if (!known.empty()) {
        try {
            n = core::to_int(core::py_get(known.back(), "n")) + 1;
        } catch (const std::exception&) {
            return written;
        }
    }
    std::ofstream handle(index_file(project), std::ios::binary | std::ios::app);
    if (!handle) return written;
    for (const Json& index : pages) {
        const core::Page* page = nullptr;
        for (const auto& p : doc.pages) {
            if (core::py_equals(p->index.json(), index)) {
                page = p.get();
                break;
            }
        }
        if (page == nullptr) continue;
        const double longest = std::max(page->spec.width_mm.value(), page->spec.height_mm.value());
        const int dpi = static_cast<int>(std::max<std::int64_t>(10, core::py_round_int(kLongSide / (longest / 25.4))));
        std::string bytes;
        std::int64_t number = 0;
        try {
            RenderOptions options;
            options.mode = "proof";
            bytes = write_jpeg(render_page(*page, dpi, options, &doc).image, 85);
            number = core::py_int(index);
        } catch (const std::exception&) {  // (a picture that cannot be made now must never stop the save)
            continue;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "%06lld_p%03lld.jpg", static_cast<long long>(n), static_cast<long long>(number));
        std::ofstream picture(dir / name, std::ios::binary | std::ios::trunc);
        picture.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        picture.close();
        if (!picture) continue;
        const double at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        Json item = Json::object();
        item["n"] = n;
        item["page"] = index;
        item["at"] = at;
        item["file"] = name;
        handle << core::dump_python(item) << "\n";
        handle.flush();
        written.push_back(dir / name);
        ++n;
    }
    return written;
}

void after_save(const std::filesystem::path& project, const core::Document& doc, const Json& before) {
    if (!is_on(doc)) return;
    try {
        // (not ((old_payload or {}).get("timelapse") or {}).get("on"): just turned on, every page)
        const Json was = before.is_object() ? core::py_or(core::py_get(before, "timelapse"), Json::object()) : Json::object();
        const bool was_on = was.is_object() && core::py_truthy(core::py_get(was, "on"));
        std::vector<Json> pages;
        if (!was_on) {
            for (const auto& p : doc.pages) pages.push_back(p->index.json());
        } else {
            pages = changed_pages(before, storage::read_disk_state(project).payload);
        }
        record(project, doc, pages);
    } catch (const std::exception&) {
        // (the save is done; the timelapse never undoes it)
    }
}

void clear(const std::filesystem::path& project) {
    std::error_code ignored;
    std::filesystem::remove_all(folder(project), ignored);
}

std::filesystem::path export_timelapse(const std::filesystem::path& project, const std::filesystem::path& dest, std::optional<Json> page,
                                       double fps, std::optional<double> seconds, std::optional<std::string> fmt, double hold,
                                       int* frames_out) {
    std::vector<Json> items = frames(project, page);
    if (items.empty()) throw core::PyValueError("nothing has been recorded yet (turn the timelapse on and work for a while)");
    std::string format = fmt.value_or("");
    if (format.empty()) {
        const std::string suffix = core::path_to_utf8(dest.extension());
        format = suffix.size() > 1 ? suffix.substr(1) : std::string("webp");
    }
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (format == "apng") format = "png";
    if (std::find(kFormats.begin(), kFormats.end(), format) == kFormats.end()) {
        throw core::PyValueError("format must be one of webp, gif, png, mp4");
    }
    if (!(1 <= fps && fps <= 60)) throw core::PyValueError("fps is 1 to 60");
    if (seconds && *seconds != 0) {
        const std::int64_t wanted = std::max<std::int64_t>(2, static_cast<std::int64_t>(*seconds * fps));
        if (static_cast<std::int64_t>(items.size()) > wanted) {
            const double step = static_cast<double>(items.size()) / static_cast<double>(wanted);
            std::vector<Json> kept;
            for (std::int64_t i = 0; i < wanted - 1; ++i) kept.push_back(items[static_cast<std::size_t>(static_cast<double>(i) * step)]);
            kept.push_back(items.back());
            items = std::move(kept);
        }
    }
    const std::filesystem::path dir = folder(project);
    // the pictures there are, and the largest size (read from their headers: the pictures are drawn one by one)
    std::vector<std::filesystem::path> files;
    Size board{0, 0};
    for (const Json& item : items) {
        const Json file = core::py_get(item, "file");
        if (!file.is_string() || !plain_name(file.get<std::string>())) continue;
        const std::filesystem::path path = dir / core::path_from_utf8(file.get<std::string>());
        std::optional<Size> size = jpeg_size(read_text(path));
        if (!size) {
            const auto image = open_recorded(path);
            if (!image) continue;
            size = image->size();
        }
        board = Size{std::max(board.width, size->width), std::max(board.height, size->height)};
        files.push_back(path);
    }
    std::filesystem::path out = dest;
    out.replace_extension("." + format);
    std::optional<movie::Writer> writer;
    for (const auto& path : files) {
        const auto image = open_recorded(path);
        if (!image) continue;
        if (!writer) writer.emplace(out, fps, format, hold, true);
        Image framed = Image::create("RGB", board, Ink{128, 128, 128});
        framed.paste(*image, Point{(board.width - image->width()) / 2, (board.height - image->height()) / 2});
        writer->add(framed);
    }
    if (!writer) throw core::PyValueError("the recorded pictures are missing");
    return writer->finish(frames_out);
}

}  // namespace genko::render::timelapse
