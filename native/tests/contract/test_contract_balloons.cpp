// render::text::draw_lines — balloons (M4: balloons.cpp) — against Python's genko/balloons.py, from
// tools/migration/render_harness.py balloon-cases (Pillow held to its BASIC layout, as the reference was measured):
//   1. the shapes' geometry, bit for bit: _edge_point, _tail_polygon, _polyline_tail, _uneven, _electric, _outline,
//      _wobbly, _smooth_closed, _turned, _thought_trail and tails_of, on chosen and random boxes, tips and seeds;
//   2. groups of lines drawn by draw_lines on an RGB or RGBA picture: chosen ones reaching every shape (speech, rounded,
//      box, cloud, thought, shout, electric, flash, whisper, narration, sfx, none, picture, the three 飾り枠, an
//      unknown one), every tail kind (wedge, straight, zigzag, fade, bubbles, an unknown one; curved through a point,
//      bent at several, the old single tail), a thought's bubbles inside its panel, joined balloons (style.group,
//      Python's dict keys), turned ones (rotate_deg), picture balloons, the balloon eraser (cuts), hand-drawn
//      outlines (path, path_curve) and every style key of the balloon (border, fill, colours, double, hand, wobble,
//      spikes, bumps); and random ones at dpi 72 to 400 — every byte, or Python's own error;
//   3. the same groups drawn in parts (a render region: the image holding a box of the page): the whole picture cut
//      there, from nothing and from the remembered letters and masks.
// Skipped without the Python reference.

#include <QtTest>

#include <QTemporaryDir>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/pyconv.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace text = genko::render::text;
namespace balloon = genko::render::text::balloon;
using genko::test::bits_of;
using genko::test::from_bits;

namespace {

std::optional<genko::core::Num> num_of(const Json& j) { return genko::core::Num::from_json(j); }

// migrate._line(data), as the reader makes a line
genko::core::StoryLine line_of(const Json& d) {
    genko::core::StoryLine line;
    line.id = d.at("id").get<std::string>();
    line.page_index = genko::core::Num(d.at("page_index").get<std::int64_t>());
    line.text = d.at("text").get<std::string>();
    line.speaker = d.value("speaker", std::string());
    if (d.contains("frame_id") && d["frame_id"].is_string()) line.frame_id = d["frame_id"].get<std::string>();
    line.x_mm = genko::core::Num(genko::core::py_float(d.value("x_mm", Json(0))));
    line.y_mm = genko::core::Num(genko::core::py_float(d.value("y_mm", Json(0))));
    line.w_mm = genko::core::Num(genko::core::py_float(d.value("w_mm", Json(40))));
    line.h_mm = genko::core::Num(genko::core::py_float(d.value("h_mm", Json(20))));
    line.balloon = d.value("balloon", std::string("speech"));
    if (d.contains("tail") && genko::core::py_truthy(d["tail"])) line.tail = genko::core::Point{*num_of(d["tail"][0]), *num_of(d["tail"][1])};
    line.wrap = d.value("wrap", std::string("horizontal"));
    for (const Json& item : d.value("ruby_runs", Json::array())) line.ruby_runs.push_back(genko::core::py_list(item));
    if (d.contains("path") && genko::core::py_truthy(d["path"])) {
        std::vector<genko::core::Point> path;
        for (const Json& p : d["path"]) path.push_back(genko::core::Point{*num_of(p[0]), *num_of(p[1])});
        line.path = std::move(path);
    }
    for (const Json& item : d.value("emphasis_runs", Json::array())) line.emphasis_runs.push_back(genko::core::py_str(item));
    for (const Json& r : d.value("style_runs", Json::array())) {
        if (genko::core::py_truthy(r) && r.size() > 1) line.style_runs.emplace_back(genko::core::py_str(r[0]), genko::core::py_dict(r[1]));
    }
    line.style = d.value("style", Json::object());
    if (line.style.is_null()) line.style = Json::object();
    for (const Json& t : d.value("tails", Json::array())) line.tails.push_back(genko::core::py_dict(t));
    return line;
}

std::optional<text::Panels> panels_of(const Json& j) {
    if (j.is_null()) return std::nullopt;
    text::Panels out;
    for (const auto& [id, r] : j.items()) {
        out.emplace_back(id, std::array<double, 4>{r[0].get<double>(), r[1].get<double>(), r[2].get<double>(), r[3].get<double>()});
    }
    return out;
}

balloon::Box4 box_of(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()}; }
balloon::Pt pt_of(const Json& j) { return {j[0].get<double>(), j[1].get<double>()}; }

Json pts_json(const std::vector<balloon::Pt>& points) {
    Json out = Json::array();
    for (const auto& p : points) out.push_back(Json::array({bits_of(p[0]), bits_of(p[1])}));
    return out;
}

// The exception C++ raised, as Python names it: [type, message].
Json error_of(const std::exception& e) {
    if (const auto* p = dynamic_cast<const genko::core::PyUncaught*>(&e)) return Json::array({p->type(), p->what()});
    if (dynamic_cast<const genko::core::PyValueError*>(&e) != nullptr) return Json::array({"ValueError", e.what()});
    if (dynamic_cast<const genko::core::PyTypeError*>(&e) != nullptr) return Json::array({"TypeError", e.what()});
    if (const auto* n = dynamic_cast<const render::NotYetPorted*>(&e)) return Json::array({"NotYetPorted", n->element()});
    if (const auto* c = dynamic_cast<const genko::core::Error*>(&e); c != nullptr && c->code() == "value") {
        return Json::array({"ValueError", e.what()});  // (Pillow's ValueError, raised by render's Draw and Image)
    }
    return Json::array({"C++", e.what()});
}

bool same_picture(const render::Image& cpp, const render::Image& python, const QString& name, const std::string& id) {
    if (cpp.mode() == python.mode() && cpp.size() == python.size() && cpp.tobytes() == python.tobytes()) return true;
    if (cpp.mode() != python.mode() || cpp.size() != python.size()) {
        qWarning("%s: C++ %s %dx%d, Python %s %dx%d", id.c_str(), std::string(cpp.mode()).c_str(), cpp.width(), cpp.height(),
                 std::string(python.mode()).c_str(), python.width(), python.height());
    } else {
        const auto diff = genko::test::pixel_diff(cpp, python);
        qWarning("%s: %lld pixels differ (largest %d), first at (%lld, %lld)", id.c_str(), diff.pixels, diff.largest,
                 diff.first % std::max(1, cpp.width()), diff.first / std::max(1, cpp.width()));
    }
    genko::test::keep_pictures(name, cpp, python);
    return false;
}

struct Case {
    std::vector<genko::core::StoryLine> lines;
    std::optional<text::Panels> panels;
    std::optional<std::string> font_path;
    int dpi = 0;
    bool show_speaker = false;
    render::Image blank;  // the picture before the lines are drawn

    void draw(const text::PagePart& part) const {
        std::vector<const genko::core::StoryLine*> pointers;
        for (const auto& l : lines) pointers.push_back(&l);
        text::draw_lines(part, pointers, dpi, font_path, show_speaker, panels ? &*panels : nullptr);
    }
};

Case case_of(const Json& c) {
    Case out;
    for (const Json& d : c.at("lines")) out.lines.push_back(line_of(d));
    out.panels = panels_of(c.at("panels"));
    if (!c.at("font_path").is_null()) out.font_path = c["font_path"].get<std::string>();
    out.dpi = c.at("dpi").get<int>();
    out.show_speaker = c.at("show_speaker").get<bool>();
    out.blank = render::Image::create(c.at("mode").get<std::string>(), {c["canvas"][0].get<int>(), c["canvas"][1].get<int>()},
                                      render::Ink::tuple(c.at("background").get<std::vector<std::int64_t>>()));
    return out;
}

}  // namespace

class TestContractBalloons : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json data_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + QStringLiteral("/balloons.json");
        // (GENKO_BALLOON_SEED and GENKO_BALLOON_COUNT: other random groups, for a longer run by hand)
        const QString seed = qEnvironmentVariable("GENKO_BALLOON_SEED", QStringLiteral("20261008"));
        const QString count = qEnvironmentVariable("GENKO_BALLOON_COUNT", QStringLiteral("300"));
        const auto py = genko::test::render_harness({"balloon-cases", out, "--seed", seed, "--count", count}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        data_ = genko::test::read_json(out);
        QVERIFY(data_.at("cases").size() > 400 + count.toULongLong());  // (the chosen groups at their dpi, and the random ones)
    }

    void geometry() {
        const Json& units = data_.at("units");
        int checked = 0;
        int failures = 0;
        const auto expect = [&](const char* what, std::size_t n, const Json& cpp, const Json& python) {
            ++checked;
            std::string where;
            if (!genko::test::strict_equal(cpp, python, &where)) {
                ++failures;
                qWarning("%s %zu: C++ %s, Python %s (%s)", what, n, cpp.dump().substr(0, 300).c_str(), python.dump().substr(0, 300).c_str(),
                         where.c_str());
            }
        };
        const auto guarded = [&](const char* what, std::size_t n, const auto& make, const Json& python) {
            try {
                expect(what, n, make(), python);
            } catch (const std::exception& e) {
                ++failures;
                qWarning("%s %zu: C++ raised %s", what, n, error_of(e).dump().c_str());
            }
        };
        for (std::size_t n = 0; n < units.at("edge_point").size(); ++n) {
            const Json& c = units["edge_point"][n];
            guarded("_edge_point", n, [&] {
                const auto [a, b] = balloon::edge_point(c[0].get<std::string>(), box_of(c[1]), pt_of(c[2]), from_bits(c[3]));
                return pts_json({a, b});
            }, c[4]);
        }
        for (std::size_t n = 0; n < units.at("tail_polygon").size(); ++n) {
            const Json& c = units["tail_polygon"][n];
            guarded("_tail_polygon", n, [&] {
                const std::optional<balloon::Pt> via = c[3].is_null() ? std::nullopt : std::optional<balloon::Pt>(pt_of(c[3]));
                return pts_json(balloon::tail_polygon(c[0].get<std::string>(), box_of(c[1]), pt_of(c[2]), via, from_bits(c[4]),
                                                      c[5].get<std::string>()));
            }, c[6]);
        }
        for (std::size_t n = 0; n < units.at("polyline_tail").size(); ++n) {
            const Json& c = units["polyline_tail"][n];
            guarded("_polyline_tail", n, [&] {
                std::vector<balloon::Pt> vias;
                for (const Json& v : c[3]) vias.push_back(pt_of(v));
                return pts_json(balloon::polyline_tail(c[0].get<std::string>(), box_of(c[1]), pt_of(c[2]), vias, from_bits(c[4])));
            }, c[5]);
        }
        for (std::size_t n = 0; n < units.at("uneven").size(); ++n) {
            const Json& c = units["uneven"][n];
            guarded("_uneven", n, [&] {
                const balloon::Box4 box{from_bits(c[0][0]), from_bits(c[0][1]), from_bits(c[0][2]), from_bits(c[0][3])};
                return pts_json(balloon::uneven(box, c[1].get<std::string>()));
            }, c[2]);
        }
        for (std::size_t n = 0; n < units.at("electric").size(); ++n) {
            const Json& c = units["electric"][n];
            guarded("_electric", n, [&] { return pts_json(balloon::electric(box_of(c[0]), c[1])); }, c[2]);
        }
        for (std::size_t n = 0; n < units.at("outline").size(); ++n) {
            const Json& c = units["outline"][n];
            guarded("_outline", n, [&] {
                const auto points = balloon::outline(c[0].get<std::string>(), box_of(c[1]));
                return points ? pts_json(*points) : Json();
            }, c[2]);
        }
        for (std::size_t n = 0; n < units.at("wobbly").size(); ++n) {
            const Json& c = units["wobbly"][n];
            guarded("_wobbly", n, [&] {
                std::vector<balloon::Pt> points;
                for (const Json& p : c[0]) points.push_back({from_bits(p[0]), from_bits(p[1])});
                return pts_json(balloon::wobbly(points, from_bits(c[1]), from_bits(c[2]), c[3].get<std::string>()));
            }, c[4]);
        }
        for (std::size_t n = 0; n < units.at("smooth_closed").size(); ++n) {
            const Json& c = units["smooth_closed"][n];
            guarded("_smooth_closed", n, [&] {
                std::vector<balloon::Pt> ring;
                for (const Json& p : c[0]) ring.push_back(pt_of(p));
                return pts_json(balloon::smooth_closed(ring));
            }, c[1]);
        }
        for (std::size_t n = 0; n < units.at("turned").size(); ++n) {
            const Json& c = units["turned"][n];
            guarded("_turned", n, [&] {
                return pts_json({balloon::turned({from_bits(c[0][0]), from_bits(c[0][1])}, {from_bits(c[1][0]), from_bits(c[1][1])},
                                                 from_bits(c[2]))})[0];
            }, c[3]);
        }
        for (std::size_t n = 0; n < units.at("thought_trail").size(); ++n) {
            const Json& c = units["thought_trail"][n];
            guarded("_thought_trail", n, [&] {
                const auto panels = panels_of(c[1]);
                return pts_json({balloon::thought_trail(line_of(c[0]), panels ? &*panels : nullptr)})[0];
            }, c[2]);
        }
        for (std::size_t n = 0; n < units.at("tails_of").size(); ++n) {
            const Json& c = units["tails_of"][n];
            guarded("tails_of", n, [&] { return Json(balloon::tails_of(line_of(c[0]))); }, c[1]);
        }
        qInfo("%d results of the shapes' geometry compared bit for bit", checked);
        QCOMPARE(failures, 0);
    }

    void groups() {
        int failures = 0;
        int pictures = 0;
        int errors = 0;
        int parts = 0;
        std::map<std::string, int> shapes;
        std::map<std::string, int> tails;
        std::map<std::string, int> features;
        unsigned seed = 20261008;
        const auto next = [&](int n) {
            seed = seed * 1103515245U + 12345U;
            return n <= 1 ? 0 : static_cast<int>((seed >> 8) % static_cast<unsigned>(n));
        };
        const Json& cases = data_.at("cases");
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string id = c.at("id").get<std::string>();
            const Case k = case_of(c);
            render::Image whole = k.blank;
            try {
                if (n % 4 == 0) text::clear_balloon_cache();
                k.draw(text::whole_page(whole));
            } catch (const std::exception& e) {
                const Json error = error_of(e);
                if (!c.contains("error") || error != c["error"]) {
                    qWarning("%s: C++ raised %s, Python %s", id.c_str(), error.dump().c_str(), c.value("error", Json()).dump().c_str());
                    ++failures;
                } else {
                    ++errors;
                }
                continue;
            }
            if (!c.contains("png")) {
                qWarning("%s: Python raised %s, C++ drew", id.c_str(), c.value("error", Json()).dump().c_str());
                ++failures;
                continue;
            }
            const render::Image python = render::read_png(genko::core::a2b_base64(c["png"].get<std::string>()));
            if (!same_picture(whole, python, QStringLiteral("balloon-%1").arg(QString::fromStdString(id)), id)) {
                ++failures;
                continue;
            }
            ++pictures;
            // what the exact pictures carry
            for (const Json& d : c["lines"]) {
                const std::string kind = d.value("balloon", std::string("speech"));
                ++shapes[kind.empty() ? "(empty: speech)" : kind];
                const Json style = d.value("style", Json::object());
                for (const char* key : {"group", "rotate_deg", "cuts", "picture", "wobble", "double", "fill_opacity", "spikes", "bumps"}) {
                    if (genko::core::py_truthy(style.value(key, Json()))) ++features[key];
                }
                if (genko::core::py_truthy(d.value("path", Json()))) ++features[style.value("path_curve", false) ? "path (curved)" : "path"];
                if (genko::core::py_truthy(d.value("tail", Json()))) ++tails["old single tail"];
                for (const Json& t : d.value("tails", Json::array())) {
                    if (!t.is_object() || !genko::core::py_truthy(t.value("to", Json()))) continue;
                    const Json kind_value = t.value("kind", Json());
                    ++tails[genko::core::py_truthy(kind_value) ? kind_value.get<std::string>() : std::string("(wedge)")];
                    if (genko::core::py_truthy(t.value("vias", Json()))) ++tails["bent (vias)"];
                    else if (genko::core::py_truthy(t.value("via", Json()))) ++tails["curved (via)"];
                    if (genko::core::py_truthy(t.value("width_mm", Json()))) ++tails["width_mm"];
                }
                if (kind == "thought" && d.value("tails", Json::array()).empty() && !genko::core::py_truthy(d.value("tail", Json()))) {
                    ++tails["thought's own trail"];
                }
            }
            // the same drawn in two parts of the picture (a render region)
            for (int p = 0; p < 2; ++p) {
                const int w = 1 + next(whole.width());
                const int h = 1 + next(whole.height());
                const render::Box box{next(whole.width() - w + 1), next(whole.height() - h + 1), 0, 0};
                const render::Box cut{box.x0, box.y0, box.x0 + w, box.y0 + h};
                render::Image part = k.blank.crop(cut);
                try {
                    if (p == 0 && n % 3 == 0) text::clear_balloon_cache();
                    k.draw(text::PagePart{&part, render::Point{cut.x0, cut.y0}, whole.size()});
                } catch (const std::exception& e) {
                    qWarning("%s part (%d, %d, %d, %d): C++ raised %s", id.c_str(), cut.x0, cut.y0, w, h, error_of(e).dump().c_str());
                    ++failures;
                    continue;
                }
                ++parts;
                if (!same_picture(part, whole.crop(cut), QStringLiteral("balloon-%1-part%2").arg(QString::fromStdString(id)).arg(p),
                                  id + " part")) {
                    ++failures;
                }
            }
        }
        for (const auto& [kind, count] : shapes) qInfo("  shape %s: %d lines", kind.c_str(), count);
        for (const auto& [kind, count] : tails) qInfo("  tail %s: %d", kind.c_str(), count);
        for (const auto& [key, count] : features) qInfo("  %s: %d lines", key.c_str(), count);
        qInfo("%zu groups: %d pictures byte for byte, %d of Python's errors raised alike, %d parts the same as the whole cut; "
              "%d differ",
              cases.size(), pictures, errors, parts, failures);
        QVERIFY(pictures > 400);
        QCOMPARE(failures, 0);
    }

    void cleanupTestCase() { text::clear_balloon_cache(); }
};

QTEST_GUILESS_MAIN(TestContractBalloons)
#include "test_contract_balloons.moc"
