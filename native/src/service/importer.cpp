#include "service/importer.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/png.hpp"
#include "service/guide.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/worklist.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::importer {

namespace fs = std::filesystem;
using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

fs::path resolved(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : out;
}

bool strictly_under(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (r->empty() && std::next(r) == root.end()) break;
        if (p == path.end() || *p != *r) return false;
    }
    for (; p != path.end(); ++p) {
        if (!p->empty()) return true;
    }
    return false;
}

std::string at(std::size_t index, const char* rest = "") { return "/images/" + std::to_string(index) + rest; }

double density(const render::Image& edges, const std::vector<std::array<double, 4>>& boxes01) {
    const int w = edges.width();
    const int h = edges.height();
    double total = 0;
    double count = 0;
    for (const auto& [x, y, bw, bh] : boxes01) {
        const int x0 = static_cast<int>(core::py_trunc(x * w));
        const int y0 = static_cast<int>(core::py_trunc(y * h));
        const render::Box box{x0, y0, std::max(x0 + 1, static_cast<int>(core::py_trunc((x + bw) * w))),
                              std::max(y0 + 1, static_cast<int>(core::py_trunc((y + bh) * h)))};
        const render::Image region = edges.crop(box);
        const std::vector<long> hist = region.histogram();
        for (std::size_t i = 64; i < hist.size(); ++i) count += static_cast<double>(hist[i]);
        total += static_cast<double>(region.width()) * region.height();
    }
    return total != 0 ? count / total : 0.0;
}

std::vector<std::array<double, 4>> boxes_of(const std::vector<Json>& values) {
    std::vector<std::array<double, 4>> out;
    for (const Json& v : values) {
        const std::vector<double> b = core::unpack_floats(v, 4);
        out.push_back({b[0], b[1], b[2], b[3]});
    }
    return out;
}

std::optional<double> aspect_of(const Json& request) {
    const Json size = core::py_or(py_get(request, "size"), Json::object());
    const Json px = py_get(size, "suggested_px");
    if (core::truthy_at(size, "frame_mm")) {
        const auto wh = core::unpack_values(size["frame_mm"], 2);
        const double pad = core::to_float(core::py_or(py_get(size, "pad_mm"), Json(0)));
        return (core::to_float(wh[0]) + 2 * pad) / (core::to_float(wh[1]) + 2 * pad);
    }
    if (py_truthy(px)) return core::to_float(core::subscript(px, 0)) / core::to_float(core::subscript(px, 1));
    return std::nullopt;
}

Json face_box(const Json& item, std::size_t index) {
    const Json box = item.is_object() ? py_get(item, "face_box01") : Json();
    if (box.is_null()) return Json();
    std::vector<double> v;
    try {
        v = core::unpack_floats(box, 4);
    } catch (const core::Error&) {
        throw ImportError("face_box01 は [x, y, 幅, 高さ]（画像の中の 0〜1）", at(index, "/face_box01"));
    }
    const double x = v[0], y = v[1], w = v[2], h = v[3];
    if (!(0 <= x && x < 1 && 0 <= y && y < 1 && 0 < w && w <= 1 - x + 1e-6 && 0 < h && h <= 1 - y + 1e-6)) {
        throw ImportError("face_box01 は画像の中に収める（0〜1）", at(index, "/face_box01"));
    }
    return Json::array({core::py_round(x, 4), core::py_round(y, 4), core::py_round(w, 4), core::py_round(h, 4)});
}

}  // namespace

fs::path inbox(const fs::path& project) { return project / "studio" / "inbox"; }

std::string read_source(const fs::path& project, const Json& item, std::size_t index) {
    if (!item.is_object()) throw ImportError("画像は {file} か {asset} のオブジェクト", at(index));
    if (core::truthy_at(item, "file") == core::truthy_at(item, "asset")) throw ImportError("file か asset のどちらか一方を渡す", at(index));
    if (core::truthy_at(item, "file")) {
        const fs::path root = resolved(inbox(project));
        const fs::path raw = core::path_from_utf8(py_str(item["file"]));
        const fs::path path = resolved(raw.is_absolute() ? raw : project / raw);
        if (!strictly_under(path, root)) throw ImportError("file は studio/inbox/ の中だけ: " + py_str(item["file"]), at(index, "/file"));
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) throw ImportError("ファイルがない: " + py_str(item["file"]), at(index, "/file"));
        if (fs::file_size(path, ec) > kMaxBytes) throw ImportError("画像が大きすぎる（30 MB まで）", at(index, "/file"));
        return storage::read_file(path);
    }
    const std::string ref = py_str(item["asset"]);
    std::optional<std::string> data;
    if (ref.rfind("sha256:", 0) == 0) data = storage::AssetStore(project).get_bytes(ref, ".png");
    if (!data) throw ImportError("asset " + ref + " が assets/ にない（先にアップロードする）", at(index, "/asset"));
    return *data;
}

std::tuple<std::string, std::pair<int, int>, render::Image> normalize(const std::string& blob, std::size_t index) {
    if (blob.size() > kMaxBytes) throw ImportError("画像が大きすぎる（30 MB まで）", at(index));
    render::Size size;
    // (bytes that are no picture: Pillow's words, its BytesIO's address left out — SPEC COMP-01a)
    const auto unreadable = [index](const std::exception& exc) -> ImportError {
        const auto* error = dynamic_cast<const core::Error*>(&exc);
        const std::string why = error != nullptr && error->code() == "unidentified_image" ? std::string("cannot identify image file <_io.BytesIO object>")
                                                                                         : std::string(exc.what());
        return ImportError("画像として読めない: " + why, at(index));
    };
    try {
        size = render::picture_size(blob);
        if (render::unported_image_format(blob)) throw core::Error("format", "cannot identify image file");
    } catch (const std::exception& exc) {
        throw unreadable(exc);
    }
    // (the size its header says, before its pixels are read: Python's verify() does not read them either, and a
    // picture past the limit is refused without the memory its pixels would take)
    if (static_cast<std::int64_t>(size.width) * size.height > kMaxPixels) {
        throw ImportError("画素数が多すぎる: " + std::to_string(size.width) + "x" + std::to_string(size.height) + "（64 MP まで）", at(index));
    }
    try {
        (void)render::open_image(blob, render::kPillowOpenLimits);  // (Image.verify(): the picture holds what it says)
    } catch (const std::exception& exc) {
        throw unreadable(exc);
    }
    render::Image image = open_picture(blob);
    std::string out = blob;
    static const std::string kPng("\x89PNG\r\n\x1a\n", 8);
    if (blob.compare(0, kPng.size(), kPng) != 0) {
        const std::string_view mode = image.mode();
        const bool keep = mode == "RGB" || mode == "RGBA" || mode == "L" || mode == "LA";
        out = formats::png_bytes(keep ? image : image.convert("RGB"));
    }
    return {std::move(out), {size.width, size.height}, std::move(image)};
}

Json check_origin(const Json& origin, std::size_t index) {
    if (!origin.is_object()) throw ImportError("origin（来歴）が要る: {kind, tool_id, model, prompt, …}", at(index, "/origin"));
    Json out = Json::object();
    for (const auto& [key, value] : origin.items()) {
        if (key == "kind" || key == "tool_id" || key == "tool" || key == "model" || key == "prompt" || key == "params" || key == "refs_used" ||
            key == "note") {
            out[key] = value;
        }
    }
    if (!out.contains("kind")) out["kind"] = "agent";
    if (!core::is_one_of(out["kind"], {"agent", "human", "self", "fixture", "genko"})) {  // (`in` a tuple: no hashing)
        throw ImportError("origin.kind は agent, human, self, fixture, genko のどれか", at(index, "/origin/kind"));
    }
    out["reported"] = out["kind"] == "agent";
    return out;
}

Json metrics(const render::Image& image, const Json& request) {
    const auto aspect = aspect_of(request);
    const int w = image.width();
    const int h = image.height();
    Json out = Json::object({{"px", Json::array({w, h})}});
    if (aspect && *aspect != 0.0) out["aspect_error"] = core::py_round(std::abs(std::log((static_cast<double>(w) / h) / *aspect)), 3);
    const double ratio = aspect && *aspect != 0.0 ? *aspect : static_cast<double>(w) / h;
    const std::array<int, 2> size = ratio >= 1
        ? std::array<int, 2>{kThumb, std::max(1, static_cast<int>(core::py_round_int(kThumb / ratio)))}
        : std::array<int, 2>{std::max(1, static_cast<int>(core::py_round_int(kThumb * ratio))), kThumb};
    const render::Image small = guide::fit_cover(image.convert("L"), size);
    const std::vector<long> hist = small.histogram();
    double weighted = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) weighted += static_cast<double>(i) * static_cast<double>(hist[i]);
    out["luminance"] = core::py_round(weighted / (255.0 * size[0] * size[1]), 3);
    const render::Image edges = small.filter(render::Filter::find_edges());
    const double overall = density(edges, {{0, 0, 1, 1}});
    out["edge_density"] = core::py_round(overall, 4);
    std::vector<Json> keep;
    for (const Json& k : core::iterate(core::py_or(py_get(request, "keepout"), Json::array()))) keep.push_back(util::item(k, "box01"));
    if (!keep.empty()) out["keepout_busy"] = overall != 0.0 ? Json(core::py_round(density(edges, boxes_of(keep)) / overall, 3)) : Json(0.0);
    std::vector<Json> bodies;
    for (const Json& f : core::iterate(core::py_or(py_get(request, "figures"), Json::array()))) bodies.push_back(util::item(f, "body01"));
    if (!bodies.empty() && overall != 0.0) {
        out["guide_follow"] = core::py_round(core::py_min(3.0, density(edges, boxes_of(bodies)) / overall) / 3.0, 3);
    }
    return out;
}

double rank_key(const Json& m) {
    const double aspect_error = core::to_float(py_get(m, "aspect_error", Json(0)));
    const double busy = core::to_float(py_get(m, "keepout_busy", Json(1.0)));
    const double follow = core::to_float(py_get(m, "guide_follow", Json(0.33)));
    return core::py_round(aspect_error * 2 + core::py_max(0.0, busy - 1.0) - follow, 4);
}

std::string candidate_id(const std::string& request_id, const std::string& asset) {
    return "cd_" + worklist::sha256_hex(request_id + ":" + asset).substr(0, 10);
}

std::pair<Json, std::optional<render::Image>> prepare(const fs::path& project, const Json& request, const Json& images) {
    if (!images.is_array() || images.empty()) throw ImportError("images は 1 枚以上の配列", "/images");
    if (images.size() > kMaxImages) throw ImportError("1 回に取り込めるのは " + std::to_string(kMaxImages) + " 枚まで", "/images");
    storage::AssetStore store(project);
    Json items = Json::array();
    std::optional<render::Image> first;
    for (std::size_t i = 0; i < images.size(); ++i) {
        const Json& item = images[i];
        const Json origin = check_origin(item.is_object() ? py_get(item, "origin") : Json(), i);
        auto [blob, size, image] = normalize(read_source(project, item, i), i);
        const std::string ref = store.put_bytes(blob, ".png");
        Json measured = metrics(image, request);
        measured["rank"] = rank_key(measured);
        Json entry = Json::object({{"id", candidate_id(py_str(util::item(request, "id")), ref)},
                                   {"asset", ref},
                                   {"px", Json::array({size.first, size.second})},
                                   {"mode", py_get(request, "mode", Json("new"))},
                                   {"parent", py_get(request, "parent")},
                                   {"origin", origin},
                                   {"metrics", std::move(measured)}});
        const Json face = face_box(item, i);
        if (py_truthy(face)) entry["face_box01"] = face;
        items.push_back(std::move(entry));
        if (!first) first = std::move(image);
    }
    return {std::move(items), std::move(first)};
}

}  // namespace genko::service::importer
