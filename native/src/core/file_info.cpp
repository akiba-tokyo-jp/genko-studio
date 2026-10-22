// file-info@1 and image-grid@1 (core/file_info.hpp).

#include "core/file_info.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <span>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

constexpr std::array<std::string_view, 8> kInfoKeys{"author", "author_title", "description", "keywords",
                                                    "copyright", "copyright_status", "copyright_url", "xmp"};
constexpr std::array<std::string_view, 10> kSourceKeys{"format", "name", "width_px", "height_px", "dpi",
                                                       "depth", "mode", "exif", "xmp", "camera"};
constexpr std::array<std::string_view, 6> kCameraKeys{"make", "model", "iso", "exposure", "aperture", "focal"};
constexpr std::int64_t kSideMax = 1 << 20;  // pixels of a file's side (as a smart object's file)
constexpr std::size_t kNameMax = 255;       // characters of a file's name

template <std::size_t N>
bool in(const std::array<std::string_view, N>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

[[noreturn]] void refuse(const std::string& message) { throw Error("value", message); }

bool is_ref(std::string_view text) {
    return text.size() == 71 && text.starts_with("sha256:") &&
           std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

const Json* get(const Json& object, std::string_view key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

void only_keys(const Json& object, std::span<const std::string_view> known, const std::string& what) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(known.begin(), known.end(), key) == known.end()) refuse(what + " key " + py_repr_str(key) + " is not known to this build");
    }
}

std::string text_of(const Json& object, std::string_view key, const std::string& what) {
    const Json* v = get(object, key);
    if (v == nullptr || v->is_null()) return {};
    if (!v->is_string()) refuse(what + "'s " + std::string(key) + " is text");
    return v->get<std::string>();
}

double number_of(const Json& value, const std::string& what) {
    if (!value.is_number() || value.is_boolean()) refuse(what + " is a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) refuse(what + " is a finite number");
    return v;
}

void check_text(const std::string& text, const std::string& what) {
    if (code_points(text) > kFileInfoTextMax) refuse(what + " is longer than 2000 characters");
}

void check_asset(const std::string& ref, const std::string& what) {
    if (!ref.empty() && !is_ref(ref)) refuse(what + " is an asset (sha256:…)");
}

}  // namespace

bool FileInfo::empty() const {
    return author.empty() && author_title.empty() && description.empty() && keywords.empty() && copyright.empty() &&
           copyright_status.empty() && copyright_url.empty() && xmp.empty();
}

Json to_json(const FileInfo& info) {
    Json out = Json::object();
    const auto put = [&](const char* key, const std::string& value) {
        if (!value.empty()) out[key] = value;
    };
    put("author", info.author);
    put("author_title", info.author_title);
    put("description", info.description);
    if (!info.keywords.empty()) out["keywords"] = info.keywords;
    put("copyright", info.copyright);
    put("copyright_status", info.copyright_status);
    put("copyright_url", info.copyright_url);
    put("xmp", info.xmp);
    return out;
}

FileInfo file_info_from_json(const Json& value) {
    const std::string what = "the book's file_info";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, kInfoKeys, what);
    FileInfo info;
    info.author = text_of(value, "author", what);
    info.author_title = text_of(value, "author_title", what);
    info.description = text_of(value, "description", what);
    if (const Json* keywords = get(value, "keywords"); keywords != nullptr && !keywords->is_null()) {
        if (!keywords->is_array()) refuse(what + "'s keywords are a list of text");
        for (const Json& keyword : *keywords) {
            if (!keyword.is_string()) refuse(what + "'s keywords are a list of text");
            info.keywords.push_back(keyword.get<std::string>());
        }
    }
    info.copyright = text_of(value, "copyright", what);
    info.copyright_status = text_of(value, "copyright_status", what);
    info.copyright_url = text_of(value, "copyright_url", what);
    info.xmp = text_of(value, "xmp", what);
    check_file_info(info);
    return info;
}

void check_file_info(const FileInfo& info) {
    const std::string what = "the book's file_info";
    check_text(info.author, what + "'s author");
    check_text(info.author_title, what + "'s author_title");
    check_text(info.description, what + "'s description");
    check_text(info.copyright, what + "'s copyright");
    check_text(info.copyright_url, what + "'s copyright_url");
    if (info.keywords.size() > kKeywordsMax) refuse(what + " has more than 200 keywords");
    for (const std::string& keyword : info.keywords) {
        if (keyword.empty()) refuse(what + "'s keywords are not empty");
        check_text(keyword, what + "'s keyword");
    }
    if (!info.copyright_status.empty() && !in(kCopyrightStatuses, info.copyright_status))
        refuse(what + "'s copyright_status is unknown, copyrighted or public_domain");
    check_asset(info.xmp, what + "'s xmp");
}

Json to_json(const NativeSource& source) {
    Json out = Json::object();
    out["format"] = source.format;
    out["name"] = source.name;
    out["width_px"] = source.width_px;
    out["height_px"] = source.height_px;
    if (source.dpi) out["dpi"] = Json::array({source.dpi->first, source.dpi->second});
    out["depth"] = source.depth;
    out["mode"] = source.mode;
    if (!source.exif.empty()) out["exif"] = source.exif;
    if (!source.xmp.empty()) out["xmp"] = source.xmp;
    if (source.camera) {
        Json camera = Json::object();
        camera["make"] = source.camera->make;
        camera["model"] = source.camera->model;
        if (source.camera->iso) camera["iso"] = *source.camera->iso;
        if (source.camera->exposure) camera["exposure"] = *source.camera->exposure;
        if (source.camera->aperture) camera["aperture"] = *source.camera->aperture;
        if (source.camera->focal) camera["focal"] = *source.camera->focal;
        out["camera"] = std::move(camera);
    }
    return out;
}

NativeSource native_source_from_json(const Json& value) {
    const std::string what = "a page's native_source";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, kSourceKeys, what);
    NativeSource source;
    source.format = text_of(value, "format", what);
    source.name = text_of(value, "name", what);
    const auto whole = [&](const char* key) {
        const Json* v = get(value, key);
        if (v == nullptr || !v->is_number_integer()) refuse(what + "'s " + key + " is a whole number");
        return v->get<std::int64_t>();
    };
    source.width_px = whole("width_px");
    source.height_px = whole("height_px");
    if (const Json* dpi = get(value, "dpi"); dpi != nullptr && !dpi->is_null()) {
        if (!dpi->is_array() || dpi->size() != 2) refuse(what + "'s dpi is [x, y]");
        source.dpi = std::make_pair(number_of((*dpi)[0], what + "'s dpi"), number_of((*dpi)[1], what + "'s dpi"));
    }
    source.depth = static_cast<int>(whole("depth"));
    source.mode = text_of(value, "mode", what);
    source.exif = text_of(value, "exif", what);
    source.xmp = text_of(value, "xmp", what);
    if (const Json* camera = get(value, "camera"); camera != nullptr && !camera->is_null()) {
        const std::string who = what + "'s camera";
        if (!camera->is_object()) refuse(who + " is an object");
        only_keys(*camera, kCameraKeys, who);
        CameraInfo info;
        info.make = text_of(*camera, "make", who);
        info.model = text_of(*camera, "model", who);
        const auto number = [&](const char* key) -> std::optional<double> {
            const Json* v = get(*camera, key);
            if (v == nullptr || v->is_null()) return std::nullopt;
            return number_of(*v, who + "'s " + key);
        };
        info.iso = number("iso");
        info.exposure = number("exposure");
        info.aperture = number("aperture");
        info.focal = number("focal");
        source.camera = std::move(info);
    }
    check_native_source(source);
    return source;
}

void check_native_source(const NativeSource& source) {
    const std::string what = "a page's native_source";
    if (!in(kSourceFormats, source.format)) refuse(what + "'s format is not one this build knows (" + py_repr_str(source.format) + ")");
    if (source.name.empty() || code_points(source.name) > kNameMax || source.name.find('/') != std::string::npos ||
        source.name.find('\\') != std::string::npos)
        refuse(what + "'s name is a file's name (1..255 characters, no folder)");
    if (source.width_px < 1 || source.height_px < 1 || source.width_px > kSideMax || source.height_px > kSideMax)
        refuse(what + "'s size in pixels");
    if (source.dpi && !(source.dpi->first > 0 && source.dpi->first <= 1e6 && source.dpi->second > 0 && source.dpi->second <= 1e6))
        refuse(what + "'s dpi is more than 0 and at most 1000000");
    if (source.depth != 1 && source.depth != 8 && source.depth != 16 && source.depth != 32) refuse(what + "'s depth is 1, 8, 16 or 32 bits");
    if (!in(kSourceModes, source.mode)) refuse(what + "'s mode is not one this build knows (" + py_repr_str(source.mode) + ")");
    check_asset(source.exif, what + "'s exif");
    check_asset(source.xmp, what + "'s xmp");
    if (source.camera) {
        const std::string who = what + "'s camera";
        if (code_points(source.camera->make) > kNameMax || code_points(source.camera->model) > kNameMax)
            refuse(who + "'s make and model are at most 255 characters");
        for (const auto& v : {source.camera->iso, source.camera->exposure, source.camera->aperture, source.camera->focal}) {
            if (v && !(*v > 0)) refuse(who + "'s iso, exposure, aperture and focal are more than 0");
        }
    }
}

std::string grid_from_json(const Json& value) {
    if (!value.is_string() || !in(kGrids, value.get_ref<const std::string&>())) refuse("a paper's grid is \"finest\"");
    return value.get<std::string>();
}

bool uses_file_info(const Document& doc) {
    if (doc.file_info && !doc.file_info->empty()) return true;
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->native_source) return true;
    }
    return false;
}

bool uses_image_grid(const Document& doc) {
    if (doc.spec.grid) return true;
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->spec.grid) return true;
    }
    return false;
}

void validate_file_info(const Document& doc) {
    if (doc.file_info) check_file_info(*doc.file_info);
    const auto grid = [](const PageSpec& spec) {
        if (spec.grid && !in(kGrids, *spec.grid)) refuse("a paper's grid is \"finest\"");
    };
    grid(doc.spec);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        grid(written.page->spec);
        if (written.page->native_source) check_native_source(*written.page->native_source);
    }
}

std::vector<std::pair<std::string, std::string_view>> file_info_assets(const Document& doc) {
    std::vector<std::pair<std::string, std::string_view>> out;
    std::set<std::string> seen;
    const auto add = [&](const std::string& ref, std::string_view suffix) {
        if (!ref.empty() && seen.insert(ref + std::string(suffix)).second) out.emplace_back(ref, suffix);
    };
    if (doc.file_info) add(doc.file_info->xmp, kXmpSuffix);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (const auto& source = written.page->native_source) {
            add(source->exif, kExifSuffix);
            add(source->xmp, kXmpSuffix);
        }
    }
    return out;
}

}  // namespace genko::core
