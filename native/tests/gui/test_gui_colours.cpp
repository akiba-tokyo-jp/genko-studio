// カラー (M3④-2, Python's genko/app/colours.py) on the offscreen platform: the colour sets, 中間色 and 近似色 summed
// as Python's colorsys sums them (the reference run with its Qt names standing in), and the panel: the square, the
// hue bar, RGB and hex choosing the pen's colour, the main and sub colours swapped, 履歴, a set of one's own (a
// built-in set copied first) kept in colorsets.json, the corners of 中間色.
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <fstream>
#include <random>
#include "gui_support.hpp"
#include "testsupport.hpp"
#include "app/brush_panel.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "core/json.hpp"
#include "core/poses.hpp"
#include "storage/fsutil.hpp"
using namespace genko;
using core::Json;
using app::Rgb;

namespace {

Json grid_json(const std::vector<std::vector<Rgb>>& grid) {
    Json out = Json::array();
    for (const auto& row : grid) {
        Json line = Json::array();
        for (const Rgb& c : row) line.push_back(Json::array({c[0], c[1], c[2]}));
        out.push_back(line);
    }
    return out;
}

Json sets_json(const std::vector<app::colours::ColourSet>& sets) {
    Json out = Json::object();
    for (const auto& [name, colours] : sets) {
        Json list = Json::array();
        for (const Rgb& c : colours) list.push_back(Json::array({c[0], c[1], c[2]}));
        out[name] = list;
    }
    return out;
}

void write_text(const QString& path, const std::string& text) {
    std::ofstream file(path.toStdString(), std::ios::binary);
    file << text;
}

}  // namespace

class TestGuiColours : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    void sumsLikePython() {
        QTemporaryDir scratch;
        const QString config = scratch.path() + "/config";
        QDir().mkpath(config);
        // one's own sets: a long colour cut to three, numbers as Python's int() reads them, a built-in name taken over;
        // then a set that cannot be read (the reading stops there, the set after it is not read)
        write_text(config + "/colorsets.json",
                   R"({"sets": {"線画": [[10, 20, 30], [4, 5, 6, 7], [1.7, "12", true]], "肌・髪": [[1, 2, 3]], )"
                   R"("壊れた": [[1, 2, 3], [null, 2, 3]], "その後": [[9, 9, 9]]}})");
        std::vector<Rgb> asked{{0, 0, 0}, {255, 255, 255}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {200, 120, 40},
                               {20, 20, 20}, {1, 0, 0}, {255, 254, 255}, {0, 1, 1}, {250, 5, 128}};
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int i = 0; i < 60; ++i) asked.push_back({byte(rng), byte(rng), byte(rng)});
        std::vector<std::array<Rgb, 4>> corners{{Rgb{255, 255, 255}, Rgb{230, 60, 60}, Rgb{60, 90, 220}, Rgb{20, 20, 20}},
                                                {Rgb{0, 0, 0}, Rgb{0, 0, 0}, Rgb{255, 255, 255}, Rgb{255, 255, 255}}};
        for (int i = 0; i < 12; ++i)
            corners.push_back({Rgb{byte(rng), byte(rng), byte(rng)}, Rgb{byte(rng), byte(rng), byte(rng)}, Rgb{byte(rng), byte(rng), byte(rng)},
                               Rgb{byte(rng), byte(rng), byte(rng)}});
        Json request{{"near", Json::array()}, {"between", Json::array()}};
        for (const Rgb& c : asked) request["near"].push_back(Json::array({c[0], c[1], c[2]}));
        for (const auto& four : corners) {
            Json list = Json::array();
            for (const Rgb& c : four) list.push_back(Json::array({c[0], c[1], c[2]}));
            request["between"].push_back(list);
        }
        const QString out = scratch.path() + "/out.json";
        const auto py = genko::test::harness({"colour-grids", config, out, QString::fromStdString(request.dump())}, scratch.path() + "/pyenv");
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const Json want = Json::parse(storage::read_file(storage::path_from_utf8(out.toStdString())));

        QCOMPARE(sets_json(app::colours::built_in_sets()).dump(), want["built_in"].dump());
        QCOMPARE(sets_json(app::colours::load_sets(storage::path_from_utf8(config.toStdString()))).dump(), want["load_sets"].dump());
        for (std::size_t i = 0; i < asked.size(); ++i)
            QCOMPARE(grid_json(app::colours::nearby(asked[i])).dump(), want["near"][i].dump());
        for (std::size_t i = 0; i < corners.size(); ++i)
            QCOMPARE(grid_json(app::colours::between(corners[i])).dump(), want["between"][i].dump());
        // (every hue, saturation and brightness Python's colorsys gives back for a colour comes back the same)
        for (const Rgb& c : asked) {
            const auto hsv = app::colours::rgb_to_hsv(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
            const auto rgb = app::colours::hsv_to_rgb(hsv[0], hsv[1], hsv[2]);
            for (std::size_t k = 0; k < 3; ++k) QVERIFY(std::abs(rgb[k] * 255 - c[k]) < 1e-9);
        }
    }

    void panelChoosesThePensColour() {
        auto store = app::settings();
        for (const char* key : {"colour/sub", "colour/history", "colour/corners"}) store->remove(QString::fromLatin1(key));
        store.reset();
        app::BrushPanel brush;
        app::ColourPanel panel(&brush);
        panel.resize(260, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QCOMPARE(panel.sub_rgb(), (Rgb{255, 255, 255}));
        QCOMPARE(panel.tabs->count(), 4);

        // hex and the numbers
        panel.hex->setText(QStringLiteral(" ##3366cc"));
        emit panel.hex->editingFinished();
        QCOMPARE(brush.rgb(), (Rgb{0x33, 0x66, 0xCC}));
        QCOMPARE(panel.hex->text(), QStringLiteral("#3366CC"));
        QCOMPARE(panel.spins[2]->value(), 0xCC);
        panel.hex->setText(QStringLiteral("#12345G"));  // (not a colour: nothing changes)
        emit panel.hex->editingFinished();
        QCOMPARE(brush.rgb(), (Rgb{0x33, 0x66, 0xCC}));
        panel.spins[0]->setValue(200);
        QCOMPARE(brush.rgb(), (Rgb{200, 0x66, 0xCC}));
        QCOMPARE(panel.main->rgb(), (Rgb{200, 0x66, 0xCC}));

        // the square (white at its top left, black along its bottom) and the hue bar
        panel.transparent->setChecked(true);
        panel.square->pick(QPointF(1, 1));
        QCOMPARE(brush.rgb(), (Rgb{255, 255, 255}));
        QVERIFY(!panel.transparent->isChecked());  // (a colour chosen: the pen draws again)
        panel.square->pick(QPointF(panel.square->width() - 1, 1));
        const auto c = app::colours::hsv_to_rgb(panel.bar->hue, 1, 1);
        QCOMPARE(brush.rgb(), (Rgb{static_cast<int>(std::lround(c[0] * 255)), static_cast<int>(std::lround(c[1] * 255)),
                                   static_cast<int>(std::lround(c[2] * 255))}));
        panel.bar->pick(QPointF(5, 1 + (panel.bar->height() - 2) / 3.0));  // (a third down: green)
        QCOMPARE(brush.rgb(), (Rgb{0, 255, 0}));
        panel.square->pick(QPointF(5, panel.square->height() - 1));
        QCOMPARE(brush.rgb(), (Rgb{0, 0, 0}));

        // main and sub
        brush.set_colour({10, 20, 30});
        panel.swap();
        QCOMPARE(brush.rgb(), (Rgb{255, 255, 255}));
        QCOMPARE(panel.sub_rgb(), (Rgb{10, 20, 30}));
        QCOMPARE(app::settings()->value(QStringLiteral("colour/sub")).toString(), QStringLiteral("10,20,30"));
        QTest::mouseClick(panel.sub, Qt::LeftButton);  // (the sub colour clicked: swapped back)
        QCOMPARE(brush.rgb(), (Rgb{10, 20, 30}));
        QCOMPARE(panel.sub_rgb(), (Rgb{255, 255, 255}));

        // 履歴: the newest first, once each, 24 at most
        for (int i = 0; i < 30; ++i) panel.remember({i, i, i});
        panel.remember({5, 5, 5});
        QCOMPARE(panel.history().size(), std::size_t{24});
        QCOMPARE(panel.history().front(), (Rgb{5, 5, 5}));
        QCOMPARE(panel.history()[1], (Rgb{29, 29, 29}));
        QCOMPARE(std::count(panel.history().begin(), panel.history().end(), Rgb{5, 5, 5}), 1);
        const auto history_swatches = panel.history_page->findChildren<app::Swatch*>();
        QCOMPARE(history_swatches.size(), 24);
        QTest::mouseClick(history_swatches.front(), Qt::LeftButton);
        QCOMPARE(brush.rgb(), (Rgb{5, 5, 5}));
        {
            app::BrushPanel brush2;
            app::ColourPanel again(&brush2);  // (kept in the settings)
            QCOMPARE(again.history(), panel.history());
            QCOMPARE(again.sub_rgb(), panel.sub_rgb());
        }

        // a colour added to a built-in set goes to one's own copy of it, kept in colorsets.json
        panel.set_choice->setCurrentText(QStringLiteral("マンガのグレー"));
        brush.set_colour({1, 2, 3});
        panel.add_to_set();
        QCOMPARE(panel.set_choice->currentText(), QStringLiteral("マンガのグレー（自分）"));
        QCOMPARE(panel.set_box->findChildren<app::Swatch*>().size(), 12);
        const auto file = core::poses::config_dir() / "colorsets.json";
        Json kept = Json::parse(storage::read_file(file));
        QCOMPARE(kept["sets"]["マンガのグレー（自分）"].size(), std::size_t{12});
        QCOMPARE(kept["sets"]["マンガのグレー（自分）"].back().dump(), std::string("[1,2,3]"));
        const auto built = app::colours::load_sets(core::poses::config_dir());
        QCOMPARE(std::find_if(built.begin(), built.end(), [](const auto& s) { return s.first == "マンガのグレー"; })->second.size(),
                 std::size_t{11});  // (the built-in set as it was)
        panel.new_set(QStringLiteral("  線画  "));
        QCOMPARE(panel.set_choice->currentText(), QStringLiteral("線画"));
        kept = Json::parse(storage::read_file(file));
        QCOMPARE(kept["sets"]["線画"].dump(), std::string("[[1,2,3]]"));
        QCOMPARE(kept["sets"].size(), std::size_t{2});
        brush.set_colour({7, 8, 9});
        panel.add_to_set();
        kept = Json::parse(storage::read_file(file));
        QCOMPARE(kept["sets"]["線画"].dump(), std::string("[[1,2,3],[7,8,9]]"));

        // a colorsets.json that cannot be read is left as it is
        write_text(QString::fromStdString(core::path_to_utf8(file)), "{not json");
        gui_test::Answers answers;
        panel.new_set(QStringLiteral("もう一つ"));
        QCOMPARE(storage::read_file(file), std::string("{not json"));
        QCOMPARE(answers.asked.size(), 1);
        QVERIFY(answers.asked.front().startsWith(QStringLiteral("カラーセット: ")));
        std::filesystem::remove(file);

        // 中間色: the corners from the current colour
        brush.set_colour({0, 0, 0});
        panel.set_corner(0);
        brush.set_colour({255, 255, 255});
        panel.set_corner(3);
        QCOMPARE(panel.corners()[0], (Rgb{0, 0, 0}));
        QCOMPARE(app::settings()->value(QStringLiteral("colour/corners")).toString(), QStringLiteral("0,0,0;230,60,60;60,90,220;255,255,255"));
        const auto between = panel.between_page->findChildren<app::Swatch*>();
        QCOMPARE(between.size(), 25);
        QTest::mouseClick(between.front(), Qt::LeftButton);
        QCOMPARE(brush.rgb(), (Rgb{0, 0, 0}));

        // 近似色 follow the colour
        brush.set_colour({200, 120, 40});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto near = panel.near_page->findChildren<app::Swatch*>();
        QCOMPARE(near.size(), 25);
        QCOMPARE(near[12]->rgb(), (Rgb{200, 120, 40}));

        // スポイト's source
        QSignalSpy chosen(&panel, &app::ColourPanel::pick_source_chosen);
        panel.pick_source->setCurrentIndex(1);
        emit panel.pick_source->activated(1);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.front().front().toString(), QStringLiteral("layer"));
    }
};

QTEST_MAIN(TestGuiColours)
#include "test_gui_colours.moc"
