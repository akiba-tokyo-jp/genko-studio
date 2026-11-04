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
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <thread>

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

#include "api/mcp_tools.hpp"
#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/smart.hpp"
#include "core/paths.hpp"
#include "render/ink.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/plugins.hpp"
#include "render/smart.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"
#include "storage/asset_store.hpp"
#include "storage/gc.hpp"
#include "storage/reader.hpp"
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
        // on a CMYK page (color-mode@1, 群C): the plugin works the picture the layer shows, the layer stays in inks
        {
            const std::string icc = genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")));
            genko::core::Document cmyk = genko::core::new_episode("p", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
            cmyk = bus.apply(cmyk, Json::array({Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", genko::core::b64encode(icc)}},
                                                Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}}, put,
                                                Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:sepia"}, {"amount", 1}}}),
                             person, false)
                       .doc;
            const genko::core::Layer* inked = nullptr;
            for (const auto& l : cmyk.page(0).layers) {
                if (l.id == "p") inked = &l;
            }
            QVERIFY(inked != nullptr && inked->ink_raster && !inked->raster_png);
            const genko::render::Image shown = genko::render::ink::to_rgba8(genko::core::decode_ink_raster(*inked->ink_raster), *cmyk.page(0).color);
            const auto sepia = shown.getpixel(30, 30);
            QVERIFY(sepia[0] > sepia[2] && sepia[3] == 255);
            QCOMPARE(shown.getpixel(90, 130)[3], 0.0);
        }
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

// PC5 J-f (plugins:WASM-ABI-v1, SPEC.md PROD-01, docs/PLUGIN_WASM.md): WebAssembly plugins through the ops, without Python —
// nothing runs until plugins are on and the module is chosen (as its bytes are; changed: chosen again); filter_raster with
// a module on a paint layer of 8 bits (inside the selection), of precise colour, of a page's inks, on an alpha channel
// (one grey channel); a host that falls over leaves the book as it was; a module's smart filter keeps its picture and the
// smart object looks the same where the plugin cannot run; MCP has no tool that installs, chooses or consents.
class TestContractWasmPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    QString folder_;
    QString host_;
    const genko::core::CommandBus bus_{genko::render::ops_registry()};
    const genko::core::Actor person_{"human:作者"};

    void enable(bool on) {
        plugins::Settings s = plugins::settings();
        s.enabled = on;
        plugins::save_settings(s);
    }
    genko::core::Document applied(const genko::core::Document& doc, const Json& ops) { return bus_.apply(doc, ops, person_, false).doc; }
    const genko::core::Layer& layer_of(const genko::core::Document& doc, const std::string& id) {
        return doc.page(0).layers[genko::core::layer_by_id(doc.page(0), id)];
    }
    genko::core::Document painted() {  // (a blue square on a clear page of 8 bits, layer "p")
        genko::core::Document doc = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        const Json put{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", genko::core::b64encode(genko::render::write_png(picture()))}};
        return applied(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}}, put}));
    }
    genko::render::Image pixels(const genko::core::Document& doc, const std::string& id) {
        return genko::render::read_png(*layer_of(doc, id).raster_png).convert("RGBA");
    }
    // the mean difference of two pictures of the page, a sample
    static double apart(const genko::render::Image& a, const genko::render::Image& b) {
        const std::string x = a.tobytes(), y = b.tobytes();
        if (x.size() != y.size() || x.empty()) return 255.0;
        double sum = 0;
        for (std::size_t i = 0; i < x.size(); ++i) sum += std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i])));
        return sum / double(x.size());
    }
    // The page drawn with the plugin running, and again where it cannot (plugins off: the pictures kept): the same look.
    void same_look_without_plugin(const genko::core::Document& doc, const char* what) {
        enable(true);
        genko::render::smart::clear_caches();
        plugins::forget_made();
        genko::render::RenderOptions output;
        const genko::render::Image drawn = genko::render::render_page(doc.page(0), 40, output, &doc).image;
        enable(false);
        genko::render::smart::clear_caches();
        plugins::forget_made();
        genko::render::RenderOptions again;
        const genko::render::Image kept = genko::render::render_page(doc.page(0), 40, again, &doc).image;
        QVERIFY2(apart(drawn, kept) < 0.5, qPrintable(QStringLiteral("%1: %2").arg(QString::fromUtf8(what)).arg(apart(drawn, kept))));
        enable(true);
    }

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        const QString config = scratch_.path() + "/config";
        folder_ = config + "/plugins";
        QDir().mkpath(folder_);
        qputenv("GENKO_CONFIG_DIR", config.toUtf8());
        host_ = QStringLiteral(GENKO_TEST_PLUGIN_HOST);
        if (!QFileInfo::exists(host_)) QSKIP("genko-plugin-host was not built (no cargo): the WebAssembly plugins cannot run in this build");
        plugins::set_host_for_testing(genko::core::path_from_utf8((host_).toStdString()));
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_DATA "/plugins/invert.wasm"), folder_ + "/invert.wasm"));
    }

    // Off at first, and off until chosen as its bytes are: the op refused, the book as it was.
    void offUntilChosenAsItIs() {
        const genko::core::Document doc = painted();
        const Json op = Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:invert"}}});
        QVERIFY(error_of([&] { (void)applied(doc, op); }).contains(QStringLiteral("plugin invert is not chosen to run")));
        enable(true);
        QVERIFY(error_of([&] { (void)applied(doc, op); }).contains(QStringLiteral("plugin invert is not chosen to run")));
        plugins::choose("invert", true);
        const genko::core::Document done = applied(doc, op);
        QCOMPARE(pixels(done, "p").getpixel(30, 30), (std::vector<double>{225, 135, 35, 255}));
        QCOMPARE(pixels(done, "p").getpixel(90, 130), (std::vector<double>{0, 0, 0, 0}));
        // changed: chosen again
        {
            QFile file(folder_ + "/invert.wasm");
            QVERIFY(file.open(QIODevice::Append));
            file.write("\0", 1);
        }
        QVERIFY(error_of([&] { (void)applied(doc, op); }).contains(QStringLiteral("plugin invert is not chosen to run")));
        QFile::remove(folder_ + "/invert.wasm");
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_DATA "/plugins/invert.wasm"), folder_ + "/invert.wasm"));
        QVERIFY(plugins::allowed("invert"));  // (the bytes chosen are back)
        // inside the selection only (its coverage mixed as the bands are laid)
        const genko::core::Document inside = applied(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:invert"},
                                                                            {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({5, 0}),
                                                                                                                Json::array({5, 200}), Json::array({0, 200})})}}}}}));
        const genko::render::Image in = pixels(inside, "p");
        QCOMPARE(in.getpixel(15, 30), (std::vector<double>{225, 135, 35, 255}));  // (within 5 mm of the left at 200 dpi)
        QCOMPARE(in.getpixel(55, 30), (std::vector<double>{30, 120, 220, 255}));  // (outside: as it was)
    }

    // A layer of precise colour (16 bits) and a page's inks (Lab, 16 bits): their own samples through the module.
    void preciseAndInkLayers() {
        enable(true);
        plugins::choose("invert", true);
        auto doc = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::custom(50, 50, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        doc = applied(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 2}, {"precision", "u16"},
                                             {"pixels", Json::array({1000, 2000, 3000, 65535, 1000, 2000, 3000, 65535, 1000, 2000, 3000, 65535, 1000, 2000,
                                                                     3000, 65535})}}}));
        const std::string precise = doc.page(0).layers.back().id;
        const std::string before = *layer_of(doc, precise).color_raster;
        doc = applied(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", precise}, {"kind", "plugin:invert"}}}));
        const genko::core::ColorRasterView was(before), now(*layer_of(doc, precise).color_raster);
        for (std::size_t i = 0; i < std::size_t(now.width()) * now.height(); ++i) {
            for (int c = 0; c < 3; ++c) QVERIFY(std::abs(now.pixel(i)[std::size_t(c)] - (1.0 - was.pixel(i)[std::size_t(c)])) < 1.0 / 65535 + 1e-9);
            QCOMPARE(now.pixel(i)[3], was.pixel(i)[3]);
        }
        auto lab = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::custom(50, 50, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        lab = applied(lab, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}},
                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "ink"}},
                                        Json{{"op", "fill"}, {"page", 1}, {"layer_id", "ink"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({100, 150, 200})}}}));
        QVERIFY(layer_of(lab, "ink").ink_raster);
        const genko::core::InkPixels ink_before = genko::core::decode_ink_raster(*layer_of(lab, "ink").ink_raster);
        lab = applied(lab, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "ink"}, {"kind", "plugin:invert"}}}));
        QVERIFY(layer_of(lab, "ink").ink_raster && !layer_of(lab, "ink").raster_png);  // (its inks themselves, not a lent picture)
        const genko::core::InkPixels ink_after = genko::core::decode_ink_raster(*layer_of(lab, "ink").ink_raster);
        const std::size_t middle = std::size_t(ink_after.height / 2) * ink_after.width + ink_after.width / 2;
        for (unsigned c = 0; c < 3; ++c) QVERIFY(std::abs(ink_after.value(middle, c) - (1.0 - ink_before.value(middle, c))) < 1.0 / 65535 + 1e-9);
        QCOMPARE(ink_after.value(middle, 3), ink_before.value(middle, 3));
    }

    // An alpha channel (and so the Quick Mask) given as one grey channel: the channel's density turned over.
    void alphaChannelAsGrey() {
        enable(true);
        plugins::choose("invert", true);
        genko::core::Document doc = painted();
        doc = applied(doc, Json::array({Json{{"op", "add_channel"}, {"page", 1}}}));
        const std::string channel = doc.page(0).channels.at(0).id;
        doc = applied(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:invert"}, {"channel", channel}}}));
        QVERIFY(doc.page(0).channels.at(0).raster);
        const genko::core::InkPixels mask = genko::core::decode_ink_raster(*doc.page(0).channels.at(0).raster);
        QCOMPARE(mask.channels(), 1u);
        QVERIFY(mask.value(0, 0) > 0.99);  // (an empty channel, all clear: turned over, all full)
        QCOMPARE(pixels(doc, "p").getpixel(30, 30), (std::vector<double>{30, 120, 220, 255}));  // (the layer as it was)
    }

    // A host that falls over: the op refused, the book as it was (and the app goes on).
    void hostFallsOverBookStays() {
        enable(true);
        plugins::choose("invert", true);
        plugins::forget_made();
        const QString crash = scratch_.path() + "/crash-host";
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), crash));
        QVERIFY(QFile::setPermissions(crash, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        plugins::set_host_for_testing(genko::core::path_from_utf8((crash).toStdString()));
        const genko::core::Document doc = painted();
        QVERIFY(error_of([&] { (void)applied(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:invert"}}})); })
                    .contains(QStringLiteral("plugin invert stopped before it answered")));
        QCOMPARE(pixels(doc, "p").getpixel(30, 30), (std::vector<double>{30, 120, 220, 255}));
        plugins::set_host_for_testing(genko::core::path_from_utf8((host_).toStdString()));
    }

    // plugin-filters@1: a module's smart filter keeps the picture it made; where the plugin cannot run (not chosen, not
    // installed) the smart object looks as it did.
    void smartFilterKeepsItsLook() {
        enable(true);
        plugins::choose("invert", true);
        genko::core::Document doc = applied(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p"})}, {"new_id", "so"}}}));
        const genko::core::ApplyResult added = bus_.apply(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "plugin:invert"}}}), person_, false);
        doc = added.doc;
        const Json& filter = (*layer_of(doc, "so").smart)["filters"][0];
        QCOMPARE(filter["kind"], Json("plugin:invert"));
        QVERIFY(filter.contains("kept"));
        QCOMPARE(filter["kept"]["suffix"], Json(".png"));
        QVERIFY(doc.smart_files.contains(filter["kept"]["asset"].get<std::string>()));
        QVERIFY(genko::core::smart::uses_plugin_filters(doc));
        genko::core::smart::validate_smart(doc);
        // (the mean difference of two pictures of the page, a sample)
        const auto apart = [](const genko::render::Image& a, const genko::render::Image& b) {
            const std::string x = a.tobytes(), y = b.tobytes();
            if (x.size() != y.size() || x.empty()) return 255.0;
            double sum = 0;
            for (std::size_t i = 0; i < x.size(); ++i) sum += std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i])));
            return sum / double(x.size());
        };
        genko::render::RenderOptions output;
        const genko::render::Image drawn = genko::render::render_page(doc.page(0), 40, output, &doc).image;
        enable(false);  // (the plugin cannot run here now: the kept picture, made at 200 dpi, drawn at this resolution)
        genko::render::smart::clear_caches();  // (the smart object drawn again, not the picture kept from before)
        plugins::forget_made();
        genko::render::RenderOptions again;
        const genko::render::Image kept = genko::render::render_page(doc.page(0), 40, again, &doc).image;
        QVERIFY2(apart(drawn, kept) < 0.5, qPrintable(QString::number(apart(drawn, kept))));
        // (and not as the smart object without its filter: the look kept is the filtered one)
        const genko::render::Image plain = genko::render::render_page(painted().page(0), 40, again, nullptr).image;
        QVERIFY2(apart(plain, kept) > apart(drawn, kept) + 0.05, qPrintable(QString::number(apart(plain, kept))));
        // an op on the stack where the plugin cannot run: said
        const genko::core::ApplyResult moved = bus_.apply(doc, Json::array({Json{{"op", "smart_filter_set"}, {"page", 1}, {"id", "so"}, {"opacity", 0.5}}}), person_, false);
        QVERIFY(std::any_of(moved.warnings.begin(), moved.warnings.end(),
                            [](const Json& n) { return n.is_string() && n.get<std::string>().find("cannot run here") != std::string::npos; }));
        enable(true);
    }

    // (review P2-2) The picture kept is the filter's own output (the smart object's picture before its finish and tone,
    // through the filters up to the plugin's), so where the plugin cannot run the page looks as it did: a picture's
    // smart object on a monochrome page (its finish screens it in the page, not twice) and a tone's smart object (its
    // black laid by the page, not twice).
    void keptLookOfFinishedAndToneSmartObjects() {
        enable(true);
        plugins::choose("invert", true);
        // a grey picture placed on a monochrome page: its finish draws it in dots
        genko::render::Image grey = genko::render::Image::create("RGBA", genko::render::Size{240, 320}, genko::render::Ink{255, 255, 255, 255});
        grey.paste(genko::render::Ink{90, 90, 90, 255}, genko::render::Box{20, 20, 180, 260});
        genko::core::Document file = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        file = applied(file, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", genko::core::b64encode(genko::render::write_png(grey))}, {"name", "grey.png"}, {"new_id", "file"}}}));
        const std::string placed = "file";
        QVERIFY(layer_of(file, placed).smart);
        file = applied(file, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", placed}, {"kind", "plugin:invert"}}}));
        QVERIFY((*layer_of(file, placed).smart)["filters"][0].contains("kept"));
        same_look_without_plugin(file, "a picture's smart object on a monochrome page");
        // a tone made a smart object
        genko::core::Document tone = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        tone = applied(tone, Json::array({Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.3}, {"lpi", 60}, {"angle", 45},
                                               {"area", Json{{"poly", Json::array({Json::array({40, 120}), Json::array({160, 120}), Json::array({160, 240}),
                                                                                  Json::array({40, 240})})}}}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"tn"})}, {"new_id", "so"}},
                                          Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "plugin:invert"}}}));
        QVERIFY((*layer_of(tone, "so").smart)["filters"][0].contains("kept"));
        same_look_without_plugin(tone, "a tone's smart object");
        enable(true);
    }

    // (review P2-3) plugin-filters@1 saved and read again: the feature and the kept pictures with it; a kept picture
    // missing said (missing_asset); the plugin's filters removed, the feature goes and the book is written as it was
    // before they came; `genko gc` keeps the pictures kept.
    void keptPicturesSavedAndReadAgain() {
        enable(true);
        plugins::choose("invert", true);
        const genko::core::Document before = applied(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p"})}, {"new_id", "so"}}}));
        const genko::core::Document doc = applied(before, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "plugin:invert"}},
                                                                       Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur"}, {"radius", 2}}}));
        const Json kept = (*layer_of(doc, "so").smart)["filters"][0]["kept"];
        const std::string ref = kept["asset"].get<std::string>(), suffix = kept["suffix"].get<std::string>();
        const std::filesystem::path book = genko::core::path_from_utf8((scratch_.path() + "/book").toStdString());
        genko::test::write_project(doc, book);
        const Json written = genko::test::read_json(QString::fromStdString((book / "project.json").string()));
        QVERIFY(std::find(written["features"].begin(), written["features"].end(), Json("plugin-filters@1")) != written["features"].end());
        const genko::storage::AssetStore store(book);
        QVERIFY(store.has(ref, suffix));
        // read again: the same filters, kept picture and all
        const genko::storage::LoadResult again = genko::storage::load_document(book);
        QVERIFY2(again.report.clean(), again.report.to_json().dump().c_str());
        QCOMPARE((*layer_of(again.document, "so").smart)["filters"], (*layer_of(doc, "so").smart)["filters"]);
        // (the kept picture read from the book's files where the plugin cannot run: the look as before it was saved)
        {
            enable(false);
            genko::render::smart::clear_caches();
            plugins::forget_made();
            genko::render::RenderOptions one, two;
            const genko::render::Image held = genko::render::render_page(doc.page(0), 40, one, &doc).image;
            genko::render::smart::clear_caches();
            const genko::render::Image read = genko::render::render_page(again.document.page(0), 40, two, &again.document).image;
            QVERIFY2(apart(held, read) < 0.01, qPrintable(QString::number(apart(held, read))));
            const genko::render::Image plain = genko::render::render_page(before.page(0), 40, two, &before).image;
            QVERIFY2(apart(plain, read) > 0.05, qPrintable(QString::number(apart(plain, read))));  // (not the smart object without its filters)
            enable(true);
        }
        // `genko gc`: the kept picture stays (an asset nothing refers to, as old, goes)
        const std::string orphan = genko::storage::AssetStore(book).put_bytes("nothing refers to me", ".png");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(book / "assets")) {
            if (entry.is_regular_file()) std::filesystem::last_write_time(entry.path(), std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
        }
        const Json swept = genko::storage::gc(book, false);
        QCOMPARE(swept["removed"], Json::array({genko::storage::AssetStore::relpath(orphan, ".png")}));
        QVERIFY(store.has(ref, suffix));
        QVERIFY(genko::storage::load_document(book).report.clean());
        // the kept picture missing: said, the rest read
        std::filesystem::remove(store.path(ref, suffix));
        const genko::storage::LoadResult lost = genko::storage::load_document(book);
        QVERIFY(std::any_of(lost.report.issues.begin(), lost.report.issues.end(),
                            [&](const genko::storage::LoadIssue& issue) { return issue.kind == "missing_asset" && issue.ref == ref; }));
        // the plugin's filter removed (the other one too): no feature, the book as it was written before they came
        const genko::core::Document removed = applied(doc, Json::array({Json{{"op", "smart_filter_delete"}, {"page", 1}, {"id", "so"}, {"index", 0}},
                                                                        Json{{"op", "smart_filter_delete"}, {"page", 1}, {"id", "so"}, {"index", 0}}}));
        QVERIFY(!genko::core::smart::uses_plugin_filters(removed));
        const std::filesystem::path first = book.parent_path() / "before", second = book.parent_path() / "after";
        genko::test::write_project(before, first);
        genko::test::write_project(removed, second);
        const std::string a = genko::test::read_bytes(QString::fromStdString((first / "project.json").string()));
        const std::string b = genko::test::read_bytes(QString::fromStdString((second / "project.json").string()));
        QVERIFY(a.find("plugin-filters@1") == std::string::npos);
        QCOMPARE(QString::fromStdString(b), QString::fromStdString(a));
        enable(true);
    }

    // (review P3-10) A plugin's smart filter refused where it cannot be: a plugin installed but not chosen, none of the
    // name; on a page of inks an 8-bit plugin (protocol 1), a module taken.
    void smartFilterRefusedWhereItCannotBe() {
        enable(true);
        plugins::choose("invert", false);
        QVERIFY(plugins::installed("invert") && !plugins::allowed("invert"));
        const genko::core::Document so = applied(painted(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p"})}, {"new_id", "so"}}}));
        const auto add = [&](const genko::core::Document& doc, const char* kind) {
            return error_of([&] { (void)applied(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", kind}}})); });
        };
        QVERIFY2(add(so, "plugin:invert").contains(QStringLiteral("plugin invert is not chosen to run")), qPrintable(add(so, "plugin:invert")));
        QVERIFY(add(so, "plugin:nothing").contains(QStringLiteral("no plugin nothing")));
        // an 8-bit native plugin (protocol 1), chosen
        const QString program = QStringLiteral("filter") + (QFileInfo(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN)).suffix() == QLatin1String("exe") ? ".exe" : "");
        QVERIFY(QDir().mkpath(folder_ + "/eight"));
        write(folder_ + "/eight/plugin.json", genko::core::dump(Json{{"protocol", 1}, {"name", "8 bit"}, {"executable", program.toStdString()},
                                                                     {"args", Json::array({"invert"})}},
                                                                genko::core::DumpOptions{}));
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), folder_ + "/eight/" + program));
        QVERIFY(QFile::setPermissions(folder_ + "/eight/" + program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        plugins::choose("eight", true);
        plugins::choose("invert", true);
        QVERIFY(add(so, "plugin:eight").isEmpty());  // (an 8-bit page: taken)
        auto inks = genko::core::new_episode("w", genko::core::Num(1), 1, genko::core::PageSpec::custom(50, 50, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        inks = applied(inks, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}},
                                          Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}},
                                          Json{{"op", "fill"}, {"page", 1}, {"layer_id", "p"}, {"x_mm", 25}, {"y_mm", 25}, {"rgb", Json::array({100, 150, 200})}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p"})}, {"new_id", "so"}}}));
        QVERIFY(inks.page(0).color);
        QVERIFY2(add(inks, "plugin:eight").contains(QStringLiteral("a filter plugin of 8 bits cannot be a smart filter on a page of inks")), qPrintable(add(inks, "plugin:eight")));
        QVERIFY2(add(inks, "plugin:invert").isEmpty(), qPrintable(add(inks, "plugin:invert")));
        plugins::choose("eight", false);
        QDir(folder_ + "/eight").removeRecursively();
    }

    // MCP offers no tool that installs a plugin, chooses one or consents (plugin.list is inspect's, read only).
    void noWayInFromMcp() {
        for (const auto& tool : genko::api::mcp::tool_specs()) QVERIFY2(std::string(tool.name).find("plugin") == std::string::npos, std::string(tool.name).c_str());
        QVERIFY(genko::api::mcp::find_tool("plugin_install") == nullptr);
        enable(false);
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "900000");
    QCoreApplication app(argc, argv);
    // (PC5 J-f: the WebAssembly plugins first — they need no Python — then the Python plugins, skipped without it)
    TestContractWasmPlugins wasm;
    const int wasm_failed = QTest::qExec(&wasm, argc, argv);
    TestContractPlugins test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv) + wasm_failed;
}
#include "test_contract_plugins.moc"
