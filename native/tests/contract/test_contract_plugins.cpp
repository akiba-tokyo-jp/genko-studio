// Filter plugins (M3③, Python's genko/plugins.py, docs/cpp-migration/SPEC.md COMP-04) with the reference Python as the
// runner's Python: a plugin's picture the same as Python's plugins.run makes it, every pixel (the sepia plugin of
// tests/test_k1.py, with its settings and without); filter_raster through it; and what COMP-04 adds: nothing runs until
// plugins are on and the plugin is chosen (as the file it was then), listing reads no code (a plugin's top-level code
// has not run), Python's own refusals (no plugin, cannot be loaded, no run, failed, no picture), a runner that crashes,
// prints, takes too long or writes back too much, no Python, and the environment's secrets not passed on. Skipped
// without the Python reference.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <chrono>
#include <fstream>
#include <stop_token>
#include <thread>

#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/plugins.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace plugins = genko::render::plugins;

namespace {

const char* const kSepia = R"PY(
NAME = "セピア"
PARAMS = {"amount": {"label": "強さ", "min": 0, "max": 1, "default": 1}}


def run(image, amount=1.0):
    from PIL import Image, ImageOps

    toned = ImageOps.colorize(image.convert("L"), (40, 20, 0), (255, 240, 200)).convert("RGB")
    return Image.blend(image.convert("RGB"), toned, float(amount))
)PY";

void write(const QString& path, const std::string& text) {
    std::ofstream file(genko::core::path_from_utf8(path.toStdString()), std::ios::binary | std::ios::trunc);
    file << text;
}

genko::render::Image picture() {  // (test_k1's: a blue square on a clear 100 × 140)
    genko::render::Image im = genko::render::Image::create("RGBA", genko::render::Size{100, 140}, genko::render::Ink{0, 0, 0, 0});
    im.paste(genko::render::Ink{30, 120, 220, 255}, genko::render::Box{10, 10, 60, 60});
    return im;
}

template <class F>
QString error_of(F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return QString::fromUtf8(e.what());
    }
    return QString();
}

}  // namespace

class TestContractPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    QString config_;
    QString folder_;

    void enable(bool on, const QString& python = genko::test::python_ref()) {
        plugins::Settings s = plugins::settings();
        s.enabled = on;
        s.python = python.toStdString();
        plugins::save_settings(s);
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        config_ = scratch_.path() + "/config";
        folder_ = config_ + "/plugins";
        QDir().mkpath(folder_);
        qputenv("GENKO_CONFIG_DIR", config_.toUtf8());
        qputenv("GENKO_TEST_SECRET", "do-not-pass-me");
        write(folder_ + "/sepia.py", kSepia);
        write(folder_ + "/sepia.json", R"({"name": "セピア（説明）"})");
        write(folder_ + "/broken.py", "def nothing(:\n");
        write(folder_ + "/norun.py", "NAME = 'x'\n");
        write(folder_ + "/_private.py", "raise SystemExit\n");
        write(folder_ + "/sideeffect.py", "open(" + genko::core::dump(Json((scratch_.path() + "/ran.txt").toStdString()), genko::core::DumpOptions{}) +
                                              ", 'w').write('ran')\ndef run(image):\n    return image\n");
        write(folder_ + "/crasher.py", "import os\ndef run(image):\n    os._exit(3)\n");
        write(folder_ + "/printer.py", "def run(image):\n    print('hello ' * 1000)\n    return image\n");
        write(folder_ + "/flood.py", "import sys\ndef run(image):\n    sys.__stdout__.buffer.write(b'x' * 50000000)\n    return image\n");
        write(folder_ + "/sleepy.py", "import time\ntime.sleep(30)\ndef run(image):\n    return image\n");
        write(folder_ + "/nopicture.py", "def run(image):\n    return 5\n");
        write(folder_ + "/fails.py", "def run(image):\n    raise RuntimeError('boom')\n");
        write(folder_ + "/smaller.py", "def run(image):\n    return image.convert('RGB').resize((10, 10))\n");
        write(folder_ + "/counted.py", "import time\ndef run(image):\n    open(" +
                                           genko::core::dump(Json((scratch_.path() + "/counted.txt").toStdString()), genko::core::DumpOptions{}) +
                                           ", 'a').write('x')\n    time.sleep(2)\n    return image\n");
        write(folder_ + "/slowrun.py", "import time\ndef run(image):\n    time.sleep(60)\n    return image\n");
        write(folder_ + "/beside.py", "from pathlib import Path\nfrom PIL import Image\ndef run(image):\n"
                                      "    v = int((Path(__file__).parent / 'beside.txt').read_text())\n"
                                      "    return Image.new('RGBA', image.size, (v, v, v, 255))\n");
        write(folder_ + "/beside.txt", "77");
        write(folder_ + "/env.py", "import os\nfrom PIL import Image\ndef run(image):\n"
                                   "    bad = 'GENKO_TEST_SECRET' in os.environ\n"
                                   "    return Image.new('RGBA', image.size, (255, 0, 0, 255) if bad else (0, 255, 0, 255))\n");
    }

    void listingRunsNothing() {
        std::vector<std::string> keys;
        for (const auto& item : plugins::listed()) keys.push_back(item.key);
        QCOMPARE(keys, (std::vector<std::string>{"beside", "broken", "counted", "crasher", "env", "fails", "flood", "nopicture", "norun", "printer", "sepia",
                                                 "sideeffect", "sleepy", "slowrun", "smaller"}));
        for (const auto& item : plugins::listed()) {
            if (item.key == "sepia") {
                QVERIFY(item.manifest.has_value());
                QCOMPARE((*item.manifest)["name"], Json("セピア（説明）"));
            }
            QVERIFY(!item.chosen);
        }
        QVERIFY(!QFile::exists(scratch_.path() + "/ran.txt"));  // (its top-level code never ran)
    }

    void offUntilChosen() {
        QVERIFY(!plugins::settings().enabled);
        QVERIFY(plugins::available().empty());
        QVERIFY(error_of([] { plugins::run("plugin:sepia", picture()); }).contains("is not chosen to run"));
        plugins::choose("sepia", true);
        QVERIFY(!plugins::allowed("sepia"));  // (chosen, but plugins are off)
        enable(true);
        QVERIFY(plugins::allowed("sepia"));
        QVERIFY(!plugins::allowed("broken"));
        QVERIFY(!QFile::exists(scratch_.path() + "/ran.txt"));
    }

    void likePython() {
        enable(true);
        plugins::choose("sepia", true);
        const auto found = plugins::available();
        QCOMPARE(found.size(), std::size_t{1});
        QCOMPARE(found[0].name, std::string("セピア"));
        const auto asked = plugins::fields("plugin:sepia");
        QCOMPARE(asked.size(), std::size_t{1});
        QCOMPARE(std::get<0>(asked[0]), std::string("amount"));
        QCOMPARE(std::get<1>(asked[0]), std::string("強さ"));
        QCOMPARE(std::get<2>(asked[0]), 0.0);
        QCOMPARE(std::get<3>(asked[0]), 1.0);
        QCOMPARE(std::get<4>(asked[0]), 1.0);
        const QString src = scratch_.path() + "/in.png";
        genko::render::save_png(picture(), genko::core::path_from_utf8(src.toStdString()));
        for (const char* params : {"{}", R"({"amount": 0.4})"}) {
            const QString want_path = scratch_.path() + "/py-out.png";
            const auto py = genko::test::harness({"plugin-run", config_, "plugin:sepia", src, want_path, params}, scratch_.path() + "/pyenv");
            QVERIFY2(py.finished && py.exit_code == 0, py.err.right(3000).constData());
            const genko::render::Image want = genko::render::read_png(genko::test::read_bytes(want_path)).convert("RGBA");
            const genko::render::Image got = plugins::run("plugin:sepia", picture(), genko::core::parse_python_json(params));
            QCOMPARE(got.size(), want.size());
            QVERIFY2(got.tobytes() == want.tobytes(), params);
        }
        // through filter_raster on a paint layer
        genko::core::Document doc = genko::core::new_episode("p", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor person("human:作者");
        doc = bus.apply(doc, Json::parse(R"([{"op": "add_layer", "page": 1, "kind": "paint", "id": "p"}])"), person, false).doc;
        const std::string png = genko::render::write_png(picture());
        Json put{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", genko::core::b64encode(png)}};
        doc = bus.apply(doc, Json::array({put, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:sepia"}, {"amount", 1}}}),
                        person, false).doc;
        const genko::core::Layer* layer = nullptr;
        for (const auto& l : doc.page(0).layers) {
            if (l.id == "p") layer = &l;
        }
        QVERIFY(layer != nullptr && layer->raster_png);
        const genko::render::Image out = genko::render::read_png(*layer->raster_png).convert("RGBA");
        const auto blue = out.getpixel(30, 30);
        QVERIFY(blue[0] > blue[2] && blue[3] == 255);  // (blue became sepia)
        QCOMPARE(out.getpixel(90, 130)[3], 0.0);         // (the clear part stays clear)
        const QString none = error_of([&] {
            (void)bus.apply(doc, Json::parse(R"([{"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:none"}])"), person, false);
        });
        QVERIFY2(none.contains("no plugin none"), qPrintable(none));
        const QString evil = error_of([&] {
            (void)bus.apply(doc, Json::parse(R"([{"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:../evil"}])"), person, false);
        });
        QVERIFY2(evil.contains("no plugin ../evil"), qPrintable(evil));
        // a changed file is chosen again
        write(folder_ + "/sepia.py", std::string(kSepia) + "\n# changed\n");
        QVERIFY(!plugins::allowed("sepia"));
        QVERIFY(error_of([] { plugins::run("plugin:sepia", picture()); }).contains("is not chosen to run"));
        write(folder_ + "/sepia.py", kSepia);
        QVERIFY(plugins::allowed("sepia"));
    }

    void refusalsAndBounds() {
        enable(true);
        for (const char* key : {"broken", "norun", "crasher", "printer", "flood", "sleepy", "nopicture", "fails", "smaller", "env"})
            plugins::choose(key, true);
        QCOMPARE(error_of([] { plugins::describe("broken"); }).left(30), QStringLiteral("plugin broken cannot be loaded"));
        QCOMPARE(error_of([] { plugins::describe("norun"); }), QStringLiteral("plugin norun has no run(image)"));
        QVERIFY(error_of([] { plugins::run("plugin:fails", picture()); }).startsWith(QStringLiteral("plugin fails failed (boom")));
        QCOMPARE(error_of([] { plugins::run("plugin:nopicture", picture()); }), QStringLiteral("plugin nopicture did not return a picture"));
        // the runner's own trouble is its own error (never the plugin's answer: a correction layer is not quietly left as it was)
        const auto runner_code = [](auto&& f) {
            try {
                f();
            } catch (const genko::core::Error& e) {
                return QString::fromStdString(e.code()) + QStringLiteral(": ") + QString::fromUtf8(e.what());
            }
            return QString();
        };
        QVERIFY(runner_code([] { plugins::run("plugin:crasher", picture()); }).startsWith(QStringLiteral("plugin_runner: plugin crasher stopped before it answered")));
        QVERIFY(runner_code([] { plugins::run("plugin:flood", picture()); }).startsWith(QStringLiteral("plugin_runner: plugin flood wrote back more than a picture")));
        QElapsedTimer slow;
        slow.start();
        QCOMPARE(runner_code([] { plugins::describe("sleepy"); }), QStringLiteral("plugin_runner: plugin sleepy did not finish in 10 seconds"));
        QVERIFY(slow.elapsed() >= 9000);
        slow.restart();  // (its failure is kept: asked again, it is not run again)
        QCOMPARE(runner_code([] { plugins::describe("sleepy"); }), QStringLiteral("plugin_runner: plugin sleepy did not finish in 10 seconds"));
        QVERIFY2(slow.elapsed() < 2000, qPrintable(QString::number(slow.elapsed())));
        // what a plugin prints never reaches the reply; a smaller picture comes back resized, its alpha kept
        QCOMPARE(plugins::run("plugin:printer", picture()).tobytes(), picture().tobytes());
        const genko::render::Image small = plugins::run("plugin:smaller", picture());
        QCOMPARE(small.size(), picture().size());
        QCOMPARE(small.getpixel(90, 130)[3], 0.0);
        // a plugin finds the files beside it (its __file__ is where it lives, though the copy chosen is what runs)
        plugins::choose("beside", true);
        QCOMPARE(plugins::run("plugin:beside", picture()).getpixel(5, 5), (std::vector<double>{77, 77, 77, 255}));
        // the environment's secrets stay here
        QCOMPARE(plugins::run("plugin:env", picture()).getpixel(0, 0), (std::vector<double>{0, 255, 0, 255}));
        // the broken ones are left out of the list
        std::vector<std::string> names;
        for (const auto& d : plugins::available()) names.push_back(d.key);
        QVERIFY(std::find(names.begin(), names.end(), "broken") == names.end());
        QVERIFY(std::find(names.begin(), names.end(), "sepia") != names.end());
        // no Python: said, nothing run (the pictures made before forgotten: nothing comes from a run kept)
        plugins::forget_made();
        enable(true, scratch_.path() + "/no-such-python");
        try {
            plugins::run("plugin:sepia", picture());
            QFAIL("ran without Python");
        } catch (const genko::core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("plugin_runner"));
        }
        enable(false);
    }

    void chosenAsShown() {
        enable(true);
        plugins::choose("sepia", false);
        const std::string listed = [] {
            for (const auto& item : plugins::listed())
                if (item.key == "sepia") return item.sha256;
            return std::string();
        }();
        write(folder_ + "/sepia.py", std::string(kSepia) + "\n# changed after it was shown\n");
        try {
            plugins::choose("sepia", true, listed);
            QFAIL("chose a file other than the one shown");
        } catch (const genko::core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("plugin_changed"));
        }
        QVERIFY(!plugins::allowed("sepia"));
        write(folder_ + "/sepia.py", kSepia);
        plugins::choose("sepia", true, listed);
        QVERIFY(plugins::allowed("sepia"));
        // a settings file a person changed by hand: read as far as it can be, never a crash
        const QString file = config_ + "/plugin_settings.json";
        const std::string kept = genko::test::read_bytes(file);
        for (const char* odd : {R"({"enabled": 1})", R"({"enabled": "yes", "chosen": []})", "[1, 2]", "{not json", R"({"python": 5})"}) {
            write(file, odd);
            QVERIFY2(!plugins::settings().enabled, odd);
            QVERIFY2(!plugins::allowed("sepia"), odd);
        }
        write(file, kept);
        QVERIFY(plugins::allowed("sepia"));
        enable(false);
    }

    void oneRunForOneRequest() {
        enable(true);
        plugins::choose("counted", true);
        plugins::forget_made();
        QFile::remove(scratch_.path() + "/counted.txt");
        std::vector<std::thread> asking;
        std::vector<std::string> got(4);
        for (std::size_t i = 0; i < got.size(); ++i) {
            asking.emplace_back([&got, i] { got[i] = plugins::run("plugin:counted", picture()).tobytes(); });
        }
        for (auto& t : asking) t.join();
        for (const auto& bytes : got) QVERIFY(bytes == picture().tobytes());
        QCOMPARE(genko::test::read_bytes(scratch_.path() + "/counted.txt"), std::string("x"));  // (asked four times, run once)
        enable(false);
    }

    void stoppedWhenNoLongerWanted() {
        enable(true);
        plugins::choose("slowrun", true);
        std::stop_source stop;
        std::thread later([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            stop.request_stop();
        });
        QElapsedTimer clock;
        clock.start();
        bool cancelled = false;
        try {
            plugins::run("plugin:slowrun", picture(), Json::object(), stop.get_token());
        } catch (const genko::render::Cancelled&) {
            cancelled = true;
        }
        later.join();
        QVERIFY(cancelled);
        QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));  // (the runner was stopped, not waited for)
        enable(false);
    }

    void correctionLayerNeverQuietlyLeftOut() {
        // a correction layer whose plugin cannot run: refused for output, left out and said (with why) on screen
        enable(true);
        plugins::choose("crasher", true);
        genko::core::Document doc = genko::core::new_episode("p", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor person("human:作者");
        doc = bus.apply(doc, Json::parse(R"([{"op": "add_layer", "page": 1, "kind": "adjust", "id": "a1"},
                                             {"op": "add_layer", "page": 1, "kind": "adjust", "id": "a2"}])"),
                        person, false)
                  .doc;
        // (no op makes a correction layer of a plugin — add_layer takes the built-in adjustments only — but a book may
        // hold one, and the page draws it as Python's _adjusted does)
        for (genko::core::Layer& layer : doc.edit_page(0).layers) {
            if (layer.id == "a1") layer.adjust = Json{{"kind", "plugin:crasher"}};
            if (layer.id == "a2") layer.adjust = Json{{"kind", "plugin:sepia"}};
        }
        plugins::choose("sepia", false);
        genko::render::RenderOptions output;
        const QString refused = [&] {
            try {
                (void)genko::render::render_page(doc.page(0), 20, output, &doc);
            } catch (const genko::core::Error& e) {
                return QString::fromStdString(e.code());
            }
            return QString();
        }();
        QVERIFY2(refused == QStringLiteral("plugin_runner") || refused == QStringLiteral("plugin_not_allowed"), qPrintable(refused));
        genko::render::RenderOptions screen;
        screen.skip_unported = true;
        const auto shown = genko::render::render_page(doc.page(0), 20, screen, &doc);
        QVERIFY(std::find(shown.omitted.begin(), shown.omitted.end(), "plugin_failed:crasher") != shown.omitted.end());
        QVERIFY(std::find(shown.omitted.begin(), shown.omitted.end(), "plugin_off:sepia") != shown.omitted.end());
        enable(false);
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "900000");
    QCoreApplication app(argc, argv);
    TestContractPlugins test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_contract_plugins.moc"
