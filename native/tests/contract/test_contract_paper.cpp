// BRUSH-01 紙質 against the Python baseline: the inherited brushes' reference page (data/paper/expected/
// inherited-brushes.png, the brushes without a paper as this build draws them: test_paper) is Python's own drawing of
// the same lines (render.layer_image), every pixel, and so is this build's drawing of them. (The rest of BRUSH-01 is
// this build's own and needs no Python: test_paper, which Windows runs as it is.)

#include <QtTest>

#include <QTemporaryDir>

#include <string>

#include "core/model.hpp"
#include "paperlines.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"

using namespace genko;
using core::Json;

namespace {

bool same(const render::Image& a, const render::Image& b, const QString& name) {
    if (a.mode() == b.mode() && a.size() == b.size() && a.tobytes() == b.tobytes()) return true;
    if (a.mode() == b.mode() && a.size() == b.size()) {
        const auto diff = genko::test::pixel_diff(a, b);
        qWarning("%s: %lld pixels differ (largest %d)", qPrintable(name), diff.pixels, diff.largest);
    } else {
        qWarning("%s: %dx%d %s against %dx%d %s", qPrintable(name), a.width(), a.height(), std::string(a.mode()).c_str(), b.width(),
                 b.height(), std::string(b.mode()).c_str());
    }
    genko::test::keep_pictures(name, a, b);
    return false;
}

}  // namespace

class TestContractPaper : public QObject {
    Q_OBJECT

private slots:
    // The inherited brushes' reference page is the Python baseline's own drawing of the same lines (render.layer_image),
    // and this build's.
    void inheritedIsPythons() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        const std::vector<core::StrokePtr> strokes = genko::test::paper_lines::inherited();
        Json lines = Json::array();
        for (const core::StrokePtr& s : strokes) {
            Json points = Json::array();
            for (const core::PointF& p : s->points) points.push_back(Json::array({p.x, p.y}));
            lines.push_back(Json{{"id", s->id}, {"kind", s->kind}, {"width_mm", s->width_mm}, {"points", points}, {"pressure", s->pressure}});
        }
        QTemporaryDir tmp;
        genko::test::write_bytes(tmp.path() + QStringLiteral("/lines.json"), lines.dump());
        const QString script = QStringLiteral(
            "import json, sys\n"
            "from genko import models, render\n"
            "lines = json.load(open(sys.argv[1], encoding='utf-8'))\n"
            "spec = models.PageSpec.custom(100, 80, 90, 70, 2, 4, 4, 4, 4, 300, 'mono')\n"
            "book = models.new_episode('紙質', 1, 2, spec)\n"
            "page = book.pages[1]\n"
            "page.frames = []\n"
            "ink = next(l for l in page.layers if l.role == models.LayerRole.INK)\n"
            "ink.strokes = [models.Stroke(id=s['id'], points=[tuple(p) for p in s['points']], pressure=s['pressure'],\n"
            "                             width_mm=s['width_mm'], kind=s['kind']) for s in lines]\n"
            "render.layer_image(page, ink, 300, book).save(sys.argv[2])\n");
        const auto py = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), script, tmp.path() + QStringLiteral("/lines.json"),
                                                                     tmp.path() + QStringLiteral("/python.png")},
                                         genko::test::python_env(tmp.path()));
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const render::Image python = render::read_png(genko::test::read_bytes(tmp.path() + QStringLiteral("/python.png")));
        const render::Image expected =
            render::read_png(genko::test::read_bytes(genko::test::test_data(QStringLiteral("paper/expected/inherited-brushes.png"))));
        QVERIFY(same(expected, python, QStringLiteral("inherited-python")));
        // this build's drawing of the same page (as test_paper's reference book has it)
        core::Document doc = core::new_episode("紙質", core::Num(1), 2, core::PageSpec::custom(100, 80, 90, 70, 2, 4, 4, 4, 4, 300, "mono"));
        doc.edit_page(1).frames.clear();
        core::Layer* ink = nullptr;
        for (core::Layer& layer : doc.edit_page(1).layers) {
            if (layer.role == core::LayerRole::Ink) ink = &layer;
        }
        QVERIFY(ink != nullptr);
        ink->strokes = core::make_strokes(strokes);
        render::brushes::register_book(doc);
        QVERIFY(same(render::layer_image(doc.page(1), *ink, 300, &doc), python, QStringLiteral("inherited-cpp")));
    }
};

QTEST_GUILESS_MAIN(TestContractPaper)
#include "test_contract_paper.moc"
