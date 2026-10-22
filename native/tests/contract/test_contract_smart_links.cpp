// smart-objects@1, PC3 A1 (SPEC.md COLOR-05「中身とリンク」) through the command line, as a person or an agent uses it: a
// file placed linked, `genko smart --links` telling it ok, modified (the file changed) and missing (gone: the page still
// drawn from the book's copy), found again from the book's folder by its relative place, updated, relinked, embedded; the
// contents written out with `genko smart --export` (a file's own bytes; a page's as a PSD psd-tools reads — its layers,
// no paper — or a PNG); `genko package` (a new book with the linked files' copies in Links/, linked there, drawn the same;
// refused where a book is). (This build's own contract.)

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

std::string one_colour(int w, int h, int r, int g, int b) {
    return render::write_png(render::Image::create("RGB", render::Size{w, h}, render::Ink{r, g, b}));
}

// psd-tools' view of a PSD: its size and its layers' names and kinds.
constexpr const char* kPsdTools = R"PY(
import json, sys
from psd_tools import PSDImage
psd = PSDImage.open(sys.argv[2])
json.dump({"size": [psd.width, psd.height], "layers": [{"name": l.name, "kind": l.kind} for l in psd.descendants()]}, open(sys.argv[1], "w"))
)PY";

}  // namespace

class TestContractSmartLinks : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QString make_book(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--webtoon"), QStringLiteral("--pages"), QStringLiteral("1")});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        return book;
    }
    Json apply(const QString& book, const Json& ops) {
        const QString file = path(QStringLiteral("ops-%1.json").arg(applied_++));
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    Json links(const QString& book) {
        int exit = -1;
        const Json out = genko({QStringLiteral("smart"), book, QStringLiteral("--links")}, &exit);
        if (exit != 0) qWarning("%s", out.dump().c_str());
        return out.value("links", Json::array());
    }
    Json centre(const QString& book) {
        const Json saved = project(book);
        const double x = saved["spec"]["width_mm"].get<double>() / 2, y = saved["spec"]["height_mm"].get<double>() / 2;
        return genko({QStringLiteral("inspect"), book, QStringLiteral("--pixel"), QStringLiteral("1:%1,%2").arg(x).arg(y), QStringLiteral("--dpi"),
                      QStringLiteral("40")})["srgb"];
    }
    int applied_ = 0;

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void aLinkedFileFollowed() {
        const QString book = make_book(QStringLiteral("linked.genko"));
        const QString file = path(QStringLiteral("photo.png"));
        const std::string first = one_colour(120, 80, 200, 40, 90);
        genko::test::write_bytes(file, first);
        // placed linked (the window reads the file; an agent gives its bytes and its place)
        const Json placed = apply(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(first)}, {"name", "photo.png"},
                                                          {"new_id", "so"}, {"scale", 1000},
                                                          {"link", Json{{"path", file.toStdString()}, {"relative", "../photo.png"}}}}}));
        QVERIFY2(placed.value("ok", false), placed.dump().c_str());
        Json listed = links(book);
        QCOMPARE(listed.size(), std::size_t(1));
        QCOMPARE(listed[0]["status"], Json("ok"));
        QCOMPARE(listed[0]["path"], Json(file.toStdString()));
        QCOMPARE(listed[0]["relative"], Json("../photo.png"));
        QCOMPARE(listed[0]["shown"], (Json{{"page", 1}, {"id", "so"}}));
        const std::string id = listed[0]["contents"].get<std::string>();
        const Json link = project(book)["smart_contents"][id]["link"];
        QCOMPARE(link["sha256"], Json(genko::core::bytes_ref(first).substr(7)));
        QCOMPARE(link["bytes"], Json(static_cast<std::int64_t>(first.size())));
        QCOMPARE(centre(book), Json::array({200, 40, 90}));

        // the file changed: modified; updated: ok, drawn as the file now is
        const std::string second = one_colour(120, 80, 20, 140, 220);
        genko::test::write_bytes(file, second);
        QCOMPARE(links(book)[0]["status"], Json("modified"));
        QCOMPARE(centre(book), Json::array({200, 40, 90}));  // (the book's copy until it is updated)
        const Json updated = apply(book, Json::array({Json{{"op", "smart_update"}, {"page", 1}, {"id", "so"}, {"data", genko::core::b64encode(second)}}}));
        QVERIFY2(updated.value("ok", false), updated.dump().c_str());
        QCOMPARE(updated["results"][0]["updated"], Json(true));
        QCOMPARE(links(book)[0]["status"], Json("ok"));
        QCOMPARE(centre(book), Json::array({20, 140, 220}));

        // the file gone: missing, the page still drawn from the copy; the book whole (doctor)
        QVERIFY(QFile::remove(file));
        QCOMPARE(links(book)[0]["status"], Json("missing"));
        QCOMPARE(centre(book), Json::array({20, 140, 220}));
        int exit = -1;
        const Json doctor = genko({QStringLiteral("doctor"), book}, &exit);
        QVERIFY2(exit == 0, doctor.dump().c_str());

        // found again from the book's folder: its path gone, its relative place there
        QVERIFY(QDir().mkpath(path(QStringLiteral("moved"))));
        genko::test::write_bytes(path(QStringLiteral("moved/photo.png")), second);
        const Json relinked = apply(book, Json::array({Json{{"op", "smart_relink"}, {"page", 1}, {"id", "so"}, {"data", genko::core::b64encode(second)},
                                                            {"link", Json{{"path", "/nowhere/photo.png"}, {"relative", "../moved/photo.png"}}}}}));
        QVERIFY2(relinked.value("ok", false), relinked.dump().c_str());
        listed = links(book);
        QCOMPARE(listed[0]["status"], Json("ok"));
        QVERIFY2(QString::fromStdString(listed[0]["found"].get<std::string>()).endsWith(QStringLiteral("moved/photo.png")), listed.dump().c_str());

        // embedded: no link left
        QVERIFY(apply(book, Json::array({Json{{"op", "smart_embed"}, {"page", 1}, {"id", "so"}}})).value("ok", false));
        QCOMPARE(links(book).size(), std::size_t(0));
        QVERIFY(!project(book)["smart_contents"][id].contains("link"));
        QCOMPARE(centre(book), Json::array({20, 140, 220}));
        // undone: linked again, as it was
        QCOMPARE(genko::test::run_genko({QStringLiteral("undo"), book, QStringLiteral("--as"), QStringLiteral("human:test")}).exit_code, 0);
        QCOMPARE(links(book).size(), std::size_t(1));
    }

    void contentsWrittenOut() {
        const QString book = make_book(QStringLiteral("export.genko"));
        const std::string file = one_colour(60, 40, 10, 200, 30);
        render::Image picture = render::Image::create("RGBA", render::Size{80, 60}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{200, 30, 40, 255}, render::Box{10, 10, 50, 40});
        const Json made = apply(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(file)}, {"name", "leaf.png"}, {"new_id", "pic"}},
                                                   Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}, {"name", "赤い絵"}},
                                                   Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                                                   Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", genko::core::b64encode(render::write_png(picture))}},
                                                   Json{{"op", "smart_convert"}, {"page", 1}, {"id", "paint"}, {"new_id", "wrap"}}}));
        QVERIFY2(made.value("ok", false), made.dump().c_str());
        // a file's: its own bytes
        int exit = -1;
        Json out = genko({QStringLiteral("smart"), book, QStringLiteral("--export"), QStringLiteral("1"), QStringLiteral("pic"), path(QStringLiteral("leaf.png"))}, &exit);
        QVERIFY2(exit == 0 && out.value("ok", false), out.dump().c_str());
        QCOMPARE(genko::test::read_bytes(path(QStringLiteral("leaf.png"))), file);
        QCOMPARE(out["kind"], Json("file"));
        // a page's: a PSD of its layers (psd-tools reads it: the paint layer, no paper or borders), its page's size at its dpi
        out = genko({QStringLiteral("smart"), book, QStringLiteral("--export"), QStringLiteral("1"), QStringLiteral("wrap"), path(QStringLiteral("wrap.psd"))}, &exit);
        QVERIFY2(exit == 0 && out.value("ok", false), out.dump().c_str());
        QCOMPARE(out["format"], Json("psd"));
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {QStringLiteral("-c"), QString::fromUtf8(kPsdTools), path(QStringLiteral("psd.json")), path(QStringLiteral("wrap.psd"))},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, ran.err.constData());
        const Json view = genko::test::read_json(path(QStringLiteral("psd.json")));
        const Json saved = project(book);
        const double dpi = saved["spec"]["dpi"].get<double>();
        const int w = static_cast<int>(std::lround(saved["spec"]["width_mm"].get<double>() * dpi / 25.4));
        QCOMPARE(view["size"][0].get<int>(), w);
        std::vector<std::string> names;
        for (const Json& layer : view["layers"]) names.push_back(layer["name"].get<std::string>());
        QVERIFY2(std::find(names.begin(), names.end(), "赤い絵") != names.end(), view.dump().c_str());
        QVERIFY2(std::find(names.begin(), names.end(), "紙") == names.end() && std::find(names.begin(), names.end(), "コマ枠") == names.end(), view.dump().c_str());
        // ... or a PNG of its layers over nothing
        out = genko({QStringLiteral("smart"), book, QStringLiteral("--export"), QStringLiteral("1"), QStringLiteral("wrap"), path(QStringLiteral("wrap.png")),
                     QStringLiteral("--format"), QStringLiteral("png")},
                    &exit);
        QVERIFY2(exit == 0, out.dump().c_str());
        const render::Image png = render::open_image(genko::test::read_bytes(path(QStringLiteral("wrap.png"))), render::kPillowOpenLimits);
        QCOMPARE(png.mode(), std::string_view("RGBA"));
        QCOMPARE(png.width(), w);
        QCOMPARE(png.getpixel(0, 0)[3], 0.0);  // (over nothing)
        // refused: not a smart object, a file written as another format
        (void)genko({QStringLiteral("smart"), book, QStringLiteral("--export"), QStringLiteral("1"),
                     QString::fromStdString(saved["pages"][0]["layers"][0]["id"].get<std::string>()), path(QStringLiteral("x.png"))},
                    &exit);
        QCOMPARE(exit, 1);
        out = genko({QStringLiteral("smart"), book, QStringLiteral("--export"), QStringLiteral("1"), QStringLiteral("pic"), path(QStringLiteral("x.psd")),
                     QStringLiteral("--format"), QStringLiteral("psd")},
                    &exit);
        QCOMPARE(exit, 1);
        QVERIFY2(out.value("error", std::string()).find("written as they are") != std::string::npos, out.dump().c_str());
    }

    void packedWithItsLinks() {
        const QString book = make_book(QStringLiteral("pack.genko"));
        QVERIFY(QDir().mkpath(path(QStringLiteral("files"))));
        const QString a = path(QStringLiteral("files/a.png")), b = path(QStringLiteral("files/b.png"));
        const std::string first = one_colour(30, 20, 250, 10, 10), second = one_colour(30, 20, 10, 250, 10);
        genko::test::write_bytes(a, first);
        genko::test::write_bytes(b, second);
        const Json placed = apply(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(first)}, {"name", "a.png"}, {"new_id", "a"},
                                                          {"scale", 1000}, {"link", Json{{"path", a.toStdString()}}}},
                                                     Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(second)}, {"name", "b.png"}, {"new_id", "b"},
                                                          {"link", Json{{"path", b.toStdString()}}}}}));
        QVERIFY2(placed.value("ok", false), placed.dump().c_str());
        QFile::remove(b);  // (a file gone: its copy is packed all the same)
        int exit = -1;
        const Json packed = genko({QStringLiteral("package"), book, QStringLiteral("--out"), path(QStringLiteral("out"))}, &exit);
        QVERIFY2(exit == 0 && packed.value("ok", false), packed.dump().c_str());
        const QString copy = path(QStringLiteral("out/pack.genko"));
        QCOMPARE(QString::fromStdString(packed["book"].get<std::string>()), copy);
        QCOMPARE(packed["relinked"], Json(2));
        QCOMPARE(genko::test::read_bytes(copy + QStringLiteral("/Links/a.png")), first);
        QCOMPARE(genko::test::read_bytes(copy + QStringLiteral("/Links/b.png")), second);
        const Json saved = project(copy);
        QVERIFY(saved["book_id"] != project(book)["book_id"]);
        for (const auto& [id, contents] : saved["smart_contents"].items()) {
            const std::string name = contents["name"].get<std::string>();
            QCOMPARE(contents["link"]["relative"], Json("Links/" + name));
            QVERIFY2(QString::fromStdString(contents["link"]["path"].get<std::string>()).endsWith(QStringLiteral("/out/pack.genko/Links/") + QString::fromStdString(name)),
                     contents.dump().c_str());
        }
        const Json listed = links(copy);
        QCOMPARE(listed.size(), std::size_t(2));
        for (const Json& state : listed) QCOMPARE(state["status"], Json("ok"));
        QCOMPARE(centre(copy), centre(book));
        const Json doctor = genko({QStringLiteral("doctor"), copy}, &exit);
        QVERIFY2(exit == 0, doctor.dump().c_str());
        // (the package moved elsewhere: its links found from its folder)
        QVERIFY(QDir().rename(path(QStringLiteral("out")), path(QStringLiteral("elsewhere"))));
        for (const Json& state : links(path(QStringLiteral("elsewhere/pack.genko")))) QCOMPARE(state["status"], Json("ok"));
        // refused where a book is
        QVERIFY(QDir().mkpath(path(QStringLiteral("out"))));
        (void)genko({QStringLiteral("package"), book, QStringLiteral("--out"), path(QStringLiteral("out"))}, &exit);
        QCOMPARE(exit, 0);
        const Json again = genko({QStringLiteral("package"), book, QStringLiteral("--out"), path(QStringLiteral("out"))}, &exit);
        QCOMPARE(exit, 1);
        QCOMPARE(again["code"], Json("exists"));
    }
};

QTEST_MAIN(TestContractSmartLinks)
#include "test_contract_smart_links.moc"
