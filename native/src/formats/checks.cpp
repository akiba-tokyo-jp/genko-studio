// 入稿前の点検 (Python's genko/checks.py and the preflight of genko/studio/preflight.py that checks.book folds in).
// Each check in Python's order, with Python's words, numbers and exceptions (core/pyops.hpp: dict.get, float(),
// iteration and equality as Python has them).

#include "formats/checks.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/balloon_tails.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "render/text/lettering.hpp"
#include "render/tones.hpp"
#include "storage/asset_store.hpp"

namespace genko::formats::checks {

namespace fs = std::filesystem;
using core::Json;
using core::LayerKind;
using core::LayerRole;
using core::Num;

namespace {

// f"{x:.<places>f}" (Python's and printf's fixed notation round the exact binary value the same way)
std::string fixed(double x, int places) {
    char buf[512];
    const auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, places);
    return std::string(buf, r.ptr);
}

std::string num(const Num& n) { return n.repr(); }

// The characters of a UTF-8 text (Python's str is a sequence of code points).
std::vector<std::string_view> chars(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t n = 1;
        while (i + n < text.size() && (static_cast<unsigned char>(text[i + n]) & 0xC0) == 0x80) ++n;
        out.push_back(text.substr(i, n));
        i += n;
    }
    return out;
}

// text[:n]
std::string head(std::string_view text, std::size_t n) {
    std::string out;
    const auto all = chars(text);
    for (std::size_t i = 0; i < all.size() && i < n; ++i) out += all[i];
    return out;
}

// x.get(key) or fallback, as str(): the value's str when it is truthy.
std::string str_or(const Json* value, const std::string& fallback) {
    return value != nullptr && core::py_truthy(*value) ? core::py_str(*value) : fallback;
}

// [round(float(v), 2) for v in box] if box else None
Json box_json(const std::vector<double>& box) {
    if (box.empty()) return nullptr;
    Json out = Json::array();
    for (const double v : box) out.push_back(core::py_round(v, 2));
    return out;
}

Json box_json(const Json& box) {
    if (!core::py_truthy(box)) return nullptr;
    std::vector<double> values;
    for (const Json& v : core::iterate(box)) values.push_back(core::to_float(v));
    return box_json(values);
}

// checks._issue
Json issue(const char* level, const char* code, const core::Page& page, const std::string& message, Json box = nullptr,
           const char* kind = "page", Json target_id = nullptr) {
    return Json{{"level", level},
                {"code", code},
                {"page", page.index.json()},
                {"message", message},
                {"box", std::move(box)},
                {"target", Json{{"kind", kind}, {"id", std::move(target_id)}}}};
}

// checks._overlap
double overlap(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    const double w = core::py_min(a[0] + a[2], b[0] + b[2]) - core::py_max(a[0], b[0]);
    const double h = core::py_min(a[1] + a[3], b[1] + b[3]) - core::py_max(a[1], b[1]);
    return core::py_max(0.0, w) * core::py_max(0.0, h);
}

// Image.open(bytes).width / .size: a PNG's from its header, any other picture as it opens (its errors and
// NotYetPorted("image_format") as render::open_image raises them).
std::pair<std::int64_t, std::int64_t> picture_size(std::string_view bytes) {
    static constexpr std::string_view kSignature{"\x89PNG\r\n\x1a\n", 8};
    if (bytes.size() >= 24 && bytes.substr(0, 8) == kSignature && bytes.substr(12, 4) == "IHDR") {
        const auto be32 = [&](std::size_t at) {
            std::int64_t v = 0;
            for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<unsigned char>(bytes[at + i]);
            return v;
        };
        return {be32(16), be32(20)};
    }
    const render::Image image = render::open_image(bytes, render::kPillowOpenLimits);
    return {image.width(), image.height()};
}

// checks._text_size_mm: the size the lettering ends up at in its balloon (mm), or nothing when it cannot be measured
// (what this build does not letter yet is said, not passed over).
std::optional<double> text_size_mm(const core::StoryLine& line) {
    try {
        const auto [image, em] = render::text::text_image(line, 150);
        (void)image;
        return static_cast<double>(em) / 150 * 25.4;
    } catch (const render::NotYetPorted&) {
        throw;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A layer's pixels (raster_png; and the precise colour pixels of this build, which Python's books do not have).
bool has_pixels(const core::Layer& layer) {
    return (layer.raster_png && !layer.raster_png->empty()) || (layer.color_raster && !layer.color_raster->empty());
}

bool prints(const core::Layer& layer) { return layer.exportable && layer.role != LayerRole::Name && layer.role != LayerRole::Draft; }

// studio.finish.regions_stale: the faces were reported on art that is no longer the adopted one.
bool regions_stale(const Json& panel) {
    const Json* adopted = core::dict_get(panel, "adopted");
    Json art = nullptr;
    if (adopted != nullptr && core::py_truthy(*adopted)) {
        if (const Json* a = core::dict_get(*adopted, "art")) art = *a;
    }
    const Json* regions = core::dict_get(panel, "regions");
    const Json* regions_for = core::dict_get(panel, "regions_for");
    return regions != nullptr && core::py_truthy(*regions) && regions_for != nullptr && core::py_truthy(*regions_for) && core::py_truthy(art) &&
           !core::py_equals(*regions_for, art);
}

// checks._cut_faces: reported faces and people the panel's edge cuts — how far out, and the offset that brings them back.
void cut_faces(const core::Page& page, Json& out) {
    for (const core::Frame* frame : page.leaf_frames()) {
        const Json panel = frame->panel && core::py_truthy(*frame->panel) ? *frame->panel : Json::object();
        const core::Rect box = core::clip_box(page, frame, frame->bleed ? "bleed" : "frame");
        const double bx = box.x.value(), by = box.y.value(), bw = box.width.value(), bh = box.height.value();
        const Json regions = core::py_get(panel, "regions", Json::array());
        const auto quad = [](const Json* rect) { return rect != nullptr && rect->is_array() && rect->size() == 4; };
        std::vector<Json> faces;
        for (const Json& r : core::iterate(regions)) {
            const Json* kind = core::dict_get(r, "kind");
            if (kind != nullptr && core::is_one_of(*kind, {"face", "head"}) && quad(core::dict_get(r, "rect_mm"))) faces.push_back(r);
        }
        for (const Json& region : core::iterate(regions)) {
            const Json* rect = core::dict_get(region, "rect_mm");
            const Json* kind = core::dict_get(region, "kind");
            if (kind == nullptr || !core::is_one_of(*kind, {"face", "head", "person", "body"}) || !quad(rect)) continue;
            const std::vector<double> v = core::unpack_floats(*rect, 4);
            const double x = v[0], y = v[1], w = v[2], h = v[3];
            const double left = bx - x, top = by - y;
            const double right = x + w - (bx + bw), bottom = y + h - (by + bh);
            const double worst = core::py_max(core::py_max(core::py_max(left, top), right), bottom);
            const bool face = core::is_one_of(*kind, {"face", "head"});
            if (worst <= (face ? 0.5 : core::py_max(3.0, 0.15 * core::py_max(w, h)))) continue;
            if (!face) {
                const Json* chr = core::dict_get(region, "char");
                bool inside = false;
                for (const Json& f : faces) {
                    if (chr != nullptr && core::py_truthy(*chr)) {
                        const Json* other = core::dict_get(f, "char");
                        if (core::py_equals(other != nullptr ? *other : Json(nullptr), *chr)) {
                            inside = true;
                            break;
                        }
                    }
                    std::vector<double> fr;
                    for (const Json& n : core::iterate(core::subscript(f, "rect_mm"))) fr.push_back(core::to_float(n));
                    if (fr.size() != 4) core::raise_index_error("tuple index out of range");
                    if (overlap({x, y, w, h}, {fr[0], fr[1], fr[2], fr[3]}) > 0) {
                        inside = true;
                        break;
                    }
                }
                if (inside) continue;  // (a body cut by the edge with its face inside: the usual framing, the face is checked alone)
            }
            const double dx = (left > 0 ? left : 0.0) - (right > 0 ? right : 0.0);
            const double dy = (top > 0 ? top : 0.0) - (bottom > 0 ? bottom : 0.0);
            const std::string who = str_or(core::dict_get(region, "char"), "");
            const std::string slot = str_or(core::dict_get(panel, "slot"), frame->id);
            out.push_back(issue("warning", "cut_by_panel", page,
                                num(page.index) + " ページのコマ " + slot + " で" + who + "の" + (face ? "顔" : "人物") + "が枠で " + fixed(worst, 0) +
                                    " mm 切れている。set_placement の offset_mm を今の値から [" + fixed(dx, 1) + ", " + fixed(dy, 1) +
                                    "] ずらす（入らなければ scale を下げる）",
                                box_json(std::vector<double>{x, y, w, h}), "frame", frame->id));
        }
    }
}

// checks._moire: two dot or line tones over the same place at different angles or line counts beat into a moiré in
// print (found from the tones' masks at a coarse size).
void moire(const core::Page& page, Json& out) {
    std::vector<const core::Layer*> screens;
    for (const core::Layer& layer : page.layers) {
        const bool screen = layer.screen && core::py_truthy(*layer.screen);
        if (layer.visible && layer.exportable && (layer.kind == LayerKind::Tone || layer.role == LayerRole::Tone || screen)) screens.push_back(&layer);
    }
    if (screens.size() < 2) return;
    constexpr int dpi = 20;
    const render::Size size{static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(page.spec.width_mm.value() / 25.4 * dpi))),
                            static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(page.spec.height_mm.value() / 25.4 * dpi)))};
    struct Look {
        const core::Layer* layer;
        Json pattern;
        double lpi;
        double angle;
    };
    std::vector<Look> looks;
    std::vector<render::Image> masks;
    for (const core::Layer* layer : screens) {
        render::Image where;
        if (layer->screen && core::py_truthy(*layer->screen)) {
            const Json& spec = *layer->screen;
            looks.push_back(Look{layer, core::py_get(spec, "pattern", Json("dot")), core::to_float(core::py_get(spec, "lpi", Json(60))),
                                 core::py_fmod(core::to_float(core::py_get(spec, "angle", Json(45))), 90.0)});
            where = render::layer_image(page, *layer, dpi).getchannel(3);
        } else {
            const render::tones::Settings st = render::tones::settings(*layer);
            if (!core::is_one_of(st.pattern, {"dot", "line", "cross"})) continue;
            looks.push_back(Look{layer, st.pattern, st.lpi, core::py_fmod(st.angle, 90.0)});
            where = render::tone_mask(page, *layer, size, dpi);
        }
        masks.push_back(std::move(where));
    }
    for (std::size_t i = 0; i < looks.size(); ++i) {
        for (std::size_t j = i + 1; j < looks.size(); ++j) {
            const Look& a = looks[i];
            const Look& b = looks[j];
            const bool same = core::py_equals(a.pattern, b.pattern) && a.lpi == b.lpi && a.angle == b.angle;
            if (same || masks[i].size() != masks[j].size()) continue;
            const std::string mi = masks[i].tobytes();
            const std::string mj = masks[j].tobytes();
            const int w = masks[i].width();
            std::int64_t count = 0;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            for (std::size_t k = 0; k < mi.size(); ++k) {
                if (static_cast<unsigned char>(mi[k]) > 20 && static_cast<unsigned char>(mj[k]) > 20) {
                    const int x = static_cast<int>(k % static_cast<std::size_t>(w));
                    const int y = static_cast<int>(k / static_cast<std::size_t>(w));
                    if (count == 0) {
                        x0 = x1 = x;
                        y0 = y1 = y;
                    } else {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
                    ++count;
                }
            }
            if (count < 4) continue;
            const std::vector<double> box{static_cast<double>(x0) / dpi * 25.4, static_cast<double>(y0) / dpi * 25.4,
                                          static_cast<double>(x1 - x0 + 1) / dpi * 25.4, static_cast<double>(y1 - y0 + 1) / dpi * 25.4};
            const std::string ta = a.layer->title.empty() ? std::string("トーン") : a.layer->title;
            const std::string tb = b.layer->title.empty() ? std::string("トーン") : b.layer->title;
            out.push_back(issue("warning", "tone_moire", page, "トーン「" + ta + "」と「" + tb + "」が重なっていて、線数か角度が違う（印刷でモアレが出る）",
                                box_json(box), "layer", b.layer->id));
        }
    }
}

// Python's min(items) and max(items) of floats (the first unless a later one is smaller / larger).
double py_min_of(const std::vector<double>& items) {
    double out = items.front();
    for (const double v : items) {
        if (v < out) out = v;
    }
    return out;
}
double py_max_of(const std::vector<double>& items) {
    double out = items.front();
    for (const double v : items) {
        if (v > out) out = v;
    }
    return out;
}

// --- the studio's preflight ------------------------------------------------------------------------------------------

// A Python dict keyed by JSON values (1, 1.0 and True are one key): the first key kept, the last value.
struct PyDict {
    std::vector<std::pair<Json, Json>> items;

    const Json* find(const Json& key) const {
        core::require_hashable(key);
        for (const auto& [k, v] : items) {
            if (core::py_equals(k, key)) return &v;
        }
        return nullptr;
    }
    void set(const Json& key, const Json& value) {
        core::require_hashable(key);
        for (auto& [k, v] : items) {
            if (core::py_equals(k, key)) {
                v = value;
                return;
            }
        }
        items.emplace_back(key, value);
    }
};

// sorted(): str by code point, numbers by value; Python's TypeError between the two.
bool py_sort_less(const Json& a, const Json& b) {
    if (a.is_string() && b.is_string()) return a.get_ref<const std::string&>() < b.get_ref<const std::string&>();
    if (a.is_string() || b.is_string()) {
        throw core::PyTypeError(std::string("'<' not supported between instances of '") + core::py_type_name(b) + "' and '" +
                                core::py_type_name(a) + "'");
    }
    return core::py_less(a, b);
}

Json pf_issue(const char* code, const char* severity, const std::string& path, const std::string& message, const char* hint = "") {
    return Json{{"code", code}, {"severity", severity}, {"path", path}, {"message", message}, {"hint", hint}};
}

// page._find(frame_id): the frame of that id under the page's first frame (KeyError: none; IndexError: no frames).
const core::Frame* find_first(const core::Frame& node, const std::string& id) {
    if (node.id == id) return &node;
    for (const core::Frame& child : node.children) {
        if (const core::Frame* found = find_first(child, id)) return found;
    }
    return nullptr;
}
const core::Frame* find_frame(const core::Page& page, const std::optional<std::string>& id) {
    if (page.frames.empty() || !id) return nullptr;
    return find_first(page.frames.front(), *id);
}

// The panel's candidates ((frame.panel or {}).get("candidates", [])).
std::vector<Json> candidates_of(const core::Frame& frame) {
    const Json panel = frame.panel && core::py_truthy(*frame.panel) ? *frame.panel : Json::object();
    return core::iterate(core::py_get(panel, "candidates", Json::array()));
}

// preflight._candidate_for
std::optional<Json> candidate_for(const core::Page& page, const core::Layer& layer) {
    const Json source = layer.source && core::py_truthy(*layer.source) ? *layer.source : Json::object();
    const Json* cand_id = core::dict_get(source, "candidate");
    if (cand_id == nullptr || !core::py_truthy(*cand_id) || !layer.frame_id || layer.frame_id->empty()) return std::nullopt;
    const core::Frame* frame = find_frame(page, layer.frame_id);
    if (frame == nullptr) return std::nullopt;
    for (const Json& c : candidates_of(*frame)) {
        const Json* id = core::dict_get(c, "id");
        if (core::py_equals(id != nullptr ? *id : Json(nullptr), *cand_id)) return c;
    }
    return std::nullopt;
}

// (cand or {}).get(key)
Json cand_get(const std::optional<Json>& cand, std::string_view key) {
    if (!cand || !core::py_truthy(*cand)) return nullptr;
    const Json* value = core::dict_get(*cand, key);
    return value != nullptr ? *value : Json(nullptr);
}

// preflight._drawn_origin: where the picture was drawn (an enlargement followed back to the picture it enlarged).
Json drawn_origin(const core::Page& page, const core::Layer& layer, std::optional<Json> cand) {
    Json origin = core::py_or(cand_get(cand, "origin"), Json::object());
    const core::Frame* frame = find_frame(page, layer.frame_id);
    if (frame == nullptr) return origin;
    PyDict pool;
    for (const Json& c : candidates_of(*frame)) {
        const Json* id = core::dict_get(c, "id");
        pool.set(id != nullptr ? *id : Json(nullptr), c);
    }
    for (int step = 0; step < 6; ++step) {
        if (!core::py_truthy(cand_get(cand, "upscaled"))) break;
        const Json* parent = core::dict_get(*cand, "parent");
        if (parent == nullptr || !core::py_truthy(*parent)) break;
        const Json* next = pool.find(*parent);
        if (next == nullptr) break;
        cand = *next;
        origin = core::py_or(core::py_get(*cand, "origin"), Json::object());
    }
    return origin;
}

// Image.open(path).size of an asset picture; nothing when the file is not there.
std::optional<std::pair<std::int64_t, std::int64_t>> image_size(const storage::AssetStore& store, const std::string& ref) {
    const fs::path path = store.path(ref, ".png");
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw core::Error("io", "cannot read " + core::path_to_utf8(path));
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return picture_size(bytes);
}

}  // namespace

Json page_issues(const core::Document& episode, const core::Page& page) {
    Json out = Json::array();
    const core::Rect t = page.trim_rect_mm();
    const std::array<double, 4> trim{t.x.value(), t.y.value(), t.width.value(), t.height.value()};
    const core::Rect inner = page.inner_rect_mm(episode.start_side ? std::string_view(*episode.start_side) : std::string_view());
    const double ix = inner.x.value(), iy = inner.y.value(), iw = inner.width.value(), ih = inner.height.value();
    const std::vector<const core::StoryLine*> lines = episode.story_for_page(page.index);
    std::vector<std::pair<const core::StoryLine*, std::array<double, 4>>> placed;
    for (const core::StoryLine* line : lines) {
        std::string text = line->text;
        std::erase(text, '\n');
        const std::string short_text = head(text, 14) + (chars(text).size() > 14 ? "…" : "");
        if (!line->x_mm.truthy() && !line->y_mm.truthy() && line->balloon.empty()) {
            out.push_back(issue("error", "line_unplaced", page, "台詞「" + short_text + "」がどこにも置かれていない", nullptr, "line", line->id));
            continue;
        }
        const std::array<double, 4> box{line->x_mm.value(), line->y_mm.value(), line->w_mm.truthy() ? line->w_mm.value() : 0.0,
                                        line->h_mm.truthy() ? line->h_mm.value() : 0.0};
        placed.emplace_back(line, box);
        const auto [x, y, w, h] = box;
        const std::string kind = line->balloon.empty() ? std::string("speech") : line->balloon;
        if (x < trim[0] - 0.01 || y < trim[1] - 0.01 || x + w > trim[0] + trim[2] + 0.01 || y + h > trim[1] + trim[3] + 0.01) {
            out.push_back(issue("error", "text_outside_trim", page, "台詞「" + short_text + "」が仕上がり線の外にはみ出している（裁ち落とされる）",
                                box_json(std::vector<double>(box.begin(), box.end())), "line", line->id));
        } else if (kind != "sfx" && (x < ix - 0.5 || y < iy - 0.5 || x + w > ix + iw + 0.5 || y + h > iy + ih + 0.5)) {
            out.push_back(issue("warning", "text_outside_frame", page, "台詞「" + short_text + "」が基本枠の外にある（裁ちずれで切れるおそれ）",
                                box_json(std::vector<double>(box.begin(), box.end())), "line", line->id));
        }
        if (kind != "sfx" && kind != "none" && !text.empty()) {
            const auto size = text_size_mm(*line);
            if (size && *size < kMinTextMm) {
                out.push_back(issue("warning", "text_too_small", page,
                                    "台詞「" + short_text + "」の文字が " + fixed(*size, 1) + " mm まで小さくなっている（フキダシを大きくするか、文を短く）",
                                    box_json(std::vector<double>(box.begin(), box.end())), "line", line->id));
            }
        }
        std::vector<Json> tails = line->tails;
        if (tails.empty() && line->tail) tails.push_back(Json{{"to", Json::array({line->tail->x.json(), line->tail->y.json()})}});
        for (const Json& tail : tails) {
            const Json* to = core::dict_get(tail, "to");
            if (to != nullptr && core::py_truthy(*to) && core::tail_hidden(*line, *to)) {
                out.push_back(issue("warning", "tail_hidden", page,
                                    "台詞「" + short_text + "」の尾がフキダシの中に埋もれている（誰の台詞か分からない）。move_line の tail で先を外に出す",
                                    box_json(std::vector<double>(box.begin(), box.end())), "line", line->id));
            }
        }
    }
    cut_faces(page, out);
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const auto& [a, box_a] = placed[i];
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            const auto& [b, box_b] = placed[j];
            const Json* group_a = core::dict_get(a->style, "group");
            const Json* group_b = core::dict_get(b->style, "group");
            if (group_a != nullptr && core::py_truthy(*group_a) && core::py_equals(*group_a, group_b != nullptr ? *group_b : Json(nullptr))) continue;
            const double shared = overlap(box_a, box_b);
            double smaller = core::py_min(box_a[2] * box_a[3], box_b[2] * box_b[3]);
            if (smaller == 0.0) smaller = 1.0;  // (`or 1`)
            if (shared / smaller > 0.15) {
                const double x0 = core::py_max(box_a[0], box_b[0]), y0 = core::py_max(box_a[1], box_b[1]);
                const double x1 = core::py_min(box_a[0] + box_a[2], box_b[0] + box_b[2]);
                const double y1 = core::py_min(box_a[1] + box_a[3], box_b[1] + box_b[3]);
                out.push_back(issue("warning", "text_overlap", page, "台詞「" + head(a->text, 8) + "」と「" + head(b->text, 8) + "」が重なっている",
                                    box_json(std::vector<double>{x0, y0, x1 - x0, y1 - y0}), "line", a->id));
            }
        }
    }
    for (const core::Layer& layer : page.layers) {
        if (!layer.visible || !prints(layer)) continue;
        std::string label = layer.title;
        if (label.empty()) {
            label = layer.role == LayerRole::Ink ? "ペン入れ" : layer.role == LayerRole::Bg ? "背景" : layer.role == LayerRole::Tone ? "トーン" : "レイヤー";
        }
        for (const core::Patch& patch : layer.patches) {
            const Json* mode = core::get(patch.attrs, "mode");
            if (mode == nullptr || !mode->is_string() || mode->get_ref<const std::string&>() != "image" || !patch.png || patch.png->empty()) continue;
            const std::int64_t px_w = picture_size(*patch.png).first;
            const double w_mm = core::to_float(core::subscript(core::subscript(patch.attrs, "box"), 2));
            const double dpi = w_mm != 0.0 ? static_cast<double>(px_w) / (w_mm / 25.4) : 0.0;
            if (dpi < kMinDpi) {
                out.push_back(issue("warning", "low_dpi", page,
                                    "「" + label + "」に貼った画像の解像度が " + fixed(dpi, 0) + " dpi（" + std::to_string(kMinDpi) + " 未満。印刷で粗くなる）",
                                    box_json(core::subscript(patch.attrs, "box")), "layer", layer.id));
            }
        }
        if (layer.kind == LayerKind::Raster && has_pixels(layer) && render::selection::kWorkingDpi < kMinDpi) {
            out.push_back(issue("warning", "paint_dpi", page,
                                "ペイントのレイヤー「" + label + "」は " + std::to_string(render::selection::kWorkingDpi) +
                                    " dpi で持っている（細い線はペンのレイヤーで描くと 600 dpi でもきれい）",
                                nullptr, "layer", layer.id));
        }
        // lines and fills beyond the page's edge are cut off
        std::vector<double> xs, ys;
        if (layer.strokes) {
            for (const core::StrokePtr& stroke : layer.strokes->items) {
                for (const core::PointF& p : stroke->points) {
                    xs.push_back(p.x);
                    ys.push_back(p.y);
                }
            }
        }
        for (const core::Patch& patch : layer.patches) {
            const std::vector<double> v = core::unpack_floats(core::subscript(patch.attrs, "box"), 4);
            xs.push_back(v[0]);
            xs.push_back(v[0] + v[2]);
            ys.push_back(v[1]);
            ys.push_back(v[1] + v[3]);
        }
        const core::Rect b = page.bleed_rect_mm();
        if (!xs.empty()) {
            const double x0 = py_min_of(xs), y0 = py_min_of(ys), x1 = py_max_of(xs), y1 = py_max_of(ys);
            if (x0 < b.x.value() - 1 || y0 < b.y.value() - 1 || x1 > b.x.value() + b.width.value() + 1 || y1 > b.y.value() + b.height.value() + 1) {
                out.push_back(issue("warning", "art_outside_page", page, "「" + label + "」の絵が裁ち落としの外まで出ている（はみ出た所は印刷されない）",
                                    box_json(std::vector<double>{x0, y0, x1 - x0, y1 - y0}), "layer", layer.id));
            }
        }
    }
    for (const core::Frame* frame : page.leaf_frames()) {
        if (frame->panel && core::py_truthy(*frame->panel) && regions_stale(*frame->panel)) {
            out.push_back(issue("warning", "regions_stale", page,
                                num(page.index) + " ページのコマ " + str_or(core::dict_get(*frame->panel, "slot"), frame->id) +
                                    " の顔の位置は、差し替える前の絵のもの（report_regions で送り直し、仕上げをやり直す）",
                                nullptr, "frame", frame->id));
        }
    }
    moire(page, out);
    if (page.spread_with && page.spread_with->truthy()) {
        const core::Page* partner = nullptr;
        for (const auto& p : episode.pages) {
            if (p->index == *page.spread_with) {
                partner = p.get();
                break;
            }
        }
        const std::optional<std::string> problem =
            partner != nullptr ? core::facing_problem(episode, page, *partner) : std::optional<std::string>("the partner page is missing");
        if (problem && !problem->empty()) {
            out.push_back(issue("error", "spread_not_facing", page,
                                num(page.index) + " ページの見開きの相手（" + num(*page.spread_with) + " ページ）が向かい合わない"));
        }
    }
    bool art = false;
    std::vector<const core::Layer*> hidden;
    for (const core::Layer& layer : page.layers) {
        const bool drawn = layer.stroke_count() > 0 || !layer.patches.empty() || has_pixels(layer);
        if (prints(layer) && (drawn || layer.kind == LayerKind::Placed)) art = true;
        if (drawn && !prints(layer)) hidden.push_back(&layer);
    }
    if (!art && !hidden.empty()) {
        std::vector<std::string> names;
        for (const core::Layer* layer : hidden) {
            const std::string name = layer->role == LayerRole::Name    ? std::string("ネーム")
                                     : layer->role == LayerRole::Draft ? std::string("下描き")
                                     : layer->title.empty()            ? std::string("レイヤー")
                                                                       : layer->title;
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        std::string joined;
        for (const std::string& name : names) joined += (joined.empty() ? "" : "・") + name;
        out.push_back(issue("error", "art_not_printed", page,
                            num(page.index) + " ページの絵は「" + joined +
                                "」のレイヤーにだけ描かれていて、印刷・書き出しに出ない（ペン入れのレイヤーに描くか、レイヤーの「下描き（書き出さない）」を外す）",
                            nullptr, "layer", hidden.front()->id));
    } else if (!art && lines.empty() && !core::py_truthy(page.effects)) {
        out.push_back(issue("warning", "empty_page", page, num(page.index) + " ページに何も描かれていない"));
    }
    return out;
}

Json preflight(const core::Document& episode, const fs::path& project) {
    const storage::AssetStore store(project);
    Json errors = Json::array();
    Json warnings = Json::array();
    const bool studio_project = episode.strict_gates || core::py_truthy(episode.studio);
    PyDict characters;
    for (const Json& c : core::iterate(episode.bible.characters)) {
        const Json* id = core::dict_get(c, "id");
        characters.set(id != nullptr ? *id : Json(nullptr), c);
    }
    std::vector<Json> needed_sheets;  // (a set: each id once, as Python's equality has it)
    for (const auto& page_ptr : episode.pages) {
        const core::Page& page = *page_ptr;
        const std::string where = "/pages/" + num(page.index);
        if (studio_project) {
            if (!page.name_ok) errors.push_back(pf_issue("name_not_approved", "error", where, num(page.index) + " ページのネームが承認されていない"));
            if (!page.art_ok) errors.push_back(pf_issue("art_not_approved", "error", where, num(page.index) + " ページの作画が承認されていない"));
        }
        if (page.stage != "finish") {
            errors.push_back(pf_issue("page_not_finished", "error", where, num(page.index) + " ページが仕上げ（finish）に進んでいない"));
        }
        for (const core::Frame* frame : page.leaf_frames()) {
            if (!frame->panel || frame->panel->is_null()) continue;
            const Json& panel = *frame->panel;
            for (const Json& c : core::iterate(core::py_get(panel, "characters", Json::array()))) {
                if (!c.is_object()) continue;
                const Json* id = core::dict_get(c, "id");
                const Json key = id != nullptr ? *id : Json(nullptr);
                if (characters.find(key) == nullptr) continue;
                const Json value = core::subscript(c, "id");
                if (std::none_of(needed_sheets.begin(), needed_sheets.end(), [&](const Json& s) { return core::py_equals(s, value); })) {
                    needed_sheets.push_back(value);
                }
            }
            const Json* status = core::dict_get(panel, "status");
            if (status == nullptr || !core::is_one_of(*status, {"adopted", "skip"})) {
                errors.push_back(pf_issue("panel_without_art", "error", where + "/frames/" + frame->id,
                                          num(page.index) + " ページのコマ " + str_or(core::dict_get(panel, "slot"), frame->id) + " に採用した絵が無い"));
            }
        }
        for (const core::StoryLine* line : episode.story_for_page(page.index)) {
            if (!line->x_mm.truthy() && !line->y_mm.truthy()) {
                errors.push_back(pf_issue("line_unplaced", "error", where + "/lines/" + line->id,
                                          num(page.index) + " ページの台詞「" + head(line->text, 12) + "」に位置が無い"));
            }
        }
        for (const core::Layer& layer : page.layers) {
            if (layer.kind != LayerKind::Placed || !layer.exportable || !layer.visible) continue;  // (preflight.art_layers)
            const std::string lwhere = where + "/layers/" + layer.id;
            const std::string frame_id = layer.frame_id ? *layer.frame_id : std::string("None");
            const bool asset = layer.asset && !layer.asset->empty();
            if (asset) {
                std::error_code ec;
                if (!fs::is_regular_file(store.path(*layer.asset, ".png"), ec)) {
                    errors.push_back(pf_issue("asset_missing", "error", lwhere,
                                              num(page.index) + " ページの画像 " + head(*layer.asset, 19) + "… が assets/ に無い"));
                    continue;
                }
            }
            std::optional<double> dpi;  // (preflight.layer_dpi)
            if (asset && layer.placement_mm) {
                if (const auto size = image_size(store, *layer.asset)) {
                    const Num& width = layer.placement_mm->width;
                    const double effective = width > Num(0) ? static_cast<double>(size->first) / (width.value() / 25.4) : 0.0;
                    dpi = core::py_round(effective, 1);
                }
            }
            if (dpi && *dpi < kMinDpi) {
                errors.push_back(pf_issue("low_dpi", "error", lwhere,
                                          num(page.index) + " ページのコマ " + frame_id + " の実効解像度が " + fixed(*dpi, 0) + " dpi（" +
                                              std::to_string(kMinDpi) + " 未満）",
                                          "高解像度化した候補を取り込むか、--force で通す"));
            }
            const std::optional<Json> cand = candidate_for(page, layer);
            const Json origin = core::py_or(cand_get(cand, "origin"), Json::object());
            if (core::py_truthy(cand_get(cand, "upscaled"))) {  // (enlarged: prints at size, but nothing more was drawn)
                const Json up = core::subscript(*cand, "upscaled");
                const Json* scale = core::dict_get(up, "scale");
                const Json* method = core::dict_get(up, "method");
                warnings.push_back(pf_issue("upscaled", "warning", lwhere,
                                            num(page.index) + " ページのコマ " + frame_id + " の絵は " + core::py_str(scale != nullptr ? *scale : Json(nullptr)) +
                                                " 倍に拡大したもの（" + core::py_str(method != nullptr ? *method : Json(nullptr)) + "。描き込みは元の大きさのまま）"));
            }
            const Json drawn = drawn_origin(page, layer, cand);
            const Json* drawn_kind = core::dict_get(drawn, "kind");
            const Json* kind = core::dict_get(origin, "kind");
            if (drawn_kind != nullptr && drawn_kind->is_string() && drawn_kind->get_ref<const std::string&>() == "fixture") {
                errors.push_back(pf_issue("fixture_image", "error", lwhere, num(page.index) + " ページに試験用の画像（fixture）がある",
                                          "本番では使えない。--allow-fixture で通す"));
            } else if (!cand || (kind != nullptr && kind->is_string() && kind->get_ref<const std::string&>() == "agent" &&
                                 !core::py_truthy(core::py_or(core::py_get(origin, "tool_id"), core::py_get(origin, "model"))))) {
                warnings.push_back(pf_issue("provenance_missing", "warning", lwhere,
                                            num(page.index) + " ページのコマ " + frame_id + " の画像の来歴（ツール・モデル）が記録されていない"));
            }
        }
    }
    if (studio_project) {
        std::sort(needed_sheets.begin(), needed_sheets.end(), py_sort_less);
        for (const Json& cid : needed_sheets) {
            const Json* character = characters.find(cid);
            const Json* locked = character != nullptr ? core::dict_get(*character, "locked") : nullptr;
            if (locked == nullptr || !core::py_truthy(*locked)) {
                errors.push_back(pf_issue("sheet_not_approved", "error", "/characters/" + core::py_str(cid), core::py_str(cid) + " の設定画が承認されていない"));
            }
        }
    }
    return Json{{"errors", errors}, {"warnings", warnings}};
}

Json book(const core::Document& episode, const std::optional<fs::path>& project) {
    // (a book whose pages are still being read: the ones not read yet would be found empty, their problems not at all)
    if (!episode.deferred.empty()) throw core::Error("page_not_loaded", "the book's pages are still being read: try again in a moment");
    Json issues = Json::array();
    for (const auto& page : episode.pages) {
        for (Json& found : page_issues(episode, *page)) issues.push_back(std::move(found));
    }
    if (project && (episode.strict_gates || core::py_truthy(episode.studio))) {
        Json report;
        try {
            report = preflight(episode, *project);
        } catch (const render::NotYetPorted&) {
            throw;
        } catch (const std::exception&) {  // (Python's `except Exception`: no preflight issues)
            report = Json{{"errors", Json::array()}, {"warnings", Json::array()}};
        }
        for (const auto& [level, key] : {std::pair{"error", "errors"}, std::pair{"warning", "warnings"}}) {
            for (const Json& item : report[key]) {
                const Json* where_value = core::get(item, "where");
                if (where_value == nullptr || !core::py_truthy(*where_value)) where_value = core::get(item, "path");
                const std::string where = where_value != nullptr && core::py_truthy(*where_value) ? core::py_str(*where_value) : std::string();
                Json page_no = nullptr;
                std::vector<std::string> parts;
                std::size_t start = 0;
                for (std::size_t at = where.find('/'); ; at = where.find('/', start)) {
                    parts.push_back(where.substr(start, at == std::string::npos ? std::string::npos : at - start));
                    if (at == std::string::npos) break;
                    start = at + 1;
                }
                if (parts.size() > 2 && parts[1] == "pages" && !parts[2].empty() &&
                    std::all_of(parts[2].begin(), parts[2].end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    page_no = core::py_int(Json(parts[2]));
                }
                issues.push_back(Json{{"level", level},
                                      {"code", core::get_or(item, "code", nullptr)},
                                      {"page", page_no},
                                      {"message", core::get_or(item, "message", nullptr)},
                                      {"box", nullptr},
                                      {"target", Json{{"kind", "page"}, {"id", nullptr}}}});
            }
        }
    }
    std::int64_t errors = 0, warnings = 0;
    for (const Json& found : issues) {
        if (found["level"] == "error") ++errors;
        if (found["level"] == "warning") ++warnings;
    }
    return Json{{"ok", errors == 0}, {"issues", issues}, {"errors", errors}, {"warnings", warnings}};
}

}  // namespace genko::formats::checks
