// Native filter plugins (COMP-04's native extension, this build only: render/plugins.hpp, docs/PLUGIN_NATIVE.md) with a
// small program of the test tree as the plugin (support/native_test_plugin.cpp): a folder <config>/plugins/<key>/
// holding plugin.json and the program. Listed without running anything (plugin.json read, the program not started) and
// off until plugins are on and the plugin is chosen as its plugin.json and program are (either changed: chosen
// again); a picture filtered through it, directly and by filter_raster, with its settings, its environment (no secrets,
// the protocol, its folder); and what is refused: plugin.json that cannot be used (not JSON, another protocol, no name,
// a program missing, outside its folder or given as an absolute path, args that are not texts, a Python plugin of the
// same name; links: the program, a folder on its way out of the plugin's folder, the plugin's folder itself; a program
// that cannot be read), a program that cannot be started, that crashes, refuses, answers with another size, writes back
// too much or takes too long (stopped at kRunTime; and at once when no longer wanted). What runs is a copy of the bytes
// checked against the choice, in a private folder under the settings folder, removed afterwards; a program changed
// after the choice never runs. No Python.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <chrono>
#include <fstream>
#include <map>
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

using genko::core::Json;
namespace plugins = genko::render::plugins;

namespace {

void write(const QString& path, const std::string& text) {
    std::ofstream file(genko::core::path_from_utf8(path.toStdString()), std::ios::binary | std::ios::trunc);
    file << text;
}

genko::render::Image picture() {  // (a blue square on a clear 100 × 140)
    genko::render::Image im = genko::render::Image::create("RGBA", genko::render::Size{100, 140}, genko::render::Ink{0, 0, 0, 0});
    im.paste(genko::render::Ink{30, 120, 220, 255}, genko::render::Box{10, 10, 60, 60});
    return im;
}

// "<code>: <message>" of what `work` threw ("" when it threw nothing)
template <class F>
QString refusal(F&& work) {
    try {
        work();
    } catch (const genko::core::Error& e) {
        return QString::fromStdString(e.code()) + QStringLiteral(": ") + QString::fromUtf8(e.what());
    } catch (const std::exception& e) {
        return QStringLiteral("?: ") + QString::fromUtf8(e.what());
    }
    return QString();
}

}  // namespace

class TestNativePlugins : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    QString config_;
    QString folder_;
    QString program_name_;  // (the program's file name in a plugin's folder: with .exe on Windows)
    bool unreadable_ = false;  // (the unreadable plugin's program cannot be read by this process)

    QString mark() const { return scratch_.path() + "/ran.txt"; }

    // A native plugin: its folder, plugin.json (`manifest`, as text) and, when `with_program`, the test program.
    void install(const QString& key, const std::string& manifest, bool with_program = true) {
        const QString home = folder_ + "/" + key;
        QVERIFY(QDir().mkpath(home));
        write(home + "/plugin.json", manifest);
        if (with_program) {
            QFile::remove(home + "/" + program_name_);
            QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), home + "/" + program_name_));
            QVERIFY(QFile::setPermissions(home + "/" + program_name_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
    }

    // plugin.json of protocol 1 with this work for the test program (and its mark), and these settings
    std::string manifest(const std::string& name, const std::string& work, const std::string& params = "{}") const {
        Json data = Json::object();
        data["protocol"] = 1;
        data["name"] = name;
        data["executable"] = program_name_.toStdString();
        data["args"] = Json::array({work, "--mark", mark().toStdString()});
        data["params"] = genko::core::parse_python_json(params);
        return genko::core::dump(data, genko::core::DumpOptions{});
    }

    void enable(bool on) {
        plugins::Settings s = plugins::settings();
        s.enabled = on;
        plugins::save_settings(s);
    }

    std::string listed_sha(const std::string& key) const {
        for (const auto& item : plugins::listed()) {
            if (item.key == key) return item.sha256;
        }
        return {};
    }

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        config_ = scratch_.path() + "/config";
        folder_ = config_ + "/plugins";
        QVERIFY(QDir().mkpath(folder_));
        qputenv("GENKO_CONFIG_DIR", config_.toUtf8());
        qputenv("GENKO_TEST_SECRET", "do-not-pass-me");
        program_name_ = QStringLiteral("filter") + (QFileInfo(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN)).suffix() == QLatin1String("exe") ? ".exe" : "");
        install("negative", manifest("反転（ネイティブ）", "invert", R"({"red": {"label": "赤", "min": 0, "max": 255, "default": 255}})"));
        install("environment", manifest("環境", "environment"));
        write(folder_ + "/environment/home.txt", "77");
        install("fails", manifest("断る", "fail"));
        install("small", manifest("小さい", "small"));
        install("crasher", manifest("落ちる", "crash"));
        install("sleepy", manifest("遅い", "sleep"));
        install("flood", manifest("あふれる", "flood"));
        install("noexec", manifest("動かない", "invert"));
        write(folder_ + "/noexec/" + program_name_, "not a program this computer runs\n");  // (no format any system starts)
        // plugin.json that cannot be used
        install("badjson", "{not json");
        install("listjson", "[1, 2]");
        install("protocol2", R"({"protocol": 2, "name": "x", "executable": "filter"})");
        install("noname", R"({"protocol": 1, "executable": "filter"})");
        install("noprogram", R"({"protocol": 1, "name": "x", "executable": "missing"})", false);
        install("outside", R"({"protocol": 1, "name": "x", "executable": "../negative/filter"})", false);
        install("absolute", genko::core::dump(Json{{"protocol", 1}, {"name", "x"}, {"executable", QString(folder_ + "/negative/" + program_name_).toStdString()}},
                                              genko::core::DumpOptions{}));
        install("badargs", R"({"protocol": 1, "name": "x", "executable": "filter", "args": "invert"})");
        install("twin", manifest("双子", "invert"));
        write(folder_ + "/twin.py", "def run(image):\n    return image\n");
        install("where", manifest("どこ", "where"));
        // links: the program itself; a folder on the program's path leading out of the plugin's folder; the plugin's
        // folder itself (all made by the test: the program is a copy of the test program)
        install("linked", R"({"protocol": 1, "name": "x", "executable": "run"})");
        QVERIFY(QFile::link(folder_ + "/linked/" + program_name_, folder_ + "/linked/run"));
        const QString elsewhere = scratch_.path() + "/elsewhere";
        QVERIFY(QDir().mkpath(elsewhere + "/home"));
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), elsewhere + "/" + program_name_));
        install("outlink", R"({"protocol": 1, "name": "x", "executable": "bin/)" + program_name_.toStdString() + R"("})", false);
        QVERIFY(QFile::link(elsewhere, folder_ + "/outlink/bin"));
        write(elsewhere + "/home/plugin.json", manifest("x", "invert"));
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), elsewhere + "/home/" + program_name_));
        QVERIFY(QFile::link(elsewhere + "/home", folder_ + "/folderlink"));
        // a program that may be run but not read (no right to read it, as a person's other files may be)
        install("unreadable", manifest("読めない", "invert"));
        QVERIFY(QFile::setPermissions(folder_ + "/unreadable/" + program_name_, QFile::ExeOwner));
        QFile probe(folder_ + "/unreadable/" + program_name_);
        unreadable_ = !probe.open(QIODevice::ReadOnly);  // (a process that may read anything reads it all the same)
        // not plugins: a folder without plugin.json, folders starting with "_" or "."
        QVERIFY(QDir().mkpath(folder_ + "/empty"));
        install("_private", manifest("x", "invert"));
        install(".hidden", manifest("x", "invert"));
    }

    // Listing reads plugin.json and the program's bytes, and runs nothing; nothing is on at first.
    void listedWithoutRunning() {
        std::vector<std::string> keys;
        std::map<std::string, plugins::Listed> by_key;
        for (const auto& item : plugins::listed()) {
            keys.push_back(item.key + (item.native ? "/" : ".py"));
            by_key[item.key + (item.native ? "/" : ".py")] = item;
        }
        QCOMPARE(keys, (std::vector<std::string>{"absolute/", "badargs/", "badjson/", "crasher/", "environment/", "fails/", "flood/", "folderlink/",
                                                 "linked/", "listjson/", "negative/", "noexec/", "noname/", "noprogram/", "outlink/", "outside/",
                                                 "protocol2/", "sleepy/", "small/", "twin.py", "twin/", "unreadable/", "where/"}));
        const auto& negative = by_key["negative/"];
        QVERIFY(negative.native && !negative.chosen && !negative.problem && negative.sha256.size() == 64);
        QCOMPARE((*negative.manifest)["name"], Json("反転（ネイティブ）"));
        const auto problem = [&](const std::string& key) { return QString::fromStdString(by_key[key].problem.value_or("")); };
        const QString cannot = QStringLiteral("the plugin.json of plugin %1 cannot be used (%2)");
        QCOMPARE(problem("badjson/"), cannot.arg("badjson", "it is not JSON"));
        QCOMPARE(problem("listjson/"), cannot.arg("listjson", "it is not a JSON object"));
        QCOMPARE(problem("protocol2/"), cannot.arg("protocol2", "its protocol must be 1"));
        QCOMPARE(problem("noname/"), cannot.arg("noname", "its name must be a text"));
        QCOMPARE(problem("noprogram/"), cannot.arg("noprogram", "its program missing is not in its folder"));
        QCOMPARE(problem("outside/"), cannot.arg("outside", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(problem("absolute/"), cannot.arg("absolute", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(problem("badargs/"), cannot.arg("badargs", "its args must be a list of texts"));
        QCOMPARE(problem("twin/"), cannot.arg("twin", "a Python plugin twin.py has the same name"));
        QCOMPARE(problem("linked/"), cannot.arg("linked", "its program run is a link"));
        QCOMPARE(problem("outlink/"), cannot.arg("outlink", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(problem("folderlink/"), cannot.arg("folderlink", "its folder is a link"));
        if (unreadable_) {
            QCOMPARE(problem("unreadable/"), cannot.arg("unreadable", "its program " + program_name_ + " cannot be read"));
        } else {
            qInfo("this process may read any file: the program without the right to read it is read all the same");
        }
        QVERIFY(!by_key["twin.py"].native && !by_key["twin.py"].problem);
        QVERIFY(!QFile::exists(mark()));  // (no program was started)
        // off at first: nothing runs, nothing is offered
        QVERIFY(!plugins::settings().enabled);
        QVERIFY(plugins::available().empty());
        QVERIFY(!plugins::allowed("negative"));
        QVERIFY(refusal([] { plugins::run("plugin:negative", picture()); }).startsWith("plugin_not_allowed: plugin negative is not chosen to run"));
        QVERIFY(plugins::installed("negative") && plugins::installed("twin") && !plugins::installed("empty") && !plugins::installed("none"));
        QVERIFY(!QFile::exists(mark()));
    }

    // Chosen (as plugin.json and the program are) and on: its name and settings from plugin.json, a picture filtered.
    void chosenAndRun() {
        plugins::choose("negative", true, listed_sha("negative"));
        QVERIFY(!plugins::allowed("negative"));  // (chosen, but plugins are off)
        enable(true);
        QVERIFY(plugins::allowed("negative"));
        const auto found = plugins::available();
        QCOMPARE(found.size(), std::size_t{1});
        QCOMPARE(found[0].key, std::string("negative"));
        QCOMPARE(found[0].name, std::string("反転（ネイティブ）"));
        const auto asked = plugins::fields("plugin:negative");
        QCOMPARE(asked.size(), std::size_t{1});
        QCOMPARE(std::get<1>(asked[0]), std::string("赤"));
        QCOMPARE(std::get<3>(asked[0]), 255.0);
        QVERIFY(!QFile::exists(mark()));  // (described by plugin.json: still nothing started)
        const genko::render::Image in = picture();
        const genko::render::Image out = plugins::run("plugin:negative", in);
        QCOMPARE(out.size(), in.size());
        QCOMPARE(out.getpixel(30, 30), (std::vector<double>{225, 135, 35, 255}));
        QCOMPARE(out.getpixel(90, 130), (std::vector<double>{255, 255, 255, 0}));  // (the alpha kept)
        QCOMPARE(plugins::run("plugin:negative", in, Json{{"red", 7}}).getpixel(30, 30), (std::vector<double>{7, 135, 35, 255}));
        QVERIFY(QFile::exists(mark()));
        // through filter_raster on a paint layer, as a Python plugin
        genko::core::Document doc = genko::core::new_episode("p", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono());
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor person("human:作者");
        doc = bus.apply(doc, Json::parse(R"([{"op": "add_layer", "page": 1, "kind": "paint", "id": "p"}])"), person, false).doc;
        Json put{{"op", "put_raster"}, {"page", 1}, {"id", "p"}, {"png_base64", genko::core::b64encode(genko::render::write_png(in))}};
        doc = bus.apply(doc, Json::array({put, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "p"}, {"kind", "plugin:negative"}, {"red", 100}}}),
                        person, false)
                  .doc;
        const genko::core::Layer* layer = nullptr;
        for (const auto& l : doc.page(0).layers) {
            if (l.id == "p") layer = &l;
        }
        QVERIFY(layer != nullptr && layer->raster_png);
        QCOMPARE(genko::render::read_png(*layer->raster_png).convert("RGBA").getpixel(30, 30), (std::vector<double>{100, 135, 35, 255}));
        enable(false);
        QVERIFY(refusal([&] {
                    (void)bus.apply(doc, Json::parse(R"([{"op": "filter_raster", "page": 1, "id": "p", "kind": "plugin:negative"}])"), person, false);
                }).contains("plugin negative is not chosen to run"));
    }

    // plugin.json or the program changed after it was chosen: chosen again (and what the list showed is what is chosen).
    void changedIsChosenAgain() {
        enable(true);
        plugins::choose("negative", true);
        QVERIFY(plugins::allowed("negative"));
        const std::string shown = listed_sha("negative");
        const QString program = folder_ + "/negative/" + program_name_;
        {
            QFile file(program);
            QVERIFY(file.open(QIODevice::Append));
            file.write("changed", 7);
        }
        QVERIFY(!plugins::allowed("negative"));
        const QByteArray ran_before = [&] {
            QFile f(mark());
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        plugins::forget_made();
        QVERIFY(refusal([] { plugins::run("plugin:negative", picture()); }).startsWith("plugin_not_allowed: plugin negative is not chosen to run"));
        {  // (the program changed after the choice never ran)
            QFile f(mark());
            QCOMPARE(f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(), ran_before);
        }
        QVERIFY(refusal([&] { plugins::choose("negative", true, shown); }).startsWith("plugin_changed: plugin negative changed since the list was shown"));
        QVERIFY(!plugins::allowed("negative"));
        plugins::choose("negative", true, listed_sha("negative"));
        QVERIFY(plugins::allowed("negative"));
        // plugin.json changed: the same
        const std::string text = manifest("反転（ネイティブ）", "invert", R"({"red": {"label": "赤", "min": 0, "max": 255, "default": 255}})");
        write(folder_ + "/negative/plugin.json", text + "\n");
        QVERIFY(!plugins::allowed("negative"));
        write(folder_ + "/negative/plugin.json", text);
        QVERIFY(plugins::allowed("negative"));
        // a plugin.json made unusable after the choice: not chosen (nothing started)
        write(folder_ + "/negative/plugin.json", "{");
        QVERIFY(!plugins::allowed("negative"));
        QVERIFY(refusal([] { plugins::run("plugin:negative", picture()); }).startsWith("plugin_not_allowed:"));
        install("negative", text);
        plugins::choose("negative", true);
        enable(false);
    }

    // What runs is a copy of the bytes checked: in a private folder under the settings folder (not the plugin's folder,
    // not the system's temporary one), its working folder too, removed when it has answered; GENKO_PLUGIN_HOME is the
    // plugin's own folder.
    void runsACopy() {
        enable(true);
        plugins::choose("where", true);
        QFile::remove(folder_ + "/where/where.txt");
        QCOMPARE(plugins::run("plugin:where", picture()).getpixel(30, 30), (std::vector<double>{225, 135, 35, 255}));
        QFile where(folder_ + "/where/where.txt");
        QVERIFY(where.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(where.readAll()).split(QLatin1Char('\n'));
        QVERIFY(lines.size() >= 2);
        const QString runs = QDir::cleanPath(config_ + "/plugin-runs");
        QVERIFY2(QDir::cleanPath(QFileInfo(lines[0]).absolutePath()).startsWith(runs + "/run-"), qPrintable(lines[0]));
        QCOMPARE(QFileInfo(lines[0]).fileName(), program_name_);
        QCOMPARE(QDir::cleanPath(lines[1]), QDir::cleanPath(QFileInfo(lines[0]).absolutePath()));  // (its working folder: the copy's)
        QVERIFY2(!QFileInfo::exists(QFileInfo(lines[0]).absolutePath()), "the copy's folder is removed");
        QVERIFY(QDir(config_ + "/plugin-runs").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        enable(false);
    }

    // What cannot be chosen, and a program's trouble.
    void refusals() {
        enable(true);
        const QString cannot = QStringLiteral("plugin_manifest: the plugin.json of plugin %1 cannot be used (%2)");
        QCOMPARE(refusal([] { plugins::choose("badjson", true); }), cannot.arg("badjson", "it is not JSON"));
        QCOMPARE(refusal([] { plugins::choose("noprogram", true); }), cannot.arg("noprogram", "its program missing is not in its folder"));
        QCOMPARE(refusal([] { plugins::choose("outside", true); }), cannot.arg("outside", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(refusal([] { plugins::choose("absolute", true); }), cannot.arg("absolute", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(refusal([] { plugins::choose("linked", true); }), cannot.arg("linked", "its program run is a link"));
        QCOMPARE(refusal([] { plugins::choose("outlink", true); }), cannot.arg("outlink", "its executable must be a path inside the plugin's folder"));
        QCOMPARE(refusal([] { plugins::choose("folderlink", true); }), cannot.arg("folderlink", "its folder is a link"));
        if (unreadable_) {
            QCOMPARE(refusal([] { plugins::choose("unreadable", true); }), cannot.arg("unreadable", "its program " + program_name_ + " cannot be read"));
        }
        QCOMPARE(refusal([] { plugins::choose("twin", false); }), QString());  // (taking a choice back needs nothing)
        QVERIFY(refusal([] { plugins::choose("none", true); }).endsWith("no plugin none"));
        QVERIFY(!plugins::allowed("badjson") && !plugins::allowed("outside"));
        for (const char* key : {"fails", "small", "crasher", "flood", "environment", "noexec"}) plugins::choose(key, true);
        QCOMPARE(refusal([] { plugins::run("plugin:fails", picture()); }), QStringLiteral("value: plugin fails failed (no paper today)"));
        QCOMPARE(refusal([] { plugins::run("plugin:small", picture()); }), QStringLiteral("value: plugin small did not return a picture"));
        QVERIFY(refusal([] { plugins::run("plugin:crasher", picture()); }).startsWith("plugin_runner: plugin crasher stopped before it answered"));
        QVERIFY(refusal([] { plugins::run("plugin:flood", picture()); }).startsWith("plugin_runner: plugin flood wrote back more than a picture"));
        // the environment: no secrets, the protocol, its own folder
        QCOMPARE(plugins::run("plugin:environment", picture()).getpixel(3, 3), (std::vector<double>{0, 255, 77, 255}));
        // a program that cannot be started (bytes no system runs: its copy is made and refused at the start)
        QVERIFY(refusal([] { plugins::run("plugin:noexec", picture()); }).startsWith("plugin_runner: the program of plugin noexec could not be started ("));
        // the ones that can be described are offered, the others not
        std::vector<std::string> offered;
        for (const auto& d : plugins::available()) offered.push_back(d.key);
        QVERIFY(std::find(offered.begin(), offered.end(), "fails") != offered.end());
        QVERIFY(std::find(offered.begin(), offered.end(), "badjson") == offered.end());
        enable(false);
    }

    // Stopped at once when no longer wanted, and after kRunTime when it does not finish.
    void slowIsStopped() {
        enable(true);
        plugins::choose("sleepy", true);
        std::stop_source stop;
        std::thread later([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            stop.request_stop();
        });
        QElapsedTimer clock;
        clock.start();
        bool cancelled = false;
        try {
            plugins::run("plugin:sleepy", picture(), Json::object(), stop.get_token());
        } catch (const genko::render::Cancelled&) {
            cancelled = true;
        }
        later.join();
        QVERIFY(cancelled);
        QVERIFY2(clock.elapsed() < 10000, qPrintable(QString::number(clock.elapsed())));
#ifndef _WIN32
        // what it started was stopped with it (its process group): nothing writes after it
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
        QVERIFY(!QFile::exists(folder_ + "/sleepy/grandchild.txt"));
#endif
        clock.restart();
        QCOMPARE(refusal([] { plugins::run("plugin:sleepy", picture()); }),
                 QStringLiteral("plugin_runner: plugin sleepy did not finish in %1 seconds").arg(plugins::kRunTime.count()));
        QVERIFY(clock.elapsed() >= plugins::kRunTime.count() * 1000 - 1000);
        QVERIFY2(clock.elapsed() < plugins::kRunTime.count() * 1000 + 20000, qPrintable(QString::number(clock.elapsed())));
        // (stopped there as when no longer wanted, its process group with it: checked above, where what it started has
        // not yet written)
        enable(false);
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) qputenv("QTEST_FUNCTION_TIMEOUT", "600000");
    QCoreApplication app(argc, argv);
    TestNativePlugins test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_native_plugins.moc"
