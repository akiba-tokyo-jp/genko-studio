#include "render/shape_presets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <set>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/path_raster.hpp"
#include "storage/fsutil.hpp"

// PhotoCraft v0.2.0 crates/engine/src/presets/shapes.rs (1-482: the path language, its own drawings, fit, the group
// edits), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::shape_presets {

using core::Json;
namespace vec = core::vector;

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::Error("value", why); }

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * std::numbers::pi;
constexpr std::size_t kGroupsMax = 200, kShapesMax = 2000;

vec::Subpath circle(double cx, double cy, double r) {
    const double k = r * 0.55228475;
    const auto knot = [](double x, double y, double ix, double iy, double ox, double oy) {
        return vec::Knot{vec::XY{x, y}, vec::XY{ix, iy}, vec::XY{ox, oy}, true};
    };
    vec::Subpath s;
    s.closed = true;
    s.knots = {knot(cx, cy - r, cx - k, cy - r, cx + k, cy - r), knot(cx + r, cy, cx + r, cy - k, cx + r, cy + k),
               knot(cx, cy + r, cx + k, cy + r, cx - k, cy + r), knot(cx - r, cy, cx - r, cy + k, cx - r, cy - k)};
    return s;
}

std::string f3(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

// A star or burst of `n` points in the 100 box (`ratio`: inner radius / outer).
std::string star(int n, double ratio) {
    std::string s;
    for (int k = 0; k < 2 * n; ++k) {
        const double r = k % 2 == 0 ? 50.0 : 50.0 * ratio;
        const double a = kPi * k / n - kPi / 2;
        s += std::string(k == 0 ? "M " : "L ") + f3(50.0 + r * std::cos(a)) + " " + f3(50.0 + r * std::sin(a)) + " ";
    }
    return s + "Z";
}

std::string sun() {
    std::string s = "O 50 50 22 ";
    for (int k = 0; k < 12; ++k) {
        const double a = kTau * k / 12.0;
        const auto p = [&](double r, double da) { return std::pair<double, double>{50.0 + r * std::cos(a + da), 50.0 + r * std::sin(a + da)}; };
        const auto [b1x, b1y] = p(28.0, -0.12);
        const auto [b2x, b2y] = p(28.0, 0.12);
        const auto [tx, ty] = p(50.0, 0.0);
        s += "M " + f3(b1x) + " " + f3(b1y) + " L " + f3(tx) + " " + f3(ty) + " L " + f3(b2x) + " " + f3(b2y) + " Z ";
    }
    return s;
}

std::string flower() {
    std::string s;
    for (int k = 0; k < 6; ++k) {
        const double a = kTau * k / 6.0 - kPi / 2;
        s += "O " + f3(50.0 + 30.0 * std::cos(a)) + " " + f3(50.0 + 30.0 * std::sin(a)) + " 18 ";
    }
    return s + "O 50 50 20 ! O 50 50 9";
}

std::string shout() {
    std::string s;
    const int n = 14;
    for (int k = 0; k < 2 * n; ++k) {
        const double r = k % 2 == 0 ? 1.0 : 0.78;
        const double a = kPi * k / n;
        s += std::string(k == 0 ? "M " : "L ") + f3(50.0 + 50.0 * r * std::cos(a)) + " " + f3(40.0 + 40.0 * r * std::sin(a)) + " ";
    }
    return s + "Z M 30 70 L 14 100 L 46 74 Z";
}

bool is_number(std::string_view t) {
    if (t.empty()) return false;
    char* end = nullptr;
    const std::string s(t);
    (void)std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

Json group_json(const Group& g) {
    Json shapes = Json::array();
    for (const Preset& p : g.shapes) shapes.push_back(Json{{"name", p.name}, {"path", vec::to_json(p.path)}});
    return Json{{"name", g.name}, {"shapes", std::move(shapes)}};
}

std::string text_of(const Json& v, std::string_view what) {
    if (!v.is_string()) bad(std::string(what) + " is text");
    std::string out = core::py_strip(v.get<std::string>());
    if (out.empty()) bad(std::string(what) + " cannot be empty");
    if (out.size() > 200) bad(std::string(what) + " is too long");
    return out;
}

std::vector<Group>::iterator group_named(std::vector<Group>& groups, std::string_view name) {
    return std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == name; });
}

bool taken(const std::vector<Group>& groups, std::string_view name) {
    for (const Group& g : groups)
        for (const Preset& p : g.shapes)
            if (p.name == name) return true;
    return false;
}

std::string unique(const std::vector<Group>& groups, std::string_view base) {
    std::string name(base);
    for (int n = 2; taken(groups, name); ++n) name = std::string(base) + " " + std::to_string(n);
    return name;
}

}  // namespace

vec::Path parse(std::string_view source) {
    std::string text(source);
    std::replace(text.begin(), text.end(), ',', ' ');
    std::vector<std::string> toks;
    std::string cur_tok;
    for (const char c : text) {
        if (c == '!') {
            if (!cur_tok.empty()) toks.push_back(std::move(cur_tok)), cur_tok.clear();
            toks.emplace_back("!");
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur_tok.empty()) toks.push_back(std::move(cur_tok)), cur_tok.clear();
        } else {
            cur_tok += c;
        }
    }
    if (!cur_tok.empty()) toks.push_back(std::move(cur_tok));
    std::size_t i = 0;
    const auto num = [&]() -> double {
        if (i >= toks.size()) bad("the shape's path ends early");
        if (!is_number(toks[i])) bad("a number is wanted at `" + toks[i] + "`");
        const double v = std::strtod(toks[i].c_str(), nullptr);
        if (!std::isfinite(v) || std::fabs(v) > vec::kCoordinateMax) bad("a number of the shape's path is too large");
        ++i;
        return v;
    };
    std::vector<vec::Subpath> subs;
    std::optional<vec::Subpath> cur;
    vec::PathOp op = vec::PathOp::Combine;
    const auto finish = [&](bool closed) {
        if (!cur) return;
        vec::Subpath s = std::move(*cur);
        cur.reset();
        s.closed = closed;
        if (closed && s.knots.size() > 1) {
            const vec::XY f = s.knots.front().anchor, l = s.knots.back().anchor;
            if (std::fabs(f.x - l.x) < 1e-9 && std::fabs(f.y - l.y) < 1e-9) {
                s.knots.front().in = s.knots.back().in;
                s.knots.pop_back();
            }
        }
        if (!s.knots.empty()) subs.push_back(std::move(s));
    };
    while (i < toks.size()) {
        const std::string t = toks[i++];
        if (t == "!") {
            op = vec::PathOp::Subtract;
        } else if (t == "M") {
            finish(true);
            const double x = num(), y = num();
            cur = vec::Subpath{true, op, {vec::Knot::corner(x, y)}};
            op = vec::PathOp::Combine;
        } else if (t == "L") {
            if (!cur) bad("L before M");
            while (i < toks.size() && is_number(toks[i])) {
                const double x = num(), y = num();
                cur->knots.push_back(vec::Knot::corner(x, y));
            }
        } else if (t == "C" || t == "Q") {
            if (!cur) bad("a curve before M");
            while (i < toks.size() && is_number(toks[i])) {
                const vec::XY last = cur->knots.back().anchor;
                vec::XY c1, c2, p;
                if (t == "C") {
                    c1 = {num(), 0};
                    c1.y = num();
                    c2 = {num(), 0};
                    c2.y = num();
                    p = {num(), 0};
                    p.y = num();
                } else {
                    vec::XY q{num(), 0};
                    q.y = num();
                    p = {num(), 0};
                    p.y = num();
                    c1 = {last.x + (q.x - last.x) * 2.0 / 3.0, last.y + (q.y - last.y) * 2.0 / 3.0};
                    c2 = {p.x + (q.x - p.x) * 2.0 / 3.0, p.y + (q.y - p.y) * 2.0 / 3.0};
                }
                cur->knots.back().out = c1;
                cur->knots.push_back(vec::Knot{p, c2, p, false});
            }
        } else if (t == "Z") {
            finish(true);
        } else if (t == "O") {
            finish(true);
            const double cx = num(), cy = num(), r = num();
            vec::Subpath c = circle(cx, cy, r);
            c.op = op;
            op = vec::PathOp::Combine;
            subs.push_back(std::move(c));
        } else {
            bad("unknown command `" + t + "` in the shape's path");
        }
        if (subs.size() > 10000) bad("the shape's path has too many subpaths");
    }
    finish(true);
    if (subs.empty()) bad("the shape's path is empty");
    vec::Path out;
    out.subpaths = std::move(subs);
    if (out.knot_count() > vec::kKnotsMax) bad("the shape's path has too many knots");
    return out;
}

std::vector<Group> builtin() {
    const auto make = [](std::string name, std::vector<std::pair<std::string, std::string>> list) {
        Group g{std::move(name), {}};
        for (auto& [n, src] : list) g.shapes.push_back(Preset{std::move(n), parse(src)});
        return g;
    };
    return {
        make("記号",
             {{"ハート", "M 50 92 C 20 70 0 52 0 30 C 0 12 14 0 28 0 C 40 0 47 8 50 16 C 53 8 60 0 72 0 C 86 0 100 12 100 30 C 100 52 80 70 50 92 Z"},
              {"星", star(5, 0.4)},
              {"六芒星", star(6, 0.55)},
              {"バースト", star(16, 0.72)},
              {"チェックマーク", "M 5 55 L 20 40 L 38 58 L 82 12 L 97 27 L 38 88 Z"},
              {"バツ", "M 20 5 L 50 35 L 80 5 L 95 20 L 65 50 L 95 80 L 80 95 L 50 65 L 20 95 L 5 80 L 35 50 L 5 20 Z"},
              {"プラス", "M 38 0 L 62 0 L 62 38 L 100 38 L 100 62 L 62 62 L 62 100 L 38 100 L 38 62 L 0 62 L 0 38 L 38 38 Z"},
              {"リング", "O 50 50 50 ! O 50 50 32"},
              {"稲妻", "M 58 0 L 18 56 L 44 56 L 34 100 L 82 38 L 55 38 L 70 0 Z"},
              {"ひし形", "M 50 0 L 90 50 L 50 100 L 10 50 Z"}}),
        make("矢印",
             {{"右矢印", "M 0 35 L 55 35 L 55 15 L 100 50 L 55 85 L 55 65 L 0 65 Z"},
              {"左矢印", "M 100 35 L 45 35 L 45 15 L 0 50 L 45 85 L 45 65 L 100 65 Z"},
              {"上矢印", "M 35 100 L 35 45 L 15 45 L 50 0 L 85 45 L 65 45 L 65 100 Z"},
              {"下矢印", "M 35 0 L 35 55 L 15 55 L 50 100 L 85 55 L 65 55 L 65 0 Z"},
              {"両矢印", "M 0 50 L 25 20 L 25 38 L 75 38 L 75 20 L 100 50 L 75 80 L 75 62 L 25 62 L 25 80 Z"},
              {"シェブロン", "M 10 0 L 45 0 L 95 50 L 45 100 L 10 100 L 60 50 Z"},
              {"曲がった矢印", "M 8 92 C 8 45 35 22 68 22 L 68 4 L 98 34 L 68 64 L 68 44 C 48 44 30 58 30 92 Z"}}),
        make("フキダシ",
             {{"フキダシ",
               "M 15 5 L 85 5 C 93 5 100 12 100 20 L 100 60 C 100 68 93 75 85 75 L 45 75 L 20 98 L 25 75 L 15 75 C 7 75 0 68 0 60 L 0 20 C 0 12 7 5 15 5 Z"},
              {"丸フキダシ", "M 50 5 C 78 5 100 22 100 42 C 100 62 78 79 50 79 C 44 79 38 78 33 77 L 12 95 L 18 72 C 7 65 0 54 0 42 C 0 22 22 5 50 5 Z"},
              {"思考フキダシ", "M 50 5 C 78 5 100 20 100 38 C 100 56 78 71 50 71 C 22 71 0 56 0 38 C 0 20 22 5 50 5 Z O 22 82 8 O 9 95 5"},
              {"叫びフキダシ", shout()}}),
        make("自然",
             {{"葉", "M 50 0 C 80 20 95 50 80 75 C 72 88 60 92 53 92 L 53 100 L 47 100 L 47 92 C 40 92 28 88 20 75 C 5 50 20 20 50 0 Z ! M 48.5 22 L 51.5 22 L 51.5 88 L 48.5 88 Z"},
              {"太陽", sun()},
              {"三日月", "O 45 50 45 ! O 66 40 38"},
              {"雲", "M 25 80 C 10 80 0 70 0 58 C 0 46 10 37 22 37 C 24 20 38 10 54 10 C 70 10 82 20 85 34 C 95 35 100 45 100 57 C 100 70 90 80 78 80 Z"},
              {"しずく", "M 50 0 C 60 25 85 45 85 68 C 85 87 69 100 50 100 C 31 100 15 87 15 68 C 15 45 40 25 50 0 Z"},
              {"花", flower()},
              {"木", "M 50 0 L 85 45 L 68 45 L 92 75 L 56 75 L 56 100 L 44 100 L 44 75 L 8 75 L 32 45 L 15 45 Z"}}),
        make("動物",
             {{"魚", "M 0 50 C 15 25 45 15 70 32 L 96 12 L 88 50 L 96 88 L 70 68 C 45 85 15 75 0 50 Z ! O 20 45 4"},
              {"猫", "M 15 10 L 35 30 C 45 27 55 27 65 30 L 85 10 L 88 50 C 92 75 72 95 50 95 C 28 95 8 75 12 50 Z ! O 36 58 6 ! O 64 58 6"},
              {"鳥", "M 0 40 C 15 30 35 32 50 50 C 65 32 85 30 100 40 C 85 40 65 48 50 70 C 35 48 15 40 0 40 Z"},
              {"蝶", "M 50 30 C 40 5 5 0 5 25 C 5 45 30 50 45 50 C 25 55 10 75 25 90 C 38 100 48 80 50 65 C 52 80 62 100 75 90 C 90 75 75 55 55 50 C 70 50 95 45 95 25 C 95 0 60 5 50 30 Z"},
              {"肉球", "M 50 50 C 68 50 82 66 82 80 C 82 92 70 96 60 94 C 55 93 52 91 50 91 C 48 91 45 93 40 94 C 30 96 18 92 18 80 C 18 66 32 50 50 50 Z O 18 42 10 O 37 22 11 O 63 22 11 O 82 42 10"},
              {"うさぎ", "O 50 72 26 M 34 52 C 22 30 24 2 34 2 C 44 2 46 30 44 50 Z M 66 52 C 78 30 76 2 66 2 C 56 2 54 30 56 50 Z"}}),
    };
}

std::vector<Group> load(const std::filesystem::path& config_dir, std::string* problem) {
    const std::filesystem::path file = config_dir / std::string(kFile);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return builtin();
    try {
        const Json data = core::parse_python_json(storage::read_file_bounded(file, 64u << 20));
        const Json* groups = core::get(data, "groups");
        if (groups == nullptr || !groups->is_array()) bad("shapes.json has no list of groups");
        std::vector<Group> out;
        for (const Json& g : *groups) {
            Group group{text_of(g.value("name", Json()), "a group's name"), {}};
            const Json* shapes = core::get(g, "shapes");
            if (shapes == nullptr || !shapes->is_array()) bad("a group has no list of shapes");
            for (const Json& s : *shapes) group.shapes.push_back(Preset{text_of(s.value("name", Json()), "a shape's name"), vec::path_from_json(s.value("path", Json()))});
            out.push_back(std::move(group));
        }
        return out;
    } catch (const std::exception& error) {
        if (problem != nullptr) *problem = error.what();
        return builtin();
    }
}

void save(const std::filesystem::path& config_dir, const std::vector<Group>& groups) {
    Json list = Json::array();
    for (const Group& g : groups) list.push_back(group_json(g));
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    storage::write_atomic(config_dir / std::string(kFile), core::dump_canonical(Json{{"version", 1}, {"groups", std::move(list)}}));
}

vec::Path fit(const vec::Path& path, const std::array<double, 4>& rect, bool keep_aspect) {
    const auto b = path.control_bounds();
    if (!b) return path;
    const double bw = std::max((*b)[2] - (*b)[0], 1e-9), bh = std::max((*b)[3] - (*b)[1], 1e-9);
    double sx = rect[2] / bw, sy = rect[3] / bh, ox = rect[0], oy = rect[1];
    if (keep_aspect) {
        const double k = std::min(sx, sy);
        ox += (rect[2] - bw * k) / 2.0;
        oy += (rect[3] - bh * k) / 2.0;
        sx = sy = k;
    }
    return path.transformed({sx, 0.0, 0.0, sy, ox - (*b)[0] * sx, oy - (*b)[1] * sy});
}

const Preset* find(const std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group) {
    for (const Group& g : groups) {
        if (group && g.name != *group) continue;
        for (const Preset& p : g.shapes)
            if (p.name == name) return &p;
    }
    return nullptr;
}

Json list(const std::vector<Group>& groups) {
    Json out = Json::array();
    for (const Group& g : groups) {
        Json shapes = Json::array();
        for (const Preset& p : g.shapes) shapes.push_back(Json{{"name", p.name}, {"subpaths", static_cast<std::int64_t>(p.path.subpaths.size())}});
        out.push_back(Json{{"name", g.name}, {"shapes", std::move(shapes)}});
    }
    return Json{{"groups", std::move(out)}};
}

Json add(std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group, const vec::Path& path) {
    if (path.empty()) bad("the path is empty");
    const std::string base = name.empty() ? std::string("シェイプ") : core::py_strip(std::string(name));
    if (base.empty() || base.size() > 200) bad("a shape's name is 1..200 bytes");
    const std::string where = group ? core::py_strip(std::string(*group)) : std::string(kCustomGroup);
    if (where.empty()) bad("a group's name cannot be empty");
    std::size_t count = 0;
    for (const Group& g : groups) count += g.shapes.size();
    if (count >= kShapesMax) bad("there are too many shapes (" + std::to_string(kShapesMax) + " at most)");
    auto it = group_named(groups, where);
    if (it == groups.end()) {
        if (groups.size() >= kGroupsMax) bad("there are too many groups");
        groups.push_back(Group{where, {}});
        it = groups.end() - 1;
    }
    const std::string made = unique(groups, base);
    it->shapes.push_back(Preset{made, fit(path, {0.0, 0.0, 100.0, 100.0}, true)});
    return Json{{"name", made}, {"group", where}};
}

Json edit(std::vector<Group>& groups, const Json& params) {
    if (!params.is_object()) bad("the edit is {action, …}");
    const std::string action = params.value("action", std::string());
    const std::optional<std::string> group = params.contains("group") && params["group"].is_string() ? std::optional(params["group"].get<std::string>()) : std::nullopt;
    const auto presets = [&] {
        std::vector<std::string> names;
        const Json* p = core::get(params, "preset");
        if (p != nullptr && p->is_string()) names.push_back(p->get<std::string>());
        else if (p != nullptr && p->is_array())
            for (const Json& v : *p) names.push_back(text_of(v, "preset"));
        if (names.empty()) bad("preset names the shape (or shapes)");
        return names;
    };
    const auto locate = [&](const std::string& name) {
        for (Group& g : groups) {
            if (group && g.name != *group) continue;
            for (std::size_t i = 0; i < g.shapes.size(); ++i)
                if (g.shapes[i].name == name) return std::pair<Group*, std::size_t>{&g, i};
        }
        bad("no shape named " + name);
    };
    if (action == "rename") {
        const auto names = presets();
        const std::string to = text_of(params.value("name", Json()), "name");
        auto [g, i] = locate(names.front());
        if (to != names.front() && taken(groups, to)) bad("a shape named " + to + " is there already");
        g->shapes[i].name = to;
        return Json{{"name", to}};
    }
    if (action == "delete") {
        std::int64_t n = 0;
        for (const std::string& name : presets()) {
            auto [g, i] = locate(name);
            g->shapes.erase(g->shapes.begin() + static_cast<std::ptrdiff_t>(i));
            ++n;
        }
        return Json{{"deleted", n}};
    }
    if (action == "move") {
        const std::string to = text_of(params.value("to", Json()), "to");
        auto target = group_named(groups, to);
        if (target == groups.end()) bad("no group named " + to);
        std::vector<Preset> moving;
        for (const std::string& name : presets()) {
            auto [g, i] = locate(name);
            moving.push_back(g->shapes[i]);
            g->shapes.erase(g->shapes.begin() + static_cast<std::ptrdiff_t>(i));
        }
        target = group_named(groups, to);
        std::size_t at = target->shapes.size();
        if (const Json* index = core::get(params, "index"); index != nullptr && index->is_number_integer())
            at = static_cast<std::size_t>(std::clamp<std::int64_t>(index->get<std::int64_t>(), 0, static_cast<std::int64_t>(at)));
        target->shapes.insert(target->shapes.begin() + static_cast<std::ptrdiff_t>(at), moving.begin(), moving.end());
        return Json{{"moved", static_cast<std::int64_t>(moving.size())}, {"group", to}};
    }
    if (action == "new_group") {
        const std::string name = text_of(params.value("name", Json()), "name");
        if (group_named(groups, name) != groups.end()) bad("a group named " + name + " is there already");
        if (groups.size() >= kGroupsMax) bad("there are too many groups");
        groups.push_back(Group{name, {}});
        return Json{{"group", name}};
    }
    if (action == "rename_group") {
        if (!group) bad("group names the group");
        const std::string name = text_of(params.value("name", Json()), "name");
        auto it = group_named(groups, *group);
        if (it == groups.end()) bad("no group named " + *group);
        if (name != *group && group_named(groups, name) != groups.end()) bad("a group named " + name + " is there already");
        it->name = name;
        return Json{{"group", name}};
    }
    if (action == "delete_group") {
        if (!group) bad("group names the group");
        auto it = group_named(groups, *group);
        if (it == groups.end()) bad("no group named " + *group);
        groups.erase(it);
        return Json{{"deleted", 1}};
    }
    bad("action is rename, delete, move, new_group, rename_group or delete_group");
}

std::vector<Group> reset(const std::vector<Group>& groups, bool append) {
    std::vector<Group> made = builtin();
    if (!append) return made;
    std::vector<Group> out = groups;
    for (Group& g : made) {
        std::string name = g.name;
        for (int n = 2; group_named(out, name) != out.end(); ++n) name = g.name + " " + std::to_string(n);
        g.name = name;
        out.push_back(std::move(g));
    }
    return out;
}

Image thumbnail(const vec::Path& path, int size) {
    size = std::clamp(size, 4, 512);
    const double m = std::max(1.0, size * 0.08);
    const vec::Path fitted = fit(path, {m, m, size - 2 * m, size - 2 * m}, true);
    const std::vector<float> cover = path_raster::path_coverage(fitted, path_raster::PixelBox{0, 0, size, size});
    std::string bytes(cover.size(), '\0');
    for (std::size_t i = 0; i < cover.size(); ++i) bytes[i] = static_cast<char>(std::lround(std::clamp(cover[i], 0.0f, 1.0f) * 255.0f));
    return Image::frombytes("L", Size{size, size}, bytes);
}

}  // namespace genko::render::shape_presets
