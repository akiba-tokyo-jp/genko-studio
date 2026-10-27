// PC4 E2 (brush.presets.list・.save・.delete・.importAbr, brush.defineFromSelection, edit.presets.*; SPEC.md COLOR-09
// 「ブラシのプリセット」「プリセットマネージャー」): the brushes of this machine (render/brush_presets) — the built-in groups
// (Genko's and PhotoCraft v0.2.0's 36 dab brushes, paint/src/presets.rs: grouped, named once, each drawable), the person's
// in brushes.json with their groups and their pictures in brush_tips/ (saved, renamed, moved, grouped, deleted; a file
// that cannot be read never written over; a library without groups written as before), a Photoshop file's brushes taken
// in, a brush defined from a page's pixels (darkness × alpha × the area), and the presets of every kind written to one
// file, read back and taken from another settings folder (render/preset_files: upstream's edit_menu_cmds and
// migrate_cmds tests).

#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <set>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/vector.hpp"
#include "render/abr.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/dabs/tile.hpp"
#include "render/gradient_presets.hpp"
#include "render/ops_registry.hpp"
#include "render/pattern_library.hpp"
#include "render/png.hpp"
#include "render/preset_files.hpp"
#include "render/preset_store.hpp"
#include "render/raster.hpp"
#include "render/shape_presets.hpp"
#include "render/tool_presets.hpp"
#include "storage/asset_store.hpp"

using namespace genko;
using core::Json;
namespace presets = render::brush_presets;
namespace fs = std::filesystem;

namespace {

std::string text_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// a grey PNG (dark marks on light paper as a picture file gives a tip)
std::string tip_png(int w, int h) {
    std::string grey;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) grey.push_back(static_cast<char>((x + y) % 3 == 0 ? 255 : 40));
    return render::write_png(render::Image::frombytes("L", render::Size{w, h}, grey), 6);
}

Json dab_brush(const std::string& label) {
    return Json{{"label", label}, {"engine", "dabs"}, {"dabs", Json{{"hardness", 0.5}, {"spacing", 0.2}, {"tip", Json{{"png", core::b64encode(tip_png(12, 9))}}}}}};
}

const presets::Preset* own(const presets::Library& lib, const std::string& name) {
    for (const presets::Group& g : lib.groups) {
        if (g.builtin) continue;
        for (const presets::Preset& p : g.items)
            if (p.name == name) return &p;
    }
    return nullptr;
}

std::vector<std::string> own_groups(const presets::Library& lib) {
    std::vector<std::string> out;
    for (const presets::Group& g : lib.groups)
        if (!g.builtin) out.push_back(g.name);
    return out;
}

// A page 2 inches square of one paint layer "paint": white, with a black box (x, y, w, h pixels at 200 dpi).
core::Document boxed_book(int side_px, int bx, int by, int bw, int bh) {
    const double inches = side_px / 200.0;
    core::Document doc = core::new_episode("ブラシ", core::Num(1), 1, core::PageSpec::custom(inches * 25.4, inches * 25.4, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    doc.edit_page(0).numero = false;
    auto& p = doc.edit_page(0);
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    std::string bytes;
    bytes.reserve(std::size_t(side_px) * side_px * 4);
    for (int y = 0; y < side_px; ++y) {
        for (int x = 0; x < side_px; ++x) {
            const bool ink = x >= bx && x < bx + bw && y >= by && y < by + bh;
            bytes += ink ? std::string("\x00\x00\x00\xFF", 4) : std::string("\xFF\xFF\xFF\xFF", 4);
        }
    }
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{side_px, side_px}, bytes));
    p.layers.push_back(layer);
    return doc;
}

}  // namespace

class TestBrushPresets : public QObject {
    Q_OBJECT

private slots:
    // paint/src/presets.rs builtins_are_grouped_and_unique
    void builtinsAreGroupedAndUnique() {
        const auto& groups = presets::builtin_groups();
        QCOMPARE(groups.size(), std::size_t(5));
        QCOMPARE(groups[0].name, std::string(presets::kGenkoGroup));
        QCOMPARE(groups[0].items.size(), core::builtin_brushes().size());
        const std::vector<std::string> names{"一般", "ドライメディア", "ウェットメディア", "特殊効果"};
        std::set<std::string> labels, keys;
        std::size_t dabs = 0;
        for (std::size_t g = 1; g < groups.size(); ++g) {
            QCOMPARE(groups[g].name, names[g - 1]);
            QVERIFY(groups[g].items.size() >= 6);
            for (const presets::Preset& p : groups[g].items) {
                QVERIFY(p.builtin);
                QVERIFY(p.key.starts_with(presets::kBuiltinPrefix));
                QVERIFY(p.key.size() <= 40);
                QVERIFY(labels.insert(p.name).second);
                QVERIFY(keys.insert(p.key).second);
                QVERIFY(core::is_dab_brush(p.settings));
                const core::Brush b = core::brush_from_dict(p.key, p.settings, std::nullopt, {});
                QVERIFY(b.dabs);
                for (const std::string& ref : core::dab_assets(*b.dabs)) QVERIFY2(render::dabs::picture(ref) != nullptr, p.name.c_str());
                ++dabs;
            }
        }
        QCOMPARE(dabs, std::size_t(36));
        // the pressure variants follow the pressure (upstream's check)
        const presets::Preset* ps = presets::builtin("my_genko_hard_round_psize");
        QVERIFY(ps != nullptr);
        QVERIFY(core::dabs_from_json(ps->settings["dabs"]).shape.size.control == core::DabControl::Pressure);
        // the keys a book takes for them, and the person's
        QVERIFY(presets::builtin_dab_of("my_genko_chalk_2") == presets::builtin("my_genko_chalk"));
        QVERIFY(presets::builtin_dab_of("my_genko_chalk_x") == nullptr);
        QVERIFY(presets::builtin_dab_of("gpen") == nullptr);
        QVERIFY(presets::is_own("my_abc") && !presets::is_own("my_genko_chalk") && !presets::is_own("gpen"));
    }

    // brush_cmds/tests.rs presets_save_load_delete_round_trip, in the settings folder's library
    void savedWithGroupAndPictures() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        const Json made = presets::save(config, std::nullopt, dab_brush("にじみ"), std::nullopt, std::string("インク"));
        const std::string key = made["key"].get<std::string>();
        QVERIFY(key.starts_with("my_"));
        QCOMPARE(made["group"].get<std::string>(), std::string("インク"));
        const Json file = core::parse_python_json(text_of(render::brushes::library_path(config)));
        QCOMPARE(file["brushes"][key]["group"].get<std::string>(), std::string("インク"));
        QCOMPARE(file["groups"], Json::array({"インク"}));
        // its tip's picture beside it, named by its ref
        const std::string ref = file["brushes"][key]["dabs"]["tip"]["asset"].get<std::string>();
        QVERIFY(fs::exists(render::brushes::library_tip_path(config, ref)));
        QVERIFY(render::brushes::library_tip(config, ref) != nullptr);
        presets::Library lib = presets::load(config);
        QVERIFY(lib.problem.empty());
        QCOMPARE(own_groups(lib), std::vector<std::string>{"インク"});
        QVERIFY(own(lib, "にじみ") != nullptr);
        // a name taken gets " 2"; a built-in's too
        QCOMPARE(presets::save(config, std::nullopt, dab_brush("にじみ"), std::nullopt, std::nullopt)["name"].get<std::string>(), std::string("にじみ 2"));
        QCOMPARE(presets::save(config, std::nullopt, Json{{"label", "x"}, {"base", "gpen"}}, std::string("G ペン"), std::nullopt)["name"].get<std::string>(),
                 std::string("G ペン 2"));
        // saved again under its key: in its place, its group kept
        const Json again = presets::save(config, key, dab_brush("にじみ（太）"), std::nullopt, std::nullopt);
        QCOMPARE(again["group"].get<std::string>(), std::string("インク"));
        lib = presets::load(config);
        QVERIFY(own(lib, "にじみ（太）") != nullptr && own(lib, "にじみ") == nullptr);
        // the built-in ones are never changed
        for (const std::string& builtin : {std::string("gpen"), std::string("my_genko_chalk")}) {
            try {
                presets::save(config, builtin, dab_brush("x"), std::nullopt, std::nullopt);
                QFAIL("a built-in brush was changed");
            } catch (const core::Error& e) {
                QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("value"));
            }
        }
        try {
            presets::remove(config, {"ハード円ブラシ"});
            QFAIL("a built-in brush was deleted");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("value"));
        }
        // deleted by its name
        QCOMPARE(presets::remove(config, {"にじみ 2"})["deleted"].size(), std::size_t(1));
        try {
            presets::remove(config, {"にじみ 2"});
            QFAIL("a brush not there was deleted");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("key"));
        }
        // the brushes the library keeps are drawn as they were saved (brush_from_dict takes them), their pictures read from
        // the library only when a line is drawn (use_library_pictures)
        render::dabs::clear_pictures();
        QVERIFY(!render::dabs::holds_picture(ref));
        presets::use_library_pictures(config);
        QVERIFY(render::dabs::picture(ref) != nullptr);
        render::dabs::set_picture_source(nullptr);
    }

    void aLibraryWithoutGroupsIsWrittenAsBefore() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        presets::save(config, std::string("my_a"), Json{{"label", "細い線"}, {"base", "gpen"}, {"width_mm", 0.35}}, std::nullopt, std::nullopt);
        const Json file = core::parse_python_json(text_of(render::brushes::library_path(config)));
        QVERIFY(!file.contains("groups"));
        QVERIFY(!file["brushes"]["my_a"].contains("group"));
        QCOMPARE(presets::load(config).groups.back().name, std::string(presets::kOwnGroup));
        // (review P3-1) renamed, another saved, moved within 自分のブラシ and deleted: still no groups written
        presets::save(config, std::string("my_b"), Json{{"label", "B"}, {"base", "maru"}}, std::nullopt, std::nullopt);
        presets::edit(config, Json{{"action", "rename"}, {"preset", "細い線"}, {"name", "極細"}});
        presets::edit(config, Json{{"action", "move"}, {"preset", "B"}, {"to", std::string(presets::kOwnGroup)}, {"index", 0}});
        presets::edit(config, Json{{"action", "delete"}, {"preset", "B"}});
        const Json after = core::parse_python_json(text_of(render::brushes::library_path(config)));
        QVERIFY(!after.contains("groups"));
        QCOMPARE(after["brushes"]["my_a"]["label"].get<std::string>(), std::string("極細"));
        QVERIFY(!after["brushes"]["my_a"].contains("group"));
        // (moved with "group" naming where it is now, as the other kinds' edits: a wrong one refused)
        try {
            presets::edit(config, Json{{"action", "move"}, {"preset", "極細"}, {"group", "線"}, {"to", "線"}});
            QFAIL("a brush was moved from a group it is not in");
        } catch (const core::Error&) {
        }
    }

    // (review P2-2) A library with entries that are not brushes: the others read, what is wrong said, never written over.
    void entriesThatAreNotBrushes() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        fs::create_directories(config);
        const std::string text = R"({"brushes": {"my_x": "x", "my_a": {"label": "A", "base": "gpen"}}})";
        std::ofstream(render::brushes::library_path(config), std::ios::binary) << text;
        presets::Library lib;
        try {
            lib = presets::load(config);
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        QVERIFY(own(lib, "A") != nullptr);
        QVERIFY(lib.problem.find("my_x") != std::string::npos);
        try {
            presets::save(config, std::nullopt, Json{{"label", "B"}}, std::nullopt, std::nullopt);
            QFAIL("the library was written over");
        } catch (const core::Error&) {
        }
        QCOMPARE(text_of(render::brushes::library_path(config)), text);
    }

    void editedAsTheOtherPresets() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        const std::string a = presets::save(config, std::nullopt, Json{{"label", "A"}, {"base", "gpen"}}, std::nullopt, std::nullopt)["key"];
        const std::string b = presets::save(config, std::nullopt, Json{{"label", "B"}, {"base", "maru"}}, std::nullopt, std::nullopt)["key"];
        const std::string c = presets::save(config, std::nullopt, Json{{"label", "C"}, {"base", "gpen"}}, std::nullopt, std::string("線"))["key"];
        presets::edit(config, Json{{"action", "rename"}, {"preset", "A"}, {"name", "A2"}});
        QVERIFY(own(presets::load(config), "A2") != nullptr);
        // a name taken: refused, the file as it was
        const std::string before = text_of(render::brushes::library_path(config));
        try {
            presets::edit(config, Json{{"action", "rename"}, {"preset", "A2"}, {"name", "B"}});
            QFAIL("a name taken was given");
        } catch (const core::Error&) {
        }
        try {
            presets::edit(config, Json{{"action", "rename_group"}, {"group", "一般"}, {"name", "x"}});
            QFAIL("a built-in group was renamed");
        } catch (const core::Error&) {
        }
        QCOMPARE(text_of(render::brushes::library_path(config)), before);
        // moved into 線 before C, then B into a new group
        presets::edit(config, Json{{"action", "move"}, {"preset", Json::array({"A2"})}, {"to", "線"}, {"index", 0}});
        presets::Library lib = presets::load(config);
        QCOMPARE(lib.groups.back().name, std::string("線"));
        QCOMPARE(lib.groups.back().items[0].key, a);
        QCOMPARE(lib.groups.back().items[1].key, c);
        presets::edit(config, Json{{"action", "new_group"}, {"name", "空"}});
        QVERIFY(std::ranges::count(own_groups(presets::load(config)), std::string("空")) == 1);  // (an empty group kept)
        presets::edit(config, Json{{"action", "move"}, {"preset", "B"}, {"to", "空"}});
        presets::edit(config, Json{{"action", "rename_group"}, {"group", "空"}, {"name", "丸"}});
        lib = presets::load(config);
        QCOMPARE(own(lib, "B")->group, std::string("丸"));
        QCOMPARE(own(lib, "B")->key, b);
        const Json gone = presets::edit(config, Json{{"action", "delete_group"}, {"group", "線"}});
        QCOMPARE(gone["deleted"].size(), std::size_t(2));
        lib = presets::load(config);
        QVERIFY(own(lib, "A2") == nullptr && own(lib, "C") == nullptr && own(lib, "B") != nullptr);
        // and the list as the CLI gives it
        const Json listed = presets::list(lib, true);
        bool found = false;
        for (const Json& g : listed["groups"]) {
            for (const Json& item : g["brushes"]) found = found || (item["key"] == b && item.contains("settings") && item["engine"].is_null());
        }
        QVERIFY(found);
    }

    void aFileThatCannotBeReadIsNeverWrittenOver() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        fs::create_directories(config);
        std::ofstream(render::brushes::library_path(config), std::ios::binary) << "{not json";
        const presets::Library lib = presets::load(config);
        QVERIFY(!lib.problem.empty());
        QCOMPARE(lib.groups.size(), presets::builtin_groups().size());
        for (const auto& work : std::vector<std::function<void()>>{
                 [&] { presets::save(config, std::nullopt, Json{{"label", "A"}}, std::nullopt, std::nullopt); },
                 [&] { presets::edit(config, Json{{"action", "new_group"}, {"name", "x"}}); },
                 [&] { presets::import_abr(config, render::abr::write_v12(1, {}, false), "g", false); }}) {
            try {
                work();
                QFAIL("the library that cannot be read was written");
            } catch (const std::exception&) {
            }
        }
        QCOMPARE(text_of(render::brushes::library_path(config)), std::string("{not json"));
    }

    // preset_import_cmds.rs import_abr: a file's brushes as one group, read again in its place
    void aPhotoshopFileTakenIn() {
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        render::abr::LegacyBrush round;
        round.computed = true;
        round.diameter = 19;
        round.hardness = 80;
        round.spacing = 25;
        render::abr::LegacyBrush leaf;
        leaf.name = "葉っぱ";
        leaf.spacing = 30;
        leaf.sample.width = 10;
        leaf.sample.height = 7;
        leaf.sample.depth = 16;
        for (int i = 0; i < 70; ++i) leaf.sample.data += std::string(i % 2 == 0 ? "\xff\xff" : "\x10\x00", 2);
        const std::string file = render::abr::write_v12(2, {round, leaf}, true);
        Json out = presets::import_abr(config, file, "読んだ", false);
        QCOMPARE(out["group"].get<std::string>(), std::string("読んだ"));
        QCOMPARE(out["brushes"].size(), std::size_t(2));
        QCOMPARE(out["version"].get<int>(), 2);
        presets::Library lib = presets::load(config);
        const presets::Preset* p = own(lib, "葉っぱ");
        QVERIFY(p != nullptr);
        QCOMPARE(p->group, std::string("読んだ"));
        const core::DabSettings d = core::dabs_from_json(p->settings["dabs"]);
        QVERIFY(render::brushes::library_tip(config, d.tip) != nullptr);
        QCOMPARE(render::dabs::tile_of_png(*render::brushes::library_tip(config, d.tip)).width, 10u);
        // read again: added (" 2"), or in its place
        presets::import_abr(config, file, "読んだ", false);
        QVERIFY(own(presets::load(config), "葉っぱ 2") != nullptr);
        presets::import_abr(config, file, "読んだ", true);
        lib = presets::load(config);
        QCOMPARE(lib.groups.back().items.size(), std::size_t(2));
        QVERIFY(own(lib, "葉っぱ") != nullptr);
        // a file that is not one: nothing written
        try {
            presets::import_abr(config, "8BPS not a brush", "x", false);
            QFAIL("garbage was taken in");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("format"));
        }
    }

    // brush_cmds/tests.rs define_brush_from_selection: darkness × alpha × the selection, cut to what holds paint
    void definedFromThePixels() {
        const core::Document doc = boxed_book(400, 100, 120, 40, 30);
        const core::Page& page = doc.page(0);
        const std::string layer = "paint";
        presets::Defined made = presets::define_from(doc, page, nullptr, &layer, "四角");
        QCOMPARE(made.width, 40u);
        QCOMPARE(made.height, 30u);
        QVERIFY(std::abs(made.dpi - 200.0) < 1e-6);
        const core::DabSettings d = core::dabs_from_json(made.settings["dabs"]);
        QVERIFY(std::abs(d.size_mm - 40 * 25.4 / 200.0) < 1e-9);
        const render::dabs::GrayTile tile = render::dabs::tile_of_png(*made.pictures.at(d.tip));
        QCOMPARE(tile.data[0], std::uint16_t(65535));  // (black: full paint)
        // the page as it shows, within an area (mm): only that part
        const double mm = 25.4 / 200.0;
        const Json area{{"poly", Json::array({Json::array({100 * mm, 120 * mm}), Json::array({120 * mm, 120 * mm}), Json::array({120 * mm, 150 * mm}),
                                              Json::array({100 * mm, 150 * mm})})}};
        made = presets::define_from(doc, page, &area, nullptr, "半分");
        QVERIFY(made.width >= 19 && made.width <= 21);
        QVERIFY(made.height >= 29 && made.height <= 31);
        // white: nothing to make a tip of
        const Json white{{"poly", Json::array({Json::array({1.0, 1.0}), Json::array({5.0, 1.0}), Json::array({5.0, 5.0}), Json::array({1.0, 5.0})})}};
        try {
            presets::define_from(doc, page, &white, nullptr, "白");
            QFAIL("a tip of nothing was made");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("value"));
        }
        // past 2500 pixels a side: refused
        const core::Document big = boxed_book(2600, 0, 0, 2600, 10);
        try {
            presets::define_from(big, big.page(0), nullptr, &layer, "大");
            QFAIL("a tip past 2500 pixels was made");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("value"));
        }
        // kept in the library as a brush (saved with its picture), named サンプルブラシ N by default
        QTemporaryDir dir;
        const fs::path config(dir.path().toStdString());
        QCOMPARE(presets::next_sample_name(presets::load(config)), std::string("サンプルブラシ 1"));
        made = presets::define_from(doc, page, nullptr, &layer, "サンプルブラシ 1");
        presets::save(config, std::nullopt, made.settings, std::nullopt, std::nullopt, made.pictures);
        QCOMPARE(presets::next_sample_name(presets::load(config)), std::string("サンプルブラシ 2"));
    }

    // edit_menu_cmds export_import and migrate_cmds: the presets of every kind to one file and back, and from another
    // settings folder only the groups not here yet (again: nothing)
    void presetsWrittenReadAndMigrated() {
        QTemporaryDir one, two;
        const fs::path a(one.path().toStdString()), b(two.path().toStdString());
        presets::save(a, std::nullopt, dab_brush("にじみ"), std::nullopt, std::string("インク"));
        {
            auto lib = render::preset_store::load(a, render::gradient_presets::kind());
            render::preset_store::add(lib.groups, render::gradient_presets::kind(),
                                      render::gradient_presets::item_of(Json{{"stops", Json::array({"#ff0000", "#0000ff"})}}, "赤から青"), std::string("自分の"), "x");
            render::preset_store::save(a, render::gradient_presets::kind(), lib.groups);
        }
        {
            auto lib = render::preset_store::load(a, render::tool_presets::kind());
            render::preset_store::add(lib.groups, render::tool_presets::kind(), render::tool_presets::item_of("pen", std::string("太いペン"), Json{{"brush", "gpen"}}),
                                      std::string("自分の道具"), "pen");
            render::preset_store::save(a, render::tool_presets::kind(), lib.groups);
        }
        {
            auto groups = render::shape_presets::load(a);
            render::shape_presets::add(groups, "まる", std::string("自分の形"), render::shape_presets::parse("O 50 50 40"));
            render::shape_presets::save(a, groups);
        }
        {
            auto lib = render::pattern_library::load(a);
            const auto& builtin = render::pattern_library::builtins().front();
            core::Pattern p = builtin.pattern;
            p.name = "自分の模様";
            render::pattern_library::add(a, lib.groups, p, builtin.bytes, std::string("自分の模様集"));
            render::pattern_library::save(a, lib.groups);
        }
        Json counts;
        const std::string text = render::preset_files::export_text(a, {}, false, &counts);
        QCOMPARE(counts["brushes"].get<int>(), 1);
        QVERIFY(counts["gradients"].get<int>() >= 1 && counts["tool_presets"].get<int>() >= 1 && counts["shapes"].get<int>() >= 1);
        QVERIFY(counts["patterns"].get<int>() >= 1);
        const Json added = render::preset_files::import_text(b, text)["added"];
        QCOMPARE(added["brushes"].get<int>(), 1);
        const presets::Library lib = presets::load(b);
        const presets::Preset* brush = own(lib, "にじみ");
        QVERIFY(brush != nullptr);
        QCOMPARE(brush->group, std::string("インク"));
        const core::DabSettings d = core::dabs_from_json(brush->settings["dabs"]);
        QVERIFY(render::brushes::library_tip(b, d.tip) != nullptr);  // (its picture came with it)
        QVERIFY(render::preset_store::find(render::preset_store::load(b, render::gradient_presets::kind()).groups, "赤から青") != nullptr);
        QVERIFY(render::preset_store::find(render::preset_store::load(b, render::tool_presets::kind()).groups, "太いペン") != nullptr);
        QVERIFY(render::shape_presets::find(render::shape_presets::load(b), "まる") != nullptr);
        QVERIFY(render::preset_store::find(render::pattern_library::load(b).groups, "自分の模様") != nullptr);
        // read again: what is there as it is is not added twice
        const Json again = render::preset_files::import_text(b, text)["added"];
        QCOMPARE(again["brushes"].get<int>(), 0);
        QCOMPARE(again["gradients"].get<int>(), 0);
        // migrate from the folder: only groups not here yet (none now), then into a third folder, then idempotent
        QCOMPARE(render::preset_files::migrate(b, a)["migrated"].get<int>(), 0);
        QTemporaryDir three;
        const fs::path c(three.path().toStdString());
        const Json moved = render::preset_files::migrate(c, a);
        QVERIFY(moved["migrated"].get<int>() > 0);
        QCOMPARE(moved["added"]["brushes"].get<int>(), 1);
        QCOMPARE(render::preset_files::migrate(c, a)["migrated"].get<int>(), 0);
        // and from a written file
        const fs::path written = c / "presets.genkopresets";
        std::ofstream(written, std::ios::binary) << text;
        QTemporaryDir four;
        QVERIFY(render::preset_files::migrate(fs::path(four.path().toStdString()), written)["migrated"].get<int>() > 0);
        // not such a file
        try {
            render::preset_files::import_text(b, "{\"genko_presets\": 9}");
            QFAIL("another file was read");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("format"));
        }
        // the Preset Manager's list and an edit of each kind
        const Json listed = render::preset_files::manager_list(b);
        for (const std::string& k : render::preset_files::kinds()) QVERIFY2(listed.contains(k), k.c_str());
        render::preset_files::manager_edit(b, "gradients", Json{{"action", "rename"}, {"preset", "赤から青"}, {"name", "赤青"}});
        QVERIFY(render::preset_store::find(render::preset_store::load(b, render::gradient_presets::kind()).groups, "赤青") != nullptr);
        render::preset_files::manager_edit(b, "brushes", Json{{"action", "rename"}, {"preset", "にじみ"}, {"name", "にじみ2"}});
        QVERIFY(own(presets::load(b), "にじみ2") != nullptr);
        render::preset_files::manager_edit(b, "shapes", Json{{"action", "delete"}, {"preset", "まる"}});
        QVERIFY(render::shape_presets::find(render::shape_presets::load(b), "まる") == nullptr);
        try {
            render::preset_files::manager_edit(b, "styles", Json{{"action", "rename"}});
            QFAIL("a kind not known was edited");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("value"));
        }
    }
};

QTEST_GUILESS_MAIN(TestBrushPresets)
#include "test_brush_presets.moc"
