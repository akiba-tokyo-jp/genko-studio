#pragma once

// Helpers for the 3D tests (header only; not part of the product): the digests tools/migration/geom3d_harness.py
// writes, made the same way from the C++ values.

#include <QCryptographicHash>
#include <QString>
#include <QStringList>

#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/mesh3d.hpp"
#include "core/model.hpp"
#include "core/prim3d.hpp"
#include "core/strokes.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"

namespace genko::test {

inline std::string sha256(std::string_view bytes) {
    return QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

// sha256 of little-endian float64 (float32) values
inline std::string sha_f64(std::span<const double> values) {
    std::string bytes(values.size() * sizeof(double), '\0');
    if (!values.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
    return sha256(bytes);
}

inline std::string sha_f32(std::span<const float> values) {
    std::string bytes(values.size() * sizeof(float), '\0');
    if (!values.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
    return sha256(bytes);
}

inline std::string lines_text(const std::vector<core::mesh3d::Line2>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += ';';
        for (std::size_t k = 0; k < lines[i].size(); ++k) {
            if (k > 0) out += ',';
            out += bits_of(lines[i][k][0]) + bits_of(lines[i][k][1]);
        }
    }
    return out;
}

inline core::Json lines_digest(const std::vector<core::mesh3d::Line2>& lines) {
    std::size_t points = 0;
    for (const auto& line : lines) points += line.size();
    return core::Json::object({{"count", static_cast<std::int64_t>(lines.size())},
                               {"points", static_cast<std::int64_t>(points)},
                               {"sha", sha256(lines_text(lines))}});
}

inline core::Json edges_digest(const std::vector<core::prim3d::Edge>& edges) {
    std::string text;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (i > 0) text += ';';
        const auto& e = edges[i];
        text += bits_of(e.a[0]) + bits_of(e.a[1]) + bits_of(e.b[0]) + bits_of(e.b[1]) + (e.seen ? "1" : "0");
    }
    return core::Json::object({{"count", static_cast<std::int64_t>(edges.size())}, {"sha", sha256(text)}});
}

// tools/migration/geom3d_harness.py <args>, run by the Python reference
inline Run geom3d_harness(const QStringList& args, const QString& scratch, int timeout_ms = 3600000) {
    QStringList full{repo_root() + QStringLiteral("/tools/migration/geom3d_harness.py")};
    full += args;
    return run(python_ref(), full, python_env(scratch), timeout_ms);
}

// project.json v4 as Python's v3 writer would write it
inline core::Json as_v3(core::Json payload) {
    for (const char* key : {"min_reader", "writer", "book_id", "features", "revision"}) payload.erase(key);
    payload["version"] = 3;
    return payload;
}

inline std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

// geom3d_harness._png_pixels_sha: the mode Pillow opens a picture in, its size and the sha256 of its RGBA pixels
inline core::Json raster_digest(const core::Bytes& png) {
    if (!png || png->empty()) return core::Json();
    const render::Image im = render::open_image(*png);
    return core::Json::object({{"mode", std::string(im.mode())},
                               {"size", core::Json::array({im.width(), im.height()})},
                               {"sha", sha256(im.convert("RGBA").tobytes())}});
}

// geom3d_harness.state_of: every page's prims, rulers and extra keys, and each layer's strokes (the bytes of its
// .strokes.json), pixels and screen
inline core::Json state_of(const core::Document& doc) {
    core::Json pages = core::Json::array();
    for (const auto& page : doc.pages) {
        core::Json layers = core::Json::array();
        for (const auto& layer : page->layers) {
            const bool has = layer.stroke_count() > 0;
            layers.push_back(core::Json::object({{"id", layer.id},
                                                 {"role", std::string(core::to_string(layer.role))},
                                                 {"kind", std::string(core::to_string(layer.kind))},
                                                 {"strokes", has ? core::Json(sha256(core::strokes_blob(*layer.strokes))) : core::Json()},
                                                 {"stroke_count", static_cast<std::int64_t>(layer.stroke_count())},
                                                 {"raster", raster_digest(layer.raster_png)},
                                                 {"screen", layer.screen ? *layer.screen : core::Json()}}));
        }
        pages.push_back(core::Json::object({{"index", page->index.json()},
                                            {"prims", page->prims},
                                            {"rulers", page->rulers},
                                            {"extra", page->extra},
                                            {"layers", std::move(layers)}}));
    }
    return pages;
}

// Python's saved project.json against the C++ payload (as_v3 of project_payload_v4 into cpp_assets): the pictures a
// 3D op made anew are compared by their pixels (their PNG bytes differ); "" when they are the same.
inline std::string project_difference(core::Json mine, core::Json theirs, const QString& py_book, const QString& cpp_assets) {
    const auto file = [](const QString& root, const std::string& ref) {
        const std::string hex = ref.substr(7);
        return root + "/assets/" + QString::fromStdString(hex.substr(0, 2)) + "/" + QString::fromStdString(hex) + ".png";
    };
    const auto pixels = [](const std::string& bytes) {
        const render::Image im = render::read_png(bytes);
        return std::string(im.mode()) + std::to_string(im.width()) + "x" + std::to_string(im.height()) + im.tobytes();
    };
    for (std::size_t p = 0; p < mine["pages"].size() && p < theirs["pages"].size(); ++p) {
        core::Json& a = mine["pages"][p]["layers"];
        core::Json& b = theirs["pages"][p]["layers"];
        for (std::size_t l = 0; l < a.size() && l < b.size(); ++l) {
            if (!a[l].contains("asset") || !b[l].contains("asset") || a[l]["asset"] == b[l]["asset"]) continue;
            if (pixels(read_bytes(file(cpp_assets, a[l]["asset"].get<std::string>()))) !=
                pixels(read_bytes(file(py_book, b[l]["asset"].get<std::string>())))) {
                return "page " + std::to_string(p) + " layer " + std::to_string(l) + ": the pictures differ";
            }
            a[l]["asset"] = "<the same pixels>";
            b[l]["asset"] = "<the same pixels>";
        }
    }
    theirs.erase("revision");
    std::string where;
    if (!strict_equal(as_v3(std::move(mine)), theirs, &where)) return "project.json: " + where;
    return {};
}

}  // namespace genko::test
