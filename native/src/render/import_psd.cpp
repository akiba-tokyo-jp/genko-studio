// import_psd (Python's fileops.import_psd and fileops.placed): every layer of a PSD or PSB as a Genko layer on the page,
// read by render/psd (psd.read_psd).

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#endif

#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;

constexpr std::int64_t kMaxImagePixels = 120'000'000;  // ops.MAX_IMAGE_PIXELS
constexpr double kMaxSide = 12'000;                    // fileops.MAX_SIDE
constexpr int kMaskDpi = 150;                          // ops.MASK_DPI
constexpr std::int64_t kMaxPagePixels = 12'001LL * 12'001LL;
// The largest file read (Photoshop's own limit for a PSD; a PSB past it would not fit what one page can hold).
constexpr std::uint64_t kMaxFileBytes = std::uint64_t{1} << 31;

// --- the file -----------------------------------------------------------------------------------------------------

// A POSIX path as pathlib holds it: its root ("", "/" or exactly "//") and its parts (without "" and ".").
struct PyPath {
    std::string root;
    std::vector<std::string> parts;

    // str(path)
    std::string text() const {
        std::string out = root;
        for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : "/") + parts[i];
        return out.empty() ? std::string(".") : out;
    }
};

// pathlib's _parse_path with posixpath.splitroot
PyPath parse_path(const std::string& text) {
    PyPath path;
    std::string rest = text;
    if (!text.empty() && text[0] == '/') {
        const bool two = text.size() >= 2 && text[1] == '/' && !(text.size() >= 3 && text[2] == '/');
        path.root = two ? "//" : "/";
        rest = text.substr(two ? 2 : 1);
    }
    std::string part;
    for (const char c : rest + "/") {
        if (c != '/') {
            part += c;
            continue;
        }
        if (!part.empty() && part != ".") path.parts.push_back(part);
        part.clear();
    }
    return path;
}

#ifndef _WIN32
// posixpath.expanduser of a path's first part ("~" or "~user"): the part unchanged when there is no such home.
std::string expand_home(const std::string& first) {
    std::string home;
    if (first == "~") {
        if (const char* env = std::getenv("HOME")) {
            home = env;
        } else {
            const passwd* entry = getpwuid(getuid());
            if (entry == nullptr || entry->pw_dir == nullptr) return first;
            home = entry->pw_dir;
        }
    } else {
        const std::string name = first.substr(1);
        if (name.find('\0') != std::string::npos) throw core::PyValueError("embedded null character");
        const passwd* entry = getpwnam(name.c_str());
        if (entry == nullptr || entry->pw_dir == nullptr) return first;
        home = entry->pw_dir;
    }
    while (!home.empty() && home.back() == '/') home.pop_back();
    return home.empty() ? std::string("/") : home;
}

// OSError's words: "[Errno N] <strerror>: '<filename>'"
[[noreturn]] void cannot_read(int error, const std::string& filename) {
    throw OpError("the file cannot be read ([Errno " + std::to_string(error) + "] " + std::strerror(error) + ": " +
                  core::py_repr_str(filename) + ")");
}

// Path.read_bytes(): open(path, "rb").read(), with its errors (ApplyError "the file cannot be read (<OSError>)").
// Beyond Python, a file larger than kMaxFileBytes is refused (it reads the whole file, whatever its size).
std::string read_bytes(const std::string& filename) {
    if (filename.find('\0') != std::string::npos) throw core::PyValueError("embedded null byte");
    const int fd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) cannot_read(errno, filename);
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) cannot_read(EISDIR, filename);  // (FileIO refuses a folder)
    const auto too_large = [] { throw OpError("the file cannot be read (it is larger than 2 GiB)"); };
    if (S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) > kMaxFileBytes) too_large();
    std::string out;
    char buffer[1 << 16];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) continue;
            cannot_read(errno, filename);
        }
        if (n == 0) break;
        out.append(buffer, static_cast<std::size_t>(n));
        if (out.size() > kMaxFileBytes) too_large();
    }
    return out;
}
#endif

// fileops.import_psd's reading of op["path"]: Path(str(path)).expanduser(), relative to the book's folder when it has
// one, read whole.
std::string read_path(const core::Document& doc, const Json& value) {
#ifndef _WIN32
    PyPath source = parse_path(core::py_str(value));
    if (source.root.empty() && !source.parts.empty() && source.parts[0].substr(0, 1) == "~") {
        const std::string home = expand_home(source.parts[0]);
        if (home.substr(0, 1) == "~") throw core::PyUncaught("RuntimeError", "Could not determine home directory.");
        PyPath expanded = parse_path(home);
        expanded.parts.insert(expanded.parts.end(), source.parts.begin() + 1, source.parts.end());
        source = std::move(expanded);
    }
    if (source.root.empty() && doc.asset_dir) {  // (relative to the book's folder)
        PyPath joined = parse_path(core::path_to_utf8(*doc.asset_dir));
        joined.parts.insert(joined.parts.end(), source.parts.begin(), source.parts.end());
        source = std::move(joined);
    }
    return read_bytes(source.text());
#else
    // (Windows: deferred with the rest of that build; the file as std::filesystem finds it)
    std::filesystem::path source = core::path_from_utf8(core::py_str(value));
    if (source.is_relative() && doc.asset_dir) source = *doc.asset_dir / source;
    std::ifstream in(source, std::ios::binary);
    if (!in) throw OpError("the file cannot be read (" + core::path_to_utf8(source) + ")");
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (out.size() > kMaxFileBytes) throw OpError("the file cannot be read (it is larger than 2 GiB)");
    return out;
#endif
}

// --- where it lands -----------------------------------------------------------------------------------------------

// a / b of Python numbers: ZeroDivisionError (which apply_ops lets through) for b == 0
double divided(std::int64_t a, const core::Num& b) {
    if (!b.truthy()) throw core::PyUncaught("ZeroDivisionError", b.is_int() ? "division by zero" : "float division by zero");
    return (core::Num(a) / b).value();
}

struct Placement {
    std::int64_t page_w = 1, page_h = 1;    // the page layer's size, px
    std::int64_t shown_w = 1, shown_h = 1;  // the picture's size on it
    std::int64_t at_x = 0, at_y = 0;        // its top left
};

// fileops.placed(page, size, fit)
Placement placed(const core::Page& page, std::int64_t width, std::int64_t height, const std::string& fit) {
    const core::Num width_mm = page.spec.width_mm;
    const core::Num height_mm = page.spec.height_mm;
    const core::Rect bleed = page.bleed_rect_mm();  // (Python's dict makes both)
    const core::Rect trim = page.trim_rect_mm();
    core::Rect area{core::Num(0.0), core::Num(0.0), width_mm, height_mm};
    if (fit == "bleed") area = bleed;
    if (fit == "trim") area = trim;
    const double across = divided(width, area.width);
    double px_per_mm = core::py_min(across, divided(height, area.height));
    double scale = 1.0;
    const double longest = core::py_max(width_mm.value(), height_mm.value()) * px_per_mm;
    if (longest > kMaxSide) {
        scale = kMaxSide / longest;
        px_per_mm *= scale;
    }
    Placement out;
    out.page_w = std::max<std::int64_t>(1, core::py_round_int(width_mm.value() * px_per_mm));
    out.page_h = std::max<std::int64_t>(1, core::py_round_int(height_mm.value() * px_per_mm));
    out.shown_w = std::max<std::int64_t>(1, core::py_round_int(static_cast<double>(width) * scale));
    out.shown_h = std::max<std::int64_t>(1, core::py_round_int(static_cast<double>(height) * scale));
    const double fit_w = area.width.value() * px_per_mm;
    const double fit_h = area.height.value() * px_per_mm;
    out.at_x = core::py_round_int(area.x.value() * px_per_mm + (fit_w - static_cast<double>(out.shown_w)) / 2);
    out.at_y = core::py_round_int(area.y.value() * px_per_mm + (fit_h - static_cast<double>(out.shown_h)) / 2);
    return out;
}

// Image.paste(im, (x, y)): Pillow takes the box as C ints (OverflowError, which apply_ops lets through, past them).
Point paste_corner(const Placement& p) {
    for (const std::int64_t v : {p.at_x, p.at_y, p.at_x + p.shown_w, p.at_y + p.shown_h}) {
        if (v > INT_MAX) throw core::PyUncaught("OverflowError", "signed integer is greater than maximum");
        if (v < INT_MIN) throw core::PyUncaught("OverflowError", "signed integer is less than minimum");
    }
    return Point{static_cast<int>(p.at_x), static_cast<int>(p.at_y)};
}

// str[:80], by code points
std::string first_chars(const std::string& text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t n = 0; n < count && at < text.size(); ++n) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
    }
    return text.substr(0, at);
}

// --- the op -------------------------------------------------------------------------------------------------------

void import_psd(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t index = core::require_page(doc, op);
    std::string data;
    if (core::truthy_at(op, "psd")) {
        try {
            data = selection::b64decode(Json(core::py_str(op["psd"])));
        } catch (const core::Error&) {  // (binascii.Error and a str that is not ASCII: ValueError)
            throw OpError("psd is the file's bytes in base64");
        }
    } else if (core::truthy_at(op, "path")) {
        data = read_path(doc, op["path"]);
    } else {
        throw OpError("import_psd needs path or psd (base64)");
    }
    const std::string fit = core::truthy_at(op, "fit") ? core::py_str(op["fit"]) : std::string("bleed");
    if (fit != "paper" && fit != "bleed" && fit != "trim") throw OpError("fit must be paper, bleed or trim");
    std::optional<psd::File> read;
    try {
        read = psd::File::read(std::move(data));
    } catch (const psd::ReadError& error) {
        // (fileops catches PSDError, struct.error, ValueError and IndexError; a KeyError and a zlib.error go through)
        if (error.caught()) throw OpError(std::string("the PSD cannot be read (") + error.what() + ")");
        if (error.type() == "KeyError") throw core::OpKeyError(error.what());
        throw core::PyUncaught(error.type(), error.what());
    }
    const psd::File& file = *read;
    if (static_cast<std::uint64_t>(file.width()) * static_cast<std::uint64_t>(file.height()) > static_cast<std::uint64_t>(kMaxImagePixels)) {
        throw OpError("image too large: " + std::to_string(file.width()) + "x" + std::to_string(file.height()));
    }
    // the file's layers, or (none with a picture) its merged picture as one
    std::vector<psd::Layer> layers = file.layers();
    std::optional<Image> merged;
    if (std::all_of(layers.begin(), layers.end(), [](const psd::Layer& l) { return l.folder; })) {
        merged = file.merged();
        if (!merged) throw OpError("the PSD has no pictures to read");
        psd::Layer only;
        only.name = core::truthy_at(op, "name") ? core::py_str(op["name"]) : std::string("PSD");
        layers = {only};
    }
    const core::Page& page = doc.page(index);
    const Placement where = placed(page, file.width(), file.height(), fit);
    // (MAX_SIDE keeps the page layer within 12000 × 12000 pixels; only a paper of no positive size gets past it)
    if (where.page_w > kMaxPagePixels || where.page_h > kMaxPagePixels || where.page_w * where.page_h > kMaxPagePixels) {
        throw OpError("the page layer is too large (" + std::to_string(where.page_w) + "×" + std::to_string(where.page_h) + " pixels)");
    }
    // (a file of no area — 0 pixels high or wide — is shown at least a pixel across: as wide or as high as its header
    // says, which this build refuses past what an op makes; read_psd has refused a side past Pillow's)
    if (where.shown_w * where.shown_h > limits::kPixels) {
        throw OpError("the picture is too large (" + std::to_string(where.shown_w) + "×" + std::to_string(where.shown_h) +
                      " pixels; at most " + std::to_string(limits::kPixels) + ")");
    }
    const std::string prefix = core::truthy_at(op, "id") ? core::py_str(op["id"]) : core::new_id();
    const Json parent = core::truthy_at(op, "parent") ? Json(core::py_str(op["parent"])) : Json(nullptr);
    const std::int64_t mask_w = std::max<std::int64_t>(1, core::py_round_int(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const std::int64_t mask_h = std::max<std::int64_t>(1, core::py_round_int(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < layers.size(); ++i) ids.push_back(prefix + "-" + std::to_string(i + 1));
    const Size page_px{static_cast<int>(where.page_w), static_cast<int>(where.page_h)};
    const Size shown{static_cast<int>(where.shown_w), static_cast<int>(where.shown_h)};
    const bool same_size = where.shown_w == file.width() && where.shown_h == file.height();
    std::vector<core::Layer> made;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const psd::Layer& item = layers[i];
        core::Layer layer;
        layer.id = ids[i];
        layer.role = core::LayerRole::User;
        layer.title = first_chars(item.name, 80);
        layer.visible = item.visible;
        layer.opacity = core::py_round(core::py_max(0.0, core::py_min(1.0, item.opacity)), 3);
        layer.blend = item.blend;
        layer.clip = item.clip;
        layer.exportable = true;
        layer.parent_id = item.parent ? Json(ids[*item.parent]) : parent;
        if (item.folder) {
            layer.kind = core::LayerKind::Folder;
        } else {
            // (onto a clear layer: the pixels as they are)
            Image canvas = Image::create("RGBA", page_px, Ink{0, 0, 0, 0});
            const Image picture = merged ? *merged : file.image(item);
            canvas.paste(same_size ? picture : picture.resize(shown, Resample::Lanczos), paste_corner(where));
            layer.kind = core::LayerKind::Raster;
            // (a painting app's picture: on a monochrome page it prints in grey and tones)
            layer.source = Json::object({{"kind", "psd"}});
            layer.raster_png = fills::png_bytes(canvas);
            layer.raster_relpath = raster::page_folder(page) + "/user-" + layer.id + ".png";
            if (!merged) {
                if (const std::optional<Image> mask = file.mask(item)) {
                    limits::check_picture(mask_w, mask_h, "the layer mask");
                    const Size mask_size{static_cast<int>(mask_w), static_cast<int>(mask_h)};
                    Image full = Image::create("L", page_px, Ink(255));
                    full.paste(mask->resize(shown), paste_corner(where));
                    core::Mask kept;
                    kept.png = fills::png_bytes(full.resize(mask_size));
                    kept.enabled = true;
                    layer.mask = std::move(kept);
                }
            }
        }
        made.push_back(std::move(layer));
    }
    for (const core::Layer& item : page.layers) {
        for (const core::Layer& layer : made) {
            if (layer.id == item.id) throw OpError("layer " + prefix + " exists");
        }
    }
    core::Page& target = doc.edit_page(index);
    std::size_t at = target.layers.size();
    if (core::truthy_at(op, "after")) {
        const Json& after = op["after"];
        for (std::size_t i = 0; i < target.layers.size(); ++i) {
            if (after.is_string() && target.layers[i].id == after.get_ref<const std::string&>()) {
                at = i + 1;
                break;
            }
        }
    }
    Json listed = Json::array();
    for (const core::Layer& layer : made) listed.push_back(Json::object({{"id", layer.id}, {"title", layer.title}}));
    Json skipped = Json::array();
    for (const std::string& name : file.skipped()) skipped.push_back(name);
    target.layers.insert(target.layers.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(made.begin()),
                         std::make_move_iterator(made.end()));
    c.report = Json::object({{"layers", std::move(listed)}, {"skipped", std::move(skipped)}});
}

}  // namespace

void register_file_ops(core::OpRegistry& registry) { registry.add("import_psd", import_psd); }

}  // namespace genko::render
