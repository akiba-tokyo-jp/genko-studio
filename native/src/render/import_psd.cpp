// import_psd (Python's fileops.import_psd and fileops.placed): every layer of a PSD or PSB as a Genko layer on the page,
// read by render/psd (psd.read_psd). (PC3 A3, the user's decision 3: a placed layer whose file the PSD holds is a smart
// object of that file, Photoshop's picture of it shown until it changes.)

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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

#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "core/masks.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/fill.hpp"
#include "render/import_profile.hpp"
#include "render/op_limits.hpp"
#include "render/ops_registry.hpp"
#include "render/ink_canvas.hpp"
#include "render/psd.hpp"
#include "render/psd_adjust.hpp"
#include "render/psd_paths.hpp"
#include "render/psd_shapes.hpp"
#include "render/raster.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
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
        info = smart::look_at(linked->data, "");
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
    bool merged_only = false;
    if (std::all_of(layers.begin(), layers.end(), [](const psd::Layer& l) { return l.folder || l.kind == "adjust"; })) {
        merged_only = true;
        if (mode) {
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
    const std::int64_t mask_w = std::max<std::int64_t>(1, core::py_round_int(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const std::int64_t mask_h = std::max<std::int64_t>(1, core::py_round_int(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < layers.size(); ++i) ids.push_back(prefix + "-" + std::to_string(i + 1));
    const Size page_px{static_cast<int>(where.page_w), static_cast<int>(where.page_h)};
    const Size shown{static_cast<int>(where.shown_w), static_cast<int>(where.shown_h)};
    const bool same_size = where.shown_w == file.width() && where.shown_h == file.height();
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
    std::vector<core::Layer> made;
    std::vector<std::string> shapes_skipped, shapes_read;  // (pen-shapes@1: shape layers this build cannot read, and those read)
    std::map<std::string, std::string> contents_made;  // (PC3 A3: a placed layer's uuid → its contents' id)
    Json smart_objects = Json::array(), placed_pixels = Json::array();
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
            if (mode) {  // (color-mode@1: onto a clear layer of the page's inks)
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
                const Image picture = way.converting ? import_profile::taken(numbers_of(merged ? *merged : file.image(item)), way, page)
                                                     : merged ? *merged
                                                              : file.image(item);
                canvas.paste(same_size ? picture : picture.resize(shown, Resample::Lanczos), paste_corner(where));
                layer.raster_png = fills::png_bytes(canvas);
                layer.raster_relpath = raster::page_folder(page) + "/user-" + layer.id + ".png";
            }
            if (!merged && !merged_inks) {
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
                    Json picture = Json::object();
                    const auto keep = [&doc](core::Bytes bytes) {
                        const std::string ref = core::bytes_ref(*bytes);
                        doc.smart_files[ref] = std::move(bytes);
                        return ref;
                    };
                    if (layer.ink_raster) picture["ink_raster"] = keep(layer.ink_raster);
                    else picture["asset"] = keep(layer.raster_png);
                    const std::string block(file.placed_block(item));
                    Json record = Json{{"block", item.placed->key}, {"raw", keep(std::make_shared<const std::string>(block))}, {"shown", std::move(picture)}};
                    layer.kind = core::LayerKind::Smart;
                    layer.raster_png.reset();
                    layer.raster_relpath.reset();
                    layer.ink_raster.reset();
                    layer.source.reset();
                    layer.clip_to = "none";
                    smart->settings["psd"] = std::move(record);
                    layer.smart = std::move(smart->settings);
                    smart_objects.push_back(Json{{"id", layer.id}, {"contents", smart->contents}});
                } else {
                    placed_pixels.push_back(Json{{"id", layer.id}, {"why", smart ? smart->why : std::string("no_file")}});
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
    for (const std::string& name : merged_only ? file.python_skipped() : file.skipped()) skipped.push_back(name);
    for (const std::string& name : shapes_skipped) skipped.push_back(name);
    target.layers.insert(target.layers.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(made.begin()),
                         std::make_move_iterator(made.end()));
    c.report = Json::object({{"layers", std::move(listed)}, {"skipped", std::move(skipped)}});
    // (PC3 A3: the placed layers made smart objects, and those read as their pixels and why — said only when there are)
    if (!smart_objects.empty()) (*c.report)["smart_objects"] = std::move(smart_objects);
    if (!placed_pixels.empty()) (*c.report)["placed_as_pixels"] = std::move(placed_pixels);
    if (!shapes_read.empty()) {  // (pen-shapes@1, PC3 C-b: the shape layers read as pen layers of a shape part)
        Json shapes = Json::array();
        for (const std::string& id : shapes_read) shapes.push_back(id);
        (*c.report)["shapes"] = std::move(shapes);
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
    if (!paths.empty()) (*c.report)["paths"] = std::move(paths);
    if (op.contains("embedded_profile")) (*c.report)["embedded_profile"] = import_profile::report(way);
}

}  // namespace

void register_file_ops(core::OpRegistry& registry) { registry.add("import_psd", import_psd); }

}  // namespace genko::render
