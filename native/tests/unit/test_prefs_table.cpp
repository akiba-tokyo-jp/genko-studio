// 環境設定の表 (PC5 H-0, core/prefs_table; PhotoCraft v0.2.0's prefs/tests.rs, MIT, its ideas): every key of
// settings.ini the app keeps is in the table under a "section.key" path; a path's key and a key's path go both ways;
// each entry's value when the file has none is one of its own; a value is checked by the entry's type (range, choices,
// "#rrggbb", keys) and prefs.set writes all of the values or none; prefs.get reads the file as the app does and the
// fallback where it holds what the type cannot read; prefs.reset puts a path, a section or everything back (the app's
// memories kept).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <set>
#include <string>

#include "core/json.hpp"
#include "core/prefs_table.hpp"

namespace prefs = genko::core::prefs;
using genko::core::Json;

namespace {

// Every key the app writes or reads in settings.ini now (PC5 H-0: app/*.cpp, core/config_dir.cpp), a pattern's with a
// name of its own. A key added to the app later goes in the table and here.
const std::vector<std::string>& app_keys() {
    static const std::vector<std::string> all{
        "3d/ik", "ai/client", "align/to", "backup/folder", "backup/keep", "backup/minutes", "brush/blend", "brush/kind", "brush/rgb",
        "brush/sizes", "brush/smoothing", "brush/smoothing_amount", "brush/gpen/size", "brush/gpen/opacity", "color/askOnMismatch",
        "color/askOnMissing", "color/askOnPaste", "color/bpc", "color/dither", "color/icc", "color/intent", "color/monitorProfile",
        "color/policyCmyk", "color/policyGray", "color/policyRgb", "color/workingCmyk", "color/workingGray", "color/workingRgb",
        "colour/corners", "colour/history", "colour/sub", "eraser/crossing", "eraser/flow", "eraser/kind", "eraser/opacity",
        "export/jpeg_quality", "export/metadata", "export/quick_format", "export/quick_location", "export/srgb",
        "files/ignore_exif_profile", "files/lowercase_extension", "files/recent_count", "fill/all_layers", "fill/antialias", "fill/basis",
        "fill/blend", "fill/color_tolerance", "fill/contiguous", "fill/expand", "fill/gap", "fill/gap_size", "fill/lasso", "fill/opacity",
        "fill/reference", "fill/skip_draft", "fill/skip_text", "fill/source", "fill/tolerance", "gradient/blend", "gradient/current",
        "gradient/dither", "gradient/opacity", "gradient/reverse", "gradient/shape", "gradient/transparency", "guides/grid_mm",
        "guides/snap", "guides/show", "guides/grid", "guides/grid_snap", "gutter_h", "gutter_v", "keys/alt_tool", "keys/ctrl_tool",
        "keys/hold_swap", "keys/pen/alt", "keys/eraser/shift", "migration/done", "move/auto_group", "move/auto_select", "move/show_box",
        "names_current/ui/commandbar", "names_current/shortcuts/グラデーション", "new/paper", "patterns/current", "pencil/auto_erase",
        "picker/size", "raw/auto_tone", "raw/depth", "raw/remembered/IMG_0001.CR2", "raw/remembered_order", "raw/resolution",
        "raw/sharpen", "raw/smart_object", "raw/space", "save/after_ms", "shapes/mode", "shapes/pen_mode", "shortcuts/ブラシツール",
        "shortcuts/水平方向に反転（表示）", "shortcuts_unread/グラデーション", "snap/guides", "snap/grid", "stroke/location",
        "stroke/width_mm", "subview/images", "tablet/button", "tablet/gamma", "tool_presets/current_only", "type/default_styles", "type/language_features",
        // (PC5 G-d: 環境設定 → テキスト, スペルチェック's, 文字パネル・字形パネル's memories)
        "type/smart_quotes", "type/glyph_protection", "type/font_names_english", "type/esc_commits", "type/font_preview", "type/placeholder",
        "type/recent_fonts", "type/user_dictionary", "type/notations", "type/recent_fonts_used", "type/recent_glyphs", "ui/brightness",
        "ui/brush_preview", "ui/commandbar", "ui/cross_drawing", "ui/cursor", "ui/font_pt", "ui/gpu_canvas", "ui/gpu_memory_mb", "ui/hints", "ui/key_set", "ui/key_set_told",
        "ui/layer_details", "ui/mono_icons", "ui/next_steps", "ui/phone_view", "ui/quick", "ui/radial", "ui/raise_requests",
        "ui/reduce_motion", "ui/rest_minutes", "ui/stage", "ui/surround", "ui/theme", "ui/tool_names", "ui/ui_font", "ui/workspace",
        "view/checker_colors", "view/checker_dark", "view/checker_light", "view/checker_size", "view/gamut_color", "view/gamut_opacity",
        "warp/mesh", "workspaces/ペン入れ", "workspaces/ペン入れ/geometry", "workspaces/ペン入れ/state",
        // (PC5 H-b: 単位・定規, ガイド・グリッド・スライス, エクストラ and 表示・非表示 ▸, ガイドをロック, the guide layouts' presets)
        "units/ruler", "units/type", "units/column_width", "units/gutter", "units/print_resolution", "units/screen_resolution",
        "units/point_size", "guides/guide_color", "guides/guide_style", "guides/smart_color", "guides/grid_color", "guides/grid_style",
        "guides/grid_subdivisions", "guides/grid_unit", "guides/lock", "view/extras", "view/show_guides", "view/show_canvas_guides",
        "view/show_smart_guides", "view/show_layer_edges", "view/show_mesh", "view/show_edit_pins", "guide_layouts/三段組",
        // (PC5 J-c・J-d1: 表示・非表示 ▸ 注釈・カウント)
        "view/show_notes", "view/show_count",
        // (PC5 H-d: ワークスペース — the lock and its pane —, the person's own key sets, メニュー…, ツールバー…, a workspace kept
        // with its keys, menus and toolbar, a workspace as it was left)
        "ui/workspace_locked", "ui/workspace_remember", "ui/auto_show_panels", "ui/large_tabs", "ui/key_set_own", "key_sets/私の組/base",
        "key_sets/私の組/keys", "menus/hidden", "menus/colors/検索", "menus/colors/HSB/HSL", "toolbar/order", "toolbar/hidden",
        "workspaces/ペン入れ/keys", "workspaces/ペン入れ/menus", "workspaces/ペン入れ/toolbar", "workspace_changes/写真",
        // (PC5 J-b: ガイド・グリッド・スライス's slices, 表示・非表示 ▸ スライス, スナップ先 ▸ スライス)
        "guides/slice_color", "guides/slice_numbers", "view/show_slices", "snap/slices",
        // (PC5 H-e: 一般, インターフェイス, ワークスペース's two, ツール, ヒストリーログ, パフォーマンス, 仮想記憶ディスク, 拡張コントロール)
        "ui/color_picker", "ui/interpolation", "ui/beep_when_done", "ui/export_clipboard", "ui/place_resize", "ui/wheel_zooms",
        "ui/animated_zoom", "ui/home_screen", "ui/ui_scale", "ui/canvas_border", "ui/channels_in_color", "ui/menu_colors", "ui/tooltips",
        "ui/open_as_tabs", "ui/float_docking", "ui/tool_tooltips", "ui/zoom_to_center", "ui/flick_panning", "ui/transform_values",
        "ui/overscroll", "history_log/enabled", "history_log/destination", "history_log/file", "history_log/detail", "ui/memory_mb",
        "ui/history_states", "ui/scratch_folder", "ui/touch_gestures", "ui/pinch_zoom", "ui/trackpad_rotate"};
    return all;
}

QString q(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

class TestPrefsTable : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    std::unique_ptr<QSettings> fresh(const char* name) {
        return std::make_unique<QSettings>(dir_.filePath(QString::fromLatin1(name)), QSettings::IniFormat);
    }

private slots:
    // Every key of the app is in the table, and its path leads back to it.
    void everyKeyOfTheAppIsInTheTable() {
        for (const std::string& key : app_keys()) {
            const prefs::Pref* p = prefs::by_key(key);
            QVERIFY2(p != nullptr, key.c_str());
            const auto path = prefs::path_of(key);
            QVERIFY2(path.has_value(), key.c_str());
            QCOMPARE(q(prefs::key_of(*path).value_or("")), q(key));
            QVERIFY2(prefs::by_path(*path) == p, path->c_str());
        }
        // (an exact entry before a pattern's: keys/alt_tool is not a tool's modifier, guides/grid_mm not a view switch)
        QCOMPARE(q(prefs::by_key("keys/alt_tool")->path), QStringLiteral("tools.altTool"));
        QCOMPARE(q(prefs::by_key("keys/pen/alt")->path), QStringLiteral("tools.modifiers.*.*"));
        QCOMPARE(q(prefs::path_of("keys/pen/alt").value()), QStringLiteral("tools.modifiers.pen.alt"));
        // (PC5 H-b, the review's P2-3: Genko's own name — upstream's gridlineEvery is the thick lines' step, not this)
        QCOMPARE(q(prefs::by_key("guides/grid_mm")->path), QStringLiteral("guidesGridAndSlices.gridSpacingMm"));
        QVERIFY(prefs::by_path("guidesGridAndSlices.gridlineEvery") == nullptr);
        QCOMPARE(prefs::by_key("guides/grid_mm")->apply, prefs::Apply::Live);
        // (a command's name with dots or slashes in it: the rest of the path)
        QCOMPARE(q(prefs::key_of("shortcuts.ブラシサイズを大きく 1.5 倍").value()), QStringLiteral("shortcuts/ブラシサイズを大きく 1.5 倍"));
        QCOMPARE(q(prefs::path_of("shortcuts/HSB/HSL").value()), QStringLiteral("shortcuts.HSB/HSL"));
        QVERIFY(prefs::by_key("nothing/here") == nullptr);
        QVERIFY(prefs::by_path("nope.nothing") == nullptr);
        QVERIFY(prefs::by_path("general.notAField") == nullptr);
    }

    // The sections: upstream's eighteen in Photoshop's order, then Genko's; every entry in one; paths and keys once.
    void theSectionsAndTheEntries() {
        const auto& sections = prefs::sections();
        QCOMPARE(q(sections.front()), QStringLiteral("general"));
        QCOMPARE(q(sections[17]), QStringLiteral("integrations"));
        for (const char* own : {"tablet", "comfort", "book", "colorSettings", "shortcuts", "keyboardShortcuts", "menus", "toolbar", "view", "state"}) {
            QVERIFY2(std::find(sections.begin(), sections.end(), own) != sections.end(), own);
        }
        std::set<std::string> paths;
        std::set<std::string> keys;
        std::size_t last = 0;
        for (const prefs::Pref& p : prefs::table()) {
            QVERIFY2(paths.insert(p.path).second, p.path.c_str());
            QVERIFY2(keys.insert(p.key).second, p.key.c_str());
            const auto at = std::find(sections.begin(), sections.end(), p.path.substr(0, p.path.find('.')));
            QVERIFY2(at != sections.end(), p.path.c_str());
            const auto index = static_cast<std::size_t>(at - sections.begin());
            QVERIFY2(index >= last, p.path.c_str());  // (in the sections' order)
            last = index;
            // a preference has its words; a memory of the app has none and is never applied
            QCOMPARE(p.apply == prefs::Apply::Never, p.kind == prefs::Kind::Data);
            if (p.kind != prefs::Kind::Data) QVERIFY2(!p.label.empty(), p.path.c_str());
        }
    }

    // Each entry's fallback is a value of its own type (what decode gives with nothing kept passes check).
    void theFallbacksFitTheirTypes() {
        for (const prefs::Pref& p : prefs::table()) {
            if (p.kind == prefs::Kind::Data) continue;
            const Json value = prefs::decode(p, QVariant());
            QVERIFY2(!prefs::check(p, value).has_value(), qPrintable(q(p.path) + QStringLiteral(": ") + q(value.dump())));
            if (p.fallback.empty()) {
                QVERIFY2(p.none_ok, p.path.c_str());
            } else {
                QCOMPARE(prefs::encode(p, value).toString(), q(p.fallback));  // (written back as the app writes it)
            }
        }
        // (the app's defaults: Photoshop's 20 recent files, 2 s before saving, the checker's middle squares)
        QCOMPARE(prefs::decode(*prefs::by_path("fileHandling.recentFileCount"), QVariant()), Json(20));
        QCOMPARE(prefs::decode(*prefs::by_path("book.saveAfterMs"), QVariant()), Json(2000));
        QCOMPARE(prefs::decode(*prefs::by_path("transparencyAndGamut.gridSize"), QVariant()), Json("medium"));
        QCOMPARE(prefs::decode(*prefs::by_path("keyboardShortcuts.keySet"), QVariant()), Json("photoshop"));  // (the user's decision 8)
        QCOMPARE(prefs::decode(*prefs::by_path("interface.toolNames"), QVariant()), Json(false));
        QCOMPARE(prefs::decode(*prefs::by_path("interface.nextSteps"), QVariant()), Json(true));
        QCOMPARE(prefs::decode(*prefs::by_path("comfort.reduceMotion"), QVariant()), Json(""));  // (the computer's)
    }

    // Values read as the app reads them: its own spellings, a number out of range brought in, what cannot be read the
    // fallback.
    void valuesAreReadAsTheAppReadsThem() {
        const auto& nextSteps = *prefs::by_path("interface.nextSteps");
        QCOMPARE(prefs::decode(nextSteps, QStringLiteral("off")), Json(false));
        QCOMPARE(prefs::decode(nextSteps, QStringLiteral("on")), Json(true));
        QCOMPARE(prefs::encode(nextSteps, Json(false)).toString(), QStringLiteral("off"));
        const auto& toolNames = *prefs::by_path("interface.toolNames");
        QCOMPARE(prefs::decode(toolNames, QStringLiteral("1")), Json(true));
        QCOMPARE(prefs::encode(toolNames, Json(true)).toString(), QStringLiteral("1"));
        const auto& recent = *prefs::by_path("fileHandling.recentFileCount");
        QCOMPARE(prefs::decode(recent, QStringLiteral("500")), Json(100));
        QCOMPARE(prefs::decode(recent, QStringLiteral("many")), Json(20));
        QCOMPARE(prefs::decode(*prefs::by_path("cursors.painting"), QStringLiteral("huge")), Json("circle"));
        QCOMPARE(prefs::decode(*prefs::by_path("transparencyAndGamut.customDark"), QStringLiteral("#3366AA")), Json("#3366aa"));
        QCOMPARE(prefs::decode(*prefs::by_path("transparencyAndGamut.customDark"), QStringLiteral("cyan")), Json("#cccccc"));
        // (none chosen is a value where it is one: Alt doing nothing; the fallback where it is not)
        QCOMPARE(prefs::decode(*prefs::by_path("tools.altTool"), QString()), Json(""));
        QCOMPARE(prefs::decode(*prefs::by_path("tools.altTool"), QVariant()), Json("picker"));
        QCOMPARE(prefs::decode(*prefs::by_path("export.jpegQuality"), QString()), Json(85));
    }

    // set: every value checked first (type, range, choices, colour, keys), a path unknown refused, nothing written then;
    // all written otherwise. A section's object sets its keys. The app's memories are not set from here.
    void setChecksAndIsAllOrNothing() {
        auto s = fresh("set.ini");
        for (const auto& [path, value] : std::vector<std::pair<const char*, Json>>{
                 {"cursors.painting", "huge"},
                 {"fileHandling.recentFileCount", 101},
                 {"fileHandling.recentFileCount", "ten"},
                 {"fileHandling.recentFileCount", 2.5},
                 {"transparencyAndGamut.gamutWarningColor", "cyan"},
                 {"interface.monoIcons", "yes"},
                 {"shortcuts.ブラシツール", "Ctrl+B||"},
                 {"shortcuts.ブラシツール", 3},
                 {"state.brush.kind", "gpen"}}) {
            const prefs::Outcome out = prefs::set(*s, Json{{path, value}});
            QVERIFY2(!out.ok, path);
            QCOMPARE(q(out.code), QStringLiteral("bad_value"));
            QCOMPARE(q(out.path), QString::fromUtf8(path));
            QVERIFY(!out.message.empty());
        }
        for (const char* path : {"nope.nothing", "general.notAField", "shortcuts.*"}) {
            const prefs::Outcome out = prefs::set(*s, Json{{path, 1}});
            QVERIFY2(!out.ok && out.code == "unknown_pref", path);
        }
        // one wrong: none written
        prefs::Outcome out = prefs::set(*s, Json{{"cursors.painting", "cross"}, {"fileHandling.recentFileCount", -4}});
        QVERIFY(!out.ok);
        s->sync();
        QVERIFY(s->allKeys().isEmpty());
        // all right: all written, as the app writes them
        out = prefs::set(*s, Json{{"cursors.painting", "cross"},
                                  {"interface.nextSteps", false},
                                  {"fileHandling.recentFileCount", 7},
                                  {"shortcuts.水平方向に反転（表示）", "H"},
                                  {"transparencyAndGamut", Json{{"gridSize", "large"}, {"customDark", "#336699"}}}});
        QVERIFY2(out.ok, out.message.c_str());
        QCOMPARE(s->value(QStringLiteral("ui/cursor")).toString(), QStringLiteral("cross"));
        QCOMPARE(s->value(QStringLiteral("ui/next_steps")).toString(), QStringLiteral("off"));
        QCOMPARE(s->value(QStringLiteral("files/recent_count")).toInt(), 7);
        QCOMPARE(s->value(QStringLiteral("shortcuts/水平方向に反転（表示）")).toString(), QStringLiteral("H"));
        QCOMPARE(s->value(QStringLiteral("view/checker_size")).toString(), QStringLiteral("large"));
        QCOMPARE(s->value(QStringLiteral("view/checker_dark")).toString(), QStringLiteral("#336699"));
        QCOMPARE(out.json()["ok"], Json(true));
        QCOMPARE(out.json()["values"]["transparencyAndGamut.gridSize"], Json("large"));
        // (a refusal's form)
        const Json refusal = prefs::set(*s, Json{{"nope.nothing", 1}}).json();
        QCOMPARE(refusal["ok"], Json(false));
        QCOMPARE(refusal["error"], Json("unknown_pref"));
        QCOMPARE(refusal["path"], Json("nope.nothing"));
    }

    // get: one path, a section, everything (the app's memories only by their section's name); untouched keys their
    // fallbacks; a pattern's entries as the file has them.
    void getReadsPathsAndSections() {
        auto s = fresh("get.ini");
        s->setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
        s->setValue(QStringLiteral("shortcuts/ブラシツール"), QStringLiteral("Ctrl+Alt+K"));
        s->setValue(QStringLiteral("brush/kind"), QStringLiteral("gpen"));
        s->sync();
        prefs::Outcome out = prefs::get(*s, "interface.theme");
        QVERIFY(out.ok);
        QCOMPARE(out.values, (Json{{"interface.theme", "dark"}}));
        out = prefs::get(*s, "transparencyAndGamut");
        QCOMPARE(out.values.size(), std::size_t{6});
        QCOMPARE(out.values["transparencyAndGamut.gamutWarningColor"], Json("#808080"));
        out = prefs::get(*s);
        QCOMPARE(out.values["shortcuts.ブラシツール"], Json("Ctrl+Alt+K"));
        QCOMPARE(out.values["keyboardShortcuts.keySet"], Json("photoshop"));
        QVERIFY(!out.values.contains("state.brush.kind"));
        out = prefs::get(*s, "state");
        QCOMPARE(out.values["state.brush.kind"], Json("gpen"));
        QCOMPARE(prefs::get(*s, "shortcuts.ブラシツール").values["shortcuts.ブラシツール"], Json("Ctrl+Alt+K"));
        QCOMPARE(q(prefs::get(*s, "nope.nothing").code), QStringLiteral("unknown_pref"));
        QCOMPARE(prefs::value(*s, "interface.theme"), Json("dark"));
        QCOMPARE(prefs::value(*s, "interface.panelBrightness"), Json(0));
    }

    // reset: a path, a section, everything but the app's memories — back to the fallbacks.
    void resetPutsThemBack() {
        auto s = fresh("reset.ini");
        QVERIFY(prefs::set(*s, Json{{"interface.theme", "dark"},
                                    {"interface.uiFontSize", 14},
                                    {"cursors.painting", "dot"},
                                    {"shortcuts.ブラシツール", "Ctrl+Alt+K"}})
                    .ok);
        s->setValue(QStringLiteral("brush/kind"), QStringLiteral("gpen"));
        s->sync();
        prefs::Outcome out = prefs::reset(*s, "interface.theme");
        QVERIFY(out.ok);
        QCOMPARE(out.values["interface.theme"], Json("system"));
        QVERIFY(!s->contains(QStringLiteral("ui/theme")));
        QVERIFY(s->contains(QStringLiteral("ui/font_pt")));
        QVERIFY(prefs::reset(*s, "interface").ok);
        QVERIFY(!s->contains(QStringLiteral("ui/font_pt")));
        QVERIFY(s->contains(QStringLiteral("ui/cursor")));
        QVERIFY(prefs::reset(*s).ok);
        QVERIFY(!s->contains(QStringLiteral("ui/cursor")));
        QVERIFY(!s->contains(QStringLiteral("shortcuts/ブラシツール")));
        QCOMPARE(s->value(QStringLiteral("brush/kind")).toString(), QStringLiteral("gpen"));  // (the app's memory kept)
        QVERIFY(!prefs::reset(*s, "nope").ok);
        // the app's memories by their names: refused, kept (the workspaces kept, migration/done)
        s->setValue(QStringLiteral("workspaces/ペン入れ"), QStringLiteral("1"));
        s->setValue(QStringLiteral("migration/done"), QStringLiteral("yes"));
        for (const char* path : {"state", "state.brush.kind", "state.workspaces.ペン入れ", "state.migration.done"}) {
            const prefs::Outcome refused = prefs::reset(*s, path);
            QVERIFY2(!refused.ok && refused.code == "bad_value", path);
        }
        QCOMPARE(s->value(QStringLiteral("brush/kind")).toString(), QStringLiteral("gpen"));
        QCOMPARE(s->value(QStringLiteral("workspaces/ペン入れ")).toString(), QStringLiteral("1"));
        QCOMPARE(s->value(QStringLiteral("migration/done")).toString(), QStringLiteral("yes"));
    }

    // set writes a list or a key that may hold a reused name (グラデーション, 書き出し…) with the mark the app reads it by:
    // the new command's (app/preferences' names_current); other keys without.
    void setMarksReusedNames() {
        auto s = fresh("marks.ini");
        QVERIFY(prefs::set(*s, Json{{"shortcuts.グラデーション", "Ctrl+Alt+G"},
                                    {"shortcuts.ブラシツール", "Ctrl+Alt+B"},
                                    {"workspace.quickAccess", "グラデーション	書き出し…"},
                                    {"workspace.commandBar", "取り消し"}})
                    .ok);
        QCOMPARE(s->value(QStringLiteral("names_current/shortcuts/グラデーション")).toString(), QStringLiteral("Ctrl+Alt+G"));
        QCOMPARE(s->value(QStringLiteral("names_current/ui/quick")).toString(), QStringLiteral("グラデーション	書き出し…"));
        QCOMPARE(s->value(QStringLiteral("names_current/ui/commandbar")).toString(), QStringLiteral("取り消し"));
        QVERIFY(!s->contains(QStringLiteral("names_current/shortcuts/ブラシツール")));
        QCOMPARE(q(prefs::mark_of("shortcuts/書き出し…").value_or("")), QStringLiteral("names_current/shortcuts/書き出し…"));
        QVERIFY(!prefs::mark_of("ui/theme"));
    }

    // PC5 H-d: メニュー…'s colours one of Photoshop's (a command's name with "/" in it the rest of the path), the toolbar's order
    // and the workspace's switches as the app writes them; the person's own key sets and a workspace as it was left are the
    // app's memories (never set nor reset).
    void theMenusTheToolbarAndTheWorkspace() {
        auto s = fresh("hd.ini");
        QVERIFY(!prefs::set(*s, Json{{"menus.colors.検索", "pink"}}).ok);
        prefs::Outcome out = prefs::set(*s, Json{{"menus.colors.検索", "red"},
                                                 {"menus.colors.HSB/HSL", "violet"},
                                                 {"menus.hidden", "ピクセルグリッド	検索"},
                                                 {"toolbar.order", "act_move	|	act_pen"},
                                                 {"toolbar.hidden", "act_3d"},
                                                 {"workspace.locked", true},
                                                 {"workspace.largeTabs", true},
                                                 {"keyboardShortcuts.ownSet", "私の組"}});
        QVERIFY2(out.ok, out.message.c_str());
        QCOMPARE(s->value(QStringLiteral("menus/colors/検索")).toString(), QStringLiteral("red"));
        QCOMPARE(s->value(QStringLiteral("menus/colors/HSB/HSL")).toString(), QStringLiteral("violet"));
        QCOMPARE(s->value(QStringLiteral("ui/workspace_locked")).toString(), QStringLiteral("true"));
        QCOMPARE(s->value(QStringLiteral("toolbar/order")).toString(), QStringLiteral("act_move	|	act_pen"));
        QCOMPARE(prefs::value(*s, "workspace.rememberWorkspaceChanges"), Json(true));  // (on unless turned off)
        QCOMPARE(prefs::value(*s, "workspace.autoShowHiddenPanels"), Json(true));
        QCOMPARE(prefs::get(*s, "menus").values["menus.colors.HSB/HSL"], Json("violet"));
        for (const char* path : {"state.keySets.私の組.base", "state.workspaceChanges.写真"}) {
            QVERIFY2(!prefs::set(*s, Json{{path, "x"}}).ok, path);
        }
        QVERIFY(prefs::reset(*s, "menus").ok);
        QVERIFY(!s->contains(QStringLiteral("menus/colors/検索")) && !s->contains(QStringLiteral("menus/hidden")));
        QCOMPARE(s->value(QStringLiteral("toolbar/hidden")).toString(), QStringLiteral("act_3d"));
    }

    // PC5 H-e: every one of upstream's eighteen sections has its entries but the plug-ins' and the integrations' (節 J's) and
    // the type's (節 G's); the panes' preferences have what Genko does now as their defaults (the computer's colour window,
    // bilinear, the tabs, the wheel scrolling, the start screen, the history log off, all of ヒストリー, 256 MB of the
    // screen's tiles, the trackpad not turning the view); their ranges and choices checked.
    void thePanesOfH_e() {
        for (const char* section : {"general", "interface", "workspace", "tools", "historyLog", "fileHandling", "export", "performance",
                                    "scratchDisks", "cursors", "transparencyAndGamut", "unitsAndRulers", "guidesGridAndSlices",
                                    "enhancedControls", "rawDefaults"}) {
            const bool any = std::any_of(prefs::table().begin(), prefs::table().end(),
                                         [section](const prefs::Pref& p) { return p.path.rfind(std::string(section) + ".", 0) == 0; });
            QVERIFY2(any, section);
        }
        const auto fallback = [](const char* path) { return prefs::decode(*prefs::by_path(path), QVariant()); };
        QCOMPARE(fallback("general.colorPicker"), Json("system"));
        QCOMPARE(fallback("general.imageInterpolation"), Json("bilinear"));
        QCOMPARE(fallback("general.zoomWithScrollWheel"), Json(false));
        QCOMPARE(fallback("general.autoShowHomeScreen"), Json(true));
        QCOMPARE(fallback("general.resizeImageDuringPlace"), Json(true));
        QCOMPARE(fallback("workspace.openDocumentsAsTabs"), Json(true));
        QCOMPARE(fallback("historyLog.enabled"), Json(false));
        QCOMPARE(fallback("historyLog.destination"), Json("metadata"));
        QCOMPARE(fallback("performance.historyStates"), Json(1000));
        QCOMPARE(fallback("performance.memoryUsageMb"), Json(256));
        QCOMPARE(fallback("scratchDisks.folder"), Json(""));
        QCOMPARE(fallback("enhancedControls.rotateViewWithTrackpad"), Json(false));
        QCOMPARE(fallback("interface.canvasBorder"), Json("dropShadow"));
        QCOMPARE(prefs::by_path("interface.uiScale")->apply, prefs::Apply::Restart);
        QCOMPARE(prefs::by_path("scratchDisks.folder")->apply, prefs::Apply::Live);  // (review P3-5: a cache written after goes there)
        {  // (review P3-5: the caches' folder never in a book, never where nothing can be written, never a relative path)
            QTemporaryDir places;
            QVERIFY(QDir(places.path()).mkpath(QStringLiteral("book.genko/assets")));
            QFile project(places.path() + QStringLiteral("/book.genko/project.json"));
            QVERIFY(project.open(QIODevice::WriteOnly));
            project.close();
            const prefs::Pref& scratch = *prefs::by_path("scratchDisks.folder");
            QVERIFY(prefs::check(scratch, Json((places.path() + QStringLiteral("/book.genko")).toStdString())).has_value());
            QVERIFY(prefs::check(scratch, Json((places.path() + QStringLiteral("/book.genko/assets/cache")).toStdString())).has_value());
            QVERIFY(prefs::check(scratch, Json("relative/folder")).has_value());
            QVERIFY(!prefs::check(scratch, Json((places.path() + QStringLiteral("/caches/not/yet")).toStdString())).has_value());
            QVERIFY(!prefs::check(scratch, Json(places.path().toStdString())).has_value());
            QVERIFY(!prefs::check(scratch, Json("")).has_value());
        }
        auto s = fresh("he.ini");
        for (const auto& [path, value] : std::vector<std::pair<const char*, Json>>{{"performance.historyStates", 0},
                                                                                 {"performance.historyStates", 1001},
                                                                                 {"performance.memoryUsageMb", 32},
                                                                                 {"interface.uiScale", "150"},
                                                                                 {"historyLog.detail", "verbose"},
                                                                                 {"interface.canvasBorder", "shadow"}}) {
            QVERIFY2(!prefs::set(*s, Json{{path, value}}).ok, path);
        }
        QVERIFY(prefs::set(*s, Json{{"historyLog", Json{{"enabled", true}, {"destination", "both"}, {"filePath", "/tmp/log.txt"}, {"detail", "detailed"}}},
                                    {"performance.historyStates", 50}})
                    .ok);
        QCOMPARE(s->value(QStringLiteral("history_log/destination")).toString(), QStringLiteral("both"));
        QCOMPARE(s->value(QStringLiteral("ui/history_states")).toInt(), 50);
    }

    // PC5 H-e, review P2-3: the person's own lists (the quick access, the command bar — and 節 G's dictionary and notation
    // table once they are here) are put back only by their names: everything and their section pass over them.
    void theOwnListsAreKeptOnReset() {
        QVERIFY(prefs::by_path("workspace.quickAccess")->kept_on_reset && prefs::by_path("workspace.commandBar")->kept_on_reset);
        QVERIFY(prefs::by_path("type.userDictionary")->kept_on_reset && prefs::by_path("type.notations")->kept_on_reset);  // (節 G's, G-d)
        QVERIFY(!prefs::by_path("type.smartQuotes")->kept_on_reset);
        QVERIFY(!prefs::by_path("workspace.largeTabs")->kept_on_reset);
        auto s = fresh("kept.ini");
        QVERIFY(prefs::set(*s, Json{{"workspace.quickAccess", "検索	取り消し"}, {"workspace.commandBar", "取り消し"}, {"workspace.largeTabs", true},
                                    {"interface.theme", "dark"}, {"type.userDictionary", "Genko	manga"}, {"type.smartQuotes", true}})
                    .ok);
        QVERIFY(prefs::reset(*s).ok);
        QVERIFY(!s->contains(QStringLiteral("ui/theme")));
        QVERIFY(!s->contains(QStringLiteral("type/smart_quotes")));
        QVERIFY(prefs::reset(*s, "type").ok);
        QCOMPARE(s->value(QStringLiteral("type/user_dictionary")).toString(), QStringLiteral("Genko	manga"));
        QCOMPARE(s->value(QStringLiteral("ui/quick")).toString(), QStringLiteral("検索	取り消し"));
        QVERIFY(prefs::reset(*s, "workspace").ok);
        QVERIFY(!s->contains(QStringLiteral("ui/large_tabs")));
        QCOMPARE(s->value(QStringLiteral("ui/commandbar")).toString(), QStringLiteral("取り消し"));
        QVERIFY(prefs::reset(*s, "workspace.quickAccess").ok);  // (named: put back)
        QVERIFY(!s->contains(QStringLiteral("ui/quick")));
        QVERIFY(s->contains(QStringLiteral("ui/commandbar")));
    }

    // PC5 H-e for H-0's review P3-6: keys as Qt writes them, and nothing else.
    void keysAreKeys() {
        const prefs::Pref& keys = *prefs::by_path("shortcuts.*");
        for (const char* fine : {"Ctrl+K", "Ctrl+Shift+K", "Ctrl++", "Ctrl+-", "Ctrl+;", "Ctrl+Alt+Shift+'", "F7", "PgUp", "Shift+F12",
                                 "Ctrl+K, Ctrl+X", "Ctrl+B|Ctrl+Alt+B", "Media Play", "+", "Ctrl+,", "Ctrl+]"}) {
            QVERIFY2(!prefs::check(keys, Json(fine)).has_value(), fine);
        }
        // (review P3-1: what Qt would not read back as the same words — a name it does not know, F99, modifiers out of its
        // order or twice, a letter in lower case)
        for (const char* wrong : {"あいう", "Ctrl+", "Ctrl+B||", "Hyper+K", "Ctrl+あいう", "1, 2, 3, 4, 5", "|", "Banana", "Ctrl+Hello World", "F99", "F0",
                                  "Shift+Ctrl+K", "Ctrl+Ctrl+K", "Ctrl+a"}) {
            QVERIFY2(prefs::check(keys, Json(wrong)).has_value(), wrong);
        }
        QVERIFY(!prefs::check(keys, Json("")).has_value());  // (none: no key)
        QVERIFY(!prefs::check(keys, Json("あ")).has_value());  // (one character is a key: a keyboard's own)
    }
};

QTEST_GUILESS_MAIN(TestPrefsTable)
#include "test_prefs_table.moc"
