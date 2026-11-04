// WebAssembly filter plugins (PC5 J-f, plugins:WASM-ABI-v1, docs/PLUGIN_WASM.md: render/plugins.hpp, render/plugin_bands,
// genko-plugin-host of native/wasm-host) — the ideas of PhotoCraft v0.2.0's crates/plugins/tests/abi.rs through Genko:
// the example invert.wasm (PhotoCraft's own, 833 bytes) and its manifest, read only once it is chosen; its picture at
// every depth and colour model (RGBA 8, 16, 32 bits, one grey channel, CMYK); bands without seams (a module that reads
// its neighbour, overlap 1) and the selection's mix; the settings and _image reaching the module, a checkbox of the
// filter window given as a number; NaN and the range cleaned; hostile modules (an endless loop, a memory bomb, a trap, an
// error code, an import, a broken manifest) stopped quickly with their reason; files that are not modules or are too
// large; the instruction, time and memory budgets; a module changed after it was chosen chosen again; installing,
// listing (the additional folder too), removing and reading again; a host that falls over or is missing. The modules
// other than invert.wasm are made once from the WAT beside them (native/tests/data/plugins/MANIFEST.json).

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <fstream>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/plugin_bands.hpp"
#include "render/plugins.hpp"

using genko::core::Json;
namespace plugins = genko::render::plugins;
namespace bands = genko::render::plugin_bands;

namespace {

QString data(const QString& name) { return QStringLiteral(GENKO_TEST_DATA "/plugins/") + name; }

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

// PhotoCraft's test picture (abi.rs surface()): RGBA samples, some pixels clear
bands::Samples picture(int w, int h, int depth) {
    bands::Samples s;
    s.width = w;
    s.height = h;
    s.depth = depth;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = static_cast<float>((x * 7 + y * 13) % 31) / 30.0f;
            const float a = (x + y) % 9 == 0 ? 0.0f : 0.25f + 0.75f * (static_cast<float>(x % 4) / 3.0f);
            for (const float sample : {v, 1.0f - v, static_cast<float>(x % 5) / 4.0f, a}) s.data.push_back(sample);
        }
    }
    return s;
}

// An opaque picture (every pixel shown: the content's box is all of it)
bands::Samples opaque(int w, int h) {
    bands::Samples s = picture(w, h, 32);
    for (std::size_t i = 3; i < s.data.size(); i += 4) s.data[i] = 1.0f;
    return s;
}

}  // namespace

class TestWasmPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir scratch_;
    QString config_;
    QString folder_;
    QString host_;

    void put(const QString& name, const QString& as = {}) {
        const QString to = folder_ + "/" + (as.isEmpty() ? name : as) + ".wasm";
        QFile::remove(to);
        QVERIFY2(QFile::copy(data(name + ".wasm"), to), qPrintable(name));
        QVERIFY(QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner));
    }
    void enable(bool on) {
        plugins::Settings s = plugins::settings();
        s.enabled = on;
        plugins::save_settings(s);
    }
    void choose(const std::string& key) {
        enable(true);
        plugins::choose(key, true);
        QVERIFY(plugins::allowed(key));
    }
    const plugins::Listed* listed(const std::vector<plugins::Listed>& all, const std::string& key) {
        for (const auto& item : all) {
            if (item.key == key) return &item;
        }
        return nullptr;
    }

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        config_ = scratch_.path() + "/config";
        folder_ = config_ + "/plugins";
        QVERIFY(QDir().mkpath(folder_));
        qputenv("GENKO_CONFIG_DIR", config_.toUtf8());
        host_ = QStringLiteral(GENKO_TEST_PLUGIN_HOST);
        if (!QFileInfo::exists(host_)) QSKIP("genko-plugin-host was not built (no cargo): the WebAssembly plugins cannot run in this build");
        plugins::set_host_for_testing(genko::core::path_from_utf8((host_).toStdString()));
        for (const char* name : {"invert", "double", "shift", "canvas", "params", "nan", "loop", "bomb", "trap", "refuses", "imports", "badmanifest"})
            put(QString::fromLatin1(name));
        // files that are not modules: a broken one with the header, one without it, one too large
        std::ofstream(genko::core::path_from_utf8((folder_ + "/garbage.wasm").toStdString()), std::ios::binary) << std::string("\0asm junk", 9);
        std::ofstream(genko::core::path_from_utf8((folder_ + "/notwasm.wasm").toStdString()), std::ios::binary) << "MZ\x90";
        QFile big(folder_ + "/big.wasm");
        QVERIFY(big.open(QIODevice::WriteOnly));
        big.write(QByteArray("\0asm", 4));
        QVERIFY(big.resize((qint64{32} << 20) + 1));
        big.close();
    }

    void init() {
        plugins::set_wasm_limits_for_testing(Json());
        plugins::forget_made();
        plugins::set_host_for_testing(genko::core::path_from_utf8((host_).toStdString()));
    }

    // Listed by their files alone — nothing runs to list them (the host is not even there) —, off until chosen.
    void listedWithoutRunning() {
        enable(false);
        plugins::set_host_for_testing(genko::core::path_from_utf8(((scratch_.path() + "/no-such-host")).toStdString()));
        const auto all = plugins::listed();
        const auto* invert = listed(all, "invert");
        QVERIFY(invert != nullptr);
        QVERIFY(invert->wasm && !invert->native && !invert->chosen && !invert->problem);
        QCOMPARE(invert->bytes, std::uintmax_t{833});
        QVERIFY(!invert->manifest);  // (its manifest is the module's to say: asked once it is chosen)
        QVERIFY(QString::fromStdString(invert->path).endsWith(QStringLiteral("/plugins/invert.wasm")));
        QCOMPARE(QString::fromStdString(*listed(all, "notwasm")->problem),
                 QStringLiteral("the module of plugin notwasm cannot be used (it is not a WebAssembly module)"));
        QCOMPARE(QString::fromStdString(*listed(all, "big")->problem), QStringLiteral("the module of plugin big cannot be used (it is larger than 32 MB)"));
        QVERIFY(!listed(all, "garbage")->problem);  // (its header says it is one: what it is, the host says once chosen)
        QVERIFY(plugins::installed("invert"));
        QVERIFY(plugins::works_in_bands("invert"));
        QVERIFY(!plugins::allowed("invert"));
        QVERIFY(refusal([] { (void)plugins::describe("invert"); }).startsWith(QStringLiteral("plugin_not_allowed: plugin invert is not chosen to run")));
        bands::Samples s = picture(8, 8, 8);
        QVERIFY(refusal([&] { plugins::run_samples("plugin:invert", s, nullptr, Json::object()); }).startsWith(QStringLiteral("plugin_not_allowed")));
        enable(true);
        QVERIFY(refusal([] { plugins::choose("notwasm", true); }).contains(QStringLiteral("it is not a WebAssembly module")));
        QVERIFY(refusal([] { plugins::choose("big", true); }).contains(QStringLiteral("larger than 32 MB")));
        enable(false);
    }

    // Chosen: its manifest, asked in its sandbox (abi.rs example_invert_loads_and_reports_its_manifest).
    void invertManifestOnceChosen() {
        choose("invert");
        const plugins::Described d = plugins::describe("invert");
        QCOMPARE(d.name, std::string("Invert (WebAssembly)"));
        QCOMPARE(d.kind, std::string("wasm"));
        QVERIFY(d.params.empty());
        QVERIFY(d.manifest);
        QCOMPARE((*d.manifest)["id"], Json("org.photocraft.example.invert"));
        QCOMPARE((*d.manifest)["version"], Json("1.0.0"));
        QCOMPARE((*d.manifest)["author"], Json("PhotoCraft contributors"));
        bool offered = false;
        for (const plugins::Described& p : plugins::available()) offered = offered || p.key == "invert";
        QVERIFY(offered);
        // (now listed with what it said)
        const auto all = plugins::listed();
        QVERIFY(listed(all, "invert")->chosen);
        QVERIFY(listed(all, "invert")->manifest);
        const Json shown = plugins::listed_json(*listed(all, "invert"));
        QCOMPARE(shown["kind"], Json("wasm"));
        QCOMPARE(shown["name"], Json("Invert (WebAssembly)"));
        QCOMPARE(shown["filter"], Json("plugin:invert"));
        QCOMPARE(shown["size"], Json(833));
    }

    // abi.rs example_invert_inverts_colour_at_every_depth, and the other colour models: one grey channel, CMYK.
    void invertsEveryDepthAndModel() {
        choose("invert");
        for (const int depth : {8, 16, 32}) {
            const bands::Samples in = picture(40, 30, depth);
            bands::Samples out = in;
            plugins::run_samples("plugin:invert", out, nullptr, Json::object());
            for (std::size_t p = 0; p < in.data.size() / 4; ++p) {
                const float* a = in.data.data() + p * 4;
                const float* b = out.data.data() + p * 4;
                if (a[3] <= 0.0f) {
                    for (int c = 0; c < 4; ++c) QCOMPARE(b[c], a[c]);  // (transparent pixels are left alone)
                    continue;
                }
                for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(b[c] - (1.0f - a[c])) < 1e-6f, qPrintable(QString::number(depth)));
                QCOMPARE(b[3], a[3]);  // (alpha is kept)
            }
        }
        bands::Samples grey;  // (an alpha channel, a Quick Mask: one grey channel)
        grey.width = 5;
        grey.height = 3;
        grey.channels = 1;
        grey.alpha = false;
        grey.mode = bands::Mode::Gray;
        for (int i = 0; i < 15; ++i) grey.data.push_back(static_cast<float>(i) / 14.0f);
        plugins::run_samples("plugin:invert", grey, nullptr, Json::object());
        for (int i = 0; i < 15; ++i) QVERIFY(std::abs(grey.data[std::size_t(i)] - (1.0f - static_cast<float>(i) / 14.0f)) < 1e-6f);
        bands::Samples cmyk;  // (a page of inks: C, M, Y, K and the alpha, 16 bits)
        cmyk.width = 4;
        cmyk.height = 2;
        cmyk.channels = 5;
        cmyk.depth = 16;
        cmyk.mode = bands::Mode::Cmyk;
        for (int i = 0; i < 8; ++i) {
            for (const float v : {0.1f, 0.2f, 0.3f, 0.4f, 1.0f}) cmyk.data.push_back(v);
        }
        QCOMPARE(cmyk.format(), 16u | 256u | (4u << 16));
        plugins::run_samples("plugin:invert", cmyk, nullptr, Json::object());
        QVERIFY(std::abs(cmyk.data[0] - 0.9f) < 1e-6f && std::abs(cmyk.data[3] - 0.6f) < 1e-6f && cmyk.data[4] == 1.0f);
        // the 8-bit picture of a paint layer (render::plugins::run, as Python's and native plugins take it)
        genko::render::Image im = genko::render::Image::create("RGBA", genko::render::Size{100, 140}, genko::render::Ink{0, 0, 0, 0});
        im.paste(genko::render::Ink{30, 120, 220, 255}, genko::render::Box{10, 10, 60, 60});
        const genko::render::Image inverted = plugins::run("plugin:invert", im);
        QCOMPARE(inverted.getpixel(30, 30), (std::vector<double>{225, 135, 35, 255}));
        QCOMPARE(inverted.getpixel(90, 130), (std::vector<double>{0, 0, 0, 0}));
    }

    // abi.rs bands_are_seamless_and_selection_blends: a module reading its left neighbour (overlap 1) in tiny bands the
    // same as in large ones, each pixel its neighbour's (the picture's edge repeated); the selection mixes.
    void bandsSeamlessSelectionMixes() {
        choose("shift");
        choose("invert");
        const bands::Samples in = opaque(300, 270);  // (more than one tile each way)
        bands::Samples big = in;
        plugins::run_samples("plugin:shift", big, nullptr, Json::object());
        plugins::set_wasm_limits_for_testing(Json{{"band_bytes", 256 * 256 * 4 * 4}});
        bands::Samples small = in;
        plugins::run_samples("plugin:shift", small, nullptr, Json::object());
        QVERIFY(big.data == small.data);
        for (const auto& [x, y] : std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {255, 3}, {256, 3}, {257, 260}, {299, 269}}) {
            for (int c = 0; c < 4; ++c) {
                const std::size_t at = (std::size_t(y) * 300 + std::size_t(x)) * 4 + std::size_t(c);
                const std::size_t from = (std::size_t(y) * 300 + std::size_t(std::max(0, x - 1))) * 4 + std::size_t(c);
                QCOMPARE(big.data[at], in.data[from]);
            }
        }
        plugins::set_wasm_limits_for_testing(Json());
        // half strength over the top of the left half, full below it, none on the right
        const bands::Samples s = picture(40, 30, 16);
        std::vector<float> cover(40 * 30, 0.0f);
        for (int y = 0; y < 30; ++y) {
            for (int x = 0; x < 20; ++x) cover[std::size_t(y) * 40 + std::size_t(x)] = y < 10 ? 0.5f : 1.0f;
        }
        bands::Samples out = s;
        plugins::run_samples("plugin:invert", out, &cover, Json::object());
        const auto px = [](const bands::Samples& p, int x, int y) { return p.data.data() + (std::size_t(y) * 40 + std::size_t(x)) * 4; };
        for (int c = 0; c < 4; ++c) QCOMPARE(px(out, 30, 20)[c], px(s, 30, 20)[c]);  // (outside the selection)
        QVERIFY(std::abs(px(out, 4, 20)[0] - (1.0f - px(s, 4, 20)[0])) < 1e-6f);    // (fully selected)
        const float a = px(s, 5, 5)[0];
        QVERIFY(std::abs(px(out, 5, 5)[0] - (a + (1.0f - a - a) * 0.5f)) < 1e-6f);  // (half selected)
    }

    // The content's box (area "content", grown by the overlap) and the whole canvas (area "canvas").
    void contentOrCanvas() {
        choose("double");
        choose("canvas");
        bands::Samples s;  // (shown pixels only in the box 10..19 × 5..9 of 64 × 32, all at 0.2)
        s.width = 64;
        s.height = 32;
        s.depth = 32;
        s.data.assign(std::size_t(64) * 32 * 4, 0.2f);
        for (std::size_t i = 3; i < s.data.size(); i += 4) s.data[i] = 0.0f;
        for (int y = 5; y < 10; ++y) {
            for (int x = 10; x < 20; ++x) s.data[(std::size_t(y) * 64 + std::size_t(x)) * 4 + 3] = 1.0f;
        }
        bands::Samples doubled = s;
        plugins::run_samples("plugin:double", doubled, nullptr, Json::object());
        QVERIFY(std::abs(doubled.data[(std::size_t(7) * 64 + 15) * 4] - 0.4f) < 1e-6f);  // (inside the box)
        QCOMPARE(doubled.data[(std::size_t(20) * 64 + 40) * 4], 0.2f);                   // (outside it: not given)
        bands::Samples filled = s;
        plugins::run_samples("plugin:canvas", filled, nullptr, Json::object());
        QCOMPARE(filled.data[(std::size_t(20) * 64 + 40) * 4], 0.5f);  // (every pixel)
        QCOMPARE(filled.data[(std::size_t(20) * 64 + 40) * 4 + 3], 0.5f);
    }

    // abi.rs nan_and_out_of_range_output_is_sanitised: NaN keeps the sample; integer depths and the alpha within 0..1,
    // 32 bits beyond.
    void cleaned() {
        choose("nan");
        choose("double");
        const bands::Samples s = picture(8, 8, 32);
        bands::Samples same = s;
        plugins::run_samples("plugin:nan", same, nullptr, Json::object());
        QVERIFY(same.data == s.data);
        for (const int depth : {8, 16, 32}) {
            bands::Samples d = opaque(4, 4);
            d.depth = depth;
            d.data[0] = 0.8f;
            plugins::run_samples("plugin:double", d, nullptr, Json::object());
            QCOMPARE(d.data[0], depth == 32 ? 1.6f : 1.0f);
            QCOMPARE(d.data[3], 1.0f);
        }
    }

    // abi.rs params_and_image_context_reach_the_plugin: the settings checked against the manifest (a number clamped, a
    // checkbox and a choice given as the filter window's numbers), with _image; a wrong type refused before it runs.
    void settingsReachTheModule() {
        choose("params");
        const plugins::Described d = plugins::describe("params");
        QCOMPARE(d.params["amount"]["max"], Json(10));
        QCOMPARE(d.params["mono"]["max"], Json(1));
        QVERIFY(!d.params.contains("mode"));  // (a choice keeps its default in the filter window)
        const auto fields = plugins::fields("plugin:params");
        QCOMPARE(fields.size(), std::size_t{2});
        bands::Samples s = picture(8, 8, 32);
        plugins::run_samples("plugin:params", s, nullptr, Json{{"amount", 3}, {"mono", 1}, {"mode", 1}});
        const float v = s.data[(std::size_t(1) * 8 + 1) * 4] * 1000.0f;
        QVERIFY2(v > 100.0f && v < 600.0f, qPrintable(QString::number(v)));
        bands::Samples t = picture(8, 8, 32);
        QCOMPARE(refusal([&] { plugins::run_samples("plugin:params", t, nullptr, Json{{"amount", "x"}}); }),
                 QStringLiteral("value: plugin params failed (plug-in parameters: `amount` must be a number)"));
    }

    // abi.rs hostile_modules_fail_quickly and malformed_and_oversized_modules_are_refused, through Genko: each refused
    // with its reason, within seconds, the picture as it was.
    void hostileModulesStopQuickly() {
        enable(true);
        const std::vector<std::pair<std::string, QString>> cases{
            {"loop", QStringLiteral("value: plugin loop failed (plug-in exceeded its instruction budget)")},
            {"bomb", QStringLiteral("value: plugin bomb failed (plug-in exceeded its instruction budget)")},
            {"trap", QStringLiteral("value: plugin trap failed (plug-in trapped: ")},
            {"refuses", QStringLiteral("value: plugin refuses failed (plug-in failed: pc_filter returned error code 7)")},
            {"imports", QStringLiteral("value: the module of plugin imports cannot be used (plug-in ABI: the module imports `wasi_snapshot_preview1.fd_write`")},
            {"badmanifest", QStringLiteral("value: the module of plugin badmanifest cannot be used (plug-in manifest: ")},
            {"garbage", QStringLiteral("value: the module of plugin garbage cannot be used (not a valid plug-in module: ")}};
        for (const auto& [key, said] : cases) {
            plugins::choose(key, true);
            const bands::Samples s = picture(32, 24, 8);
            bands::Samples out = s;
            QElapsedTimer clock;
            clock.start();
            const QString got = refusal([&] { plugins::run_samples("plugin:" + key, out, nullptr, Json::object()); });
            QVERIFY2(clock.elapsed() < 5000, key.c_str());
            QVERIFY2(got.startsWith(said), qPrintable(got));
            QVERIFY(out.data == s.data);
        }
        // a module that cannot be used is left out of the filters, with its reason when read again
        std::vector<std::string> offered;
        for (const plugins::Described& p : plugins::available()) offered.push_back(p.key);
        QVERIFY(std::find(offered.begin(), offered.end(), "imports") == offered.end());
        const auto again = plugins::reload();
        QVERIFY(listed(again, "badmanifest")->problem->find("plug-in manifest") != std::string::npos);
        for (const auto& [key, said] : cases) plugins::choose(key, false);
    }

    // abi.rs budgets_stop_runaway_plugins, wall_clock_budget_stops_runaway_plugins, memory_cap_limits_bands.
    void budgets() {
        choose("loop");
        choose("invert");
        bands::Samples s = picture(32, 24, 8);
        plugins::set_wasm_limits_for_testing(Json{{"fuel_base", 1000000}, {"fuel_per_sample", 10}});
        QCOMPARE(refusal([&] { plugins::run_samples("plugin:loop", s, nullptr, Json::object()); }),
                 QStringLiteral("value: plugin loop failed (plug-in exceeded its instruction budget)"));
        plugins::set_wasm_limits_for_testing(Json{{"wall_time_ms", 1}});
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(refusal([&] { plugins::run_samples("plugin:loop", s, nullptr, Json::object()); }),
                 QStringLiteral("value: plugin loop failed (plug-in exceeded its time budget)"));
        QVERIFY(clock.elapsed() < 3000);
        plugins::set_wasm_limits_for_testing(Json{{"max_memory_bytes", 4 << 20}});
        bands::Samples fits = picture(64, 8, 8);
        QVERIFY(refusal([&] { plugins::run_samples("plugin:invert", fits, nullptr, Json::object()); }).isEmpty());  // (small bands still fit)
        plugins::set_wasm_limits_for_testing(Json{{"max_memory_bytes", 1024}});
        QVERIFY(!refusal([&] { plugins::run_samples("plugin:invert", fits, nullptr, Json::object()); }).isEmpty());
        // limits are only ever lowered: asking for more time and instructions gets the host's own (the loop still stopped)
        plugins::set_wasm_limits_for_testing(Json{{"wall_time_ms", 99999999}, {"fuel_base", std::uint64_t{1} << 62}});
        clock.restart();
        QCOMPARE(refusal([&] { plugins::run_samples("plugin:loop", s, nullptr, Json::object()); }),
                 QStringLiteral("value: plugin loop failed (plug-in exceeded its instruction budget)"));
        QVERIFY(clock.elapsed() < 5000);
    }

    // A module changed after it was chosen is chosen again (its bytes are checked as they go to the host).
    void changedIsChosenAgain() {
        put("invert", "changing");
        choose("changing");
        const std::string shown = [&] {
            for (const auto& item : plugins::listed())
                if (item.key == "changing") return item.sha256;
            return std::string();
        }();
        {
            QFile file(folder_ + "/changing.wasm");
            QVERIFY(file.open(QIODevice::Append));
            file.write("\0", 1);
        }
        QVERIFY(!plugins::allowed("changing"));
        bands::Samples s = picture(8, 8, 8);
        QVERIFY(refusal([&] { plugins::run_samples("plugin:changing", s, nullptr, Json::object()); }).startsWith(QStringLiteral("plugin_not_allowed")));
        QVERIFY(refusal([&] { plugins::choose("changing", true, shown); }).startsWith(QStringLiteral("plugin_changed")));
        QFile::remove(folder_ + "/changing.wasm");
    }

    // plugin.install, plugin.list, plugin.remove, plugin.reload; the additional plugins folder.
    void installListRemoveReload() {
        enable(true);
        const QString elsewhere = scratch_.path() + "/downloads";
        QVERIFY(QDir().mkpath(elsewhere));
        QVERIFY(QFile::copy(data("invert.wasm"), elsewhere + "/myinvert.wasm"));
        QCOMPARE(plugins::install(genko::core::path_from_utf8((elsewhere + "/myinvert.wasm").toStdString())), std::string("myinvert"));
        QVERIFY(QFileInfo::exists(folder_ + "/myinvert.wasm"));
        const auto* added = listed(plugins::listed(), "myinvert");
        QVERIFY(added != nullptr && added->wasm && !added->chosen);  // (installed: not chosen)
        std::ofstream(genko::core::path_from_utf8((elsewhere + "/notes.txt").toStdString())) << "x";
        QVERIFY(refusal([&] { (void)plugins::install(genko::core::path_from_utf8((elsewhere + "/notes.txt").toStdString())); }).startsWith(QStringLiteral("plugin_install: only WebAssembly modules")));
        std::ofstream(genko::core::path_from_utf8((elsewhere + "/fake.wasm").toStdString())) << "MZ";
        QCOMPARE(refusal([&] { (void)plugins::install(genko::core::path_from_utf8((elsewhere + "/fake.wasm").toStdString())); }),
                 QStringLiteral("plugin_install: fake.wasm is not a WebAssembly module"));
        std::ofstream(genko::core::path_from_utf8((folder_ + "/twin.py").toStdString())) << "def run(image):\n    return image\n";
        QVERIFY(QFile::copy(data("invert.wasm"), elsewhere + "/twin.wasm"));
        QCOMPARE(refusal([&] { (void)plugins::install(genko::core::path_from_utf8((elsewhere + "/twin.wasm").toStdString())); }),
                 QStringLiteral("plugin_install: a plugin of the name twin of another kind is installed"));
        plugins::choose("myinvert", true);
        plugins::remove("myinvert");
        QVERIFY(!QFileInfo::exists(folder_ + "/myinvert.wasm"));
        QVERIFY(listed(plugins::listed(), "myinvert") == nullptr);
        QVERIFY(!plugins::settings().chosen.contains("myinvert"));
        QVERIFY(refusal([] { plugins::remove("nothing"); }).contains(QStringLiteral("no plugin nothing")));
        plugins::remove("twin");  // (a Python file too)
        QVERIFY(!QFileInfo::exists(folder_ + "/twin.py"));
        // the additional folder: listed after the plugins' folder, a name taken there first said so, removed there by hand
        const QString extra = scratch_.path() + "/extra";
        QVERIFY(QDir().mkpath(extra));
        QVERIFY(QFile::copy(data("invert.wasm"), extra + "/outside.wasm"));
        QVERIFY(QFile::copy(data("double.wasm"), extra + "/invert.wasm"));
        plugins::Settings s = plugins::settings();
        s.use_extra_folder = true;
        s.extra_folder = extra.toStdString();
        plugins::save_settings(s);
        const auto all = plugins::reload();
        QVERIFY(listed(all, "outside") != nullptr);
        QVERIFY(QString::fromStdString(listed(all, "outside")->path).startsWith(extra));
        int inverts = 0;
        for (const auto& item : all) inverts += item.key == "invert" ? 1 : 0;
        QCOMPARE(inverts, 2);  // (the plugins' folder's, and the other said to come after it)
        plugins::choose("outside", true);
        QCOMPARE(plugins::describe("outside").name, std::string("Invert (WebAssembly)"));
        QVERIFY(refusal([] { plugins::remove("outside"); }).startsWith(QStringLiteral("plugin_install: plugin outside is in the additional plugins folder")));
        s.use_extra_folder = false;
        plugins::save_settings(s);
        QVERIFY(listed(plugins::listed(), "outside") == nullptr);
        QVERIFY(!plugins::allowed("outside"));
    }

    // A host that falls over, or none: the run refused as the runner's trouble (render::plugins' callers leave the book as
    // it was), never the app.
    void hostThatFallsOverOrIsMissing() {
        choose("invert");
        const QString crash = scratch_.path() + "/crash-host";
        QFile::remove(crash);
        QVERIFY(QFile::copy(QStringLiteral(GENKO_TEST_NATIVE_PLUGIN), crash));
        QVERIFY(QFile::setPermissions(crash, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        plugins::set_host_for_testing(genko::core::path_from_utf8((crash).toStdString()));
        bands::Samples s = picture(8, 8, 8);
        QVERIFY2(refusal([&] { plugins::run_samples("plugin:invert", s, nullptr, Json::object()); }).startsWith(QStringLiteral("plugin_runner: plugin invert stopped before it answered")),
                 qPrintable(refusal([&] { plugins::run_samples("plugin:invert", s, nullptr, Json::object()); })));
        plugins::set_host_for_testing(genko::core::path_from_utf8(((scratch_.path() + "/no-such-host")).toStdString()));
        QCOMPARE(refusal([&] { plugins::run_samples("plugin:invert", s, nullptr, Json::object()); }),
                 QStringLiteral("plugin_runner: the WebAssembly plugins need genko-plugin-host beside Genko (it was not found), so plugin invert cannot run"));
        // (a Python plugin takes 8-bit pictures only)
        std::ofstream(genko::core::path_from_utf8((folder_ + "/eight.py").toStdString())) << "def run(image):\n    return image\n";
        QVERIFY(!plugins::works_in_bands("eight"));
        QVERIFY(refusal([&] { plugins::run_samples("plugin:eight", s, nullptr, Json::object()); }).startsWith(QStringLiteral("plugin_8bit")));
        QFile::remove(folder_ + "/eight.py");
    }
};

QTEST_GUILESS_MAIN(TestWasmPlugins)
#include "test_wasm_plugins.moc"
