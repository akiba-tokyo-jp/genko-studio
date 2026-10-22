// PC4 E-0 (SPEC.md COLOR-08「プリセット」): the presets' libraries in the settings folder (render/preset_store, upstream's
// presets/mod.rs and presets/tests.rs): the built-in groups when there is no file; the groups written and read back;
// add's names made unique over every group and its group made when it is not there; each edit action (rename, delete,
// move with an index, new_group, rename_group, delete_group) and what each refuses; reset and reset --append (the
// person's groups kept, the built-in ones after them, a name taken getting a number); a file that cannot be read is shown
// as the built-in groups and never written over.

#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/preset_store.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::preset_store::Group;
namespace store = render::preset_store;
namespace fs = std::filesystem;

namespace {

Json item(const std::string& name, double value = 1.0) { return Json{{"name", name}, {"value", value}}; }

store::Kind kind() {
    store::Kind k;
    k.file = "things.json";
    k.default_group = "自分の";
    k.builtin = [] { return std::vector<Group>{{"基本", {item("赤"), item("青")}}, {"特殊", {item("虹")}}}; };
    k.check = [](const Json& it) {
        const Json* v = it.contains("value") ? &it["value"] : nullptr;
        if (v == nullptr || !v->is_number() || v->get<double>() < 0) throw core::Error("value", "a thing's value is 0 or more");
    };
    k.max_items = 6;
    k.max_groups = 4;
    return k;
}

std::vector<std::string> names(const std::vector<Group>& groups) {
    std::vector<std::string> out;
    for (const Group& g : groups) {
        out.push_back("[" + g.name + "]");
        for (const Json& it : g.items) out.push_back(it["name"].get<std::string>());
    }
    return out;
}

std::string error_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

}  // namespace

class TestPresetStore : public QObject {
    Q_OBJECT

private slots:
    // No file: the built-in groups; written and read back as they were; the file's layout.
    void theLibraryIsWrittenAndReadBack() {
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "config";
        store::Library lib = store::load(dir, kind());
        QVERIFY(!lib.from_file && lib.problem.empty());
        QCOMPARE(names(lib.groups), (std::vector<std::string>{"[基本]", "赤", "青", "[特殊]", "虹"}));
        QCOMPARE(store::add(lib.groups, kind(), item("緑", 2), std::nullopt, "x"), Json({{"name", "緑"}, {"group", "自分の"}}));
        store::save(dir, kind(), lib.groups);
        const Json file = genko::test::read_json(QString::fromStdString((dir / "things.json").string()));
        QCOMPARE(file["version"], Json(1));
        QCOMPARE(file["groups"].size(), std::size_t(3));
        QCOMPARE(file["groups"][2]["name"], Json("自分の"));
        QCOMPARE(file["groups"][2]["items"].size(), std::size_t(1));
        QCOMPARE(file["groups"][2]["items"][0]["value"], Json(2.0));
        const store::Library back = store::load(dir, kind());
        QVERIFY(back.from_file && back.problem.empty());
        QCOMPARE(names(back.groups), (std::vector<std::string>{"[基本]", "赤", "青", "[特殊]", "虹", "[自分の]", "緑"}));
        QCOMPARE(*store::find(back.groups, "緑"), item("緑", 2));
        QVERIFY(store::find(back.groups, "緑", std::string_view("基本")) == nullptr);
        QCOMPARE(store::list(back.groups)["groups"][0], Json({{"name", "基本"}, {"items", Json::array({"赤", "青"})}}));
    }

    // add: a name taken gets " 2", " 3" (over every group); the name given or the fallback, its spaces taken off; the
    // group made; what the kind refuses, too many, a name that cannot be.
    void addMakesNamesUnique() {
        std::vector<Group> groups = kind().builtin();
        QCOMPARE(store::add(groups, kind(), item("赤"), std::string_view("特殊"), "x")["name"], Json("赤 2"));
        QCOMPARE(store::add(groups, kind(), Json{{"value", 3}}, std::nullopt, "  新しい  ")["name"], Json("新しい"));
        QCOMPARE(store::unique_name(groups, "赤"), std::string("赤 3"));
        QCOMPARE(names(groups), (std::vector<std::string>{"[基本]", "赤", "青", "[特殊]", "虹", "赤 2", "[自分の]", "新しい"}));
        QVERIFY(error_of([&] { store::add(groups, kind(), item("負", -1), std::nullopt, "x"); }).find("0 or more") != std::string::npos);
        QVERIFY(error_of([&] { store::add(groups, kind(), item(""), std::nullopt, "x"); }).find("1 to 200") != std::string::npos);
        QVERIFY(error_of([&] { store::add(groups, kind(), item(std::string(201, 'a')), std::nullopt, "x"); }).find("1 to 200") != std::string::npos);
        QVERIFY(error_of([&] { store::add(groups, kind(), Json::array(), std::nullopt, "x"); }).find("object") != std::string::npos);
        (void)store::add(groups, kind(), item("六"), std::nullopt, "x");
        QVERIFY(error_of([&] { store::add(groups, kind(), item("七"), std::nullopt, "x"); }).find("too many presets") != std::string::npos);
        QCOMPARE(groups.back().items.size(), std::size_t(2));
    }

    // Each edit action, and what each refuses.
    void editActions() {
        std::vector<Group> groups = kind().builtin();
        const auto edit = [&](const char* text) { return store::edit(groups, kind(), Json::parse(text)); };
        const auto refused = [&](const char* text) { return error_of([&] { (void)edit(text); }); };
        QCOMPARE(edit(R"({"action": "rename", "preset": "赤", "name": "朱"})"), Json({{"name", "朱"}}));
        QVERIFY(refused(R"({"action": "rename", "preset": "朱", "name": "青"})").find("is there already") != std::string::npos);
        QVERIFY(refused(R"({"action": "rename", "preset": "赤", "name": "x"})").find("no preset named 赤") != std::string::npos);
        QVERIFY(refused(R"({"action": "rename", "preset": "青", "group": "特殊", "name": "x"})").find("no preset named 青") != std::string::npos);
        QCOMPARE(edit(R"({"action": "new_group", "name": "箱"})"), Json({{"group", "箱"}}));
        QVERIFY(refused(R"({"action": "new_group", "name": "箱"})").find("is there already") != std::string::npos);
        QCOMPARE(edit(R"({"action": "move", "preset": ["青", "朱"], "to": "箱"})"), Json({{"moved", 2}, {"group", "箱"}}));
        QCOMPARE(edit(R"({"action": "move", "preset": "虹", "to": "箱", "index": 1})"), Json({{"moved", 1}, {"group", "箱"}}));
        QCOMPARE(names(groups), (std::vector<std::string>{"[基本]", "[特殊]", "[箱]", "青", "虹", "朱"}));
        QVERIFY(refused(R"({"action": "move", "preset": "虹", "to": "無い"})").find("no group named 無い") != std::string::npos);
        QCOMPARE(edit(R"({"action": "rename_group", "group": "箱", "name": "箱 2"})"), Json({{"group", "箱 2"}}));
        QVERIFY(refused(R"({"action": "rename_group", "group": "基本", "name": "特殊"})").find("is there already") != std::string::npos);
        QVERIFY(refused(R"({"action": "rename_group", "name": "x"})").find("group names the group") != std::string::npos);
        QCOMPARE(edit(R"({"action": "delete", "preset": ["青", "朱"]})"), Json({{"deleted", 2}}));
        QCOMPARE(edit(R"({"action": "delete_group", "group": "箱 2"})"), Json({{"deleted", 1}}));
        QCOMPARE(names(groups), (std::vector<std::string>{"[基本]", "[特殊]"}));
        QVERIFY(refused(R"({"action": "delete_group", "group": "箱 2"})").find("no group named") != std::string::npos);
        QVERIFY(refused(R"({"action": "delete"})").find("preset names") != std::string::npos);
        QVERIFY(refused(R"({"action": "paint"})").find("action is rename") != std::string::npos);
        QVERIFY(refused(R"([])").find("the edit is") != std::string::npos);
        QCOMPARE(edit(R"({"action": "new_group", "name": "3"})")["group"], Json("3"));
        QCOMPARE(edit(R"({"action": "new_group", "name": "4"})")["group"], Json("4"));
        QVERIFY(refused(R"({"action": "new_group", "name": "5"})").find("too many groups") != std::string::npos);
    }

    // reset: the built-in groups; with append the person's first, a group's name taken and an item's name taken getting
    // a number.
    void resetAndAppend() {
        std::vector<Group> mine{{"基本", {item("赤", 5)}}, {"自分の", {item("紫")}}};
        QCOMPARE(names(store::reset(mine, kind(), false)), (std::vector<std::string>{"[基本]", "赤", "青", "[特殊]", "虹"}));
        const std::vector<Group> appended = store::reset(mine, kind(), true);
        QCOMPARE(names(appended), (std::vector<std::string>{"[基本]", "赤", "[自分の]", "紫", "[基本 2]", "赤 2", "青", "[特殊]", "虹"}));
        QCOMPARE(appended[0].items[0]["value"], Json(5));  // (the person's kept as they were)
        // what is appended can be saved (the names unique)
        QTemporaryDir tmp;
        std::vector<Group> few{{"x", {item("赤")}}};
        store::save(fs::path(tmp.path().toStdString()), [] {
            store::Kind k = kind();
            k.max_items = 100;
            return k;
        }(), store::reset(few, kind(), true));
    }

    // A file that cannot be read (not JSON, another version, an item its kind refuses, a name twice, too large) is shown
    // as the built-in groups with why, and saving over it is refused: the file stays as it was. Groups that cannot be
    // are not written.
    void aFileThatCannotBeReadIsNotWrittenOver() {
        for (const std::string& text : {std::string("{not json"), std::string(R"({"version": 2, "groups": []})"),
                                       std::string(R"({"version": 1, "groups": [{"name": "g", "items": [{"name": "a", "value": -1}]}]})"),
                                       std::string(R"({"version": 1, "groups": [{"name": "g", "items": [{"name": "a", "value": 1}, {"name": "a", "value": 2}]}]})"),
                                       std::string(R"({"version": 1, "groups": {}})"), std::string(store::kFileMaxBytes + 1, ' ')}) {
            QTemporaryDir tmp;
            const fs::path dir = fs::path(tmp.path().toStdString());
            storage::write_atomic(dir / "things.json", text);
            const store::Library lib = store::load(dir, kind());
            QVERIFY(!lib.from_file);
            QVERIFY2(!lib.problem.empty(), text.substr(0, 60).c_str());
            QCOMPARE(names(lib.groups), names(kind().builtin()));
            QVERIFY(error_of([&] { store::save(dir, kind(), lib.groups); }).find("not written over") != std::string::npos);
            QCOMPARE(storage::read_file_bounded(dir / "things.json", store::kFileMaxBytes + 2), text);
        }
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString());
        QVERIFY(error_of([&] { store::save(dir, kind(), {{"g", {item("a"), item("a")}}}); }).find("twice") != std::string::npos);
        QVERIFY(error_of([&] { store::save(dir, kind(), {{"g", {item("a", -2)}}}); }).find("0 or more") != std::string::npos);
        QVERIFY(error_of([&] { store::save(dir, kind(), {{"", {}}}); }).find("group's name") != std::string::npos);
        QVERIFY(!fs::exists(dir / "things.json"));
    }
};

QTEST_GUILESS_MAIN(TestPresetStore)
#include "test_preset_store.moc"
