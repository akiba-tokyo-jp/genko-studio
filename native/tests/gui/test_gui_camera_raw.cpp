// PC4 F-e (SPEC.md FILE-01「カメラ RAW」, design 5.3 and 8-3 (a)): Camera Raw's window when a camera RAW file is opened —
// the RAW stage at the top of 基本補正 (the camera's white balance shown as its temperature, the panel's relative ones
// hidden), its preview made on a thread, 画像を開く a page of the development, Shift (and スマートオブジェクトとして開く)
// オブジェクトを開く a smart object of the RAW, 完了 the settings remembered for the file and nothing opened, キャンセル nothing —;
// a RAW opened with Shift held, without the window, as 環境設定 → Camera Raw sets it; 環境設定 → Camera Raw's defaults; a RAW
// placed by 埋め込みを配置… and its コンテンツを編集 in the same window (set_camera_raw, one step to undo). The RAW files are
// PhotoCraft's synthetic ones (tests/support/raw_testgen).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>

#include "app/camera_raw_open.hpp"
#include "app/camera_raw_window.hpp"
#include "app/history_panel.hpp"
#include "app/icons.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "core/color_model.hpp"
#include "core/smart.hpp"
#include "gui_support.hpp"
#include "raw_testgen.hpp"
#include "render/raw.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::Session;
using genko::core::Json;
namespace core = genko::core;
namespace rawgen = genko::test::rawgen;
namespace cro = genko::app::camera_raw_open;

namespace {

constexpr std::array<double, 9> kSrgbCamera{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};

// A flat grey of `level` under the camera's white (0.5, 1, 0.7): a 96 × 64 DNG of 12 bits.
std::string grey_dng(double level) {
    const std::size_t w = 96, h = 64;
    std::vector<std::array<float, 3>> rgb(w * h);
    const std::array<double, 3> neutral{0.5, 1.0, 0.7};
    for (auto& p : rgb)
        for (int c = 0; c < 3; ++c) p[std::size_t(c)] = static_cast<float>(level * neutral[std::size_t(c)]);
    rawgen::DngSpec d = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(rgb, w, {0, 1, 1, 2}, 64, 4000));
    d.bits = 12;
    d.white = 4000;
    d.black = {64};
    d.as_shot_neutral = neutral;
    d.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
    return d.build();
}

template <class T>
T* child(QDialog* d, const char* name) {
    return d->findChild<T*>(QString::fromLatin1(name));
}

// A desk: a window on a saved book, Camera Raw's window answered by `raw` (any other dialog: cancelled).
struct Desk {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    Answers answers;
    std::unique_ptr<MainWindow> window;
    std::function<int(QDialog*)> raw;
    int raw_windows = 0;

    Desk() {
        book = path_of(dir.filePath("book.genko"));
        make_book(book, 1);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        answers.responder->exec = [this](QDialog* d) {
            answers.asked << QString::fromLatin1(d->metaObject()->className()) + QStringLiteral(" ") + d->objectName();
            if (d->objectName() != QLatin1String("camera_raw_open")) return 0;
            ++raw_windows;
            return raw ? raw(d) : 0;
        };
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    fs::path file(const char* name, const std::string& bytes) const {
        const fs::path path = path_of(dir.filePath(QString::fromUtf8(name)));
        genko::test::write_bytes(qpath(path), bytes);
        return path;
    }
    int tabs() const { return static_cast<int>(window->documents().size()); }
    const core::Document& doc() const { return window->session().document(); }
};

// Wait for the window's preview, then press a button (with these keys held).
int press(QDialog* d, const char* button, Qt::KeyboardModifiers keys = Qt::NoModifier) {
    if (!wait_for([&] { return d->property("raw_previews").toInt() >= 1; })) return 0;
    auto* b = child<QPushButton>(d, button);
    if (b == nullptr) return 0;
    QTest::mouseClick(b, Qt::LeftButton, keys);
    return d->result();
}

}  // namespace

class TestGuiCameraRaw : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    void init() { cro::set_workflow(cro::Workflow{}); }

    // 開く… of a RAW: Camera Raw's window — the RAW stage at the top of 基本補正 (撮影時の設定 shown as its temperature, the
    // panel's 色温度・色かぶり補正・露光量 hidden), its preview, the ワークフロー of 環境設定 —; 画像を開く: a page of the
    // development in a tab of its own, the RAW file never written.
    void theWindowWhenOpening() {
        Desk d;
        const std::string bytes = grey_dng(0.18);
        const fs::path dng = d.file("IMG_0100.dng", bytes);
        QString white, space, stage_shown;
        int temperature = 0;
        bool relative_hidden = false, parts = false;
        d.raw = [&](QDialog* w) {
            if (!wait_for([&] { return w->property("raw_previews").toInt() >= 1; })) return 0;
            white = child<QComboBox>(w, "raw_white_balance")->currentData().toString();
            temperature = child<QSpinBox>(w, "raw_temperature")->value();
            space = child<QComboBox>(w, "raw_space")->currentData().toString();
            parts = child<QWidget>(w, "raw_stage") != nullptr && child<QWidget>(w, "camera_raw_panel") != nullptr;
            // (the panel's relative white balance and exposure hidden: the RAW's own replace them)
            auto* exposure = w->findChild<QWidget*>(QStringLiteral("camera_raw_exposure"));
            relative_hidden = exposure == nullptr || !exposure->isVisibleTo(w);
            // (a change of the RAW stage: a new preview)
            const int shown = w->property("raw_previews").toInt();
            child<QDoubleSpinBox>(w, "raw_exposure")->setValue(0.5);
            if (!wait_for([&] { return w->property("raw_previews").toInt() > shown; })) return 0;
            stage_shown = child<QLabel>(w, "raw_status")->text();
            return press(w, "raw_open");
        };
        d.window->open_path(dng);
        QVERIFY(wait_for([&] { return d.tabs() == 2; }, 30000));
        QCOMPARE(d.raw_windows, 1);
        QCOMPARE(white, QStringLiteral("as_shot"));
        QVERIFY2(temperature > 2000 && temperature < 50000, qPrintable(QString::number(temperature)));
        QCOMPARE(space, QStringLiteral("adobe-rgb-compat"));
        QVERIFY(relative_hidden);
        QVERIFY(parts);
        QCOMPARE(d.doc().title, std::string("IMG_0100"));
        const core::Page& page = d.doc().page(0);
        QVERIFY(page.rgb);
        QCOMPARE(page.rgb->depth, 16);
        QCOMPARE(page.rgb->profile, core::builtin_profile("adobe-rgb-compat")->ref());
        QCOMPARE(page.native_source->format, std::string("raw"));
        QVERIFY(!d.window->session().path());
        QCOMPARE(genko::test::read_bytes(qpath(dng)), bytes);
    }

    // Shift: the button says オブジェクトを開く and opens a smart object of the RAW (camera-raw@1); キャンセル opens nothing.
    void shiftOpensAnObject() {
        Desk d;
        const fs::path dng = d.file("obj.dng", grey_dng(0.3));
        QString held, let_go;
        d.raw = [&](QDialog* w) {
            if (!wait_for([&] { return w->property("raw_previews").toInt() >= 1; })) return 0;
            auto* open = child<QPushButton>(w, "raw_open");
            QTest::keyPress(w, Qt::Key_Shift, Qt::ShiftModifier);
            held = open->text();
            QTest::keyRelease(w, Qt::Key_Shift);
            let_go = open->text();
            const int pressed = press(w, "raw_open", Qt::ShiftModifier);
            QTest::keyRelease(w, Qt::Key_Shift);  // (the application's keys let go: the next 開く… is not one with Shift held)
            return pressed;
        };
        d.window->open_path(dng);
        QVERIFY(wait_for([&] { return d.tabs() == 2; }, 30000));
        QCOMPARE(held, QStringLiteral("オブジェクトを開く"));
        QCOMPARE(let_go, QStringLiteral("画像を開く"));
        const core::Page& page = d.doc().page(0);
        QCOMPARE(page.layers.size(), std::size_t(1));
        QCOMPARE(page.layers[0].kind, core::LayerKind::Smart);
        const core::SmartContents* contents = core::smart::contents_of(d.doc(), page.layers[0]);
        QVERIFY(contents != nullptr && contents->format == "raw" && contents->raw);
        // (キャンセル: nothing)
        d.raw = [&](QDialog* w) { return press(w, "raw_cancel"); };
        d.window->open_path(d.file("none.dng", grey_dng(0.4)));
        QVERIFY(wait_for([&] { return d.raw_windows == 2; }, 30000));
        QTest::qWait(200);
        QCOMPARE(d.tabs(), 2);
    }

    // 完了: the settings remembered for the file (nothing opened), the window starting from them the next time.
    void doneRemembers() {
        Desk d;
        const fs::path dng = d.file("kept.dng", grey_dng(0.2));
        d.raw = [&](QDialog* w) {
            if (!wait_for([&] { return w->property("raw_previews").toInt() >= 1; })) return 0;
            child<QDoubleSpinBox>(w, "raw_exposure")->setValue(1.25);
            child<QComboBox>(w, "raw_space")->setCurrentIndex(child<QComboBox>(w, "raw_space")->findData(QStringLiteral("srgb")));
            return press(w, "raw_done");
        };
        d.window->open_path(dng);
        QVERIFY(wait_for([&] { return d.raw_windows == 1; }, 30000));
        QTest::qWait(200);
        QCOMPARE(d.tabs(), 1);
        double exposure = 0;
        QString space;
        d.raw = [&](QDialog* w) {
            exposure = child<QDoubleSpinBox>(w, "raw_exposure")->value();
            space = child<QComboBox>(w, "raw_space")->currentData().toString();
            return press(w, "raw_cancel");
        };
        d.window->open_path(dng);
        QVERIFY(wait_for([&] { return d.raw_windows == 2; }, 30000));
        QCOMPARE(exposure, 1.25);
        QCOMPARE(space, QStringLiteral("srgb"));
    }

    // 環境設定 → Camera Raw: its defaults (Adobe RGB, 16 bits, 300 ppi, なし, no smart object, no 自動補正), kept; a RAW opened
    // with Shift held — no window — as they say.
    void thePreferences() {
        Desk d;
        genko::app::preferences::PreferencesDialog prefs(d.window.get());
        QStringList pages;
        for (int i = 0; i < prefs.tabs->count(); ++i) pages << prefs.tabs->tabText(i);
        QVERIFY2(pages.contains(QStringLiteral("Camera Raw")), qPrintable(pages.join(QStringLiteral(" | "))));
        QCOMPARE(prefs.pages_list->count(), prefs.tabs->count());
        QCOMPARE(prefs.raw_space->currentData().toString(), QStringLiteral("adobe-rgb-compat"));
        QCOMPARE(prefs.raw_depth->currentData().toString(), QStringLiteral("16"));
        QCOMPARE(prefs.raw_resolution->value(), 300);
        QCOMPARE(prefs.raw_sharpen->currentData().toString(), QStringLiteral("none"));
        QVERIFY(!prefs.raw_smart->isChecked() && !prefs.raw_auto->isChecked());
        prefs.raw_space->setCurrentIndex(prefs.raw_space->findData(QStringLiteral("srgb")));
        prefs.raw_depth->setCurrentIndex(prefs.raw_depth->findData(QStringLiteral("8")));
        prefs.raw_resolution->setValue(240);
        prefs.save();
        const cro::Workflow flow = cro::workflow();
        QCOMPARE(flow.space, std::string("srgb"));
        QCOMPARE(flow.depth, 8);
        QCOMPARE(flow.dpi, 240.0);
        d.window->open_raw_file(d.file("shift.dng", grey_dng(0.18)), false);
        QVERIFY(wait_for([&] { return d.tabs() == 2; }, 30000));
        QCOMPARE(d.raw_windows, 0);
        const core::Page& page = d.doc().page(0);
        QVERIFY(!page.rgb && !page.color);  // (sRGB 8 bits: an 8-bit page of no profile)
        QCOMPARE(page.spec.dpi.value(), 240.0);
        // (and the window starts from them)
        QString space;
        d.raw = [&](QDialog* w) {
            space = child<QComboBox>(w, "raw_space")->currentData().toString();
            return press(w, "raw_cancel");
        };
        d.window->open_path(d.file("window.dng", grey_dng(0.18)));
        QVERIFY(wait_for([&] { return d.raw_windows == 1; }, 30000));
        QCOMPARE(space, QStringLiteral("srgb"));
    }

    // 埋め込みを配置… of a RAW: a smart object at camera-raw@1's defaults, then Camera Raw's window (OK: set_camera_raw);
    // its コンテンツを編集 the same window, one step to undo.
    void aPlacedRawsContents() {
        Desk d;
        const fs::path dng = d.file("placed.dng", grey_dng(0.18));
        bool editing = false;
        d.raw = [&](QDialog* w) {
            editing = child<QPushButton>(w, "raw_done") == nullptr && child<QPushButton>(w, "raw_ok") != nullptr;  // (a smart object's window)
            if (!wait_for([&] { return w->property("raw_previews").toInt() >= 1; })) return 0;
            child<QDoubleSpinBox>(w, "raw_exposure")->setValue(1.0);
            return press(w, "raw_ok");
        };
        QVERIFY(d.window->place_embedded(qpath(dng)));
        QCOMPARE(d.raw_windows, 1);
        QVERIFY(editing);
        std::string id;
        for (const auto& [key, contents] : d.doc().smart_contents)
            if (contents && contents->format == "raw") id = key;
        QVERIFY(!id.empty());
        QCOMPARE((*d.doc().smart_contents.at(id)->raw)["develop"]["exposure_ev"], Json(1.0));
        // (コンテンツを編集: the window again, from the development it has)
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : d.doc().page(0).layers)
            if (core::smart::contents_id(l) == id) layer = &l;
        QVERIFY(layer != nullptr);
        d.window->set_target_layer(layer->id);
        double started = 0;
        d.raw = [&](QDialog* w) {
            started = child<QDoubleSpinBox>(w, "raw_exposure")->value();
            if (!wait_for([&] { return w->property("raw_previews").toInt() >= 1; })) return 0;
            child<QComboBox>(w, "raw_depth")->setCurrentIndex(child<QComboBox>(w, "raw_depth")->findData(QStringLiteral("8")));
            return press(w, "raw_ok");
        };
        QVERIFY(d.window->edit_contents());
        QCOMPARE(started, 1.0);
        QCOMPARE(d.doc().smart_contents.at(id)->depth, 8);
        d.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(wait_for([&] { return d.doc().smart_contents.at(id)->depth == 16; }));
        QCOMPARE((*d.doc().smart_contents.at(id)->raw)["develop"]["exposure_ev"], Json(1.0));
        // (review F-e P3-2: its step's name in 作業の履歴 — an op of the book's, not one inside the contents)
        QCOMPARE(genko::app::history::describe(Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}}})),
                 QStringLiteral("Camera Raw で現像の設定を変えた"));
        // (review F-e P2-3: コンテンツを置換 with a NEF — a TIFF-shaped RAW, known by its name —: a RAW's contents)
        const fs::path nef = d.file("other.nef", rawgen::tiff_ep("NIKON", "D3X", 64, 48,
                                                                 rawgen::mosaic(rawgen::scene(64, 48), 64, {0, 1, 1, 2}, 0, 4095), {0, 1, 1, 2}, 12));
        QVERIFY(d.window->replace_contents(qpath(nef)));
        const core::SmartContents* replaced = nullptr;
        for (const core::Layer& l : d.doc().page(0).layers)
            if (l.kind == core::LayerKind::Smart) replaced = core::smart::contents_of(d.doc(), l);
        QVERIFY(replaced != nullptr);
        QCOMPARE(replaced->format, std::string("raw"));
        QCOMPARE(replaced->suffix, std::string(".nef"));
    }
};

QTEST_MAIN(TestGuiCameraRaw)
#include "test_gui_camera_raw.moc"
