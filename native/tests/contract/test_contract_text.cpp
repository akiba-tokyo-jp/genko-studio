// render::text — the letters of a line (M4: tategaki.cpp, lettering.cpp, fonts.cpp) — against Python's
// genko/tategaki.py, the text side of genko/balloons.py and genko/fonts.py, from tools/migration/render_harness.py
// text-cases (Pillow held to its BASIC layout, as the reference was measured):
//   1. the pure functions: cells (縦中横, Latin laid on its side, variation selectors), columns_of (kinsoku), phrases,
//      phrase_columns, without_periods, mark_tcy, mono_runs, char_styles, the ruby spans and 傍点 cells,
//      weight_level, bold_px, Face.normalize, Face.font and has_glyph, on chosen and random texts;
//   2. each cell's picture: glyph (vertical forms, small kana, punctuation, turned marks), tcy_glyph, latin_glyph and
//      draw_mark, for every bundled face;
//   3. the lines: chosen ones reaching every style key that changes the letters and every balloon kind, across and
//      down, at a few dpi, and random ones — text_layout's picture, em and corner, and what _paint_text paints on a
//      white page (the letters, text on a path, the speaker's name), or Python's own error.
// Every byte, offset and number must be the same — except the pictures of a line whose letters are warped
// (style.warp): Python takes that map from numpy's SVD, so those are compared within ARCHITECTURE.md §9 (see lines()).
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
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/tategaki.hpp"
#include "opsupport.hpp"
#include "rendertest.hpp"

using genko::core::Json;
namespace render = genko::render;
namespace text = genko::render::text;
using genko::test::from_bits;

namespace {

std::u32string u(const Json& j) { return text::u32(j.get<std::string>()); }

Json texts_json(const std::vector<std::u32string>& items) {
    Json out = Json::array();
    for (const auto& s : items) out.push_back(text::utf8(s));
    return out;
}

Json columns_json(const std::vector<text::Column>& cols) {
    Json out = Json::array();
    for (const auto& col : cols) out.push_back(texts_json(col));
    return out;
}

text::StyleRuns style_runs_of(const Json& runs) {
    text::StyleRuns out;
    for (const Json& r : runs) {
        if (!genko::core::py_truthy(r) || r.size() <= 1) continue;
        out.emplace_back(genko::core::py_str(r[0]), genko::core::py_dict(r[1]));
    }
    return out;
}

// migrate._line(data), as the reader makes a line
genko::core::StoryLine line_of(const Json& d) {
    genko::core::StoryLine line;
    line.id = d.at("id").get<std::string>();
    line.page_index = genko::core::Num(d.at("page_index").get<std::int64_t>());
    line.text = d.at("text").get<std::string>();
    line.speaker = d.value("speaker", std::string());
    line.x_mm = genko::core::Num(genko::core::py_float(d.value("x_mm", Json(0))));
    line.y_mm = genko::core::Num(genko::core::py_float(d.value("y_mm", Json(0))));
    line.w_mm = genko::core::Num(genko::core::py_float(d.value("w_mm", Json(40))));
    line.h_mm = genko::core::Num(genko::core::py_float(d.value("h_mm", Json(20))));
    line.balloon = d.value("balloon", std::string("speech"));
    line.wrap = d.value("wrap", std::string("horizontal"));
    for (const Json& item : d.value("ruby_runs", Json::array())) line.ruby_runs.push_back(genko::core::py_list(item));
    for (const Json& item : d.value("emphasis_runs", Json::array())) line.emphasis_runs.push_back(genko::core::py_str(item));
    line.style_runs = style_runs_of(d.value("style_runs", Json::array()));
    line.style = d.value("style", Json::object());
    return line;
}

render::Image picture(const Json& png) {
    return render::read_png(genko::core::a2b_base64(png.get<std::string>()));
}

// The font file each bundled name opens (Face.font answers with a FreeTypeFont's path).
std::string file_of(const std::string& name) {
    static const std::map<std::string, std::string> files{{"gothic", "ZenKakuGothicNew-Bold.ttf"}, {"mincho", "ZenOldMincho-SemiBold.ttf"},
                                                          {"maru", "ZenMaruGothic-Bold.ttf"},      {"hand", "Yomogi-Regular.ttf"},
                                                          {"sfx", "DelaGothicOne-Regular.ttf"},    {"sfx_pop", "ReggaeOne-Regular.ttf"}};
    const auto it = files.find(name);
    return it == files.end() ? name : it->second;
}

// The exception C++ raised, as Python names it: [type, message].
Json error_of(const std::exception& e) {
    if (const auto* p = dynamic_cast<const genko::core::PyUncaught*>(&e)) return Json::array({p->type(), p->what()});
    if (dynamic_cast<const genko::core::PyValueError*>(&e) != nullptr) return Json::array({"ValueError", e.what()});
    if (dynamic_cast<const genko::core::PyTypeError*>(&e) != nullptr) return Json::array({"TypeError", e.what()});
    if (const auto* n = dynamic_cast<const render::NotYetPorted*>(&e)) return Json::array({"NotYetPorted", n->element()});
    return Json::array({"C++", e.what()});
}

bool same_picture(const render::Image& cpp, const render::Image& python, const QString& name, const std::string& id) {
    if (cpp.mode() == python.mode() && cpp.size() == python.size() && cpp.tobytes() == python.tobytes()) return true;
    if (cpp.mode() != python.mode() || cpp.size() != python.size()) {
        qWarning("%s: C++ %s %dx%d, Python %s %dx%d", id.c_str(), std::string(cpp.mode()).c_str(), cpp.width(), cpp.height(),
                 std::string(python.mode()).c_str(), python.width(), python.height());
    } else {
        const auto diff = genko::test::pixel_diff(cpp, python);
        qWarning("%s: %lld pixels differ (largest %d), first at %lld", id.c_str(), diff.pixels, diff.largest, diff.first);
    }
    genko::test::keep_pictures(name, cpp, python);
    return false;
}

}  // namespace

class TestContractText : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json data_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const QString out = scratch_.path() + QStringLiteral("/text.json");
        // (GENKO_TEXT_SEED and GENKO_TEXT_COUNT: other random lines, for a longer run by hand)
        const QString seed = qEnvironmentVariable("GENKO_TEXT_SEED", QStringLiteral("20261008"));
        const QString count = qEnvironmentVariable("GENKO_TEXT_COUNT", QStringLiteral("400"));
        const auto py = genko::test::render_harness({"text-cases", out, "--seed", seed, "--count", count}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        data_ = genko::test::read_json(out);
        QVERIFY(data_.at("cases").size() > 400 + count.toULongLong());  // (the chosen lines and the random ones)
        QVERIFY(data_.at("glyphs").size() > 300);
    }

    void units() {
        const Json& units = data_.at("units");
        int checked = 0;
        int failures = 0;
        const auto expect = [&](const char* what, std::size_t n, const Json& cpp, const Json& python) {
            ++checked;
            std::string where;
            if (!genko::test::strict_equal(cpp, python, &where)) {
                ++failures;
                qWarning("%s %zu: C++ %s, Python %s (%s)", what, n, cpp.dump().substr(0, 400).c_str(), python.dump().substr(0, 400).c_str(),
                         where.c_str());
            }
        };
        for (std::size_t n = 0; n < units.at("cells").size(); ++n) {
            const Json& c = units["cells"][n];
            expect("cells", n, texts_json(text::cells(u(c[0]), c[1].get<bool>(), c[2].get<bool>())), c[3]);
        }
        for (std::size_t n = 0; n < units.at("columns").size(); ++n) {
            const Json& c = units["columns"][n];
            expect("columns_of", n, columns_json(text::columns_of(u(c[0]), c[1].get<int>(), c[2].get<bool>(), c[3].get<bool>())), c[4]);
        }
        for (std::size_t n = 0; n < units.at("phrases").size(); ++n) {
            const Json& c = units["phrases"][n];
            expect("phrases", n, texts_json(text::phrases(u(c[0]))), c[1]);
        }
        for (std::size_t n = 0; n < units.at("phrase_columns").size(); ++n) {
            const Json& c = units["phrase_columns"][n];
            const bool tcy = c[2].get<bool>();
            const bool latin = c[3].get<bool>();
            const auto cols = text::phrase_columns(u(c[0]), c[1].get<int>(), [&](std::u32string_view part) {
                return static_cast<std::int64_t>(text::cells(part, tcy, latin).size());
            });
            expect("phrase_columns", n, texts_json(cols), c[4]);
        }
        for (std::size_t n = 0; n < units.at("without_periods").size(); ++n) {
            const Json& c = units["without_periods"][n];
            expect("without_periods", n, Json(text::utf8(text::without_periods(u(c[0])))), c[1]);
        }
        for (std::size_t n = 0; n < units.at("mark_tcy").size(); ++n) {
            const Json& c = units["mark_tcy"][n];
            expect("mark_tcy", n, Json(text::utf8(text::mark_tcy(u(c[0]), style_runs_of(c[1])))), c[2]);
        }
        for (std::size_t n = 0; n < units.at("char_styles").size(); ++n) {
            const Json& c = units["char_styles"][n];
            const auto styles = text::char_styles(u(c[0]), style_runs_of(c[1]), c[2]);
            expect("char_styles", n, Json(styles), c[3]);
        }
        for (std::size_t n = 0; n < units.at("mono_runs").size(); ++n) {
            const Json& c = units["mono_runs"][n];
            expect("mono_runs", n, Json(text::mono_runs(c[0].get<std::vector<Json>>())), c[1]);
        }
        for (std::size_t n = 0; n < units.at("ruby_spans").size(); ++n) {
            const Json& c = units["ruby_spans"][n];
            const auto cols = text::columns_of(u(c[0]), c[1].get<int>(), c[2].get<bool>(), c[3].get<bool>());
            Json spans = Json::array();
            for (const auto& s : text::ruby_spans(cols, c[4].get<std::vector<Json>>())) {
                spans.push_back(Json::array({s.column, s.first, s.last, text::utf8(s.ruby)}));
            }
            expect("ruby_spans", n, spans, c[5]);
        }
        for (std::size_t n = 0; n < units.at("emphasis_cells").size(); ++n) {
            const Json& c = units["emphasis_cells"][n];
            const auto cols = text::columns_of(u(c[0]), c[1].get<int>(), c[2].get<bool>(), c[3].get<bool>());
            std::vector<std::u32string> marks;
            for (const Json& m : c[4]) marks.push_back(u(m));
            Json cells = Json::array();
            for (const auto& [col, row] : text::emphasis_cells(cols, marks)) cells.push_back(Json::array({col, row}));
            expect("emphasis_cells", n, cells, c[5]);
        }
        for (std::size_t n = 0; n < units.at("weight_level").size(); ++n) {
            const Json& c = units["weight_level"][n];
            expect("weight_level", n, Json(text::weight_level(c[0])), c[1]);
        }
        for (std::size_t n = 0; n < units.at("bold_px").size(); ++n) {
            const Json& c = units["bold_px"][n];
            expect("bold_px", n, Json(text::bold_px(c[0].get<std::int64_t>(), c[1])), c[2]);
        }
        text::Fonts fonts;
        for (std::size_t n = 0; n < units.at("normalize").size(); ++n) {
            const Json& c = units["normalize"][n];
            expect("normalize", n, Json(text::utf8(fonts.normalize(text::face_of(c[0]), u(c[1])))), c[2]);
        }
        for (std::size_t n = 0; n < units.at("face_font").size(); ++n) {
            const Json& c = units["face_font"][n];
            const text::Face face = text::face_of(c[0]);
            // which font Face.font chooses: the one whose glyphs it draws with
            std::string chosen;
            const auto& font = fonts.font(face, 20, u(c[1]));
            for (const std::string name : {"gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"}) {
                if (&fonts.truetype(name, 20) == &font) chosen = file_of(name);
            }
            expect("Face.font", n, Json(chosen), c[2]);
        }
        for (std::size_t n = 0; n < units.at("has_glyph").size(); ++n) {
            const Json& c = units["has_glyph"][n];
            expect("has_glyph", n, Json(fonts.has_glyph(c[0].get<std::string>(), u(c[1]))), c[2]);
        }
        qInfo("%d results of the pure functions compared", checked);
        QCOMPARE(failures, 0);
    }

    void glyphs() {
        text::Fonts fonts;
        int failures = 0;
        const Json& glyphs = data_.at("glyphs");
        for (std::size_t n = 0; n < glyphs.size(); ++n) {
            const Json& g = glyphs[n];
            const std::string kind = g.at("kind").get<std::string>();
            render::Image image;
            if (kind == "mark") {
                image = render::Image::create("RGBA", {40, 40}, render::Ink{0, 0, 0, 0});
                text::draw_mark(image, from_bits(g["centre"][0]), from_bits(g["centre"][1]), from_bits(g["size"]),
                                g["mark"].get<std::string>(), {30, 80, 200}, g["vertical"].get<bool>());
            } else {
                const text::Face face = text::face_of(g["font"]);
                const std::u32string cell = u(g["text"]);
                const auto em = g["em"].get<std::int64_t>();
                const text::Rgb fill = g["fill"].get<std::vector<std::int64_t>>();
                const int bold = g["bold"].get<int>();
                if (kind == "glyph") image = text::glyph(cell, fonts.font(face, em, cell), em, fill, bold);
                if (kind == "tcy") image = text::tcy_glyph(cell, fonts.font(face, em, U"0"), em, fill, bold);
                if (kind == "latin") image = text::latin_glyph(cell, fonts.font(face, em, U"A"), em, fill, bold);
            }
            const std::string id = "glyph " + std::to_string(n) + " " + kind + " " + g.value("font", std::string()) + " " +
                                   g.value("text", std::string());
            if (!same_picture(image, picture(g["png"]), QStringLiteral("text-glyph-%1").arg(n), id)) ++failures;
        }
        qInfo("%zu cell pictures compared", glyphs.size());
        QCOMPARE(failures, 0);
    }

    void lines() {
        int failures = 0;
        int layouts = 0;
        int painted = 0;
        int errors = 0;
        int warps_exact = 0;
        int warps_near = 0;
        const Json& cases = data_.at("cases");
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string id = c.at("id").get<std::string>();
            const genko::core::StoryLine line = line_of(c.at("line"));
            const int dpi = c.at("dpi").get<int>();
            const std::optional<std::string> font_path =
                c.at("font_path").is_null() ? std::nullopt : std::optional<std::string>(c["font_path"].get<std::string>());
            // A warped line (style.warp, unless the text runs along a path) is the one place the same bytes cannot be
            // asked for. Python's warped_letters takes its homography from numpy.linalg.svd (LAPACK through OpenBLAS):
            // its coefficients carry noise of about 1e-13 (0.6000000000000084 for 0.6, 1e-16 for 0). This build solves
            // the same eight equations by Gaussian elimination, and the perspective transform's bicubic samples then
            // land on the other side of a rounding in a few pixels, by one or two levels. So for these lines only, the
            // pictures are compared within ARCHITECTURE.md §9 (genko::test::compare_picture_near: the same mode and
            // size, the mean difference at most 2/255, 99% of the pixels within 32/255) — the em and the corner exactly
            // — and the count of byte-identical and near-equal ones is reported. Every other line is compared exactly.
            const bool warped = !c.contains("error") && genko::core::py_truthy(line.style.value("warp", Json())) &&
                                !genko::core::py_truthy(line.style.value("text_path", Json()));
            if (warped) {
                try {
                    const Json& want = c.at("layout");
                    const text::Layout layout = text::text_layout(line, dpi, font_path);
                    const QString name = QStringLiteral("text-%1").arg(QString::fromStdString(id));
                    bool exact = true;
                    bool near = true;
                    const auto compare = [&](const render::Image& cpp, const Json& png, const QString& what) {
                        const render::Image python = picture(png);
                        if (cpp.mode() == python.mode() && cpp.size() == python.size() && cpp.tobytes() == python.tobytes()) return;
                        exact = false;
                        const QString base = genko::test::artifact_dir() + QLatin1Char('/') + name + QLatin1Char('-') + what;
                        render::save_png(cpp, (base + QStringLiteral("-cpp.png")).toStdString());
                        genko::test::write_bytes(base + QStringLiteral("-python.png"), genko::core::a2b_base64(png.get<std::string>()));
                        const std::string why = genko::test::compare_picture_near(base + QStringLiteral("-cpp.png"), base + QStringLiteral("-python.png"));
                        if (!why.empty()) {
                            const auto diff = genko::test::pixel_diff(cpp, python);
                            qWarning("%s (%s, warped): beyond ARCHITECTURE.md §9: %s (%lld pixels differ, largest %d)", id.c_str(),
                                     what.toUtf8().constData(), why.c_str(), diff.pixels, diff.largest);
                            near = false;
                        }
                    };
                    compare(layout.image, want["png"], QStringLiteral("layout"));
                    if (layout.em != want["em"].get<std::int64_t>() || layout.corner_x != from_bits(want["corner"][0]) ||
                        layout.corner_y != from_bits(want["corner"][1])) {
                        qWarning("%s (warped): em %lld corner (%.17g, %.17g), Python em %lld corner (%.17g, %.17g)", id.c_str(),
                                 static_cast<long long>(layout.em), layout.corner_x, layout.corner_y,
                                 static_cast<long long>(want["em"].get<std::int64_t>()), from_bits(want["corner"][0]),
                                 from_bits(want["corner"][1]));
                        near = false;
                    }
                    render::Image page = render::Image::create("RGB", {c["canvas"][0].get<int>(), c["canvas"][1].get<int>()},
                                                               render::Ink{255, 255, 255});
                    text::paint_text(page, line, dpi, c.at("show_speaker").get<bool>(), font_path);
                    compare(page, c.at("painted"), QStringLiteral("painted"));
                    if (!near) {
                        ++failures;
                    } else if (exact) {
                        ++warps_exact;
                    } else {
                        ++warps_near;
                    }
                } catch (const std::exception& e) {
                    qWarning("%s (warped): C++ raised %s", id.c_str(), error_of(e).dump().c_str());
                    ++failures;
                }
                continue;
            }
            Json error;
            try {
                if (c.contains("layout")) {
                    const Json& want = c["layout"];
                    const text::Layout layout = text::text_layout(line, dpi, font_path);
                    bool same = same_picture(layout.image, picture(want["png"]), QStringLiteral("text-%1-layout").arg(QString::fromStdString(id)), id);
                    if (layout.em != want["em"].get<std::int64_t>() || layout.corner_x != from_bits(want["corner"][0]) ||
                        layout.corner_y != from_bits(want["corner"][1])) {
                        qWarning("%s: em %lld corner (%.17g, %.17g), Python em %lld corner (%.17g, %.17g)", id.c_str(),
                                 static_cast<long long>(layout.em), layout.corner_x, layout.corner_y,
                                 static_cast<long long>(want["em"].get<std::int64_t>()), from_bits(want["corner"][0]),
                                 from_bits(want["corner"][1]));
                        same = false;
                    }
                    if (!same) ++failures;
                    ++layouts;
                }
                render::Image page = render::Image::create("RGB", {c["canvas"][0].get<int>(), c["canvas"][1].get<int>()},
                                                           render::Ink{255, 255, 255});
                text::paint_text(page, line, dpi, c.at("show_speaker").get<bool>(), font_path);
                if (!c.contains("painted")) {
                    qWarning("%s: Python raised %s, C++ painted", id.c_str(), c.value("error", Json()).dump().c_str());
                    ++failures;
                } else {
                    if (!same_picture(page, picture(c["painted"]), QStringLiteral("text-%1-painted").arg(QString::fromStdString(id)), id)) {
                        ++failures;
                    }
                    ++painted;
                }
            } catch (const std::exception& e) {
                error = error_of(e);
                if (!c.contains("error") || error != c["error"]) {
                    qWarning("%s: C++ raised %s, Python %s", id.c_str(), error.dump().c_str(), c.value("error", Json()).dump().c_str());
                    ++failures;
                } else {
                    ++errors;
                }
            }
        }
        qInfo("%zu lines: %d layouts and %d paintings compared byte for byte, %d of Python's errors raised alike; %d warped "
              "lines: %d byte for byte, %d within ARCHITECTURE.md §9 (numpy's SVD); %d differ",
              cases.size(), layouts, painted, errors, warps_exact + warps_near, warps_exact, warps_near, failures);
        QCOMPARE(failures, 0);
    }
};

QTEST_GUILESS_MAIN(TestContractText)
#include "test_contract_text.moc"
