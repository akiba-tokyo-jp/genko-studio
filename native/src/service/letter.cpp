#include "service/letter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/pynum.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/pyfmt.hpp"
#include "service/result.hpp"
#include "service/text.hpp"

namespace genko::service::letter {

namespace {

Box head_of(const Figure& f) { return service::as_box(f.head); }
Box body_of(const Figure& f) { return service::as_box(f.body); }

const double kTailPastMm[2] = {3.0, 8.0};
constexpr double kFitPad = 0.15;  // balloons.FIT_PAD
constexpr double kHandPower = 2.6;

bool in(const std::string& value, std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&value](const char* name) { return value == name; });
}

bool ellipse_kind(const std::string& balloon) { return in(balloon, {"speech", "thought", "shout", "whisper"}); }
std::optional<double> spiked(const std::string& balloon) { return balloon == "shout" ? std::optional<double>(0.26) : std::nullopt; }
const char* const kJoined[] = {"speech", "rounded", "thought", "whisper"};

// balloons.hug_power and _hug_keep for the style letter gives them ({} or {"spike_depth": …})
double hug_power(const std::string& kind) { return in(kind, {"speech", "thought", "whisper"}) ? kHandPower : 2.0; }
double hug_keep(const std::string& kind, std::optional<double> spike) {
    if (kind == "shout") return 1 - core::py_max(0.05, core::py_min(0.6, spike.value_or(0.2))) - 0.02;
    if (in(kind, {"speech", "thought", "whisper"})) return 0.955;
    return 1.0;
}

// balloons.block_points(rects, pad): the corners of the rectangles (x0, y0, x1, y1), pad out, around the block's
// middle, as numpy stacks them (every rectangle's (x0, y0), then (x1, y0), (x0, y1), (x1, y1)).
std::vector<std::array<double, 2>> block_points(const std::vector<std::array<double, 4>>& rects, double pad) {
    double min_x0 = rects[0][0];
    double max_x1 = rects[0][2];
    double min_y0 = rects[0][1];
    double max_y1 = rects[0][3];
    for (const auto& r : rects) {
        min_x0 = std::min(min_x0, r[0]);
        max_x1 = std::max(max_x1, r[2]);
        min_y0 = std::min(min_y0, r[1]);
        max_y1 = std::max(max_y1, r[3]);
    }
    const double cx = (min_x0 + max_x1) / 2;
    const double cy = (min_y0 + max_y1) / 2;
    std::vector<std::array<double, 2>> out;
    const auto x0 = [&](const auto& r) { return r[0] - pad - cx; };
    const auto y0 = [&](const auto& r) { return r[1] - pad - cy; };
    const auto x1 = [&](const auto& r) { return r[2] + pad - cx; };
    const auto y1 = [&](const auto& r) { return r[3] + pad - cy; };
    for (const auto& r : rects) out.push_back({x0(r), y0(r)});
    for (const auto& r : rects) out.push_back({x1(r), y0(r)});
    for (const auto& r : rects) out.push_back({x0(r), y1(r)});
    for (const auto& r : rects) out.push_back({x1(r), y1(r)});
    return out;
}

// np.geomspace(0.25, 4.0, 48) as the reference numpy computes it (its own log10 and power: kept as numbers)
const double kRatios[48] = {
    0x1.0000000000000p-2, 0x1.0f8e5b9be9bb2p-2, 0x1.200eb51dda99ap-2, 0x1.318fc0f7bb411p-2, 0x1.4421185b7b40dp-2, 0x1.57d3472185d94p-2,
    0x1.6cb7da877268cp-2, 0x1.82e170d4143bbp-2, 0x1.9a63c9eed9e31p-2, 0x1.b353d8f945082p-2, 0x1.cdc7d6fa29b26p-2, 0x1.e9d756ab57ed6p-2,
    0x1.03cdacbdaac53p-1, 0x1.139732eaf01e9p-1, 0x1.24564f6389c4fp-1, 0x1.3619ee8721709p-1, 0x1.48f1e4db6ff86p-1, 0x1.5ceefd2790643p-1,
    0x1.7223076ac5110p-1, 0x1.88a0e8bc049f3p-1, 0x1.a07cac2073d03p-1, 0x1.b9cb9467cd874p-1, 0x1.d4a42f1ea3872p-1, 0x1.f11e68a75a232p-1,
    0x1.07a9d0c5e3ddfp+0, 0x1.17af628d3bbe5p+0, 0x1.28ae30b1db299p+0, 0x1.3ab56055e54eap+0, 0x1.4dd5023481c86p+0, 0x1.621e20f2d70efp+0,
    0x1.77a2d04fb9a0ap+0, 0x1.8e763d3f98503p+0, 0x1.a6acbf0300acep+0, 0x1.c05be946f5c74p+0, 0x1.db9a9f5f40781p+0, 0x1.f88128abdb958p+0,
    0x1.0b94a31dd4871p+1, 0x1.1bd724dfdbcf1p+1, 0x1.2d1696f1c6614p+1, 0x1.3f6258101257fp+1, 0x1.52cab61055c84p+1, 0x1.6760fc68aca4ap+1,
    0x1.7d3783992a621p+1, 0x1.9461c1850b458p+1, 0x1.acf45aca36e9fp+1, 0x1.c70535268828bp+1, 0x1.e2ab8afb3efefp+1, 0x1.0000000000000p+2,
};

// np.linspace(-span, span, 9)
std::array<double, 9> linspace9(double span) {
    const double start = -span;
    const double step = (span - start) / 8;
    std::array<double, 9> out{};
    for (int i = 0; i < 9; ++i) out[i] = (step == 0 ? static_cast<double>(i) / 8 * (span - start) : static_cast<double>(i) * step) + start;
    out[8] = span;
    return out;
}

// |v| ** n as numpy raises an array to a float power (2.0 squares)
double power(double v, double n) {
    const double a = std::fabs(v);
    return n == 2.0 ? a * a : std::pow(a, n);
}

Box grow(const Box& box, double by) { return {box[0] - by, box[1] - by, box[2] + 2 * by, box[3] + 2 * by}; }

// letter._over: how far (mm, all sides together) a box runs past the panel's inner margin.
double over(const Box& box, const Box& rect) {
    const auto [x, y, w, h] = box;
    const auto [x0, y0, rw, rh] = rect;
    return (core::py_max(0.0, x0 + kMarginMm - x) + core::py_max(0.0, x + w - (x0 + rw - kMarginMm)) + core::py_max(0.0, y0 + kMarginMm - y) +
            core::py_max(0.0, y + h - (y0 + rh - kMarginMm)));
}

bool centre_in(const Box& box, const Box& rect) {
    const double cx = box[0] + box[2] / 2;
    const double cy = box[1] + box[3] / 2;
    return rect[0] <= cx && cx <= rect[0] + rect[2] && rect[1] <= cy && cy <= rect[1] + rect[3];
}

bool after(const Box& box, const std::optional<Box>& prev) {
    if (!prev) return true;
    const auto [x, y, w, h] = box;
    const auto [px, py, pw, ph] = *prev;
    (void)h;
    return x + w <= px + pw * 0.5 || y >= py + ph * 0.5;
}

bool touches_any(const Box& box, const std::vector<Box>& others, std::size_t count = static_cast<std::size_t>(-1)) {
    const Box grown = grow(box, kGapMm);
    for (std::size_t i = 0; i < others.size() && i < count; ++i) {
        if (overlap(grown, others[i]) > 0) return true;
    }
    return false;
}

std::optional<std::pair<Box, bool>> joined_spot(const Box& prev, std::array<double, 2> size, const Box& rect, const std::vector<Box>& placed,
                                                std::size_t placed_count, const std::vector<Figure>& figs, const Room& room) {
    const auto [px, py, pw, ph] = prev;
    const auto [bw, bh] = size;
    const double lap = 0.22 * core::py_min(pw, bw);
    const double x = px - bw + lap;
    for (const double share : {0.3, 0.2, 0.4, 0.1, 0.5, 0.0, 0.6}) {
        const Box box{x, py + ph * share, bw, bh};
        if (!centre_in(box, rect) || over(box, rect) > kOverMm * 1.5) continue;
        if (touches_any(box, placed, placed_count)) continue;
        Room fewer = room;  // (the first balloon is the last placed)
        const std::size_t taken = room.taken != nullptr ? std::min(room.taken_count, room.taken->size()) : 0;
        fewer.taken_count = taken > 0 ? taken - 1 : 0;
        if (!fewer.allows(box)) continue;
        if (std::any_of(figs.begin(), figs.end(), [&box](const Figure& f) { return overlap(box, head_of(f)) > 0; })) continue;
        return std::pair<Box, bool>{box, false};
    }
    return std::nullopt;
}

bool obvious(const Box& box, const Figure& speaker, const std::vector<Figure>& figs) {
    std::vector<const Figure*> others;
    for (const Figure& f : figs) {
        if (&f != &speaker) others.push_back(&f);
    }
    if (others.empty()) return true;
    const double near = gap(box, head_of(speaker));
    return near <= 3.0 && std::all_of(others.begin(), others.end(), [&](const Figure* f) { return gap(box, head_of(*f)) >= core::py_max(12.0, near + 10.0); });
}

std::optional<std::array<double, 2>> tail_of(const Box& box, const std::string& balloon, const Json& speaker_id, const std::vector<Figure>& figs) {
    if (in(balloon, {"narration", "sfx", "aside", "none", "dotted_box", "tone_box", "fancy_box"}) || !core::py_truthy(speaker_id)) return std::nullopt;
    const auto fig = std::find_if(figs.begin(), figs.end(), [&](const Figure& f) { return core::py_equals(f.char_id, speaker_id); });
    if (fig == figs.end()) return std::nullopt;
    if (!in(balloon, {"thought", "shout", "electric"}) && obvious(box, *fig, figs)) return std::nullopt;
    return tail_to(box, head_of(*fig), balloon);
}

std::vector<std::string> strings_of(const Json& value) {
    std::vector<std::string> out;
    for (const Json& item : core::iterate(value)) out.push_back(core::py_str(item));
    return out;
}

std::string joined(const std::vector<std::string>& parts, const std::string& with) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? with : "") + parts[i];
    return out;
}

Json template_shape(const Json& name) {
    const Json& known = layout::templates();
    if (name.is_string() && known.contains(name.get<std::string>())) return known[name.get<std::string>()];
    return Json::object();
}

std::vector<BalloonPlacement> title_placements(const std::string& slot, const Box& rect, const Json& bible, const Json& look) {
    const std::string title = core::py_strip(core::py_str(core::py_or(core::py_get(bible, "title"), Json(""))));
    if (title.empty()) return {};
    const auto [x, y, w, h] = rect;
    const double room = h - 2 * kMarginMm;
    const double count = static_cast<double>(std::max<std::size_t>(1, text::length(title)));
    const double em = core::py_max(6.0, std::min({kTitleMaxMm, room * 0.85 / count, w * 0.35}));
    Json scaled = look;
    scaled["scale"] = nullptr;
    Json style = look_style(scaled, "none");
    const Box title_box{x + w - kMarginMm - em * 1.1, y + kMarginMm, em * 1.1,
                        core::py_min(room, em * static_cast<double>(text::length(title)) * 1.02)};
    Json title_style = style;
    title_style["size_mm"] = core::py_round(em, 2);
    std::vector<BalloonPlacement> out;
    BalloonPlacement first;
    first.slot = slot;
    first.line_index = -1;
    first.text = title;
    first.balloon = "none";
    first.x_mm = core::py_round(title_box[0], 2);
    first.y_mm = core::py_round(title_box[1], 2);
    first.w_mm = core::py_round(title_box[2], 2);
    first.h_mm = core::py_round(title_box[3], 2);
    first.style = title_style;
    out.push_back(first);
    const std::string author = core::py_strip(core::py_str(core::py_or(core::py_get(bible, "author"), Json(""))));
    if (!author.empty()) {
        const double small = core::py_max(5.0, core::py_round(em / 3, 2));
        const double height = core::py_min(room, small * static_cast<double>(text::length(author)) * 1.02);
        Json author_style = Json::object();
        if (style.contains("font")) author_style["font"] = style["font"];
        author_style["size_mm"] = small;
        BalloonPlacement second;
        second.slot = slot;
        second.line_index = -2;
        second.text = author;
        second.balloon = "none";
        second.x_mm = core::py_round(title_box[0] - small * 1.4, 2);
        second.y_mm = core::py_round(title_box[1] + title_box[3] - height, 2);
        second.w_mm = core::py_round(small * 1.1, 2);
        second.h_mm = core::py_round(height, 2);
        second.style = author_style;
        out.push_back(second);
    }
    return out;
}

Box as_box(const layout::RectMm& r) { return {r[0].value(), r[1].value(), r[2].value(), r[3].value()}; }

}  // namespace

std::array<double, 4> ellipse_around(const std::vector<std::array<double, 2>>& points, double power_n) {
    const double n = power_n;
    double min_x = points[0][0], max_x = points[0][0], min_y = points[0][1], max_y = points[0][1];
    for (const auto& p : points) {
        min_x = std::min(min_x, p[0]);
        max_x = std::max(max_x, p[0]);
        min_y = std::min(min_y, p[1]);
        max_y = std::max(max_y, p[1]);
    }
    const double ptp_x = max_x - min_x;
    const double ptp_y = max_y - min_y;
    const double span_x = ptp_x != 0 ? ptp_x : 1.0;
    const double span_y = ptp_y != 0 ? ptp_y : 1.0;
    bool have = false;
    double best_area = 0, best_ratio = 0, best_b = 0;
    std::array<double, 2> best_off{0, 0};
    std::array<double, 2> centre{0, 0};
    for (const double step : {0.18, 0.05}) {
        const auto xs = linspace9(span_x * step);
        const auto ys = linspace9(span_y * step);
        std::vector<std::array<double, 2>> offs;
        for (int r = 0; r < 9; ++r) {
            for (int c = 0; c < 9; ++c) offs.push_back({centre[0] + xs[c], centre[1] + ys[r]});
        }
        for (const double ratio : kRatios) {
            std::size_t k = 0;
            double least = 0;
            for (std::size_t o = 0; o < offs.size(); ++o) {
                double need = -std::numeric_limits<double>::infinity();
                for (const auto& p : points) {
                    const double d = power((p[0] - offs[o][0]) / ratio, n) + power(p[1] - offs[o][1], n);
                    need = std::max(need, d);
                }
                if (o == 0 || need < least) {
                    least = need;
                    k = o;
                }
            }
            const double b = std::pow(least, 1 / n);
            const double area = ratio * b * b;
            if (!have || area < best_area) {
                have = true;
                best_area = area;
                best_ratio = ratio;
                best_b = b;
                best_off = offs[k];
            }
        }
        centre = best_off;
    }
    return {2 * best_ratio * best_b, 2 * best_b, best_off[0], best_off[1]};
}

Json looks(const Json& bible) {
    Json out = Json::object();
    out["shout"] = Json::object({{"scale", 1.3}, {"weight", "bold"}});
    out["whisper"] = Json::object({{"scale", 0.8}});
    out["aside"] = Json::object({{"font", "hand"}, {"scale", 0.8}, {"weight", "bold"}});
    const Json lettering = core::py_or(core::py_get(core::py_or(bible, Json::object()), "lettering"), Json::object());
    for (const auto& [kind, look] : lettering.items()) {
        if (!look.is_object()) continue;
        Json merged = out.contains(kind) ? out[kind] : Json::object();
        for (const auto& [k, v] : look.items()) {
            if (!v.is_null()) merged[k] = v;
        }
        out[kind] = merged;
    }
    return out;
}

Json look_style(const Json& look, const std::string& balloon) {
    Json style = Json::object();
    if (core::truthy_at(look, "font")) style["font"] = core::py_str(look["font"]);
    if (core::truthy_at(look, "weight") && !(look["weight"].is_string() && look["weight"] == "normal")) style["weight"] = core::py_str(look["weight"]);
    const double scale = core::to_float(core::py_or(core::py_get(look, "scale"), Json(1.0)));
    if (balloon != "sfx" && std::abs(scale - 1.0) > 1e-3) style["size_mm"] = core::py_round(kEmMm * scale, 2);
    return style;
}

std::array<double, 2> measure(const std::vector<std::string>& breaks, const std::string& balloon, double em) {
    std::vector<std::string> cols;
    for (const std::string& b : breaks) {
        if (!b.empty()) cols.push_back(b);
    }
    if (cols.empty()) cols.push_back(" ");
    std::size_t longest = 0;
    for (const std::string& col : cols) longest = std::max(longest, text::length(col));
    const double count = static_cast<double>(cols.size());
    if (balloon == "sfx") return {kSfxEmMm * count, kSfxEmMm * static_cast<double>(longest)};
    const double text_w = em * count + em * kLeading * (count - 1);
    const double text_h = em * static_cast<double>(longest);
    if (ellipse_kind(balloon) || spiked(balloon)) {
        std::vector<std::array<double, 4>> rects;
        for (std::size_t i = 0; i < cols.size(); ++i) {  // (the first column on the right)
            const double x1 = text_w - static_cast<double>(i) * em * (1 + kLeading);
            rects.push_back({x1 - em, 0.0, x1, em * static_cast<double>(std::max<std::size_t>(1, text::length(cols[i])))});
        }
        const auto oval = ellipse_around(block_points(rects, em * kFitPad), hug_power(balloon));
        const double keep = hug_keep(balloon, spiked(balloon));
        return {oval[0] / keep, oval[1] / keep};
    }
    double pad = in(balloon, {"none", "aside"}) ? 0.0 : em / 4;
    if (in(balloon, {"dotted_box", "tone_box", "fancy_box"})) pad *= 3;
    if (balloon == "electric") return {(text_w + 2 * pad) / 0.76, (text_h + 2 * pad) / 0.76};
    return {text_w + 2 * pad, text_h + 2 * pad};
}

std::string fits(const Box& rect, const std::string& balloon, std::size_t others, double em) {
    const double w = rect[2];
    const double h = rect[3];
    const double room_w = w - 2 * kMarginMm;
    const double room_h = h - 2 * kMarginMm;
    double chars = 0;
    double cols = 0;
    if (balloon == "sfx") {
        chars = py_floordiv(room_h, kSfxEmMm);
        cols = py_floordiv(room_w, kSfxEmMm);
    } else {
        const double grow_by = (ellipse_kind(balloon) || spiked(balloon)) ? std::pow(2.0, 0.5) / (1 - spiked(balloon).value_or(0.0)) : 1.0;
        const double pad = balloon == "none" ? 0.0 : em / 4;
        chars = py_floordiv((room_h - 2 * pad) / grow_by, em);
        cols = py_floordiv(((room_w - 2 * pad) / grow_by) + em * kLeading, em * (1 + kLeading));
    }
    // (int(…) of the quotient, then max(0, …))
    const auto count = [](double v) { return std::to_string(std::max<long long>(0, core::py_trunc_int(v))); };
    const std::string where = "このコマ（" + fixed(w, 0) + "×" + fixed(h, 0) + " mm）のこの形のフキダシには、1 列 " + count(chars) + " 字・" +
                              count(cols) + " 列まで";
    return where + (others ? "（ほかの台詞と場所を分け合うので、実際はもっと少ない）" : "");
}

double gap(const Box& a, const Box& b) {
    const auto [ax, ay, aw, ah] = a;
    const auto [bx, by, bw, bh] = b;
    const double dx = core::py_max(0.0, core::py_max(bx - (ax + aw), ax - (bx + bw)));
    const double dy = core::py_max(0.0, core::py_max(by - (ay + ah), ay - (by + bh)));
    return std::pow(dx * dx + dy * dy, 0.5);
}

double overlap(const Box& a, const Box& b) {
    const auto [ax, ay, aw, ah] = a;
    const auto [bx, by, bw, bh] = b;
    const double dx = core::py_min(ax + aw, bx + bw) - core::py_max(ax, bx);
    const double dy = core::py_min(ay + ah, by + bh) - core::py_max(ay, by);
    return dx > 0 && dy > 0 ? dx * dy : 0.0;
}

bool Room::allows(const Box& box) const {
    if (std::any_of(neighbours.begin(), neighbours.end(), [&box](const Box& n) { return overlap(box, n) > 0; })) return false;
    if (taken == nullptr) return true;
    return !touches_any(box, *taken, taken_count);
}

std::optional<std::pair<Box, bool>> find_spot(const Box& rect, std::array<double, 2> size, const std::vector<Box>& placed,
                                              const std::vector<Figure>& figs, const Json& speaker_id,
                                              const std::optional<std::array<double, 2>>& near, const Room* room) {
    const auto [x0, y0, w, h] = rect;
    const auto [bw, bh] = size;
    double left = x0 + kMarginMm;
    double top = y0 + kMarginMm;
    double right = x0 + w - kMarginMm;
    double bottom = y0 + h - kMarginMm;
    const double inner_right = right;
    const double inner_top = top;
    if (room != nullptr && !near) {
        const double ox = core::py_min(kOverMm, bw * 0.3);
        const double oy = core::py_min(kOverMm, bh * 0.3);
        left = x0 - ox;
        top = y0 - oy;
        right = x0 + w + ox;
        bottom = y0 + h + oy;
        if (room->page) {  // (at most a little past the panels' outer edge, into the page's margin)
            const auto [px0, py0, px1, py1] = *room->page;
            left = core::py_max(left, px0 - 4.0);
            top = core::py_max(top, py0 - 4.0);
            right = core::py_min(right, px1 + 4.0);
            bottom = core::py_min(bottom, py1 + 4.0);
        }
    }
    if (bw > right - left || bh > bottom - top) return std::nullopt;
    const std::optional<Box> prev = placed.empty() ? std::nullopt : std::optional<Box>(placed.back());
    const Figure* speaker = nullptr;
    if (core::py_truthy(speaker_id)) {
        for (const Figure& f : figs) {
            if (core::py_equals(f.char_id, speaker_id)) {
                speaker = &f;
                break;
            }
        }
    }
    bool have = false;
    double best_cost = 0;
    Box best_box{};
    bool best_face = false;
    for (double y = top; y + bh <= bottom + 1e-6; y += kStepMm) {
        for (double x = right - bw; x >= left - 1e-6; x -= kStepMm) {
            const Box box{x, y, bw, bh};
            if (near) {
                if (touches_any(box, placed)) continue;
                std::vector<double> faces;
                for (const Figure& f : figs) faces.push_back(overlap(box, head_of(f)));
                const double face = core::py_float_sum(faces);
                const double cx = x + bw / 2;
                const double cy = y + bh / 2;
                const double cost = 20000.0 * face / (bw * bh) + std::pow(std::pow(cx - (*near)[0], 2) + std::pow(cy - (*near)[1], 2), 0.5);
                if (!have || cost < best_cost) {
                    have = true;
                    best_cost = cost;
                    best_box = box;
                    best_face = face > 0;
                }
            } else if (!touches_any(box, placed) && after(box, prev) && (room == nullptr || room->allows(box)) && centre_in(box, rect)) {
                std::vector<double> faces;
                std::vector<double> bodies;
                for (const Figure& f : figs) {
                    faces.push_back(overlap(box, head_of(f)));
                    bodies.push_back(overlap(box, body_of(f)));
                }
                const double face = core::py_float_sum(faces);
                const double body = core::py_float_sum(bodies);
                double cost = 20000.0 * face / (bw * bh) + 3.0 * body / (bw * bh) + 0.6 * over(box, rect);
                if (speaker != nullptr) {
                    const double to_speaker = gap(box, head_of(*speaker));
                    const double cx = x + bw / 2;
                    const Box head = head_of(*speaker);
                    const double hx = head[0] + head[2] / 2;
                    cost += 2.0 * to_speaker + 0.25 * std::abs(cx - hx) +
                            0.3 * core::py_max(0.0, y + bh / 2 - (head[1] + head[3]));
                    cost += 8.0 * core::py_max(0.0, kTailRoomMm - to_speaker);  // (room for the tail between balloon and face)
                    for (const Figure& f : figs) {
                        if (&f == speaker) continue;
                        if (gap(box, head_of(f)) + 1.0 < to_speaker) {
                            cost += 60.0;  // (nearer someone else: it would read as their line)
                            break;
                        }
                    }
                } else {
                    cost += std::abs(inner_right - (x + bw)) + 1.2 * std::abs(y - inner_top);
                }
                if (!have || cost < best_cost) {
                    have = true;
                    best_cost = cost;
                    best_box = box;
                    best_face = face > 0;
                }
            }
        }
    }
    if (!have) return std::nullopt;
    return std::pair<Box, bool>{best_box, best_face};
}

std::optional<std::array<double, 2>> tail_to(const Box& box, const Box& head, const std::string& balloon) {
    const auto [x, y, w, h] = box;
    const double sx = x + w / 2;
    const double sy = y + h / 2;
    const auto [hx, hy, hw, hh] = head;
    const double mx = hx + hw / 2;
    const double my = hy + hh * 0.72;
    const double stop = balloon == "thought" ? 2.5 : 1.2;
    const Box grown{hx - stop, hy - stop, hw + 2 * stop, hh + 2 * stop};
    const double dx = mx - sx;
    const double dy = my - sy;
    const double dist = std::pow(dx * dx + dy * dy, 0.5);
    if (dist < 1e-6) return std::nullopt;
    // (the first point of the line from the balloon's middle that enters the head box, grown by the gap)
    double enter = 1.0;
    for (int i = 0; i <= 200; ++i) {
        const double t = static_cast<double>(i) / 200;
        const double px = sx + dx * t;
        const double py = sy + dy * t;
        if (grown[0] <= px && px <= grown[0] + grown[2] && grown[1] <= py && py <= grown[1] + grown[3]) {
            enter = t;
            break;
        }
    }
    double tx = sx + dx * enter;
    double ty = sy + dy * enter;
    if (x <= tx && tx <= x + w && y <= ty && ty <= y + h) {  // (the head is right against the balloon: a short point)
        const double out = core::py_min(dx != 0 ? (w / 2) / std::abs(dx / dist) : 1e9, dy != 0 ? (h / 2) / std::abs(dy / dist) : 1e9);
        double face = dist;
        for (int i = 0; i <= 400; ++i) {
            const double t = static_cast<double>(i) / 400;
            const double px = sx + dx * t;
            const double py = sy + dy * t;
            if (hx <= px && px <= hx + hw && hy <= py && py <= hy + hh) {
                face = t * dist;
                break;
            }
        }
        const double reach = core::py_max(out + 0.3, core::py_min(out + 2.0, face - 0.5));
        return std::array<double, 2>{sx + dx / dist * reach, sy + dy / dist * reach};
    }
    const double reach = std::pow(std::pow(tx - sx, 2) + std::pow(ty - sy, 2), 0.5);
    const double ux = dx / dist;
    const double uy = dy / dist;
    const double edge = 1 / core::py_max(1e-6, std::pow(std::pow(ux / (w / 2), 2) + std::pow(uy / (h / 2), 2), 0.5));  // (the ellipse's edge)
    const double past = core::py_max(kTailPastMm[0], core::py_min(kTailPastMm[1], core::py_min(w, h) / 4));
    const double longest = core::py_min(edge + past, core::py_max(w, h) / 2 + kTailMaxMm);
    if (reach > longest) {  // (it points at the mouth and stops well short: the reader's eye does the rest)
        tx = sx + ux * longest;
        ty = sy + uy * longest;
    }
    return std::array<double, 2>{tx, ty};
}

std::pair<std::vector<BalloonPlacement>, std::vector<studio::Issue>> place_page(const Json& plan, const layout::CompiledLayout& compiled,
                                                                                 const Json& bible, const Json& speakers) {
    const Json kinds = looks(bible);
    std::optional<std::string> title_slot;
    if (core::truthy_at(plan, "title")) {  // (扉: the title and the author's name, set before the lines in their panel)
        const Json shape = core::truthy_at(plan, "tiers") ? Json::object() : template_shape(core::py_get(plan, "template"));
        const Json given = core::py_get(shape, "title");
        if (core::py_truthy(given)) {
            title_slot = core::py_str(given);
        } else if (!compiled.reading_order.empty()) {
            title_slot = compiled.reading_order[0];
        }
    }
    std::vector<BalloonPlacement> placements;
    std::vector<studio::Issue> issues;
    std::optional<std::array<double, 4>> page;
    if (!compiled.leaf_rects_mm.empty()) {
        std::array<double, 4> edge{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (const auto& [slot, r] : compiled.leaf_rects_mm) {
            const Box b = as_box(r);
            edge[0] = std::min(edge[0], b[0]);
            edge[1] = std::min(edge[1], b[1]);
            edge[2] = std::max(edge[2], b[0] + b[2]);
            edge[3] = std::max(edge[3], b[1] + b[3]);
        }
        page = edge;
    }
    std::vector<Box> on_page;  // (every balloon so far, so one running over its border never meets the next panel's)
    const std::vector<Json> panels = core::iterate(core::py_get(plan, "panels", Json::array()));
    for (std::size_t pi = 0; pi < panels.size(); ++pi) {
        const Json& panel = panels[pi];
        const std::string slot = core::py_str(core::py_get(panel, "slot"));
        const layout::RectMm* rect_mm = core::py_get(panel, "slot").is_string() ? compiled.rect_of(slot) : nullptr;
        if (rect_mm == nullptr) continue;
        const Box rect = as_box(*rect_mm);
        const std::vector<Figure> figs = figures(panel, *rect_mm);
        std::vector<Box> placed;
        Room room;
        room.page = page;
        for (const auto& [other, r] : compiled.leaf_rects_mm) {
            if (other != slot) room.neighbours.push_back(grow(as_box(r), -2.0));
        }
        room.taken = &on_page;
        if (title_slot && slot == *title_slot) {
            for (BalloonPlacement& item : title_placements(slot, rect, bible, kinds.contains("title") ? kinds["title"] : Json::object())) {
                placed.push_back({item.x_mm, item.y_mm, item.w_mm, item.h_mm});
                placements.push_back(std::move(item));
            }
        }
        const std::vector<Json> lines = core::iterate(core::py_get(panel, "lines", Json::array()));
        for (std::size_t li = 0; li < lines.size(); ++li) {
            const Json& line = lines[li];
            const std::string path = "/panels/" + std::to_string(pi) + "/lines/" + std::to_string(li);
            const std::string balloon = core::py_str(core::py_get(line, "balloon", Json("speech")));
            Json style = look_style(kinds.contains(balloon) ? kinds[balloon] : Json::object(), balloon);
            if (balloon == "aside") style["rotate_deg"] = li % 2 == 0 ? kAsideTilt : -kAsideTilt;  // (呟き: leaning one way then the other)
            const double em = core::to_float(core::py_or(core::py_get(style, "size_mm"), Json(kEmMm)));
            const std::vector<std::string> breaks = strings_of(core::py_get(line, "breaks", Json::array()));
            const std::array<double, 2> size = measure(breaks, balloon, em);
            const Json beat_id = core::py_get(line, "beat_id");
            const Json speaker_id = speakers.is_object() && beat_id.is_string() && speakers.contains(beat_id.get<std::string>())
                                        ? speakers[beat_id.get<std::string>()]
                                        : Json();
            std::optional<std::array<double, 2>> near;
            const Json where = core::py_get(panel, "sfx_at");
            if (balloon == "sfx" && where.is_array() && where.size() == 2) {  // (the sound's source, 0..1)
                near = std::array<double, 2>{rect[0] + rect[2] * core::py_min(1.0, core::py_max(0.0, core::to_float(where[0]))),
                                             rect[1] + rect[3] * core::py_min(1.0, core::py_max(0.0, core::to_float(where[1])))};
            }
            const bool prev_here = !placements.empty() && placements.back().slot == slot;
            std::optional<std::string> group;
            std::optional<std::pair<Box, bool>> spot;
            if (prev_here) {
                const BalloonPlacement& prev = placements.back();
                const bool same = core::py_truthy(speaker_id) && core::py_equals(Json(prev.speaker_id), speaker_id) &&
                                  std::any_of(std::begin(kJoined), std::end(kJoined), [&balloon](const char* k) { return balloon == k; }) &&
                                  prev.balloon == balloon;
                if (same) {  // (the same person again: the second balloon runs out of the first)
                    spot = joined_spot({prev.x_mm, prev.y_mm, prev.w_mm, prev.h_mm}, size, rect, placed, placed.empty() ? 0 : placed.size() - 1,
                                       figs, room);
                    if (spot) {
                        const Json prev_group = prev.style ? core::py_get(*prev.style, "group") : Json();
                        group = core::py_truthy(prev_group) ? core::py_str(prev_group)
                                                            : "said" + std::to_string(pi) + "." + std::to_string(prev.line_index);
                        Json restyled = prev.style ? *prev.style : Json::object();
                        restyled["group"] = *group;
                        placements.back().style = restyled;
                    }
                }
            }
            if (!spot) spot = find_spot(rect, size, placed, figs, speaker_id, near, &room);
            if (!spot) {
                issues.push_back(error("balloon_overflow", path, "コマ " + slot + " に台詞が入らない（" + fixed(size[0], 0) + "×" + fixed(size[1], 0) + " mm）",
                                       fits(rect, balloon, placed.size(), em) + "。台詞を短くする、breaks で列を分ける、" +
                                           "台詞を別のコマに移す、コマを大きくする"));
                continue;
            }
            const auto [box, covers_face] = *spot;
            if (covers_face && balloon != "sfx" && balloon != "none") {  // (a sound or bare text over a face is the drawing's)
                issues.push_back(warning("balloon_covers_face", path, "コマ " + slot + " で台詞が顔にかかる", "台詞を減らすか、人物の pos を変える"));
            }
            placed.push_back(box);
            on_page.push_back(box);
            if (group) style["group"] = *group;
            BalloonPlacement item;
            item.slot = slot;
            item.line_index = static_cast<std::int64_t>(li);
            item.beat_id = core::py_get(line, "beat_id", Json(""));
            item.text = joined(breaks, "\n");
            item.balloon = balloon == "aside" ? "none" : balloon;
            item.x_mm = core::py_round(box[0], 2);
            item.y_mm = core::py_round(box[1], 2);
            item.w_mm = core::py_round(box[2], 2);
            item.h_mm = core::py_round(box[3], 2);
            if (!group) item.tail = tail_of(box, core::py_str(core::py_get(line, "balloon", Json("speech"))), speaker_id, figs);
            item.speaker_id = core::py_truthy(speaker_id) ? core::py_str(speaker_id) : std::string();
            if (!style.empty()) item.style = style;
            placements.push_back(std::move(item));
        }
        const std::vector<Json> characters = core::iterate(core::py_or(core::py_get(panel, "characters", Json::array()), Json::array()));
        for (std::size_t ci = 0; ci < characters.size(); ++ci) {  // (名札: a small label by the character)
            const Json& character = characters[ci];
            const std::string tag = core::py_strip(core::py_str(core::py_or(core::py_get(character, "tag"), Json(""))));
            const Json id = core::py_get(character, "id");
            const auto fig = std::find_if(figs.begin(), figs.end(), [&id](const Figure& f) { return core::py_equals(f.char_id, id); });
            if (tag.empty() || fig == figs.end()) continue;
            const auto measured = measure({tag}, "rounded", kTagEmMm);  // (set across: the column's size turned)
            const Box head = head_of(*fig);
            const Box body = body_of(*fig);
            const std::array<double, 2> near{head[0] + head[2] / 2, body[1] + body[3] * 0.55};
            const auto spot = find_spot(rect, {measured[1], measured[0]}, placed, figs, Json(), near);
            if (!spot) {
                issues.push_back(warning("tag_overflow", "/panels/" + std::to_string(pi) + "/characters/" + std::to_string(ci) + "/tag",
                                         "コマ " + slot + " に名札が入らない", "名札を短くするか、コマを大きくする"));
                continue;
            }
            const Box box = spot->first;
            placed.push_back(box);
            on_page.push_back(box);
            BalloonPlacement item;
            item.slot = slot;
            item.line_index = -10 - static_cast<std::int64_t>(ci);
            item.text = tag;
            item.balloon = "rounded";
            item.x_mm = core::py_round(box[0], 2);
            item.y_mm = core::py_round(box[1], 2);
            item.w_mm = core::py_round(box[2], 2);
            item.h_mm = core::py_round(box[3], 2);
            item.style = Json::object({{"font", "gothic"}, {"size_mm", kTagEmMm}});
            item.wrap = "horizontal";
            placements.push_back(std::move(item));
        }
    }
    return {std::move(placements), std::move(issues)};
}

Json placements_to_ops(const Json& page_index, const std::vector<BalloonPlacement>& placements, const layout::CompiledLayout& compiled) {
    Json ops = Json::array();
    for (const BalloonPlacement& p : placements) {
        const std::string* frame_id = compiled.frame_of(p.slot);
        if (frame_id == nullptr) throw core::PyUncaught("KeyError", core::py_repr_str(p.slot));
        Json op = Json::object();
        op["op"] = "add_line";
        op["page"] = page_index;
        op["text"] = p.text;
        // The renderer prints the speaker name above the balloon (also in print), so leave it empty.
        op["speaker"] = "";
        op["frame_id"] = *frame_id;
        op["balloon"] = p.balloon;
        op["wrap"] = p.wrap;
        op["x_mm"] = p.x_mm;
        op["y_mm"] = p.y_mm;
        op["w_mm"] = p.w_mm;
        op["h_mm"] = p.h_mm;
        if (p.tail) op["tail"] = Json::array({core::py_round((*p.tail)[0], 2), core::py_round((*p.tail)[1], 2)});
        Json style = p.style ? *p.style : Json::object();
        if (!p.speaker_id.empty()) style["speaker_id"] = p.speaker_id;
        if (!style.empty()) op["style"] = style;
        ops.push_back(std::move(op));
    }
    return ops;
}

}  // namespace genko::service::letter
