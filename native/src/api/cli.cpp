#include "api/cli.hpp"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "api/buildinfo.hpp"
#include "api/server_cli.hpp"
#include "api/studio_cli.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/filter_kinds.hpp"
#include "core/config_dir.hpp"
#include "core/actor.hpp"
#include "core/clock.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/ids.hpp"
#include "core/json.hpp"
#include "core/lut3d.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "formats/open_file.hpp"
#include "formats/pillow_save.hpp"
#include "formats/smart_files.hpp"
#include "render/anim.hpp"
#include "render/brushes.hpp"
#include "render/draw.hpp"
#include "render/inspect.hpp"
#include "render/layer_bounds.hpp"
#include "render/path_info.hpp"
#include "render/proof.hpp"
#include "render/movie.hpp"
#include "render/shape_presets.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/codecs/codecs.hpp"
#include "render/png.hpp"
#include "render/timelapse.hpp"
#include "storage/doctor.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/migrate.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"

namespace genko::api {

namespace {

namespace fs = std::filesystem;
using core::Json;

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

void write_json(const nlohmann::json& value, std::FILE* to = stdout) {
    write_text(value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n", to);
}

// Python's _print_json: json.dumps(payload, ensure_ascii=…) and a newline, on stdout.
void print_json(const core::Json& value, bool ascii) { write_text(core::dump_python(value, ascii) + "\n", stdout); }

int usage(const std::string& message, int exit_code = 2) {
    write_json({{"ok", false}, {"error", message}, {"code", "usage"}}, stderr);
    return exit_code;
}

// {"ok": false, "error": …, "code": …} on stdout, as Python's __main__ prints its failures (plus the code,
// docs/cpp-migration/ARCHITECTURE.md §8).
int fail(const std::string& message, const std::string& code, bool ascii, int exit_code, const Json* report = nullptr) {
    core::Json out = core::Json::object();
    out["ok"] = false;
    out["error"] = message;
    out["code"] = code;
    if (report != nullptr) out["report"] = *report;
    print_json(out, ascii);
    return exit_code;
}

// The path as Python's pathlib writes it ("a//b/./c/" → "a/b/c", "" → "."), so error messages name the same file.
std::string pathlib_text(const std::string& text) {
    const bool absolute = !text.empty() && text.front() == '/';
    std::vector<std::string> parts;
    std::string part;
    for (const char c : text + "/") {
        if (c == '/') {
            if (!part.empty() && part != ".") parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : "/") + parts[i];
    if (out.empty()) out = ".";
    return out;
}

fs::path path_arg(const QString& arg) { return storage::path_from_utf8(pathlib_text(arg.toStdString())); }

// --- arguments (argparse's rules: options anywhere, "--name value" or "--name=value", unique prefixes) ---------

struct Option {
    const char* name;  // "--title"
    bool takes_value;
};

struct Arguments {
    std::vector<QString> positional;
    std::map<std::string, QString> values;
    std::set<std::string> flags;

    bool flag(const char* name) const { return flags.contains(name); }
    std::optional<QString> value(const char* name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<QString>(it->second);
    }
};

bool negative_number(const QString& text) {
    bool ok = false;
    text.toDouble(&ok);
    return ok && text.startsWith(QLatin1Char('-'));
}

// The error message, or nothing when the arguments are right.
std::optional<std::string> parse_arguments(const QStringList& args, std::span<const Option> options,
                                           std::size_t positionals, const char* names, Arguments& out) {
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (!arg.startsWith(QLatin1String("--")) || arg.size() == 2) {
            if (arg.startsWith(QLatin1Char('-')) && arg != QLatin1String("-") && !negative_number(arg)) {
                return "unrecognized arguments: " + arg.toStdString();
            }
            out.positional.push_back(arg);
            continue;
        }
        const qsizetype equals = arg.indexOf(QLatin1Char('='));
        const std::string given = (equals < 0 ? arg : arg.left(equals)).toStdString();
        const Option* match = nullptr;
        std::vector<const Option*> prefixed;
        for (const Option& option : options) {
            if (given == option.name) match = &option;
            if (std::string_view(option.name).starts_with(given)) prefixed.push_back(&option);
        }
        if (match == nullptr) {
            if (prefixed.size() > 1) {
                std::string could;
                for (const Option* option : prefixed) could += (could.empty() ? "" : ", ") + std::string(option->name);
                return "ambiguous option: " + given + " could match " + could;
            }
            if (prefixed.empty()) return "unrecognized arguments: " + arg.toStdString();
            match = prefixed.front();
        }
        if (!match->takes_value) {
            if (equals >= 0) return "argument " + std::string(match->name) + ": ignored explicit argument '" + arg.mid(equals + 1).toStdString() + "'";
            out.flags.insert(match->name);
            continue;
        }
        if (equals >= 0) {
            out.values[match->name] = arg.mid(equals + 1);
            continue;
        }
        if (i + 1 >= args.size() ||
            (args[i + 1].startsWith(QLatin1Char('-')) && args[i + 1] != QLatin1String("-") && !negative_number(args[i + 1]))) {
            return "argument " + std::string(match->name) + ": expected one argument";
        }
        out.values[match->name] = args[++i];
    }
    if (out.positional.size() < positionals) return std::string("the following arguments are required: ") + names;
    if (out.positional.size() > positionals) return "unrecognized arguments: " + out.positional[positionals].toStdString();
    return std::nullopt;
}

// int(value) for an option (argparse's type=int).
std::optional<std::int64_t> int_option(const QString& text) {
    try {
        return core::py_int(Json(text.toStdString()));
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

std::string read_stdin() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::string out;
    char buf[1 << 16];
    for (;;) {
        const std::size_t got = std::fread(buf, 1, sizeof buf, stdin);
        if (got == 0) break;
        out.append(buf, got);
    }
    return out;
}

core::ParseOptions text_file() {
    core::ParseOptions options;
    options.universal_newlines = true;
    return options;
}

// A v1–v3 book is never written: found before anything is touched (no project.lock is made in it).
void refuse_legacy(const fs::path& dir) {
    const int version = storage::project_version(core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, text_file()));
    if (version < 4) throw storage::NeedsMigration(dir, version);
}

// Python's _is_studio_project: an agent's book (strict gates, studio state, or M0 sidecars) does not trust an unnamed
// caller as a person.
bool is_studio_project(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir / "studio" / "drafts", ec)) return true;
    try {
        const Json payload = core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, text_file());
        const auto truthy = [&payload](const char* key) {
            const auto it = payload.find(key);
            return it != payload.end() && core::py_truthy(*it);
        };
        return payload.is_object() && (truthy("strict_gates") || truthy("studio"));
    } catch (const core::Error&) {
        return false;
    }
}

// --- commands ----------------------------------------------------------------------------------------------------

// PC2a (image.adjustments.colorLookup.list): the looks a color_lookup can take (PhotoCraft's eight, their ids and names)
// and the book's color_lookup corrections ({page, id, title, look | lut and size, name}).
core::Json looks_listing(const core::Document& doc) {
    core::Json looks = core::Json::array();
    for (const core::lut3d::Look& look : core::lut3d::looks()) looks.push_back(core::Json{{"id", std::string(look.id)}, {"name", std::string(look.label)}});
    core::Json layers = core::Json::array();
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const core::Page& page = doc.page(i);
        for (const core::Layer& layer : page.layers) {
            if (layer.kind != core::LayerKind::Adjust || !layer.adjust || layer.adjust->value("kind", core::Json()) != core::Json("color_lookup")) continue;
            core::Json item{{"page", page.index.json()}, {"id", layer.id}, {"title", layer.title}};
            for (const char* key : {"look", "lut", "size", "name"}) {
                if (layer.adjust->contains(key)) item[key] = (*layer.adjust)[key];
            }
            layers.push_back(std::move(item));
        }
    }
    return core::Json{{"looks", looks}, {"layers", layers}};
}

int inspect(const QStringList& args, bool ascii) {
    QString src;
    bool have_src = false;
    bool full = false;
    QString stroke;
    // (color-model@1, the C++ build's own: --pixel <page>:<x_mm>,<y_mm> [--layer ID] [--dpi N]; PC2d: --profile <id|page|sha256>)
    QString pixel, layer, dpi_text, profile;
    bool looks = false;  // (PC2a, the C++ build's own: --looks, image.adjustments.colorLookup.list)
    bool filters = false;  // (PC4 節 D, the C++ build's own: --filters, フィルター's kinds and their settings)
    bool file_info = false;  // (PC4 F-b, the C++ build's own: --file-info, file.fileInfo's reading)
    QString area_text, page_text;  // (PC3 B1, the C++ build's own: --page N --area '<area>'; PC3 C-a: --paths [--page N])
    bool paths = false;  // (vector@1, PC3 C-a, the C++ build's own: --paths [--page N], path.list / path.info / layer.vectorMask.info)
    QString at_text;  // (PC3 C-d, the C++ build's own: --at <page>:<x_mm>,<y_mm>, layer.pickAt — the layers showing there)
    const auto value_of = [&](qsizetype& i, const char* name, QString& into) -> bool {
        const QString& arg = args[i];
        const QString flag = QString::fromLatin1(name);
        if (arg == flag) {
            if (i + 1 >= args.size()) return false;
            into = args[++i];
            return true;
        }
        into = arg.mid(flag.size() + 1);
        return true;
    };
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--full")) {
            full = true;
        } else if (arg == QLatin1String("--looks")) {
            looks = true;
        } else if (arg == QLatin1String("--filters")) {
            filters = true;
        } else if (arg == QLatin1String("--file-info")) {
            file_info = true;
        } else if (arg == QLatin1String("--paths")) {
            paths = true;
        } else if (arg == QLatin1String("--stroke")) {
            if (i + 1 >= args.size()) return usage("argument --stroke: expected one argument");
            stroke = args[++i];
        } else if (arg.startsWith(QLatin1String("--stroke="))) {
            stroke = arg.mid(9);
        } else if (arg == QLatin1String("--pixel") || arg.startsWith(QLatin1String("--pixel="))) {
            if (!value_of(i, "--pixel", pixel)) return usage("argument --pixel: expected one argument");
        } else if (arg == QLatin1String("--layer") || arg.startsWith(QLatin1String("--layer="))) {
            if (!value_of(i, "--layer", layer)) return usage("argument --layer: expected one argument");
        } else if (arg == QLatin1String("--dpi") || arg.startsWith(QLatin1String("--dpi="))) {
            if (!value_of(i, "--dpi", dpi_text)) return usage("argument --dpi: expected one argument");
        } else if (arg == QLatin1String("--profile") || arg.startsWith(QLatin1String("--profile="))) {
            if (!value_of(i, "--profile", profile) || profile.isEmpty()) return usage("argument --profile: expected one argument");
        } else if (arg == QLatin1String("--area") || arg.startsWith(QLatin1String("--area="))) {
            if (!value_of(i, "--area", area_text) || area_text.isEmpty()) return usage("argument --area: expected one argument");
        } else if (arg == QLatin1String("--page") || arg.startsWith(QLatin1String("--page="))) {
            if (!value_of(i, "--page", page_text) || page_text.isEmpty()) return usage("argument --page: expected one argument");
        } else if (arg == QLatin1String("--at") || arg.startsWith(QLatin1String("--at="))) {
            if (!value_of(i, "--at", at_text) || at_text.isEmpty()) return usage("argument --at: expected one argument");
        } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
            return usage("unrecognized arguments: " + arg.toStdString());
        } else if (!have_src) {
            src = arg;
            have_src = true;
        } else {
            return usage("unrecognized arguments: " + arg.toStdString());
        }
    }
    if (!have_src) return usage("genko inspect <dir> [--full] [--stroke ID]: the following arguments are required: src");
    if ((!layer.isEmpty() || !dpi_text.isEmpty()) && pixel.isEmpty()) return usage("argument --layer, --dpi: only with --pixel");
    std::optional<std::int64_t> pixel_page;
    double x_mm = 0, y_mm = 0;
    int dpi = 0;
    if (!pixel.isEmpty()) {
        // <page>:<x_mm>,<y_mm>
        const qsizetype colon = pixel.indexOf(QLatin1Char(':'));
        const QStringList at = colon < 0 ? QStringList() : pixel.mid(colon + 1).split(QLatin1Char(','));
        bool ok_page = false, ok_x = false, ok_y = false;
        if (colon > 0 && at.size() == 2) {
            pixel_page = pixel.left(colon).toLongLong(&ok_page);
            x_mm = at[0].toDouble(&ok_x);
            y_mm = at[1].toDouble(&ok_y);
        }
        if (!ok_page || !ok_x || !ok_y) return usage("argument --pixel: <page>:<x_mm>,<y_mm>, e.g. 1:10.5,20");
        if (!dpi_text.isEmpty()) {
            bool ok = false;
            dpi = dpi_text.toInt(&ok);
            if (!ok || dpi < 1 || dpi > 2400) return usage("argument --dpi: an integer 1..2400");
        }
    }
    if (!profile.isEmpty() && !pixel.isEmpty()) return usage("argument --profile: not with --pixel");
    if (!area_text.isEmpty() && page_text.isEmpty()) return usage("argument --area, --page: --area <area> with --page <N>");
    if (!page_text.isEmpty() && area_text.isEmpty() && !paths) return usage("argument --page: only with --area or --paths");
    if (paths && !area_text.isEmpty()) return usage("argument --paths: not with --area");
    std::optional<std::int64_t> page_number;
    if (!page_text.isEmpty()) {
        bool ok = false;
        page_number = page_text.toLongLong(&ok);
        if (!ok) return usage("argument --page: invalid int value: " + core::py_repr_str(page_text.toStdString()));
    }
    std::optional<core::Json> area;
    if (!area_text.isEmpty()) {
        if (!pixel.isEmpty() || !profile.isEmpty()) return usage("argument --area: not with --pixel or --profile");
        try {
            area = core::Json::parse(area_text.toStdString());
        } catch (const std::exception&) {
            return usage("argument --area: an area as JSON, e.g. '{\"range\": {\"select\": \"reds\"}}'");
        }
    }
    std::optional<std::int64_t> at_page;
    double at_x = 0, at_y = 0;
    if (!at_text.isEmpty()) {  // (<page>:<x_mm>,<y_mm>, as --pixel)
        if (!pixel.isEmpty() || !profile.isEmpty() || !area_text.isEmpty() || paths || !page_text.isEmpty())
            return usage("argument --at: not with --pixel, --profile, --area, --paths or --page");
        const qsizetype colon = at_text.indexOf(QLatin1Char(':'));
        const QStringList xy = colon < 0 ? QStringList() : at_text.mid(colon + 1).split(QLatin1Char(','));
        bool ok_page = false, ok_x = false, ok_y = false;
        if (colon > 0 && xy.size() == 2) {
            at_page = at_text.left(colon).toLongLong(&ok_page);
            at_x = xy[0].toDouble(&ok_x);
            at_y = xy[1].toDouble(&ok_y);
        }
        if (!ok_page || !ok_x || !ok_y) return usage("argument --at: <page>:<x_mm>,<y_mm>, e.g. 1:10.5,20");
    }
    const auto loaded = storage::load_document(path_arg(src));
    if (at_page) {  // (PC3 C-d, layer.pickAt: the layers showing something there, the topmost first)
        const core::Document& doc = loaded.document;
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.pages[i]->index != core::Num(*at_page)) continue;
            if (doc.is_deferred(i)) break;
            const core::Page& page = doc.page(i);
            core::Json layers = core::Json::array();
            for (const std::string& id : render::layer_bounds::layers_at(doc, page, at_x, at_y)) {
                for (const core::Layer& shown : page.layers) {
                    if (shown.id != id) continue;
                    const std::string folder = render::layer_bounds::outermost(page, id);
                    layers.push_back(core::Json{{"id", id}, {"name", shown.title}, {"kind", std::string(core::to_string(shown.kind))},
                                                {"folder", folder != id ? core::Json(folder) : core::Json(nullptr)}});
                }
            }
            print_json(core::Json{{"page", *at_page}, {"at", core::Json::array({at_x, at_y})}, {"layers", layers}}, ascii);
            return 0;
        }
        throw core::Error("key", "no page " + std::to_string(*at_page));
    }
    if (paths) {  // (vector@1: the pages' paths, work paths, clipping paths and the layers' vector masks and shapes)
        print_json(render::path_info::book_paths(loaded.document, page_number), ascii);
        return 0;
    }
    if (file_info) {  // (PC4 F-b, file.fileInfo: the book's File Info and title, and what each page was opened from)
        const core::Document& doc = loaded.document;
        core::Json out = core::Json::object();
        out["title"] = doc.title;
        out["file_info"] = doc.file_info ? core::to_json(*doc.file_info) : core::Json::object();
        core::Json pages = core::Json::array();
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            const core::Page& page = *doc.pages[i];
            pages.push_back(core::Json{{"page", page.index.json()}, {"native_source", page.native_source ? core::to_json(*page.native_source) : core::Json(nullptr)}});
        }
        out["pages"] = std::move(pages);
        print_json(out, ascii);
        return 0;
    }
    if (filters) {  // (PC4 節 D: each kind's upstream id, Photoshop's name, settings, smart filter, inks; the presets)
        print_json(core::filter_catalogue(), ascii);
        return 0;
    }
    if (looks) {  // the built-in looks and the book's lookup tables (PC2a: カラールックアップの一覧)
        print_json(looks_listing(loaded.document), ascii);
        return 0;
    }
    if (!profile.isEmpty()) {  // (edit.profileInfo: what the profile says of itself, and the built-in ones)
        print_json(render::proof::profile_info_of(loaded.document, profile.toStdString()), ascii);
        return 0;
    }
    if (area) {  // (PC3 B1: the area as the ops are given it — resolved —, how much of the page it covers, its box)
        print_json(render::inspect_area(loaded.document, *page_number, *area), ascii);
        return 0;
    }
    if (pixel_page) {
        const core::Document& doc = loaded.document;
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.pages[i]->index != core::Num(*pixel_page)) continue;
            if (doc.is_deferred(i)) break;
            const core::Page& page = doc.page(i);
            const int at_dpi = dpi > 0 ? dpi : std::max(1, static_cast<int>(std::lround(page.spec.dpi.value())));
            print_json(render::pixel_at(page, &doc, x_mm, y_mm, layer.isEmpty() ? std::nullopt : std::optional<std::string>(layer.toStdString()), at_dpi),
                       ascii);
            return 0;
        }
        throw core::Error("key", "no page " + std::to_string(*pixel_page));
    }
    if (!stroke.isEmpty()) {
        print_json(storage::inspect_stroke(loaded.document, stroke.toStdString()), ascii);
    } else {
        print_json(storage::snapshot(loaded.document, full), ascii);
    }
    return 0;
}

// argparse's type=int: int(text), with spaces around and "_" between digits allowed.
std::optional<std::int64_t> py_int_arg(const QString& raw) {
    const std::string text = raw.trimmed().toStdString();
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    if (i >= text.size()) return std::nullopt;
    std::int64_t value = 0;
    bool digit_before = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            if (!digit_before || i + 1 >= text.size() || text[i + 1] < '0' || text[i + 1] > '9') return std::nullopt;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        if (value > (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10) return std::nullopt;
        value = value * 10 + (c - '0');
        digit_before = true;
    }
    return negative ? -value : value;
}

// The format Image.save picks from an extension among those written here ("" for the others).
std::string picture_format(const std::string& ext) {
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".jpe" || ext == ".jfif") return "jpeg";
    if (ext == ".tif" || ext == ".tiff") return "tiff";
    if (ext == ".pdf") return "pdf";
    if (ext == ".webp") return "webp";
    if (ext == ".bmp") return "bmp";
    if (ext == ".dib") return "dib";  // (BmpImagePlugin._dib_save: no file header)
    // (PC4 F-a: through the codecs)
    if (ext == ".gif") return "gif";
    if (ext == ".ppm" || ext == ".pgm" || ext == ".pbm") return "pnm";
    if (ext == ".tga") return "tga";
    if (ext == ".ico") return "ico";
    if (ext == ".qoi") return "qoi";
    if (ext == ".avif") return "avif";
    return {};
}

// (PC4 F-a) The drawn picture as the codecs' picture: grey, grey and alpha, RGB, RGBA as they are, any other mode as RGBA.
render::codecs::CodecImage codec_picture(const render::Image& image) {
    using render::codecs::Layout;
    const std::string_view mode = image.mode();
    const auto w = static_cast<std::uint32_t>(image.width()), h = static_cast<std::uint32_t>(image.height());
    const auto take = [&](const render::Image& p, Layout layout) {
        return render::codecs::CodecImage::from_bytes(w, h, layout, render::codecs::Sample::U8, p.tobytes());
    };
    if (mode == "L") return take(image, Layout::Gray);
    if (mode == "LA") return take(image, Layout::GrayA);
    if (mode == "RGB") return take(image, Layout::Rgb);
    return take(image.mode() == "RGBA" ? image : image.convert("RGBA"), Layout::Rgba);
}

// (PC4 F-a) Pillow's ICO of a picture: one entry for each of its sizes (16, 24, 32, 48, 64, 128, 256) the picture is at
// least as large as, each the picture made to fit it (thumbnail, Lanczos) — the picture itself when it is smaller than 16.
std::string icon_bytes(const render::Image& image) {
    std::vector<render::codecs::CodecImage> sizes;
    for (const int side : {16, 24, 32, 48, 64, 128, 256}) {
        if (side > image.width() || side > image.height()) continue;
        render::Image frame = image.copy();
        frame.thumbnail(render::Size{side, side}, render::Resample::Lanczos, std::nullopt);
        sizes.push_back(codec_picture(frame));
    }
    if (sizes.empty()) sizes.push_back(codec_picture(image));
    return render::codecs::encode_icon(sizes);
}

// Image.save(path) with Pillow's defaults for the format.
std::string picture_bytes(const render::Image& image, const std::string& format, const std::filesystem::path& out) {
    if (format == "png") return render::write_png(image);
    if (format == "jpeg") return formats::jpeg_bytes(image);
    if (format == "tiff") {
        formats::TiffSave save;
        save.compression = "raw";
        return formats::tiff_bytes(image, save);
    }
    if (format == "pdf") {
        return formats::pdf_bytes({image}, 72.0, core::path_to_utf8(out.stem()), core::utc_time_text("%Y%m%d%H%M%SZ"));
    }
    if (format == "bmp") return render::codecs::pillow_bmp_bytes(image);
    if (format == "dib") return render::codecs::pillow_bmp_bytes(image, false);
    // (PC4 F-a: the formats Pillow's rarer writers wrote, through the codecs — one 8-bit picture; AVIF at Pillow's
    // quality 75)
    if (format == "ico") return icon_bytes(image);
    if (format == "pnm") {  // (Pillow's PpmImagePlugin: whatever the extension, P4 of "1", P5 of "L", P6 of RGB and RGBA)
        render::codecs::EncodeOptions options;
        if (image.mode() == "1") options.pnm_kind = render::codecs::PnmKind::Bitmap;
        const bool grey = image.mode() == "1" || image.mode() == "L";
        return render::codecs::encode(codec_picture(grey ? image.convert("L") : image.mode() == "RGB" ? image : image.convert("RGB")),
                                      render::codecs::Format::Pnm, options);
    }
    if (format == "gif" || format == "tga" || format == "qoi" || format == "avif") {
        render::codecs::EncodeOptions options;
        if (format == "avif") options.jpeg_quality = 75;
        return render::codecs::encode(codec_picture(image), *render::codecs::from_id(format), options);
    }
    if (format == "webp") {
        // (one picture: WebPEncode, lossy, quality 80, method 4 — what Image.save writes)
        const std::filesystem::path scratch = out.parent_path() / (".genko-render-" + core::new_id() + ".webp");
        render::movie::write_movie({image.mode() == "RGB" ? image : image.convert("RGB")}, scratch, 1.0, "webp");
        std::string bytes;
        try {
            bytes = storage::read_file(scratch);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(scratch, ignored);
            throw;
        }
        std::error_code ignored;
        std::filesystem::remove(scratch, ignored);
        return bytes;
    }
    throw render::NotYetPorted("format:" + format);
}

// `genko render <dir> --page N [--dpi D] [--mode print|proof|name] --out file.png` (Python's render command): the
// page drawn as `python -m genko render` draws it, written as PNG — or JPEG, TIFF, PDF, WebP or BMP by --out's
// extension, as Image.save picks; {"ok": true, "path", "mode"}.
int render_command(const QStringList& args, bool ascii) {
    QString src;
    bool have_src = false;
    QString out;
    bool have_out = false;
    std::int64_t page_number = 1;
    std::int64_t dpi = 150;
    std::string mode = "print";
    const auto value_of = [&](qsizetype& i, const QString& arg, const QString& flag, QString& value) {
        if (arg == flag) {
            if (i + 1 >= args.size()) return 1;
            value = args[++i];
            return 2;
        }
        if (arg.startsWith(flag + QLatin1Char('='))) {
            value = arg.mid(flag.size() + 1);
            return 2;
        }
        return 0;
    };
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        QString value;
        int found = 0;
        if ((found = value_of(i, arg, QStringLiteral("--page"), value)) != 0) {
            if (found == 1) return usage("argument --page: expected one argument");
            const auto n = py_int_arg(value);
            if (!n) return usage("argument --page: invalid int value: '" + value.toStdString() + "'");
            page_number = *n;
        } else if ((found = value_of(i, arg, QStringLiteral("--dpi"), value)) != 0) {
            if (found == 1) return usage("argument --dpi: expected one argument");
            const auto n = py_int_arg(value);
            if (!n) return usage("argument --dpi: invalid int value: '" + value.toStdString() + "'");
            dpi = *n;
        } else if ((found = value_of(i, arg, QStringLiteral("--mode"), value)) != 0) {
            if (found == 1) return usage("argument --mode: expected one argument");
            mode = value.toStdString();
            if (mode != "name" && mode != "proof" && mode != "print") {
                return usage("argument --mode: invalid choice: '" + mode + "' (choose from 'name', 'proof', 'print')");
            }
        } else if ((found = value_of(i, arg, QStringLiteral("--out"), value)) != 0) {
            if (found == 1) return usage("argument --out: expected one argument");
            out = value;
            have_out = true;
        } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
            return usage("unrecognized arguments: " + arg.toStdString());
        } else if (!have_src) {
            src = arg;
            have_src = true;
        } else {
            return usage("unrecognized arguments: " + arg.toStdString());
        }
    }
    if (!have_src || !have_out) {
        std::string missing;
        if (!have_src) missing = "src";
        if (!have_out) missing += std::string(missing.empty() ? "" : ", ") + "--out";
        return usage("genko render <dir> --page N [--dpi D] [--mode print|proof|name] --out file.png: the following "
                     "arguments are required: " + missing);
    }
    // (Python takes any int: a page is at least one pixel; one too large to hold is refused while drawing)
    if (dpi < std::numeric_limits<int>::min() || dpi > std::numeric_limits<int>::max()) {
        return fail("the page is too large at this resolution", "image_too_large", ascii, 1);
    }
    const std::string out_text = pathlib_text(out.toStdString());
    const std::filesystem::path out_path = storage::path_from_utf8(out_text);
    // the format from the file's extension, as Pillow's Image.save chooses it
    std::string ext = core::path_to_utf8(out_path.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png") {
        static const char* const kPillowSaves[] = {".jpg", ".jpeg", ".jpe", ".jfif", ".tif", ".tiff", ".bmp", ".dib", ".gif",
                                                   ".webp", ".pdf", ".ppm", ".pgm", ".pbm", ".tga", ".ico", ".im", ".pcx", ".eps",
                                                   ".j2k", ".jp2", ".jpx", ".icns", ".sgi", ".spi", ".xbm", ".qoi", ".avif"};
        if (std::find(std::begin(kPillowSaves), std::end(kPillowSaves), ext) == std::end(kPillowSaves)) {
            return fail("unknown file extension: " + ext, "value", ascii, 1);
        }
        // (the formats a picture of a page is saved in: JPEG, TIFF, PDF, WebP and BMP as Image.save writes them; GIF, PPM,
        // PGM, PBM, TGA, ICO, QOI and AVIF through the codecs (PC4 F-a); Pillow's other writers — JPEG 2000, ICNS, SGI, IM,
        // PCX, EPS, XBM… — are not in the C++ build)
        if (picture_format(ext).empty()) throw render::NotYetPorted("format:" + ext.substr(1));
    }

    const auto loaded = storage::load_document(storage::path_from_utf8(pathlib_text(src.toStdString())));
    const core::Document& doc = loaded.document;
    render::brushes::clear_custom();
    render::brushes::register_book(doc);  // (the book's own brushes, as Python's reader registers them, and their papers)
    const core::Page* page = nullptr;
    for (const auto& p : doc.pages) {
        if (p->index == core::Num(page_number)) {
            page = p.get();
            break;
        }
    }
    if (page == nullptr) return fail("no page " + std::to_string(page_number), "not_found", ascii, 1);
    render::RenderOptions options;
    options.mode = mode;
    const render::RenderResult result = render::render_page(*page, static_cast<int>(dpi), options, &doc);
    if (out_path.has_parent_path()) {  // (args.out.parent.mkdir, before image.save: the WebP is written beside it)
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }
    std::string bytes;
    const std::string format = ext == ".png" ? std::string("png") : picture_format(ext);
    try {
        bytes = picture_bytes(result.image, format, out_path);
    } catch (const core::Error& error) {
        return fail(error.what(), error.code(), ascii, 1);  // (Pillow's OSError: "cannot write mode RGBA as JPEG")
    }
    storage::write_atomic(out_path, bytes);
    core::Json json = core::Json::object();
    json["ok"] = true;
    json["path"] = out_text;
    json["mode"] = mode;
    print_json(json, ascii);
    return 0;
}

// `genko export <book> <out> --format cube [--page N] [--lut-size 2..256] [--title T] [--json]` (PC2a, the C++ build's own):
// page N's visible correction layers (1 by default) as a .cube 3D LUT of the size (33) — `out` a .cube file, or a folder
// that gets p<NNN>.cube —; the path, or with --json {"ok": true, "count", "files", "size", "skipped"?}.
int export_cube(const Arguments& a, bool ascii) {
    std::int64_t size = 33, wanted = 1;
    if (const auto value = a.value("--lut-size")) {
        const auto n = int_option(*value);
        if (!n || *n < 2 || *n > 256) return usage("genko export: argument --lut-size: an integer 2..256");
        size = *n;
    }
    if (const auto value = a.value("--page")) {
        const auto n = int_option(*value);
        if (!n) return usage("genko export: argument --page: invalid int value: " + core::py_repr_str(value->toStdString()));
        wanted = *n;
    }
    const fs::path src = path_arg(a.positional[0]);
    const fs::path out = path_arg(a.positional[1]);
    const auto loaded = storage::load_document(src);
    const core::Document& doc = loaded.document;
    const core::Page* page = nullptr;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index == core::Num(wanted) && !doc.is_deferred(i)) page = &doc.page(i);
    }
    if (page == nullptr) return usage("no page " + std::to_string(wanted), 1);
    const std::string title = a.value("--title") ? a.value("--title")->toStdString() : (doc.title.empty() ? std::string("Genko") : doc.title);
    std::vector<std::string> skipped;
    const std::string cube = formats::bake_cube(doc, *page, static_cast<int>(size), title, &skipped);
    const bool file_given = !formats::detail::suffix(out).empty();
    const fs::path dest = file_given ? out : formats::detail::join(out, "p" + formats::detail::padded(page->index, 3) + ".cube");
    if (!file_given) fs::create_directories(out);
    storage::write_atomic(dest, cube);
    if (a.flag("--json")) {
        core::Json json{{"ok", true}, {"count", 1}, {"files", core::Json::array({core::path_to_utf8(dest)})}, {"size", size}};
        if (!skipped.empty()) json["skipped"] = skipped;
        print_json(json, ascii);
    } else {
        write_text(core::path_to_utf8(dest) + "\n", stdout);
    }
    return 0;
}

// vector@1 (PC3 C-a, file.export.pathsToIllustrator): `genko export <book> <out> --format ai [--page N] [--paths all|work|<name>]`
// — the page's paths as an Adobe Illustrator file (a folder: p<page>.ai in it); the file's path, or with --json {"ok",
// "count", "files", "paths"}.
int export_ai(const Arguments& a, bool ascii) {
    std::int64_t wanted = 1;
    if (const auto value = a.value("--page")) {
        const auto n = int_option(*value);
        if (!n) return usage("genko export: argument --page: invalid int value: " + core::py_repr_str(value->toStdString()));
        wanted = *n;
    }
    const std::string which = a.value("--paths") ? a.value("--paths")->toStdString() : std::string("all");
    const fs::path src = path_arg(a.positional[0]);
    const fs::path out = path_arg(a.positional[1]);
    const auto loaded = storage::load_document(src);
    const core::Document& doc = loaded.document;
    const core::Page* page = nullptr;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index == core::Num(wanted) && !doc.is_deferred(i)) page = &doc.page(i);
    }
    if (page == nullptr) return usage("no page " + std::to_string(wanted), 1);
    int count = 0;
    const std::string text = render::path_info::illustrator(doc, *page, which, &count);
    if (count == 0) return fail("no paths match " + core::py_repr_str(which) + " on page " + std::to_string(wanted), "apply", ascii, 1);
    const bool file_given = !formats::detail::suffix(out).empty();
    const fs::path dest = file_given ? out : formats::detail::join(out, "p" + formats::detail::padded(page->index, 3) + ".ai");
    if (!file_given) fs::create_directories(out);
    storage::write_atomic(dest, text);
    if (a.flag("--json")) {
        print_json(core::Json{{"ok", true}, {"count", 1}, {"files", core::Json::array({core::path_to_utf8(dest)})}, {"paths", count}}, ascii);
    } else {
        write_text(core::path_to_utf8(dest) + "\n", stdout);
    }
    return 0;
}

// `genko export <book> <out> [--format F] [options]` (Python's export command, genko/__main__.py): the book written
// out in one format; the paths of the files one a line, or with --json {"ok": true, "count", "files"}.
int export_command(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--json", false},      {"--dpi", true},       {"--format", true},     {"--width", true},
                                          {"--max-height", true}, {"--long-edge", true}, {"--jpeg", false},      {"--spreads", false},
                                          {"--color", true},      {"--icc", true},       {"--screen-lpi", true}, {"--screen-shape", true},
                                          {"--area", true},       {"--fps", true},       {"--seconds", true},    {"--page", true},
                                          {"--psd-text", true},   {"--rasterize-unsupported", false}, {"--lut-size", true},
                                          {"--title", true},      {"--paths", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, out", a)) return usage("genko export: " + *error);
    // PC2a (file.export.colorLookupTables, the C++ build's own: not among Python's formats, whose choices it leaves as they
    // are): a page's correction layers as a .cube file
    if (a.value("--format") == std::optional<QString>(QStringLiteral("cube"))) return export_cube(a, ascii);
    // vector@1 (PC3 C-a, file.export.pathsToIllustrator, the C++ build's own): a page's paths as an Illustrator file
    if (a.value("--format") == std::optional<QString>(QStringLiteral("ai"))) return export_ai(a, ascii);
    if (a.value("--paths")) return usage("genko export: argument --paths: only with --format ai");
    if (a.value("--lut-size") || a.value("--title")) return usage("genko export: argument --lut-size, --title: only with --format cube");
    // argparse's choices
    const auto choice = [&](const char* name, const char* fallback, std::initializer_list<const char*> choices) -> std::optional<std::string> {
        const std::string value = a.value(name).value_or(QString::fromLatin1(fallback)).toStdString();
        for (const char* c : choices) {
            if (value == c) return value;
        }
        return std::nullopt;
    };
    const auto invalid_choice = [&](const char* name, std::initializer_list<const char*> choices) {
        std::string listed;
        for (const char* c : choices) listed += std::string(listed.empty() ? "" : ", ") + "'" + c + "'";
        return usage("genko export: argument " + std::string(name) + ": invalid choice: " + core::py_repr_str(a.value(name)->toStdString()) +
                     " (choose from " + listed + ")");
    };
    const std::initializer_list<const char*> kFormats{"png", "tiff",   "pdf",    "strip",  "psd",       "epub",     "pack",
                                                      "webtoon", "sns", "cmyk", "layers", "kindle", "timelapse", "animation"};
    const std::initializer_list<const char*> kColours{"rgb", "cmyk", "gray"};
    const std::initializer_list<const char*> kShapes{"round", "square", "diamond", "ellipse"};
    const std::initializer_list<const char*> kAreas{"paper", "bleed", "trim"};
    const auto fmt = choice("--format", "png", kFormats);
    if (!fmt) return invalid_choice("--format", kFormats);
    const auto color = choice("--color", "rgb", kColours);
    if (!color) return invalid_choice("--color", kColours);
    const auto shape = choice("--screen-shape", "round", kShapes);
    if (!shape) return invalid_choice("--screen-shape", kShapes);
    const auto area = choice("--area", "paper", kAreas);
    if (!area) return invalid_choice("--area", kAreas);
    // PSD-01 (C++ only): the lines' text kept as text layers in a PSD (default: as pictures, Python's PSD)
    const std::initializer_list<const char*> kPsdTexts{"image", "editable"};
    const auto psd_text = choice("--psd-text", "image", kPsdTexts);
    if (!psd_text) return invalid_choice("--psd-text", kPsdTexts);
    if (*psd_text == "editable" && *fmt != "psd") return usage("genko export: argument --psd-text: editable is for --format psd");
    if (a.flag("--rasterize-unsupported") && *psd_text != "editable") {
        return usage("genko export: argument --rasterize-unsupported: only with --psd-text editable");
    }
    const formats::PsdText psd_choice{*psd_text == "editable", a.flag("--rasterize-unsupported")};
    formats::PsdTextReport psd_report;
    std::map<std::string, std::optional<std::int64_t>> ints;
    for (const char* name : {"--dpi", "--width", "--max-height", "--long-edge", "--page"}) {
        if (const auto value = a.value(name)) {
            const auto n = int_option(*value);
            if (!n) return usage("genko export: argument " + std::string(name) + ": invalid int value: " + core::py_repr_str(value->toStdString()));
            ints[name] = *n;
        }
    }
    std::map<std::string, std::optional<double>> floats;
    for (const char* name : {"--screen-lpi", "--fps", "--seconds"}) {
        if (const auto value = a.value(name)) {
            try {
                floats[name] = core::py_float(Json(value->toStdString()));
            } catch (const core::Error&) {
                return usage("genko export: argument " + std::string(name) + ": invalid float value: " + core::py_repr_str(value->toStdString()));
            }
        }
    }
    const auto int_or = [&](const char* name, std::int64_t fallback) { return ints[name].value_or(fallback); };
    const auto narrow = [](std::int64_t v) {  // (pixels or dots past what a picture can hold: refused as drawing refuses them)
        if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
            throw core::Error("image_too_large", "the picture is too large at this resolution");
        }
        return static_cast<int>(v);
    };
    const std::optional<std::int64_t> dpi = ints["--dpi"] && *ints["--dpi"] != 0 ? ints["--dpi"] : std::nullopt;  // (args.dpi or …)

    const fs::path src = path_arg(a.positional[0]);
    const fs::path out = path_arg(a.positional[1]);
    const bool file_given = !formats::detail::suffix(out).empty();  // (args.out.suffix)
    const auto loaded = storage::load_document(src);
    const core::Document& doc = loaded.document;
    render::brushes::clear_custom();
    render::brushes::register_book(doc);  // (the book's own brushes, as Python's reader registers them, and their papers)
    const std::int64_t spec_dpi = doc.spec.dpi.truthy() ? core::py_int(doc.spec.dpi) : 0;
    std::vector<fs::path> paths;
    const core::Page* animated = nullptr;  // (an animation: its page)
    const std::string& f = *fmt;
    // (the resolution of a format that takes one, 1 to 100000 dpi: refused before anything is drawn or made)
    if (f != "webtoon" && f != "sns" && f != "kindle" && f != "timelapse") {
        const std::int64_t fallback = f == "strip" || f == "epub" ? 150 : f == "animation" ? 100 : f == "layers" ? spec_dpi : spec_dpi != 0 ? spec_dpi : 600;
        formats::check_dpi(dpi.value_or(fallback));
    }
    if (f == "strip") {
        paths = {formats::export_strip(doc, file_given ? out : formats::detail::join(out, "strip.png"), narrow(dpi.value_or(150)))};
    } else if (f == "psd") {
        // a folder gets one layered PSD per page; a .psd path gets the first page
        try {
            if (file_given) {
                paths = {formats::export_psd(doc, out, dpi.value_or(spec_dpi), 1, psd_choice, &psd_report)};
            } else {
                paths = formats::export_psd_pages(doc, out, dpi, psd_choice, &psd_report);
            }
        } catch (const formats::PsdTextRefused& refused) {  // (strict: nothing written; the lines and what they use)
            const Json items = formats::issues_json(refused.issues());
            return fail(refused.what(), refused.code(), ascii, 1, &items);
        }
    } else if (f == "epub") {
        paths = {formats::export_epub(doc, file_given ? out : formats::detail::join(out, "out.epub"), narrow(dpi.value_or(150)))};
    } else if (f == "webtoon") {
        paths = formats::export_webtoon(doc, out, narrow(int_or("--width", 800)), narrow(int_or("--max-height", 1280)), 0,
                                        a.flag("--jpeg") ? "jpeg" : "png");
    } else if (f == "sns") {
        paths = formats::export_sns(doc, out, narrow(int_or("--long-edge", 2048)), a.flag("--jpeg") ? "jpeg" : "png", 92, a.flag("--spreads"));
    } else if (f == "pack") {
        paths = formats::export_pack(doc, out, "shueisha", dpi);
    } else if (f == "layers") {
        paths = formats::export_layers(doc, out, narrow(dpi.value_or(spec_dpi)), *area);
    } else if (f == "kindle") {
        const std::int64_t given = int_or("--long-edge", 2048);
        const std::int64_t long_edge = given != 2048 ? given : formats::kKindleLongEdge;
        paths = {formats::export_kindle(doc, file_given ? out : formats::detail::join(out, "kindle.epub"), narrow(long_edge))};
    } else if (f == "animation") {
        const std::int64_t wanted = ints["--page"] && *ints["--page"] != 0 ? *ints["--page"] : 1;  // (args.page or 1)
        const core::Page* page = nullptr;
        for (const auto& p : doc.pages) {
            if (p->index == core::Num(wanted)) {
                page = p.get();
                break;
            }
        }
        if (page == nullptr) {  // (Python: SystemExit with these words, exit 1)
            return usage("no page " + (ints["--page"] ? std::to_string(*ints["--page"]) : std::string("None")), 1);
        }
        const fs::path dest = file_given ? out : formats::detail::join(out, "p" + formats::detail::padded(page->index, 3) + ".gif");
        paths = render::anim::export_animation(*page, dest, &doc, narrow(dpi.value_or(100)));
        animated = page;
    } else if (f == "timelapse") {
        std::optional<Json> page;
        if (ints["--page"]) page = Json(*ints["--page"]);
        paths = {render::timelapse::export_timelapse(src, file_given ? out : formats::detail::join(out, "timelapse.webp"), page,
                                                     floats["--fps"].value_or(12.0), floats["--seconds"])};
    } else {
        std::optional<Json> screen;
        if (floats["--screen-lpi"] && *floats["--screen-lpi"] != 0.0) {
            screen = Json::object();
            (*screen)["lpi"] = *floats["--screen-lpi"];
            (*screen)["shape"] = *shape;
        }
        paths = formats::export_print(doc, out, f, dpi, 180, true, *area, f == "cmyk" ? std::string("cmyk") : *color,
                                      a.value("--icc").value_or(QString()).toStdString(), screen);
    }
    // (color-mode@1: an animation says its page's; a time-lapse is the history's pictures)
    std::vector<std::string> notes;
    if (animated != nullptr) {
        if (const auto note = formats::ink_note(*animated)) notes.push_back(*note);
    } else if (f != "timelapse") {
        notes = formats::export_notes(doc, f, f == "cmyk" ? std::string("cmyk") : *color);
    }
    if (a.flag("--json")) {
        Json files = Json::array();
        for (const auto& p : paths) files.push_back(core::path_to_utf8(p));
        Json json = Json::object();
        json["ok"] = true;
        json["count"] = static_cast<std::int64_t>(paths.size());
        json["files"] = files;
        if (psd_choice.editable) json["psd_text"] = psd_report.to_json();  // (what the editable PSD did)
        if (!notes.empty()) json["notes"] = notes;  // (color-mode@1: pages written from the colours on screen; 32 bits at 16)
        print_json(json, ascii);
    } else {
        std::string lines;
        for (const auto& p : paths) lines += core::path_to_utf8(p) + "\n";
        write_text(lines, stdout);
        for (const std::string& note : notes) write_text(note + "\n", stderr);
        // (the editable PSD's lines drawn as pictures and its font warnings, said on stderr)
        if (psd_choice.editable && (!psd_report.rasterized.empty() || !psd_report.warnings.empty())) {
            write_text(core::dump_python(Json{{"psd_text", psd_report.to_json()}}, ascii) + "\n", stderr);
        }
    }
    return 0;
}

int schema(const QStringList& args, bool ascii) {
    if (!args.isEmpty()) return usage("unrecognized arguments: " + args.join(QLatin1Char(' ')).toStdString());
    core::Json out = core::Json::object();
    out["ok"] = true;
    out["ops"] = core::ops_schema();
    print_json(out, ascii);
    return 0;
}

// genko new <dest> --from <file> [--as <ext>] and genko new <dest> --image … (PC4 F-b, SPEC.md FILE-01: 開く…・指定形式で開く…
// and 新規… → 画像 as a book written at once): the book of one page formats::open_file or formats::new_image makes, written
// to `dest` (never over a book), {"ok", "path", "notes", "snapshot"}.
int new_book_of(const Arguments& a, bool ascii) {
    const std::string dest_text = pathlib_text(a.positional[0].toStdString());
    const fs::path dest = storage::path_from_utf8(dest_text);
    std::error_code ec;
    if (fs::exists(dest / "project.json", ec)) {
        return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json") + " (genko new never writes over a book)", "exists", ascii, 1);
    }
    formats::Opened made;
    if (const auto from = a.value("--from")) {
        for (const char* other : {"--width", "--height", "--unit", "--dpi", "--mode", "--depth", "--background", "--profile"}) {
            if (a.value(other)) return usage(std::string("genko new: argument ") + other + ": only with --image");
        }
        formats::OpenOptions options;
        if (const auto as = a.value("--as")) {
            options.as = render::codecs::from_extension(as->toStdString());
            if (!options.as) return usage("genko new: argument --as: not a format this build knows: " + core::py_repr_str(as->toStdString()));
        }
        const std::string file = pathlib_text(from->toStdString());
        made = formats::open_file(storage::read_file(storage::path_from_utf8(file)), file, options);
        if (const auto title = a.value("--title")) made.doc.title = title->toStdString();
    } else {
        if (a.value("--as")) return usage("genko new: argument --as: only with --from");
        formats::NewImage image;
        image.title = a.value("--title").value_or(QStringLiteral("無題")).toStdString();
        const auto number = [&](const char* name, double fallback) -> std::optional<double> {
            const auto text = a.value(name);
            if (!text) return fallback;
            bool ok = false;
            const double v = text->trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(v)) return std::nullopt;
            return v;
        };
        const auto dpi = number("--dpi", 72);
        if (!dpi) return usage("genko new: argument --dpi: a number of pixels an inch");
        image.dpi = *dpi;
        const QString unit = a.value("--unit").value_or(QStringLiteral("px"));
        const double per = unit == QLatin1String("px") ? 1.0 : unit == QLatin1String("mm") ? image.dpi / 25.4 : unit == QLatin1String("cm") ? image.dpi / 2.54
                         : unit == QLatin1String("in") ? image.dpi : 0.0;
        if (per == 0.0) return usage("genko new: argument --unit: px, mm, cm or in");
        const auto width = number("--width", 0), height = number("--height", 0);
        if (!a.value("--width") || !a.value("--height") || !width || !height) return usage("genko new --image: --width and --height are required (numbers)");
        image.width = std::llround(*width * per);
        image.height = std::llround(*height * per);
        image.mode = a.value("--mode").value_or(QStringLiteral("rgb")).toStdString();
        const auto depth = int_option(a.value("--depth").value_or(QStringLiteral("8")));
        if (!depth) return usage("genko new: argument --depth: 8, 16 or 32");
        image.depth = static_cast<int>(*depth);
        image.background = a.value("--background").value_or(QStringLiteral("white")).toLower().toStdString();
        image.profile = a.value("--profile").value_or(QString()).toStdString();
        made = formats::new_image(image);
    }
    storage::ProjectLock lock(dest, std::string(core::kLegacyActor));
    lock.try_acquire();
    if (fs::exists(dest / "project.json", ec)) return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json"), "exists", ascii, 1);
    storage::SaveRequest request;
    request.actor = std::string(core::kLegacyActor);
    request.base_revision = 0;
    request.ops = Json::array();
    storage::Saver(lock).save(made.doc, request);
    lock.release();
    if (a.flag("--plain")) {
        write_text(dest_text + "\n", stdout);
        return 0;
    }
    Json notes = Json::array();
    for (const std::string& note : made.notes) notes.push_back(note);
    Json out = Json::object();
    out["ok"] = true;
    out["path"] = dest_text;
    out["notes"] = std::move(notes);
    out["snapshot"] = storage::snapshot(made.doc);
    print_json(out, ascii);
    return 0;
}

int new_book(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--title", true},  {"--episode", true}, {"--pages", true}, {"--webtoon", false},
                                          {"--b4", false},    {"--preset", true},  {"--paper", true}, {"--json", false},
                                          {"--plain", false},
                                          // (PC4 F-b: a book of a file, or of a new image)
                                          {"--from", true},   {"--as", true},      {"--image", false}, {"--width", true},
                                          {"--height", true}, {"--unit", true},    {"--dpi", true},    {"--mode", true},
                                          {"--depth", true},  {"--background", true}, {"--profile", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "dest", a)) return usage("genko new: " + *error);
    if (a.value("--from") || a.flag("--image")) {
        if (a.value("--from") && a.flag("--image")) return usage("genko new: argument --from: not with --image");
        for (const char* comic : {"--episode", "--pages", "--preset", "--paper"}) {
            if (a.value(comic)) return usage(std::string("genko new: argument ") + comic + ": not with --from or --image");
        }
        if (a.flag("--webtoon") || a.flag("--b4")) return usage("genko new: argument --webtoon, --b4: not with --from or --image");
        return new_book_of(a, ascii);
    }
    for (const char* image : {"--as", "--width", "--height", "--unit", "--dpi", "--mode", "--depth", "--background", "--profile"}) {
        if (a.value(image)) return usage(std::string("genko new: argument ") + image + ": only with --from or --image");
    }
    const auto episode = int_option(a.value("--episode").value_or(QStringLiteral("1")));
    if (!episode) return usage("genko new: argument --episode: invalid int value: " + core::py_repr_str(a.value("--episode")->toStdString()));
    const auto pages = int_option(a.value("--pages").value_or(QStringLiteral("8")));
    if (!pages) return usage("genko new: argument --pages: invalid int value: " + core::py_repr_str(a.value("--pages")->toStdString()));
    core::PageSpec spec;
    const std::string paper = a.value("--paper").value_or(QString()).toStdString();
    if (!paper.empty()) {
        const core::PaperPreset* preset = core::find_paper_preset(paper);
        if (preset == nullptr) {
            std::string names;
            for (const auto& item : core::paper_presets()) names += (names.empty() ? "" : ", ") + std::string(item.key);
            return usage("--paper must be one of " + names, 1);  // (Python: SystemExit with this text, exit 1)
        }
        spec = preset->make();
    } else if (a.flag("--webtoon")) {
        spec = core::PageSpec::webtoon();
    } else if (const auto preset = a.value("--preset"); preset && !preset->isEmpty()) {
        spec = core::PageSpec::publisher(preset->toStdString());
    } else if (a.flag("--b4")) {
        spec = core::PageSpec::b4_comic();
    } else {
        spec = core::PageSpec::a4_mono();
    }
    const std::string dest_text = pathlib_text(a.positional[0].toStdString());
    const fs::path dest = storage::path_from_utf8(dest_text);
    std::error_code ec;
    if (fs::exists(dest / "project.json", ec)) {
        return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json") +
                        " (genko new never writes over a book)",
                    "exists", ascii, 1);
    }
    const auto page_count = static_cast<int>(std::clamp<std::int64_t>(*pages, -1, 100000));
    core::Document doc = core::new_episode(a.value("--title").value_or(QStringLiteral("無題")).toStdString(),
                                           core::Num(*episode), page_count, spec);
    storage::ProjectLock lock(dest, std::string(core::kLegacyActor));
    lock.try_acquire();
    if (fs::exists(dest / "project.json", ec)) {
        return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json"), "exists", ascii, 1);
    }
    storage::SaveRequest request;
    request.actor = std::string(core::kLegacyActor);
    request.base_revision = 0;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
    lock.release();
    if (a.flag("--plain")) {
        write_text(dest_text + "\n", stdout);
    } else {
        Json out = Json::object();
        out["ok"] = true;
        out["path"] = dest_text;
        out["snapshot"] = storage::snapshot(doc);
        print_json(out, ascii);
    }
    return 0;
}

int apply(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--dry-run", false}, {"--expect-revision", true}, {"--agent", true}, {"--txn", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, ops", a)) return usage("genko apply: " + *error);
    std::optional<std::int64_t> expect;
    if (const auto value = a.value("--expect-revision")) {
        expect = int_option(*value);
        if (!expect) return usage("genko apply: argument --expect-revision: invalid int value: " + core::py_repr_str(value->toStdString()));
    }
    const bool dry_run = a.flag("--dry-run");
    const std::string txn = a.value("--txn").value_or(QString()).toStdString();
    if (!txn.empty() && !core::is_txn_id(txn)) return usage("genko apply: --txn takes 32 lowercase hex digits");

    // the ops first, as Python reads them
    const QString source = a.positional[1];
    const std::string raw = source == QLatin1String("-") ? read_stdin() : storage::read_file(path_arg(source));
    core::ParseRepairs repairs;
    const Json ops = core::parse_python_json(raw, &repairs, text_file());
    if (!repairs.nonfinite.empty()) {
        throw core::ApplyError("ops must not hold NaN or Infinity (at " + repairs.nonfinite.front() + ")");
    }
    if (!ops.is_array()) throw core::ApplyError("ops file must be a JSON array");

    const fs::path dir = path_arg(a.positional[0]);
    const std::string agent =
        a.value("--agent").value_or(QString()).isEmpty()
            ? (is_studio_project(dir) ? std::string("legacy:unknown") : std::string(core::kLegacyActor))
            : a.value("--agent")->toStdString();
    refuse_legacy(dir);
    storage::ProjectLock lock(dir, agent);
    lock.try_acquire();
    storage::journal::repair(dir);  // (the book is read at its last consistent point)
    // Read inside the lock, so a concurrent writer's changes are never overwritten.
    const storage::LoadResult loaded = storage::load_document(dir);
    if (loaded.report.source_version < 4) throw storage::NeedsMigration(dir, loaded.report.source_version);

    // A retry of a transaction saved before gets its revision back, whatever revision the book has reached since
    // (schema-v4 §4.3); then the base revision is checked.
    if (!txn.empty() && !dry_run) {
        const auto txns = storage::journal::transactions(storage::journal::read_lines(storage::journal::journal_file(dir)));
        if (const auto* done = storage::journal::find_committed(txns, txn)) {
            Json out = Json::object();  // (this transaction was saved before: it is not applied again)
            out["ok"] = true;
            Json applied = Json::array();
            for (const Json& op : ops) {
                const auto name = op.is_object() ? op.find("op") : op.end();
                applied.push_back(op.is_object() && name != op.end() ? core::py_str(*name) : std::string("None"));
            }
            out["applied"] = applied;
            out["snapshot"] = storage::snapshot(loaded.document);
            out["job_id"] = core::new_id();
            out["warnings"] = Json::array();
            out["revision"] = done->rev;
            out["txn"] = txn;
            out["already_committed"] = true;
            print_json(out, ascii);
            return 0;
        }
    }
    if (expect && loaded.document.revision != *expect) throw storage::RevisionConflict(*expect, loaded.document.revision);

    // [{"op": "undo"}] on its own undoes the latest saved change, as `genko undo --as <agent>` does (the same checks:
    // another actor's change, approvals that need a person, an outside edit). Python's apply_ops undoes its session's
    // changes here; a book's saved changes are in its journal. (A dry run only shows the book, as Python's does.)
    const bool undo_op = ops.size() == 1 && ops[0].is_object() && ops[0].contains("op") && ops[0]["op"] == Json("undo");
    if (undo_op && !dry_run) {
        const storage::RestoreResult restored = storage::restore(lock, agent, false, false, txn);
        const storage::LoadResult after = storage::load_document(dir);
        Json out = Json::object();
        out["ok"] = true;
        out["applied"] = Json::array({"undo"});
        out["snapshot"] = storage::snapshot(after.document);
        out["job_id"] = core::new_id();
        out["kind"] = restored.kind;
        out["rev"] = restored.rev;
        out["revision"] = restored.revision;
        out["txn"] = restored.txn;
        lock.release();
        print_json(out, ascii);
        return 0;
    }

    const core::CommandBus bus(render::ops_registry());
    const core::ApplyResult result = bus.apply(loaded.document, ops, core::Actor(agent), dry_run);
    Json out = Json::object();
    out["ok"] = true;
    out["applied"] = result.applied;
    out["snapshot"] = storage::snapshot(result.doc);
    out["job_id"] = core::new_id();
    if (result.has_warnings) out["warnings"] = result.warnings;
    if (!result.results.empty()) out["results"] = result.results;
    if (dry_run) {
        out["revision"] = loaded.document.revision;
    } else {
        storage::SaveRequest request;
        request.actor = agent;
        request.base_revision = loaded.document.revision;
        request.ops = result.journal_ops;
        request.txn = txn;
        const bool lapse = render::timelapse::is_on(result.doc);  // (タイムラプス: the pages this save changes)
        Json before;
        if (lapse) {
            try {
                before = storage::read_disk_state(dir).payload;
            } catch (const std::exception&) {  // (the timelapse never stops a save: every page is recorded then)
            }
        }
        const storage::SaveResult saved = storage::Saver(lock).save(result.doc, request);
        if (lapse && !saved.already_committed) render::timelapse::after_save(dir, result.doc, before);
        out["revision"] = saved.revision;
        out["txn"] = saved.txn;
        if (saved.repaired) out["repaired"] = true;
    }
    lock.release();
    print_json(out, ascii);
    return 0;
}

int restore(const QStringList& args, bool ascii, bool redo) {
    static constexpr Option kOptions[] = {{"--as", true}, {"--force", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "src", a)) return usage(std::string(redo ? "genko redo: " : "genko undo: ") + *error);
    const fs::path dir = path_arg(a.positional[0]);
    const std::string actor = a.value("--as").value_or(QStringLiteral("genko")).toStdString();
    refuse_legacy(dir);
    print_json(storage::restore(dir, actor, redo, a.flag("--force")).to_json(), ascii);
    return 0;
}

int gc(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--dry-run", false}, {"--legacy", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "src", a)) return usage("genko gc: " + *error);
    const fs::path dir = path_arg(a.positional[0]);
    refuse_legacy(dir);
    print_json(storage::gc(dir, a.flag("--dry-run"), std::string(core::kLegacyActor), a.flag("--legacy")), ascii);
    return 0;
}

int doctor(const QStringList& args, bool ascii) {
    Arguments a;
    if (auto error = parse_arguments(args, {}, 1, "src", a)) return usage("genko doctor: " + *error);
    Json report = storage::doctor(path_arg(a.positional[0]));
    // (maintenance.doctor: the bundled font — here in the program's resources)
    if (!render::bundled_font_present("sfx")) {
        report["problems"].push_back("bundled font missing: :/genko/text/sfx (DelaGothicOne-Regular.ttf)");
        report["ok"] = false;
    }
    print_json(report, ascii);
    return report["ok"].get<bool>() ? 0 : 1;
}

int migrate(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--as", true}, {"--accept-repairs", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, dst", a)) return usage("genko migrate: " + *error);
    const std::string actor = a.value("--as").value_or(QStringLiteral("genko")).toStdString();
    try {
        print_json(storage::convert(path_arg(a.positional[0]), path_arg(a.positional[1]), actor, a.flag("--accept-repairs")),
                   ascii);
    } catch (const storage::ConvertError& error) {
        return fail(error.what(), error.code(), ascii, 1, &error.report());
    }
    return 0;
}

// smart-objects@1 (PC3 A1, SPEC COLOR-05「中身とリンク」, the C++ build's own): `genko smart <dir> --links` — the book's
// linked files, each ok, modified or missing (the files are read here, never by an op) —, and `genko smart <dir> --export
// <page> <id> <path> [--format psd|png]` (コンテンツを書き出し…: a file's own bytes, a page's as a PSD or a PNG).
int smart_command(const QStringList& args, bool ascii) {
    QString src, format;
    bool links = false;
    std::optional<std::array<QString, 3>> exported;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--links")) {
            links = true;
        } else if (arg == QLatin1String("--export")) {
            if (i + 3 >= args.size()) return usage("genko smart: argument --export: expected three arguments (page, id, path)");
            exported = std::array<QString, 3>{args[i + 1], args[i + 2], args[i + 3]};
            i += 3;
        } else if (arg == QLatin1String("--format") || arg.startsWith(QLatin1String("--format="))) {
            if (arg.contains(QLatin1Char('='))) {
                format = arg.mid(9);
            } else {
                if (i + 1 >= args.size()) return usage("genko smart: argument --format: expected one argument");
                format = args[++i];
            }
        } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
            return usage("genko smart: unrecognized arguments: " + arg.toStdString());
        } else if (src.isEmpty()) {
            src = arg;
        } else {
            return usage("genko smart: unrecognized arguments: " + arg.toStdString());
        }
    }
    if (src.isEmpty()) return usage("genko smart <dir> --links | --export <page> <id> <path> [--format psd|png]: the following arguments are required: src");
    if (links == exported.has_value()) return usage("genko smart: one of --links or --export");
    if (!format.isEmpty() && !exported) return usage("genko smart: argument --format: only with --export");
    const fs::path dir = path_arg(src);
    refuse_legacy(dir);
    const storage::LoadResult loaded = storage::load_document(dir);
    const core::Document& doc = loaded.document;
    if (links) {
        Json listed = Json::array();
        for (const auto& state : formats::smart_files::link_states(doc)) listed.push_back(formats::smart_files::to_json(state));
        print_json(Json{{"ok", true}, {"links", listed}}, ascii);
        return 0;
    }
    const auto page_number = py_int_arg((*exported)[0]);
    if (!page_number) return usage("genko smart: argument --export: the page is a number");
    const std::string layer_id = (*exported)[1].toStdString();
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->index != core::Num(*page_number)) continue;
        for (const core::Layer& layer : doc.page(i).layers) {
            if (layer.id != layer_id) continue;
            const auto contents = core::smart::contents_id(layer);
            if (layer.kind != core::LayerKind::Smart || !contents) throw core::Error("value", "layer " + layer_id + " is not a smart object");
            Json out = formats::smart_files::export_contents(doc, *contents, path_arg((*exported)[2]), format.toStdString());
            out["ok"] = true;
            out["contents"] = *contents;
            print_json(out, ascii);
            return 0;
        }
        throw core::Error("key", "no layer " + layer_id + " on page " + std::to_string(*page_number));
    }
    throw core::Error("key", "no page " + std::to_string(*page_number));
}

// pen-shapes@1 (PC3 C-b, SPEC.md COLOR-07「シェイプのプリセット」, the C++ build's own): the shape presets of this machine
// (the settings folder's shapes.json, render/shape_presets) — `genko shapes list`, `genko shapes path <name> --rect
// x,y,w,h [--group G] [--stretch]` (the preset fitted into the box, mm: the path add_shape_part's "shape" {"kind":
// "path"} takes), `genko shapes new --path <path JSON | path language | "M …"> [--name N] [--group G]`, `genko shapes
// edit <{"action", …} JSON>`, `genko shapes reset [--append]`. The ops do not read them (a machine's setting).
int shapes_command(const QStringList& args, bool ascii) {
    namespace presets = render::shape_presets;
    if (args.isEmpty()) return usage("genko shapes list | path | new | edit | reset");
    const QString what = args.front();
    const QStringList rest = args.mid(1);
    const fs::path dir = core::config_dir();
    std::string problem;
    std::vector<presets::Group> groups = presets::load(dir, &problem);
    if (what == QLatin1String("list")) {
        Arguments a;
        if (auto error = parse_arguments(rest, {}, 0, "", a)) return usage("genko shapes list: " + *error);
        Json out = presets::list(groups);
        out["ok"] = true;
        if (!problem.empty()) out["problem"] = problem;
        print_json(out, ascii);
        return 0;
    }
    if (what == QLatin1String("path")) {
        static constexpr Option kOptions[] = {{"--rect", true}, {"--group", true}, {"--stretch", false}};
        Arguments a;
        if (auto error = parse_arguments(rest, kOptions, 1, "name", a)) return usage("genko shapes path: " + *error);
        const auto group = a.value("--group");
        const std::string group_text = group ? group->toStdString() : std::string();
        const presets::Preset* found = presets::find(groups, a.positional[0].toStdString(), group ? std::optional<std::string_view>(group_text) : std::nullopt);
        if (found == nullptr) throw core::Error("key", "no shape named " + a.positional[0].toStdString());
        const QStringList box = a.value("--rect").value_or(QString()).split(QLatin1Char(','));
        if (box.size() != 4) return usage("genko shapes path: --rect is x,y,w,h (mm)");
        std::array<double, 4> rect{};
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            rect[static_cast<std::size_t>(i)] = box[i].trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(rect[static_cast<std::size_t>(i)])) return usage("genko shapes path: --rect is x,y,w,h (mm)");
        }
        const core::vector::Path path = presets::fit(found->path, rect, !a.flag("--stretch"));
        print_json(Json{{"ok", true}, {"name", found->name}, {"path", core::vector::to_json(core::vector::path_from_json(core::vector::to_json(path)))}}, ascii);
        return 0;
    }
    if (what == QLatin1String("new")) {
        static constexpr Option kOptions[] = {{"--path", true}, {"--name", true}, {"--group", true}};
        Arguments a;
        if (auto error = parse_arguments(rest, kOptions, 0, "", a)) return usage("genko shapes new: " + *error);
        const auto given = a.value("--path");
        if (!given) return usage("genko shapes new: the following arguments are required: --path");
        const std::string text = given->trimmed().toStdString();
        const core::vector::Path path = text.starts_with("{") ? core::vector::path_from_json(core::parse_python_json(text)) : presets::parse(text);
        const auto group = a.value("--group");
        const std::string group_text = group ? group->toStdString() : std::string();
        Json out = presets::add(groups, a.value("--name").value_or(QString()).toStdString(), group ? std::optional<std::string_view>(group_text) : std::nullopt, path);
        presets::save(dir, groups);
        out["ok"] = true;
        print_json(out, ascii);
        return 0;
    }
    if (what == QLatin1String("edit")) {
        Arguments a;
        if (auto error = parse_arguments(rest, {}, 1, "edit", a)) return usage("genko shapes edit: " + *error);
        Json out = presets::edit(groups, core::parse_python_json(a.positional[0].toStdString()));
        presets::save(dir, groups);
        out["ok"] = true;
        print_json(out, ascii);
        return 0;
    }
    if (what == QLatin1String("reset")) {
        static constexpr Option kOptions[] = {{"--append", false}};
        Arguments a;
        if (auto error = parse_arguments(rest, kOptions, 0, "", a)) return usage("genko shapes reset: " + *error);
        groups = presets::reset(groups, a.flag("--append"));
        presets::save(dir, groups);
        print_json(Json{{"ok", true}, {"groups", static_cast<std::int64_t>(groups.size())}}, ascii);
        return 0;
    }
    return usage("genko shapes: list, path, new, edit or reset");
}

// brushes@1 (PC4 E1, brush.get; the C++ build's own): `genko brushes get <key> [--book <dir>]` — a brush's whole settings
// (Genko's, as a book keeps them, and a dab brush's every dab setting, the defaults too): a built-in brush, one of the
// book's own, or one of the person's own (the config folder's library). core::Error("key") for one that is not there.
int brushes_command(const QStringList& args, bool ascii) {
    if (args.isEmpty() || args.front() != QLatin1String("get")) return usage("genko brushes get <key> [--book <dir>]");
    static constexpr Option kOptions[] = {{"--book", true}};
    Arguments a;
    if (auto error = parse_arguments(args.mid(1), kOptions, 1, "key", a)) return usage("genko brushes get: " + *error);
    const std::string key = a.positional[0].toStdString();
    std::vector<core::Brush> known;
    std::string from = "builtin";
    if (const auto book = a.value("--book")) {
        const fs::path dir = path_arg(*book);
        refuse_legacy(dir);
        const auto loaded = storage::load_document(dir);
        render::brushes::register_book(loaded.document);  // (its pictures made known too)
        core::register_brushes(loaded.document.brush_custom, known);
        if (loaded.document.brush_custom.contains(key)) from = "book";
    }
    if (from != "book" && core::find_builtin(key) == nullptr) {
        try {
            core::register_brushes(render::brushes::load_own_brushes(core::config_dir()), known);
        } catch (const std::exception&) {
        }
        from = "library";
    }
    const core::Brush brush = core::find_brush(Json(key), known);
    if (brush.key != key) throw core::Error("key", "no brush " + key);
    Json out{{"ok", true}, {"key", brush.key}, {"label", brush.label}, {"from", from}, {"settings", core::brush_to_dict(brush)},
             {"engine", brush.dabs ? Json(std::string(core::kDabEngine)) : Json(nullptr)}};
    if (brush.dabs) out["dabs"] = core::dabs_to_json(*brush.dabs, true);
    print_json(out, ascii);
    return 0;
}

// smart-objects@1 (PC3 A1, the C++ build's own): `genko package <dir> --out <folder>` (パッケージ…) — the book copied as a
// new book into <folder>/<its folder's name>, with its linked files' copies in Links/ and linked there.
int package_command(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--out", true}, {"--as", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "src", a)) return usage("genko package: " + *error);
    const auto out = a.value("--out");
    if (!out || out->isEmpty()) return usage("genko package: the following arguments are required: --out");
    const fs::path dir = path_arg(a.positional[0]);
    refuse_legacy(dir);
    print_json(formats::smart_files::package(dir, path_arg(*out), a.value("--as").value_or(QStringLiteral("genko")).toStdString()), ascii);
    return 0;
}

}  // namespace

int run_cli(int argc, char** argv) {
#ifdef _WIN32
    // UTF-8 with "\n" line ends on every OS (ARCHITECTURE.md §8): no CRLF translation on Windows.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);  // (arguments in UTF-8 on Windows too)
    QStringList args = QCoreApplication::arguments().mid(1);
    const bool ascii = args.removeAll(QStringLiteral("--ascii")) > 0;
    if (args.isEmpty()) return usage("genko <command> — try `genko --version`");
    const QString command = args.takeFirst();
    try {
        if (command == QLatin1String("--version") || command == QLatin1String("version")) {
            write_json({{"ok", true}, {"build", build_info()}});
            return 0;
        }
        if (core::import_pending()) {
            // (the Python app's settings — the AI keys among them — are taken over only when the app asks: said here,
            // on stderr, so that a command or an AI working without the app knows why its keys are not found)
            std::fputs("genko: 以前の Genko Studio（Python 版）の設定はまだ引き継いでいません。`genko app` を起動すると、引き継ぐかを聞きます"
                       "（AI 接続の鍵もそのときに写せます）。\n",
                       stderr);
        }
        if (command == QLatin1String("inspect")) return inspect(args, ascii);
        if (command == QLatin1String("schema")) return schema(args, ascii);
        if (command == QLatin1String("new")) return new_book(args, ascii);
        if (command == QLatin1String("apply")) return apply(args, ascii);
        if (command == QLatin1String("undo")) return restore(args, ascii, false);
        if (command == QLatin1String("redo")) return restore(args, ascii, true);
        if (command == QLatin1String("gc")) return gc(args, ascii);
        if (command == QLatin1String("doctor")) return doctor(args, ascii);
        if (command == QLatin1String("migrate")) return migrate(args, ascii);
        if (command == QLatin1String("render")) return render_command(args, ascii);
        if (command == QLatin1String("export")) return export_command(args, ascii);
        if (command == QLatin1String("smart")) return smart_command(args, ascii);      // (smart-objects@1, PC3 A1)
        if (command == QLatin1String("package")) return package_command(args, ascii);  // (smart-objects@1, PC3 A1)
        if (command == QLatin1String("shapes")) return shapes_command(args, ascii);    // (pen-shapes@1, PC3 C-b)
        if (command == QLatin1String("brushes")) return brushes_command(args, ascii);  // (brushes@1, PC4 E1: brush.get)
        if (command == QLatin1String("studio")) return studio_command(args, ascii);
        if (command == QLatin1String("mcp")) return mcp_command(args);
        if (command == QLatin1String("serve")) return serve_command(args);
        if (command == QLatin1String("token")) return token_command(args, ascii);
        return usage("unknown command: " + command.toStdString());
    } catch (const storage::UnsupportedProjectVersion& error) {
        return fail(error.what(), error.code(), ascii, 2);
    } catch (const render::NotYetPorted& error) {
        // (what the page carries that this build does not draw yet: nothing is written)
        core::Json out = core::Json::object();
        out["ok"] = false;
        out["error"] = error.what();
        out["code"] = error.code();
        out["element"] = error.element();
        print_json(out, ascii);
        return 1;
    } catch (const core::Error& error) {
        if (error.code() == "needs_migration") return fail(error.what(), error.code(), ascii, 3);
        const std::string message = error.code() == "not_found" ? "not found: " + std::string(error.what()) : error.what();
        return fail(message, error.code(), ascii, 1);
    } catch (const std::exception& error) {
        return fail(error.what(), "internal", ascii, 1);
    }
}

}  // namespace genko::api
