// import_psd (Python's fileops.import_psd and fileops.placed): every layer of a PSD or PSB as a Genko layer on the page,
// read by render/psd (psd.read_psd). (PC3 A3, the user's decision 3: a placed layer whose file the PSD holds is a smart
// object of that file, Photoshop's picture of it shown until it changes.)

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <cstdlib>
#include <filesystem>
#include <fstream>
#endif

#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/psd_keep.hpp"
#include "render/import_psd.hpp"
#include "core/lut3d.hpp"
#include "core/masks.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/color_canvas.hpp"
#include "render/fill.hpp"
#include "render/import_profile.hpp"
#include "render/op_limits.hpp"
#include "core/patterns.hpp"
#include "render/ops_patterns.hpp"
#include "render/ops_registry.hpp"
#include "render/pat.hpp"
#include "render/ink_canvas.hpp"
#include "render/psd.hpp"
#include "render/psd_adjust.hpp"
#include "render/psd_filters.hpp"
#include "render/smart_filters.hpp"
#include "render/psd_keep.hpp"
#include "render/psd_paths.hpp"
#include "render/psd_shapes.hpp"
#include "render/psd_effects.hpp"
#include "render/psd_production.hpp"
#include "render/ops_annotations.hpp"
#include "core/layer_styles.hpp"
#include "render/raster.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/raw.hpp"
#include "render/selection.hpp"
#include "render/smart_decode.hpp"

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
// The pixels the op makes for the file's pixel layers in all, each a page layer (drawn, PNG-encoded and kept) and a
// picture as large as the canvas: 128 pictures of the largest size an op makes. A page of a book at 600 dpi with its
// bleed is some 37 (A4) to 55 (B4) million pixels a layer, so some 140 (B4) to 210 (A4) such layers are taken; a
// small file of many one-pixel layers on the largest canvas (32767 of them in 2.4 MB) is not, before any layer is made
// (Python goes on for hours, holding gigabytes of PNGs, until MemoryError).
constexpr double kMaxImportPixels = 128.0 * static_cast<double>(limits::kPixels);

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
#else
// (Windows: deferred with the rest of that build; not compiled yet)
// ntpath.expanduser of a path's first part ("~" or "~user"): USERPROFILE, else HOMEDRIVE and HOMEPATH; another user's
// home beside the current user's (USERNAME) profile folder; the part unchanged when there is no such home.
std::wstring expand_home(const std::wstring& first) {
    std::wstring home;
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) {
        home = profile;
    } else if (const wchar_t* path = _wgetenv(L"HOMEPATH")) {
        const wchar_t* drive = _wgetenv(L"HOMEDRIVE");
        home = (std::filesystem::path(drive != nullptr ? drive : L"") / path).wstring();
    } else {
        return first;
    }
    if (first.size() > 1) {  // (~user)
        const std::wstring user = first.substr(1);
        const wchar_t* current = _wgetenv(L"USERNAME");
        if (current == nullptr || user != current) {
            const std::filesystem::path own(home);
            if (current == nullptr || own.filename().wstring() != current) return first;
            home = (own.parent_path() / user).wstring();
        }
    }
    return home;
}

// Path.read_bytes() with its errors as the POSIX build's (ApplyError "the file cannot be read (…)"). Beyond Python, a
// file larger than kMaxFileBytes is refused before it is read.
std::string read_bytes(const std::filesystem::path& source) {
    std::ifstream in(source, std::ios::binary);
    if (!in) throw OpError("the file cannot be read (" + core::path_to_utf8(source) + ")");
    const auto too_large = [] { throw OpError("the file cannot be read (it is larger than 2 GiB)"); };
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size > 0 && static_cast<std::uint64_t>(size) > kMaxFileBytes) too_large();
    in.clear();
    in.seekg(0, std::ios::beg);
    std::string out;
    char buffer[1 << 16];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
        if (out.size() > kMaxFileBytes) too_large();
    }
    if (in.bad()) throw OpError("the file cannot be read (" + core::path_to_utf8(source) + ")");
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
    // (Windows: deferred with the rest of that build; as the POSIX build, with ntpath's "~" and std::filesystem's
    // paths)
    std::filesystem::path source = core::path_from_utf8(core::py_str(value));
    if (!source.has_root_name() && !source.has_root_directory() && !source.empty()) {  // (no drive and no root)
        auto part = source.begin();
        if (part->wstring().starts_with(L"~")) {
            const std::wstring home = expand_home(part->wstring());
            if (home.starts_with(L"~")) throw core::PyUncaught("RuntimeError", "Could not determine home directory.");
            std::filesystem::path expanded(home);
            for (++part; part != source.end(); ++part) expanded /= *part;
            source = std::move(expanded);
        }
    }
    if (source.is_relative() && doc.asset_dir) source = *doc.asset_dir / source;  // (relative to the book's folder)
    return read_bytes(source);
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
// (PC4 F-d, `exact`: a page made of the file — its paper the file's pixels at its resolution: the page layer the file's
// pixels, whatever their size)
Placement placed(const core::Page& page, std::int64_t width, std::int64_t height, const std::string& fit, bool exact = false) {
    if (exact) {
        Placement out;
        out.page_w = out.shown_w = std::max<std::int64_t>(1, width);
        out.page_h = out.shown_h = std::max<std::int64_t>(1, height);
        return out;
    }
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

// --- placed layers as smart objects (PC3 A3) ---------------------------------------------------------------------

// What a placed layer becomes: a smart object of the file the PSD holds for it, or (why not) its pixels as before.
struct SmartMade {
    std::string contents;  // its contents' id
    Json settings;         // the layer's "smart" (without its psd: the import adds it)
    std::string why;       // (none made: no_file, external, too_large, unreadable, flat)
};

// The page's mm of a point of the PSD's canvas laid on the page (`where`).
std::pair<double, double> on_page(const core::Page& page, const Placement& where, std::int64_t width, std::int64_t height, double x, double y) {
    const double fx = page.spec.width_mm.value() / static_cast<double>(where.page_w);
    const double fy = page.spec.height_mm.value() / static_cast<double>(where.page_h);
    const double sx = static_cast<double>(where.shown_w) / static_cast<double>(std::max<std::int64_t>(1, width));
    const double sy = static_cast<double>(where.shown_h) / static_cast<double>(std::max<std::int64_t>(1, height));
    return {fx * (static_cast<double>(where.at_x) + sx * x), fy * (static_cast<double>(where.at_y) + sy * y)};
}

// The file a placed layer shows, as smart object contents (the same contents for every layer of the same uuid: Photoshop's
// instances): its bytes kept as they are (Document::smart_files), its linked-layer item without them kept to be written
// back (contents' psd: {"uuid", "item", "at"}), and its colour use the op's.
std::optional<SmartMade> smart_contents_of(core::Document& doc, const psd::File& file, const psd::Layer& item, const Json& op,
                                           const import_profile::Taking& way, std::map<std::string, std::string>& made) {
    SmartMade out;
    const psd::Placed& placed = *item.placed;
    if (const auto known = made.find(placed.uuid); known != made.end()) {
        out.contents = known->second;
        return out;
    }
    const std::optional<psd::LinkedItem> linked = file.linked(placed.uuid);
    if (!linked) {
        out.why = "no_file";
        return out;
    }
    if (linked->kind != "liFD") {
        out.why = "external";
        return out;
    }
    if (linked->data.size() > smart::kFileMaxBytes) {
        out.why = "too_large";
        return out;
    }
    smart::FileInfo info;
    try {
        // (PC4 F-e: a TIFF-shaped camera RAW — NEF, ARW… — is known by its name's suffix alone)
        const auto named_raw = core::smart::suffix_of_name(linked->name);
        info = smart::look_at(linked->data, named_raw && raw::raw_suffix(*named_raw) ? *named_raw : std::string());
    } catch (const NotYetPorted&) {
        out.why = "unreadable";
        return out;
    } catch (const core::Error&) {
        out.why = "unreadable";
        return out;
    }
    if (info.width > core::smart::kSideMax || info.height > core::smart::kSideMax) {
        out.why = "too_large";
        return out;
    }
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "file";
    std::string name = linked->name;
    const auto named = core::smart::suffix_of_name(name);
    contents->suffix = named && core::smart::format_of_suffix(*named) == info.format ? *named : info.suffix;
    if (name.empty()) name = (item.name.empty() ? std::string("スマートオブジェクト") : item.name) + contents->suffix;
    contents->name = first_chars(name, core::smart::kNameMax);
    const std::string bytes(linked->data);
    contents->asset = core::bytes_ref(bytes);
    doc.smart_files[contents->asset] = std::make_shared<const std::string>(bytes);
    contents->format = info.format;
    contents->width_px = info.width;
    contents->height_px = info.height;
    const auto usable = [](double dpi) { return std::isfinite(dpi) && dpi >= 1 && dpi <= 100000; };
    contents->dpi = usable(info.dpi) ? info.dpi : usable(placed.resolution) ? placed.resolution : usable(file.dpi()) ? file.dpi() : 72.0;
    contents->depth = info.depth;
    contents->mode = info.mode;
    // (its colours as the op takes the file's: kept, from the profile it carries, or the op's)
    contents->profile = Json::object({{"use", "keep"}});
    if (op.contains("embedded_profile") && way.way != "keep" && way.converting) {
        Json use = Json::object();
        if (way.way == "convert" && info.icc) {
            use["use"] = "embedded";
        } else if (way.way == "assign" && way.from) {
            const std::string ref = core::bytes_ref(*way.from);
            doc.smart_files[ref] = way.from;
            use["use"] = "assign";
            use["icc"] = ref;
        }
        if (!use.empty()) {
            use["intent"] = way.intent;
            use["bpc"] = way.bpc;
            contents->profile = std::move(use);
        }
    }
    if (info.format == "raw") {
        // (PC4 F-e, camera-raw@1: Photoshop's Camera Raw settings are not read — Genko's default development, its colours
        // its output space's —; Photoshop's picture of it shown until it is developed again: the import keeps `shown`)
        try {
            contents->raw = raw::default_raw(bytes);
        } catch (const core::Error&) {
            out.why = "unreadable";
            return out;
        }
        contents->profile = Json::object({{"use", "embedded"}, {"intent", "relative"}, {"bpc", true}});
    }
    // (the item without the file's bytes, and where they go: written back as it was while the contents stay)
    const std::size_t at = static_cast<std::size_t>(linked->data.data() - linked->item.data());
    std::string rest(linked->item.substr(0, at));
    rest += linked->item.substr(at + linked->data.size());
    const std::string rest_ref = core::bytes_ref(rest);
    doc.smart_files[rest_ref] = std::make_shared<const std::string>(std::move(rest));
    contents->psd = Json{{"uuid", placed.uuid}, {"item", rest_ref}, {"at", static_cast<std::int64_t>(at)}};
    out.contents = smart_ops::fresh_contents_id(doc);
    doc.smart_contents[out.contents] = std::move(contents);
    made[placed.uuid] = out.contents;
    return out;
}

// A placed layer's smart object settings: its matrix (the contents' mm onto the page's, through its corners on the
// canvas and the canvas's place on the page), its corners on the page when they are not a parallelogram, its warp (in the
// contents' mm). None (`why` flat) when its corners squash it or are not convex.
SmartMade smart_settings(const core::Document& doc, const core::Page& page, const Placement& where, const psd::File& file, const psd::Placed& placed,
                         SmartMade made) {
    const core::SmartContents& contents = *doc.smart_contents.at(made.contents);
    const double w = static_cast<double>(contents.width_px), h = static_cast<double>(contents.height_px);
    const std::array<double, 8>& p = placed.corners;
    const double k = contents.dpi / 25.4;  // (the contents' px a mm)
    const auto [x0, y0] = on_page(page, where, file.width(), file.height(), p[0], p[1]);
    const auto [x1, y1] = on_page(page, where, file.width(), file.height(), p[2], p[3]);
    const auto [x3, y3] = on_page(page, where, file.width(), file.height(), p[6], p[7]);
    core::smart::Matrix m{(x1 - x0) / w * k, (y1 - y0) / w * k, (x3 - x0) / h * k, (y3 - y0) / h * k, x0, y0};
    m = core::smart::snapped(m);
    if (!(std::abs(m.det()) > core::smart::kDetMin)) {
        made.why = "flat";
        return made;
    }
    Json settings = core::smart::settings(made.contents, m, "lanczos");
    const double size = std::max({1.0, std::abs(p[0]), std::abs(p[1]), std::abs(p[4]), std::abs(p[5])});
    if (!psd::parallelogram(p, 1e-9 * size)) {
        Json corners = Json::array();
        std::array<std::pair<double, double>, 4> q;
        for (std::size_t i = 0; i < 4; ++i) {
            q[i] = on_page(page, where, file.width(), file.height(), p[2 * i], p[2 * i + 1]);
            corners.push_back(Json::array({q[i].first, q[i].second}));
        }
        int sign = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& [ax, ay] = q[i];
            const auto& [bx, by] = q[(i + 1) % 4];
            const auto& [cx, cy] = q[(i + 2) % 4];
            const double cross = (bx - ax) * (cy - by) - (by - ay) * (cx - bx);
            const int here = cross > 1e-9 ? 1 : cross < -1e-9 ? -1 : 0;
            if (here == 0 || (sign != 0 && here != sign)) {
                made.why = "flat";
                return made;
            }
            sign = here;
        }
        settings["perspective"] = std::move(corners);
    }
    if (placed.warp) {  // (its box and mesh from the contents' units — Sz, else their pixels — to their mm)
        const double ux = placed.width > 0 ? w / placed.width : 1.0, uy = placed.height > 0 ? h / placed.height : 1.0;
        Json warp = *placed.warp;
        Json& box = warp["bounds"];
        for (std::size_t i = 0; i < 4; ++i) box[i] = box[i].get<double>() * (i % 2 == 0 ? ux : uy) / k;
        if (warp.contains("mesh")) {
            for (Json& point : warp["mesh"]["points"]) point = Json::array({point[0].get<double>() * ux / k, point[1].get<double>() * uy / k});
        }
        // (PC3 A5: its bend and distortions within Photoshop's −100..100 %; a warp this build cannot keep — bounds without a
        // size — left out: Photoshop's picture of it shows until it changes)
        for (const char* key : {"bend", "h_distort", "v_distort"}) {
            if (warp.contains(key) && warp[key].is_number()) warp[key] = std::clamp(warp[key].get<double>(), -100.0, 100.0);
        }
        try {
            core::smart::check_warp(warp);
            settings["warp"] = std::move(warp);
        } catch (const core::Error&) {
        }
    }
    made.settings = std::move(settings);
    return made;
}

// --- psd-keep@1 and the layers at their depth (PC4 F-d) ----------------------------------------------------------

// Bytes kept with the book (Document::native_files, written as assets with their suffix), by their asset ref.
std::string kept(core::Document& doc, std::string_view bytes) {
    const std::string ref = core::bytes_ref(bytes);
    if (!doc.native_files.contains(ref)) doc.native_files.emplace(ref, std::make_shared<const std::string>(bytes));
    return ref;
}

// Blending ranges all at their default (each channel's source and destination 0..255: 00 00 ff ff twice).
bool default_ranges(std::string_view ranges) {
    if (ranges.size() % 8 != 0) return false;
    static constexpr std::string_view kFull("\x00\x00\xff\xff\x00\x00\xff\xff", 8);
    for (std::size_t at = 0; at < ranges.size(); at += 8) {
        if (ranges.substr(at, 8) != kFull) return false;
    }
    return true;
}

// A Pascal name's bytes as text (each byte the code point of its value: the writer gives the same bytes back).
std::string latin1_text(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x80) {
            out += c;
        } else {
            out += static_cast<char>(0xC0 | (b >> 6));
            out += static_cast<char>(0x80 | (b & 0x3F));
        }
    }
    return out;
}

// What is kept of one file in all (its blocks and resources together): past it, the rest is not kept (said: `cut`).
constexpr std::uint64_t kKeptBytesMax = std::uint64_t{1} << 30;

// What one import keeps, and whether anything was left out (more blocks or resources than a layer or a page keeps, one
// past kPsdBlockMaxBytes, all past kKeptBytesMax: the report's psd_blocks_cut).
struct Keeping {
    std::uint64_t bytes = 0;
    bool cut = false;
    // May `size` more bytes be kept as the `count`th of a list of at most kPsdKeptMax? (Else it is cut.)
    bool room(std::size_t count, std::uint64_t size) {
        if (count >= core::kPsdKeptMax || size > core::kPsdBlockMaxBytes || size > kKeptBytesMax - bytes) {
            cut = true;
            return false;
        }
        bytes += size;
        return true;
    }
};

// What the file's page holds that this build does not read (the page's resources and global blocks), the patterns of its
// pattern blocks a fill took into the book left out of them (render/psd_keep's entries) — none when there is none. Its
// layer comps and slices not when they may name a layer not made (`named_gone`: one not read, or the merged picture read).
core::PsdPageKeep page_keep(core::Document& doc, const psd::File& file, const std::set<std::string>& patterns_taken, bool named_gone, Keeping& keeping) {
    core::PsdPageKeep keep;
    const std::vector<psd::File::ImageResource> resources = file.resources();
    if (resources.size() > core::kPsdKeptMax) keeping.cut = true;  // (the reader holds one more than a page keeps)
    for (const psd::File::ImageResource& r : resources) {
        if (!core::psd_resource_kept(static_cast<int>(r.id))) continue;
        if (named_gone && core::psd_resource_names_layers(static_cast<int>(r.id))) continue;
        if (!keeping.room(keep.resources.size(), r.data.size())) continue;
        keep.resources.push_back(core::PsdResource{static_cast<int>(r.id), latin1_text(r.name), kept(doc, r.data)});
    }
    const std::vector<psd::File::TaggedBlock> globals = file.global_blocks();
    if (globals.size() > core::kPsdKeptMax) keeping.cut = true;
    for (const psd::File::TaggedBlock& g : globals) {
        if (!core::psd_global_kept(g.key)) continue;
        if (core::psd_pattern_key(g.key)) {  // (the patterns no fill took)
            std::vector<psd_keep::PatternEntry> entries = psd_keep::pattern_entries(g.data);
            std::erase_if(entries, [&](const psd_keep::PatternEntry& e) { return patterns_taken.contains(e.id); });
            if (entries.empty()) continue;
            const std::string payload = psd_keep::pattern_payload(entries);
            if (!keeping.room(keep.globals.size(), payload.size())) continue;
            keep.globals.push_back(core::PsdGlobal{g.signature, g.key, kept(doc, payload)});
            continue;
        }
        if (!keeping.room(keep.globals.size(), g.data.size())) continue;
        keep.globals.push_back(core::PsdGlobal{g.signature, g.key, kept(doc, g.data)});
    }
    return keep;
}

// A layer's own (its blocks this build keeps — its locks' lspf only when it locks something: its bits this build does not
// read kept so —, its blending ranges unless all at their default, its layer id): none when it has nothing and neither
// its locks nor its page's (`ids`: its page keeps what names layers by their ids) ask for its id.
std::optional<core::PsdLayerKeep> layer_keep(core::Document& doc, const psd::File& file, const psd::Layer& item, bool ids, bool& effects, Keeping& keeping) {
    core::PsdLayerKeep keep;
    if (file.blocks_cut(item)) keeping.cut = true;
    for (const psd::File::TaggedBlock& b : file.blocks(item)) {
        if (!core::psd_layer_block_kept(b.key)) continue;
        if (b.key.size() != 4 || std::any_of(b.key.begin(), b.key.end(), [](char ch) { return ch < 0x20 || ch >= 0x7f; })) continue;
        if (b.key == "lspf" && b.data.find_first_not_of('\0') == std::string_view::npos) continue;  // (no lock: nothing to keep)
        if (!keeping.room(keep.blocks.size(), b.data.size())) continue;
        keep.blocks.emplace_back(b.key, kept(doc, b.data));
        effects = effects || (core::psd_effect_key(b.key) && !item.folder);  // (a folder's are not written back: no mark)
    }
    if (const std::string_view ranges = file.ranges(item); !ranges.empty() && !default_ranges(ranges) && keeping.room(0, ranges.size())) {
        keep.ranges = kept(doc, ranges);
    }
    const auto locks = file.protection(item);
    const auto id = file.layer_id(item);
    if (keep.blocks.empty() && keep.ranges.empty() && !(ids && id) && !(locks && *locks != 0)) return std::nullopt;
    if (id) keep.id = static_cast<std::int64_t>(*id);
    return keep;
}

// layer-styles@1 (PC4 E6): the colour channels of a file's mode as Blend If names them.
std::vector<std::string> blend_if_names(int mode) {
    if (mode == 3) return {"red", "green", "blue"};
    if (mode == 4) return {"cyan", "magenta", "yellow", "black"};
    if (mode == 9) return {"lightness", "a", "b"};
    if (mode == 1 || mode == 8) return {"gray"};
    return {};
}

// layer-styles@1 (PC4 E6): a record's effects (lmfx, else lfx2), fill opacity (iOpa) and Blend If (its blending ranges) as
// the layer's style (render/psd_effects) — the effects' blocks still kept, written back as they were while the style is
// unchanged; the fill opacity and the ranges read so not kept besides. `light`: the file's global light when it is not
// the book's (an effect that takes the global light takes this one as its own). Whether a style was made. `budget`: the file's one descriptor budget (its smart filters' too).
bool style_from_record(core::Layer& layer, const psd::File& file, const psd::Layer& item, const psd_shapes::Placement& placement,
                       const std::optional<std::array<double, 2>>& light, std::vector<std::string>& notes, std::size_t& budget) {
    std::string_view effects;
    bool multi = false;
    std::optional<double> fill;
    for (const psd::File::TaggedBlock& b : file.blocks(item)) {
        if (b.key == "lmfx") {
            effects = b.data;
            multi = true;
        } else if (b.key == "lfx2" && !multi) {
            effects = b.data;
        } else if (b.key == "iOpa") {
            fill = psd_effects::fill_of_iopa(b.data);
        }
    }
    Json style = Json{{"effects", Json::array()}};
    if (!effects.empty()) {
        if (std::optional<Json> read = psd_effects::style_of(effects, placement, notes, &budget)) style = std::move(*read);
    }
    const bool filled = fill && *fill < 1.0;
    if (filled) style["fill_opacity"] = *fill;
    Json blend_if = Json::array();
    if (const std::string_view ranges = file.ranges(item); !ranges.empty() && !blend_if_names(file.mode()).empty()) {
        blend_if = psd_effects::blend_if_of(ranges, blend_if_names(file.mode()));
        if (!blend_if.empty()) style["blend_if"] = blend_if;
    }
    bool relit = false;
    if (light) {  // (the book has another light: the file's taken by each effect that took it)
        for (Json& e : style["effects"]) {
            if (!e.value("global_light", false)) continue;
            e["global_light"] = false;
            e["angle"] = (*light)[0];
            if (e.contains("altitude")) e["altitude"] = (*light)[1];
            relit = true;
        }
    }
    Json normal;
    try {
        normal = core::layer_style_spec(style);
    } catch (const core::Error& error) {
        notes.push_back(std::string("レイヤースタイルを読めませんでした（") + error.what() + "）");
        return false;
    }
    if (normal.is_null()) return false;
    layer.style = std::move(normal);
    if (layer.native_psd) {
        // (review P3-2: an iOpa of 255 says no fill opacity — kept to be written back as it was; one read as the style's
        // fill opacity is written from it)
        if (filled) std::erase_if(layer.native_psd->blocks, [](const auto& block) { return block.first == "iOpa"; });
        // (review P2-7: effects that took the file's global light now hold its angle as their own — the file's effects
        // block, written back, would take the book's: forgotten, the effects written from the style)
        if (relit) std::erase_if(layer.native_psd->blocks, [](const auto& block) { return core::psd_effect_key(block.first); });
        // (the ranges kept as they are: written back while they say the style's Blend If)
        if (layer.native_psd->empty()) layer.native_psd.reset();
    }
    return true;
}

// Photoshop's locks (lspf: bit 0 transparency, 1 the pixels, 2 the position, 31 all) as the layer's own.
void take_locks(core::Layer& layer, std::uint32_t locks) {
    const bool all = (locks & 0x80000000u) != 0;
    layer.lock_alpha = all || (locks & 1u) != 0;
    layer.locked = all || (locks & 2u) != 0;
    layer.lock_position = all || (locks & 4u) != 0;
}

// The way a layer's pixels come in at their depth (the user's decision 3: the layers at the file's depth): a page in
// precise colour of an RGB file of its depth, 16 or 32 bits (its values its colour raster's, as the numbers are taken:
// kept), a page of inks of a grey file of its depth (16, 32), of a CMYK or Lab file of 16 bits in its own mode (a CMYK
// file's plates as they are: it carries no profile, or the page's) — each when the picture is not resampled. Else 8 bits,
// as before (a file of another depth than its page's too: a 32-bit file onto a 16-bit page is read as before F-d).
enum class DeepWay { None, Rgb, Inks };
DeepWay deep_way(const core::Page& page, const psd::File& file, bool converting, bool same_size) {
    if (psd::python_view() || converting || !same_size || (file.depth() != 16 && file.depth() != 32)) return DeepWay::None;
    if (page.rgb && !page.color && page.rgb->depth == file.depth() && !page.rgb->table && file.mode() == 3) return DeepWay::Rgb;
    if (!page.color || page.color->depth != file.depth() || page.color->bitmap || !page.color->duotone.empty()) return DeepWay::None;
    const core::PageColor& color = *page.color;
    if (file.mode() == 1 && color.space == core::InkSpace::Gray) return DeepWay::Inks;
    if (file.depth() != 16) return DeepWay::None;  // (Photoshop has no 32-bit CMYK or Lab: one is read as before)
    if (file.mode() == 9 && color.space == core::InkSpace::Lab) return DeepWay::Inks;
    if (file.mode() == 4 && color.space == core::InkSpace::Cmyk && (file.icc().empty() || (color.icc && *color.icc == file.icc()))) return DeepWay::Inks;
    return DeepWay::None;
}

// Is the page's profile linear light (the built-in linear-srgb: an OpenEXR's page, or a 32-bit PSD's opened)?
bool linear_page(const core::Page& page) {
    if (!page.rgb || !page.rgb->icc) return false;
    const core::BuiltinProfile* linear = core::builtin_profile("linear-srgb");
    return linear != nullptr && linear->bytes && *linear->bytes == *page.rgb->icc;
}

// A 32-bit file's samples are linear light (Photoshop's 32 bits): taken in at the file's depth they are encoded by the
// page's own tone curve (PC5-0, the user's decision of 2026-10-28: the inverse of what this build writes, render's
// RgbSpace and GrayCurve) — as they are on a page of linear light —, values past 0..1 by the same formula.

// A part of the file (a layer's box, or the merged picture) on the page layer (`size` pixels, the file's canvas at `at`),
// as a page of inks of the page's depth holds it: each sample as it is (a 32-bit grey file's encoded by the page's
// curve), clear around it.
core::InkPixels deep_inks(const psd::File::DeepPart& part, const core::PageColor& color, Size size, Point at) {
    const std::int64_t width = std::max(1, size.width), height = std::max(1, size.height);
    core::InkPixels canvas = core::blank_ink_pixels(color.space, color.depth, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    const unsigned colours = canvas.colours();
    const bool as_is = part.depth != 32;
    const GrayCurve curve = as_is ? GrayCurve() : GrayCurve(color);
    const auto encoded = [&](float v) { return as_is ? static_cast<double>(v) : finite_sample(curve.encode(v)); };
    const std::int64_t left = part.left + at.x, top = part.top + at.y;
    for (std::int64_t y = std::max<std::int64_t>(0, top); y < std::min(height, top + part.height); ++y) {
        for (std::int64_t x = std::max<std::int64_t>(0, left); x < std::min(width, left + part.width); ++x) {
            const std::size_t from = static_cast<std::size_t>((y - top) * part.width + (x - left));
            const std::size_t to = static_cast<std::size_t>(y * width + x);
            for (unsigned c = 0; c < colours && c < part.colours; ++c) canvas.set(to, c, encoded(part.value(c, from)));
            canvas.set(to, colours, std::clamp(part.value(part.colours, from), 0.0f, 1.0f));
        }
    }
    return canvas;
}

// The same as a colour raster of the page's depth (u16 or f32: the values as they are — a 32-bit file's encoded by the
// page's curves unless the page is linear —, past 0..1 kept at 32 bits).
std::string deep_color_raster(const psd::File::DeepPart& part, const core::Page& page, Size size, Point at, bool linear) {
    const int depth = page.rgb->depth;
    const auto w = static_cast<std::uint32_t>(std::max(1, size.width)), h = static_cast<std::uint32_t>(std::max(1, size.height));
    const bool as_is = part.depth != 32 || linear;
    const RgbSpace space = as_is ? RgbSpace() : RgbSpace::of(page);
    const auto encoded = [&](unsigned c, float v) { return as_is ? static_cast<double>(v) : finite_sample(space.encode(c, v)); };
    return core::encode_color_pixels(w, h, depth == 16 ? "u16" : "f32", [&](std::size_t i) {
        const std::int64_t x = static_cast<std::int64_t>(i % w) - part.left - at.x, y = static_cast<std::int64_t>(i / w) - part.top - at.y;
        if (x < 0 || y < 0 || x >= part.width || y >= part.height) return std::array<double, 4>{0.0, 0.0, 0.0, 0.0};
        const auto k = static_cast<std::size_t>(y * part.width + x);
        return std::array<double, 4>{encoded(0, part.value(0, k)), encoded(1, part.value(1, k)), encoded(2, part.value(2, k)),
                                     std::clamp(static_cast<double>(part.value(3, k)), 0.0, 1.0)};
    });
}

// PC5-0 (Z0's review P2-1): a 32-bit RGB or grey file's pixel layer as 8 bits over the whole canvas ("RGBA", as
// File::image gives it) — its linear colours encoded by the tone curve of the file's profile (encode_by_profile: the
// inverse of the 32-bit PSD this build writes; Python's view reads them by 1/2.2), its transparency as it is —; nothing
// for another file (or one whose layer is not read so).
std::optional<Image> eight_bits_of_32(const psd::File& file, const psd::Layer& item) {
    if (psd::python_view() || file.depth() != 32 || (file.mode() != 3 && file.mode() != 1)) return std::nullopt;
    const std::optional<psd::File::DeepPart> part = file.layer_deep(item);
    if (!part || (part->colours != 3 && part->colours != 1)) return std::nullopt;
    const unsigned colours = part->colours, all = colours + 1;
    const std::size_t n = static_cast<std::size_t>(std::max<std::int64_t>(0, part->width)) * static_cast<std::size_t>(std::max<std::int64_t>(0, part->height));
    std::vector<float> samples(n * all);
    for (std::size_t i = 0; i < n; ++i)
        for (unsigned c = 0; c < all; ++c) samples[i * all + c] = part->value(c, i);
    encode_by_profile(samples, all, true, file.icc());
    const auto byte = [](float v) { return static_cast<unsigned char>(std::lround(std::clamp(std::isnan(v) ? 0.0f : v, 0.0f, 1.0f) * 255.0f)); };
    const std::int64_t width = std::max<std::int64_t>(1, file.width()), height = std::max<std::int64_t>(1, file.height());
    std::string rgba(static_cast<std::size_t>(width * height) * 4, '\0');
    for (std::int64_t y = std::max<std::int64_t>(0, part->top); y < std::min(height, part->top + part->height); ++y) {
        for (std::int64_t x = std::max<std::int64_t>(0, part->left); x < std::min(width, part->left + part->width); ++x) {
            const std::size_t from = static_cast<std::size_t>((y - part->top) * part->width + (x - part->left)) * all;
            char* to = rgba.data() + static_cast<std::size_t>(y * width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) to[c] = static_cast<char>(byte(samples[from + (colours == 1 ? 0 : c)]));
            to[3] = static_cast<char>(byte(samples[from + colours]));
        }
    }
    return Image::frombytes("RGBA", Size{static_cast<int>(width), static_cast<int>(height)}, rgba);
}

// The bytes of the precise colour rasters the book holds (each allocation once, as core::validate_color_document counts).
std::uint64_t book_color_bytes(const core::Document& doc) {
    std::uint64_t bytes = 0;
    std::set<const std::string*> seen;
    for (const auto& page : doc.pages) {
        if (!page) continue;
        for (const core::Layer& layer : page->layers) {
            if (layer.color_raster && seen.insert(layer.color_raster.get()).second) bytes += layer.color_raster->size();
        }
    }
    return bytes;
}

// --- the op -------------------------------------------------------------------------------------------------------

// import_psd's work, of the file's bytes (render/import_psd.hpp): its report.
Json import_data(core::Document& doc, const Json& op, std::string data, bool exact) {
    const std::size_t index = core::require_page(doc, op);
    const std::string fit = core::truthy_at(op, "fit") ? core::py_str(op["fit"]) : std::string("bleed");
    if (fit != "paper" && fit != "bleed" && fit != "trim") throw OpError("fit must be paper, bleed or trim");
    // (color-mode@1: onto a CMYK or Lab page the layers are the page's inks, and a Lab file is read too)
    const std::optional<core::PageColor> mode = doc.page(index).color;
    std::optional<psd::File> read;
    try {
        read = psd::File::read(std::move(data), psd::default_limits(), mode.has_value());
    } catch (const psd::ReadError& error) {
        // (fileops catches PSDError, struct.error, ValueError and IndexError; a KeyError and a zlib.error go through)
        if (error.caught()) throw OpError(std::string("the PSD cannot be read (") + error.what() + ")");
        if (error.type() == "KeyError") throw core::OpKeyError(error.what());
        throw core::PyUncaught(error.type(), error.what());
    }
    const psd::File& file = *read;
    // (PC2d, the C++ build's own embedded_profile: an RGB or grey file's colours taken from its own profile (image
    // resource 1039) or the op's into the page's space. A CMYK or Lab file comes in as before — onto a page of inks
    // through its own profile —: one cannot be assigned another.)
    import_profile::Taking way;
    std::string numbers;  // (the mode of the file's numbers as the pictures hold them: "L" grey, "RGBA")
    if (op.contains("embedded_profile")) {
        const std::string& m = file.mode_name();
        numbers = m == "gray" || m == "bitmap" || m == "duotone" ? "L" : m == "rgb" || m == "indexed" ? "RGBA" : "";
        if (!numbers.empty()) {
            way = import_profile::taking(doc, doc.page(index), op, file.icc().empty() ? std::nullopt : std::optional<std::string>(file.icc()), numbers);
        } else if (op["embedded_profile"] == "assign") {
            throw OpError("embedded_profile assign takes an RGB or grey PSD (a " + m + " file comes in through its own profile)");
        } else {
            way = import_profile::taking(doc, doc.page(index), op, std::nullopt, "LAB");  // (its arguments checked; kept)
        }
    }
    // (a picture of the file as the way takes it: its numbers, a grey file's as grey)
    const auto numbers_of = [&](const Image& picture) { return numbers == "L" ? picture.convert("LA") : picture; };
    if (static_cast<std::uint64_t>(file.width()) * static_cast<std::uint64_t>(file.height()) > static_cast<std::uint64_t>(kMaxImagePixels)) {
        throw OpError("image too large: " + std::to_string(file.width()) + "x" + std::to_string(file.height()));
    }
    // the file's layers, or (none with a picture: folders and adjustment layers only) its merged picture as one, as
    // Python reads it — its adjustment layers then left out (skipped), as psd.py leaves them: the merged picture shows
    // them already, laid over it again they would be laid twice (PC2, the user's decision 5)
    std::vector<psd::Layer> layers = file.layers();
    std::optional<Image> merged;
    std::optional<core::InkPixels> merged_inks;
    std::optional<psd::File::DeepPart> merged_deep;  // (PC4 F-d: at its depth)
    bool merged_only = false;
    const core::Page& page = doc.page(index);
    // (PC4 F-d: the layers at their depth, when the page holds them so and the picture is not resampled — where it lands
    // worked out first only then, the errors in Python's order otherwise)
    std::optional<Placement> early;
    DeepWay deep = deep_way(page, file, way.converting, true);
    bool resampled = false;
    if (deep != DeepWay::None) {
        early = placed(page, file.width(), file.height(), fit, exact);
        if (early->shown_w != file.width() || early->shown_h != file.height()) {
            deep = DeepWay::None;
            resampled = true;
        }
    }
    // (the PC4 F-d review: a precise colour raster is the page's size, uncompressed, a layer — those of the layers to be
    // made at their depth, with what the book holds already, kept within the book's high-precision budget
    // (core::kColorRasterBookBytes) before one is made: past it, the layers come in at 8 bits, as before, and it is said)
    bool budget = false;
    if (deep == DeepWay::Rgb) {
        const std::uint64_t sample = file.depth() == 16 ? 2 : 4;
        const std::uint64_t each_bytes = 16 + static_cast<std::uint64_t>(std::max<std::int64_t>(1, early->page_w)) *
                                                  static_cast<std::uint64_t>(std::max<std::int64_t>(1, early->page_h)) * 4 * sample;
        const auto deep_layers = static_cast<std::uint64_t>(std::max<std::ptrdiff_t>(
            1, std::count_if(layers.begin(), layers.end(), [](const psd::Layer& l) { return !l.folder && l.kind != "adjust" && l.kind != "smart"; })));
        const std::uint64_t held = book_color_bytes(doc);
        if (held > core::kColorRasterBookBytes || deep_layers > (core::kColorRasterBookBytes - held) / each_bytes) {
            deep = DeepWay::None;
            budget = true;
        }
    }
    // (why a file deeper than 8 bits onto a page that is deeper too came in at 8 bits: said when the page is made of the
    // file, and past the budget — the op's report otherwise as before)
    const int page_depth = page.color ? page.color->depth : page.rgb ? page.rgb->depth : 8;
    std::string shallow;
    if (!psd::python_view() && deep == DeepWay::None && file.depth() > 8 && page_depth > 8 && (exact || budget)) {
        shallow = budget ? "budget" : way.converting ? "converting" : file.depth() != page_depth ? "depth" : resampled ? "resampled" : "mode";
    }
    // (psd-keep@1: the layer ids of the file's layers — the comps and slices name them —, before the merged picture may
    // take their place)
    std::set<std::int64_t> file_ids;
    for (const psd::Layer& l : layers) {
        if (const auto id = file.layer_id(l)) file_ids.insert(static_cast<std::int64_t>(*id));
    }
    if (std::all_of(layers.begin(), layers.end(), [](const psd::Layer& l) { return l.folder || l.kind == "adjust"; })) {
        merged_only = true;
        if (deep != DeepWay::None) {
            merged_deep = file.merged_part();
            if (!merged_deep) throw OpError("the PSD has no pictures to read");
        } else if (mode) {
            merged_inks = file.merged_inks(*mode);
            if (!merged_inks) throw OpError("the PSD has no pictures to read");
        } else {
            merged = file.merged();
            if (!merged) throw OpError("the PSD has no pictures to read");
        }
        psd::Layer only;
        only.name = core::truthy_at(op, "name") ? core::py_str(op["name"]) : std::string("PSD");
        layers = {only};
    }
    const Placement where = early ? *early : placed(page, file.width(), file.height(), fit, exact);
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
    // (the op as a whole, before any layer is made: kMaxImportPixels)
    const auto pictures = std::count_if(layers.begin(), layers.end(), [](const psd::Layer& l) { return !l.folder; });
    const double each = static_cast<double>(where.page_w) * static_cast<double>(where.page_h) +
                        static_cast<double>(file.width()) * static_cast<double>(file.height());
    if (static_cast<double>(pictures) * each > kMaxImportPixels) {
        throw OpError("the PSD has too many layers for their size (" + std::to_string(pictures) + " layers, each a page layer of " +
                      std::to_string(where.page_w) + "×" + std::to_string(where.page_h) + " and a picture of " +
                      std::to_string(file.width()) + "×" + std::to_string(file.height()) + " pixels; at most " +
                      std::to_string(static_cast<std::int64_t>(kMaxImportPixels)) + " pixels in all)");
    }
    const std::string prefix = core::truthy_at(op, "id") ? core::py_str(op["id"]) : core::new_id();
    const Json parent = core::truthy_at(op, "parent") ? Json(core::py_str(op["parent"])) : Json(nullptr);
    // (the masks at 150 dpi; PC4 F-d, a page made of the file: at its pixels, the finest grid of its layers)
    const std::int64_t mask_w = exact ? where.page_w : std::max<std::int64_t>(1, core::py_round_int(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const std::int64_t mask_h = exact ? where.page_h : std::max<std::int64_t>(1, core::py_round_int(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < layers.size(); ++i) ids.push_back(prefix + "-" + std::to_string(i + 1));
    const Size page_px{static_cast<int>(where.page_w), static_cast<int>(where.page_h)};
    const Size shown{static_cast<int>(where.shown_w), static_cast<int>(where.shown_h)};
    const bool same_size = where.shown_w == file.width() && where.shown_h == file.height();
    // (psd-keep@1, PC4 F-d: what the file's page holds that this build does not read — the page's, unless it keeps an
    // earlier PSD's (then this file's is not kept: said in the report) —, and whether its layers keep their ids for it:
    // its layer comps or slices name them)
    const bool keeping = !psd::python_view();
    const bool page_had = page.native_psd && !page.native_psd->empty();
    const bool page_keeps = keeping && !page_had;
    const std::vector<psd::File::ImageResource> file_resources = file.resources();
    const std::vector<psd::File::TaggedBlock> file_globals = file.global_blocks();
    const bool names_layers = page_keeps && std::any_of(file_resources.begin(), file_resources.end(), [](const psd::File::ImageResource& r) {
                                  return core::psd_resource_names_layers(static_cast<int>(r.id));
                              });
    Json effects = Json::array();         // (the layers keeping effects this build does not draw)
    Keeping kept_so_far;                  // (what is kept, and whether any was left out)
    bool ranges_kept = false;             // (a layer keeps its blending ranges: its page says the mode they are of)
    std::vector<std::int64_t> kept_ids;  // (the layer ids of the layers made, which the page's comps and slices name)
    std::map<std::int64_t, std::string> lyid_layers;  // (PC5 J-0: … and the layers made of them, render/psd_production)
    // (vector@1, PC3 C-a: a path of the file — fractions of its canvas — on the page as its picture is placed, mm)
    const double mm_x = page.spec.width_mm.value() / static_cast<double>(where.page_w);
    const double mm_y = page.spec.height_mm.value() / static_cast<double>(where.page_h);
    const core::vector::Affine onto{static_cast<double>(where.shown_w) * mm_x, 0.0, 0.0, static_cast<double>(where.shown_h) * mm_y,
                                    static_cast<double>(where.at_x) * mm_x, static_cast<double>(where.at_y) * mm_y};
    const auto vector_mask_of = [&](const psd::Layer& item) -> std::optional<core::vector::VectorMask> {
        const std::optional<std::string_view> block = file.vector_mask(item);
        if (!block) return std::nullopt;
        std::optional<core::vector::VectorMask> mask = psd_paths::vector_mask_of(*block, 1.0, 1.0);
        if (!mask) return mask;
        mask->path = mask->path.transformed(onto);
        // (the user's decision 3: its density and feather, Photoshop's mask parameters' bits 2 and 3)
        if (item.vector_density) mask->density = core::py_round(std::clamp(*item.vector_density / 255.0, 0.0, 1.0), 4);
        if (item.vector_feather && file.width() > 0) {
            const double mm_per_px = mm_x * static_cast<double>(where.shown_w) / static_cast<double>(file.width());
            mask->feather_mm = core::py_round(std::clamp(*item.vector_feather * mm_per_px, 0.0, core::vector::kFeatherMax), 4);
        }
        return mask;
    };
    // (patterns@1, PC4 E3: the file's patterns — its Patt, Pat2, Pat3 blocks, read within their budget —, each made a
    // pattern of the book only when a fill names it: by its unique id, else by its name; its pixel 1/dpi inch as the
    // file's. A block that cannot be read — broken, too large, out of memory —: its fills are not read, as of a pattern
    // the file does not hold; the PSD is still read)
    std::vector<pat::PsdPattern> file_patterns;
    for (const std::string_view block : file.pattern_blocks()) {
        try {
            for (pat::PsdPattern& p : pat::parse_block(block)) file_patterns.push_back(std::move(p));
        } catch (const std::exception&) {
        }
    }
    std::map<std::string, std::optional<std::pair<std::string, double>>> patterns_taken;  // (the file's id → the book's pattern and dpi)
    Json patterns_read = Json::array();
    const auto pattern_of = [&](const std::string& id, const std::string& name) -> std::optional<std::pair<std::string, double>> {
        const auto found = std::find_if(file_patterns.begin(), file_patterns.end(), [&](const pat::PsdPattern& t) { return !id.empty() && t.id == id; });
        const auto named = found != file_patterns.end()
                               ? found
                               : std::find_if(file_patterns.begin(), file_patterns.end(),
                                              [&](const pat::PsdPattern& t) { return pat::shown_name(t.name) == pat::shown_name(name); });
        if (named == file_patterns.end()) return std::nullopt;
        if (const auto kept = patterns_taken.find(named->id); kept != patterns_taken.end()) return kept->second;
        std::optional<pat::Taken> taken;
        try {
            taken = pat::from_psd(*named, file.dpi());
        } catch (const std::exception&) {  // (its picture cannot be made: as one the file does not hold)
        }
        if (!taken) {
            patterns_taken[named->id] = std::nullopt;
            return std::nullopt;
        }
        core::Pattern given = taken->pattern;
        if (core::find_pattern(doc, taken->file_id) == nullptr && !taken->file_id.empty() && taken->file_id.size() <= 64 &&
            std::all_of(taken->file_id.begin(), taken->file_id.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_'; })) {
            given.id = taken->file_id;  // (its unique id kept: written back the same)
        }
        const auto [book_id, added] = keep_pattern(doc, std::move(given), taken->bytes);
        if (added) patterns_read.push_back(book_id);
        const std::pair<std::string, double> out{book_id, taken->pattern.dpi};
        patterns_taken[named->id] = out;
        return out;
    };
    // (layer-styles@1, PC4 E6: the layers' styles in the page's mm as the picture is placed; the file's global light —
    // resources 1037 and 1049 — the book's when the book is made of the file, else taken by the effects when it differs)
    psd_shapes::Placement style_placement;
    style_placement.mm_x = static_cast<double>(where.shown_w) / std::max<double>(1.0, static_cast<double>(file.width())) * mm_x;
    style_placement.mm_y = static_cast<double>(where.shown_h) / std::max<double>(1.0, static_cast<double>(file.height())) * mm_y;
    style_placement.x0_mm = static_cast<double>(where.at_x) * mm_x;
    style_placement.y0_mm = static_cast<double>(where.at_y) * mm_y;
    style_placement.width_px = static_cast<double>(file.width());
    style_placement.height_px = static_cast<double>(file.height());
    style_placement.dpi = file.dpi();
    style_placement.pattern = pattern_of;
    std::array<double, 2> file_light{core::kDefaultLightAngle, core::kDefaultLightAltitude};
    for (const psd::File::ImageResource& r : file_resources) {
        if (r.id == 1037) {
            if (const auto v = psd_effects::light_of_resource(r.data)) file_light[0] = std::clamp(*v, -180.0, 180.0);
        } else if (r.id == 1049) {
            if (const auto v = psd_effects::light_of_resource(r.data)) file_light[1] = std::clamp(*v, 0.0, 90.0);
        }
    }
    std::optional<std::array<double, 2>> light_differs;
    const bool book_lit = exact && keeping;  // (the file's light the book's — once a style is read: review P3-1)
    if (!book_lit && file_light != core::global_light(doc)) light_differs = file_light;
    Json styled = Json::array();
    std::vector<std::string> style_notes;
    std::vector<core::Layer> made;
    std::vector<std::string> patterns_filled;  // (patterns@1: the pattern fill layers read)
    std::vector<std::string> shapes_skipped, shapes_read;  // (pen-shapes@1: shape layers this build cannot read, and those read)
    std::map<std::string, std::string> contents_made;  // (PC3 A3: a placed layer's uuid → its contents' id)
    Json smart_objects = Json::array(), placed_pixels = Json::array();
    // (PC4 D7: Photoshop's smart filters read as Genko's — render/psd_filters —, one descriptor budget for the whole file;
    // the file's filter effects read when a placed layer has smart filters; the layers read so and those not, at most
    // kFilterReportsMax of each said)
    constexpr std::size_t kFilterReportsMax = 256;
    psd_filters::Budget filters_budget{file.descriptor_left()};  // (what the file's placed layers left of its one budget)
    std::optional<psd_filters::Masks> filter_masks;
    std::optional<std::vector<psd::File::TaggedBlock>> filter_globals;
    Json filters_read = Json::array(), filters_shown = Json::array();
    Json raw_shown = Json::array();  // (PC4 F-e: smart objects of a camera RAW shown as Photoshop drew them)
    std::size_t filters_unsaid = 0;
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
        // its mask over the page, as the page holds a layer's (none when it has none)
        const auto mask_of = [&]() -> std::optional<core::Mask> {
            const std::optional<Image> mask = file.mask(item);
            if (!mask) return std::nullopt;
            limits::check_picture(mask_w, mask_h, "the layer mask");
            const Size mask_size{static_cast<int>(mask_w), static_cast<int>(mask_h)};
            Image full = Image::create("L", page_px, Ink(255));
            full.paste(mask->resize(shown), paste_corner(where));
            core::Mask kept;
            kept.png = fills::png_bytes(full.resize(mask_size));
            kept.enabled = item.mask_enabled;  // (the user's decision 3: a mask that is off — flags bit 1 — stays off)
            // (PC3 B2, the user's decision 3: a mask whose record carries its parameters — Photoshop's 濃度 and ぼかし, as
            // this build writes a mask of masks@1 — keeps them and whether it moves with the layer; one without stays
            // fixed to the page, as every mask read before)
            if (item.mask_parameters) {
                kept.linked = !item.mask_unlinked;
                if (item.mask_density) kept.density = core::py_round(std::clamp(*item.mask_density / 255.0, 0.0, 1.0), 4);
                if (item.mask_feather && file.width() > 0) {
                    const double mm_per_px = page.spec.width_mm.value() / static_cast<double>(where.page_w) *
                                             static_cast<double>(where.shown_w) / static_cast<double>(file.width());
                    kept.feather_mm = core::py_round(std::clamp(*item.mask_feather * mm_per_px, 0.0, core::kMaskFeatherMax), 4);
                }
            }
            return kept;
        };
        if (item.folder) {
            layer.kind = core::LayerKind::Folder;
        } else if (item.kind == "shape") {
            // (pen-shapes@1, PC3 C-b: a shape layer as a new pen layer of one shape part — render/psd_shapes —, as the PSD
            // shows it: not cut by the panels; one whose fill this build cannot read, left out as before)
            std::optional<core::vector::ShapePart> part;
            if (const auto blocks = file.shape_blocks(item)) {
                psd_shapes::Placement placement;
                placement.mm_x = static_cast<double>(where.shown_w) / std::max<double>(1.0, static_cast<double>(file.width())) * mm_x;
                placement.mm_y = static_cast<double>(where.shown_h) / std::max<double>(1.0, static_cast<double>(file.height())) * mm_y;
                placement.x0_mm = static_cast<double>(where.at_x) * mm_x;
                placement.y0_mm = static_cast<double>(where.at_y) * mm_y;
                placement.width_px = static_cast<double>(file.width());
                placement.height_px = static_cast<double>(file.height());
                placement.dpi = file.dpi();
                placement.pattern = pattern_of;  // (patterns@1, PC4 E3)
                part = psd_shapes::part_of(psd_shapes::Blocks{blocks->fill_key, blocks->fill, blocks->vscg, blocks->vstk, blocks->vogk, blocks->path},
                                           placement);
            }
            if (!part) {
                shapes_skipped.push_back(item.name);
                continue;
            }
            part->id = "s1";
            try {
                part = core::vector::part_from_json(core::vector::to_json(*part));  // (its normal form, checked)
            } catch (const core::Error&) {
                shapes_skipped.push_back(item.name);
                continue;
            }
            layer.kind = core::LayerKind::Strokes;
            layer.panel_clip = false;
            layer.shapes = core::vector::make_shapes({std::move(*part)});
            if (auto mask = mask_of()) layer.mask = std::move(*mask);
            shapes_read.push_back(layer.id);
        } else if (item.kind == "adjust" && !item.adjust) {
            // (patterns@1, PC4 E3: a pattern fill layer — a fill layer of the book's pattern, as the PSD shows it: not cut by
            // the panels; one whose pattern the file does not hold, left out as before)
            std::optional<Json> spec;
            if (const auto block = file.fill_block(item); block && block->first == "PtFl") {
                psd_shapes::Placement placement;
                placement.mm_x = static_cast<double>(where.shown_w) / std::max<double>(1.0, static_cast<double>(file.width())) * mm_x;
                placement.mm_y = static_cast<double>(where.shown_h) / std::max<double>(1.0, static_cast<double>(file.height())) * mm_y;
                placement.x0_mm = static_cast<double>(where.at_x) * mm_x;
                placement.y0_mm = static_cast<double>(where.at_y) * mm_y;
                placement.width_px = static_cast<double>(file.width());
                placement.height_px = static_cast<double>(file.height());
                placement.dpi = file.dpi();
                placement.pattern = pattern_of;
                spec = psd_shapes::pattern_fill_of(block->second, placement);
            }
            if (!spec) {
                shapes_skipped.push_back(item.name);
                continue;
            }
            layer.kind = core::LayerKind::Fill;
            layer.fill = Json{{"pattern", std::move(*spec)}};
            layer.panel_clip = false;
            if (auto mask = mask_of()) layer.mask = std::move(*mask);
            layer.vector_mask = vector_mask_of(item);
            patterns_filled.push_back(layer.id);
        } else if (item.kind == "adjust" && item.adjust) {
            // (PC2, the user's decision 5: an adjustment layer a codec read, render/psd_adjust — a correction layer, its
            // blend its native.blend: a correction layer's own blend is not looked at)
            layer.kind = core::LayerKind::Adjust;
            layer.adjust = psd::imported_adjust(*item.adjust, item.blend, page);
            core::lut3d::keep_luts(doc.luts, item.luts, *layer.adjust);  // (PC2a: a color lookup's table into the book)
            layer.blend = "normal";
            if (auto mask = mask_of()) layer.mask = std::move(*mask);
            layer.vector_mask = vector_mask_of(item);
        } else {
            layer.kind = core::LayerKind::Raster;
            // (a painting app's picture: on a monochrome page it prints in grey and tones)
            layer.source = Json::object({{"kind", "psd"}});
            // (PC4 F-d: at the file's depth — not a placed layer's picture, which stays Photoshop's 8-bit look until it
            // changes —, its samples as they are)
            const std::optional<psd::File::DeepPart> part = deep == DeepWay::None || item.kind == "smart" ? std::nullopt
                                                            : merged_deep                                     ? merged_deep
                                                                                                              : file.layer_deep(item);
            if (deep == DeepWay::Inks && (part || merged_deep)) {
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(
                    part ? deep_inks(*part, *mode, page_px, paste_corner(where))
                         : core::blank_ink_pixels(mode->space, mode->depth, static_cast<std::uint32_t>(page_px.width), static_cast<std::uint32_t>(page_px.height))));
            } else if (deep == DeepWay::Rgb && item.kind != "smart") {
                layer.color_raster = std::make_shared<const std::string>(
                    part ? deep_color_raster(*part, page, page_px, paste_corner(where), linear_page(page))
                         : core::encode_color_pixels(static_cast<std::uint32_t>(page_px.width), static_cast<std::uint32_t>(page_px.height),
                                                     page.rgb->depth == 16 ? "u16" : "f32", [](std::size_t) { return std::array<double, 4>{0, 0, 0, 0}; }));
                if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end())
                    doc.features.emplace_back(core::kColorRasterFeature);
            } else if (mode) {  // (color-mode@1: onto a clear layer of the page's inks)
                core::InkPixels picture = way.converting ? import_profile::taken_inks(numbers_of(merged_inks ? *file.merged() : file.image(item)), way, *mode)
                                          : merged_inks ? *merged_inks
                                                        : file.image_inks(item, *mode);
                if (!same_size) picture = resize_inks(picture, shown, Resample::Lanczos);
                core::InkPixels canvas = core::blank_ink_pixels(mode->space, mode->depth, static_cast<std::uint32_t>(page_px.width),
                                                                static_cast<std::uint32_t>(page_px.height));
                paste_inks(canvas, picture, paste_corner(where));
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(canvas));
            } else {
                // (onto a clear layer: the pixels as they are)
                Image canvas = Image::create("RGBA", page_px, Ink{0, 0, 0, 0});
                // (PC5-0, Z0's review P2-1: a 32-bit file's layer by its profile's curve — a placed layer's picture too —)
                const auto eight = [&] {
                    if (std::optional<Image> by_curve = eight_bits_of_32(file, item)) return std::move(*by_curve);
                    return file.image(item);
                };
                const Image picture = way.converting ? import_profile::taken(numbers_of(merged ? *merged : file.image(item)), way, page)
                                                     : merged ? *merged
                                                              : eight();
                canvas.paste(same_size ? picture : picture.resize(shown, Resample::Lanczos), paste_corner(where));
                layer.raster_png = fills::png_bytes(canvas);
                layer.raster_relpath = raster::page_folder(page) + "/user-" + layer.id + ".png";
            }
            if (!merged && !merged_inks && !merged_deep) {
                if (auto mask = mask_of()) layer.mask = std::move(*mask);
                layer.vector_mask = vector_mask_of(item);
            }
            // (PC3 A3, the user's decision 3: a placed layer whose file the PSD holds is a smart object of it; the
            // picture just made — Photoshop's — is what it shows until it changes, its block kept to be written back)
            if (item.kind == "smart" && item.placed) {
                std::optional<SmartMade> smart = item.placed->has_corners ? smart_contents_of(doc, file, item, op, way, contents_made)
                                                                          : std::optional<SmartMade>(SmartMade{{}, {}, "unreadable"});
                if (smart && smart->why.empty()) *smart = smart_settings(doc, page, where, file, *item.placed, std::move(*smart));
                if (smart && smart->why.empty()) {
                    const auto keep = [&doc](core::Bytes bytes) {
                        const std::string ref = core::bytes_ref(*bytes);
                        doc.smart_files[ref] = std::move(bytes);
                        return ref;
                    };
                    const std::string block(file.placed_block(item));
                    // (PC4 D7: its Photoshop smart filters, when they are all in the table, as Genko's — drawn from its
                    // contents from then on —; else Photoshop's picture, as before, and which said)
                    std::optional<psd_filters::Read> fx;
                    if (item.placed->filters && keeping && !merged_only) {
                        if (!filter_masks) {
                            filter_globals = file.global_blocks();
                            filter_masks = psd_filters::read_masks(*filter_globals);
                        }
                        psd_filters::Placement placement;
                        placement.scale_x = static_cast<double>(where.shown_w) / std::max<double>(1.0, static_cast<double>(file.width()));
                        placement.scale_y = static_cast<double>(where.shown_h) / std::max<double>(1.0, static_cast<double>(file.height()));
                        placement.at_x = static_cast<double>(where.at_x);
                        placement.at_y = static_cast<double>(where.at_y);
                        placement.page_w = static_cast<double>(std::max<std::int64_t>(1, where.page_w));
                        placement.page_h = static_cast<double>(std::max<std::int64_t>(1, where.page_h));
                        placement.width_mm = page.spec.width_mm.value();
                        placement.height_mm = page.spec.height_mm.value();
                        placement.file_w = static_cast<double>(std::max<std::int64_t>(1, file.width()));
                        placement.file_h = static_cast<double>(std::max<std::int64_t>(1, file.height()));
                        fx = psd_filters::read(block, *item.placed, placement, *filter_masks, filters_budget);
                    }
                    const bool filtered = fx && fx->filters && !fx->filters->empty();
                    // (PC4 F-e, the coordinator's decision: a camera RAW's contents — Photoshop's Camera Raw settings not
                    // read — keep Photoshop's picture even with its filters read, until set_camera_raw or コンテンツを編集
                    // develops them; said)
                    const auto inside = doc.smart_contents.find(smart->contents);
                    const bool raw_inside = inside != doc.smart_contents.end() && inside->second && inside->second->format == "raw";
                    if (raw_inside && raw_shown.size() < kFilterReportsMax) raw_shown.push_back(Json{{"id", layer.id}, {"title", layer.title}});
                    Json record = Json{{"block", item.placed->key}, {"raw", keep(std::make_shared<const std::string>(block))}};
                    if (!filtered || raw_inside) {
                        Json picture = Json::object();
                        if (layer.ink_raster) picture["ink_raster"] = keep(layer.ink_raster);
                        else picture["asset"] = keep(layer.raster_png);
                        record["shown"] = std::move(picture);
                    }
                    if (filtered) {
                        if (filters_read.size() < kFilterReportsMax) filters_read.push_back(layer.id);
                    } else if (fx && !fx->why.empty()) {
                        if (filters_shown.size() < kFilterReportsMax) {
                            Json names = Json::array();
                            for (const std::string& n : fx->names) names.push_back(n);
                            Json said{{"id", layer.id}, {"title", layer.title}, {"why", fx->why}};
                            if (!names.empty()) said["filters"] = std::move(names);
                            if (fx->more > 0) said["more"] = fx->more;
                            filters_shown.push_back(std::move(said));
                        } else {
                            ++filters_unsaid;
                        }
                    }
                    layer.kind = core::LayerKind::Smart;
                    layer.raster_png.reset();
                    layer.raster_relpath.reset();
                    layer.ink_raster.reset();
                    layer.source.reset();
                    layer.clip_to = "none";
                    smart->settings["psd"] = std::move(record);
                    layer.smart = std::move(smart->settings);
                    if (filtered) {
                        core::smart::put_setting(layer, "filters", std::move(*fx->filters));
                        if (!fx->on) core::smart::put_setting(layer, "filters_on", false);
                        smart_filters::settle_places(doc, page, layer);  // (their places go with it, as smart_filter_add's)
                        // (review P2-5: what was read, so that a PSD written while they are the same keeps Photoshop's)
                        (*layer.smart)["psd"]["filters_read"] = core::smart::filters_digest(layer);
                    }
                    smart_objects.push_back(Json{{"id", layer.id}, {"contents", smart->contents}});
                } else {
                    placed_pixels.push_back(Json{{"id", layer.id}, {"why", smart ? smart->why : std::string("no_file")}});
                }
            }
        }
        // (psd-keep@1, PC4 F-d: what its record holds that this build does not read, kept to be written back; its locks)
        if (keeping && !merged_only) {
            bool effect = false;
            layer.native_psd = layer_keep(doc, file, item, names_layers, effect, kept_so_far);
            // (layer-styles@1, PC4 E6: its effects, fill opacity and Blend If as its style — drawn by this build)
            if (!item.folder && core::takes_layer_style(layer) &&
                style_from_record(layer, file, item, style_placement, light_differs, style_notes, filters_budget.left)) {
                styled.push_back(layer.id);
                if (core::style_shows_effects(*layer.style) || !(*layer.style)["effects"].empty()) effect = false;
            }
            if (effect) effects.push_back(layer.id);
            if (layer.native_psd && layer.native_psd->id) {
                kept_ids.push_back(*layer.native_psd->id);
                lyid_layers.emplace(*layer.native_psd->id, layer.id);
            }
            ranges_kept = ranges_kept || (layer.native_psd && !layer.native_psd->ranges.empty());
            if (const auto locks = file.protection(item); locks && *locks != 0) take_locks(layer, *locks);
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
    for (const std::string& name : merged_only ? file.python_skipped() : file.skipped()) skipped.push_back(name);
    for (const std::string& name : shapes_skipped) skipped.push_back(name);
    target.layers.insert(target.layers.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(made.begin()),
                         std::make_move_iterator(made.end()));
    Json report = Json::object({{"layers", std::move(listed)}, {"skipped", std::move(skipped)}});
    // (PC3 A3: the placed layers made smart objects, and those read as their pixels and why — said only when there are)
    if (!smart_objects.empty()) report["smart_objects"] = std::move(smart_objects);
    if (!placed_pixels.empty()) report["placed_as_pixels"] = std::move(placed_pixels);
    // (PC4 D7: the smart objects whose Photoshop smart filters were read as Genko's, and those shown as Photoshop's picture
    // and why — said only when there are)
    if (!filters_read.empty()) report["smart_filters_read"] = std::move(filters_read);
    if (!filters_shown.empty()) report["smart_filters_shown"] = std::move(filters_shown);
    if (!raw_shown.empty()) report["smart_raw_shown"] = std::move(raw_shown);
    if (filters_unsaid > 0) report["smart_filters_shown_more"] = filters_unsaid;
    if (!patterns_read.empty()) report["patterns"] = std::move(patterns_read);  // (patterns@1, PC4 E3: the patterns taken in)
    if (!patterns_filled.empty()) {  // (and the pattern fill layers read)
        Json fills = Json::array();
        for (const std::string& id : patterns_filled) fills.push_back(id);
        report["pattern_fills"] = std::move(fills);
    }
    if (!shapes_read.empty()) {  // (pen-shapes@1, PC3 C-b: the shape layers read as pen layers of a shape part)
        Json shapes = Json::array();
        for (const std::string& id : shapes_read) shapes.push_back(id);
        report["shapes"] = std::move(shapes);
    }
    // (vector@1, PC3 C-a: the file's saved paths onto the page — a name the page has already takes a number —, its work
    // path and clipping path where the page has none)
    Json paths = Json::array();
    std::map<std::string, std::string> renamed;
    for (const psd::File::PathResource& resource : file.path_resources()) {
        if (resource.id == psd_paths::kClippingPath) continue;
        std::optional<core::vector::Path> path = psd_paths::path_of(resource.data, 1.0, 1.0);
        if (!path) continue;
        *path = path->transformed(onto);
        if (resource.id == psd_paths::kWorkPath) {
            if (!target.work_path) {
                target.work_path = std::move(*path);
                paths.push_back("work");
            }
            continue;
        }
        if (target.paths.size() >= core::vector::kPathsMax) continue;
        std::string given = core::py_strip(first_chars(psd_paths::name_of(resource.name), 56));
        if (given.empty()) given = "パス";
        std::string name = given;
        for (int n = 2; std::any_of(target.paths.begin(), target.paths.end(), [&](const core::vector::NamedPath& p) { return p.name == name; }); ++n)
            name = given + " " + std::to_string(n);
        renamed[given] = name;
        target.paths.push_back(core::vector::NamedPath{name, std::move(*path)});
        paths.push_back(name);
    }
    for (const psd::File::PathResource& resource : file.path_resources()) {
        if (resource.id != psd_paths::kClippingPath || target.clipping_path) continue;
        if (auto clip = psd_paths::clipping_of(resource.data); clip && renamed.contains(clip->name)) {
            clip->name = renamed[clip->name];
            target.clipping_path = std::move(*clip);
        }
    }
    if (!paths.empty()) report["paths"] = std::move(paths);
    if (op.contains("embedded_profile")) report["embedded_profile"] = import_profile::report(way);
    // (PC5 J-0, render/psd_production: the file's layer comps, slices, notes and measurement scale read onto the page by
    // their groups — those read kept with the rest below, marked copied with what the page held of them)
    psd_production::Taken production;
    if (keeping) {
        std::vector<std::pair<int, std::string_view>> copied_resources;
        for (const psd::File::ImageResource& r : file_resources) {
            if (psd_production::copies_resource(static_cast<int>(r.id))) copied_resources.emplace_back(static_cast<int>(r.id), r.data);
        }
        std::vector<std::pair<std::string, std::string_view>> copied_globals;
        for (const psd::File::TaggedBlock& g : file_globals) {
            if (psd_production::copies_global(g.key)) copied_globals.emplace_back(g.key, g.data);
        }
        if (!copied_resources.empty() || !copied_globals.empty()) {
            psd_production::ReadContext context;
            context.onto = &onto;
            context.layers = lyid_layers;
            context.width_px = static_cast<double>(file.width());
            context.height_px = static_cast<double>(file.height());
            context.merged_only = merged_only;
            context.page_had = page_had;
            context.report = &report;
            context.doc = &doc;  // (PC5 J-a: its layers' kept blocks — their cmls —, what they show)
            production = psd_production::read(doc.edit_page(index), copied_resources, copied_globals, context);
        }
    }
    // (psd-keep@1, PC4 F-d: the page's own — the patterns a fill took left out of its pattern blocks —, the ids of the
    // layers read with them; the layers keeping effects; what was not kept)
    if (page_keeps) {
        std::set<std::string> taken;
        for (const auto& [file_id, book] : patterns_taken) {
            if (book) taken.insert(file_id);
        }
        // (its comps and slices not when a layer they may name was not made: one not read, or the merged picture read)
        const std::set<std::int64_t> made_ids(kept_ids.begin(), kept_ids.end());
        const bool named_gone = merged_only ? !file_ids.empty()
                                            : std::any_of(file_ids.begin(), file_ids.end(), [&](std::int64_t id) { return !made_ids.contains(id); });
        core::PsdPageKeep keep = page_keep(doc, file, taken, named_gone, kept_so_far);
        for (const core::PsdCopied& c : production.copied) {  // (PC5 J-0: the marks of the blocks kept — not one left out)
            const bool held = c.resource >= 0 ? std::any_of(keep.resources.begin(), keep.resources.end(), [&](const core::PsdResource& r) { return r.id == c.resource; })
                                              : std::any_of(keep.globals.begin(), keep.globals.end(), [&](const core::PsdGlobal& g) { return g.key == c.global; });
            if (held) keep.copied.push_back(c);
        }
        if (!keep.resources.empty() || !keep.globals.empty() || ranges_kept) {
            if (names_layers && !named_gone) keep.ids = kept_ids;
            keep.from = core::PsdFrom{file.psb() ? 2 : 1, file.mode(), file.depth()};
            doc.edit_page(index).native_psd = std::move(keep);
        }
    } else if (keeping && page_had && (std::any_of(file_resources.begin(), file_resources.end(),
                                                   [](const psd::File::ImageResource& r) { return core::psd_resource_kept(static_cast<int>(r.id)); }) ||
                                       std::any_of(file_globals.begin(), file_globals.end(),
                                                   [](const psd::File::TaggedBlock& g) { return core::psd_global_kept(g.key); }))) {
        report["psd_page"] = "earlier";  // (the page keeps an earlier PSD's resources and global blocks: this file's are not kept)
    }
    if (!effects.empty()) report["effects"] = std::move(effects);
    if (book_lit && !styled.empty()) {  // (no style read: the book's light left as it is, the file's kept in its resources)
        doc.global_light = file_light[0] == core::kDefaultLightAngle && file_light[1] == core::kDefaultLightAltitude
                               ? std::nullopt
                               : std::optional<std::array<double, 2>>(core::global_light_of(Json{{"angle", file_light[0]}, {"altitude", file_light[1]}}));
    }
    if (!styled.empty()) report["layer_styles"] = std::move(styled);  // (layer-styles@1, PC4 E6: the layers whose style was read)
    if (!style_notes.empty()) {  // (review P3-3: each note once, how many layers it was said of; at most 20 of them)
        std::vector<std::pair<std::string, std::size_t>> once;
        for (const std::string& note : style_notes) {
            const auto it = std::find_if(once.begin(), once.end(), [&](const auto& seen) { return seen.first == note; });
            if (it != once.end()) ++it->second;
            else once.emplace_back(note, 1);
        }
        Json notes = Json::array();
        for (std::size_t i = 0; i < once.size() && i < 20; ++i)
            notes.push_back(once[i].second > 1 ? once[i].first + "（" + std::to_string(once[i].second) + " 層）" : once[i].first);
        if (once.size() > 20) notes.push_back("ほか " + std::to_string(once.size() - 20) + " 件の知らせ");
        report["layer_style_notes"] = std::move(notes);
    }
    if (kept_so_far.cut) report["psd_blocks_cut"] = true;
    if (!shallow.empty()) report["in_8_bits"] = shallow;  // (PC4 F-d review: why not at its depth)
    return report;
}


void import_psd(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    (void)core::require_page(doc, op);
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
    const annotations::ReadingActor reader(c.actor);  // (PC5 J-c, the review's P2-3: the file's notes the op's actor's)
    c.report = import_data(doc, op, std::move(data), false);
}

}  // namespace

core::Json import_psd_data(core::Document& doc, const core::Json& op, std::string data, bool exact) { return import_data(doc, op, std::move(data), exact); }

void register_file_ops(core::OpRegistry& registry) { registry.add("import_psd", import_psd); }

}  // namespace genko::render
