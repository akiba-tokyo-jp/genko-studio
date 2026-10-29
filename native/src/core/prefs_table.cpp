#include "core/prefs_table.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "core/config_dir.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"

namespace genko::core::prefs {

namespace {

using Strings = std::vector<std::string>;

Pref boolean(std::string path, std::string key, bool fallback, std::string label, Apply apply = Apply::Live) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Boolean;
    p.fallback = fallback ? "true" : "false";
    p.label = std::move(label);
    p.apply = apply;
    return p;
}

// (a Boolean the app writes with its own words: "1"/"0", "on"/"off")
Pref spelled(Pref p, std::string on, std::string off) {
    p.fallback = p.fallback == "true" ? on : off;
    p.on = std::move(on);
    p.off = std::move(off);
    return p;
}

Pref integer(std::string path, std::string key, int fallback, int min, int max, std::string label, Apply apply = Apply::Live) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Integer;
    p.fallback = std::to_string(fallback);
    p.min = min;
    p.max = max;
    p.label = std::move(label);
    p.apply = apply;
    return p;
}

Pref number(std::string path, std::string key, std::string fallback, double min, double max, std::string label, Apply apply = Apply::Live) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Number;
    p.fallback = std::move(fallback);
    p.min = min;
    p.max = max;
    p.label = std::move(label);
    p.apply = apply;
    return p;
}

Pref choice(std::string path, std::string key, std::string fallback, Strings choices, std::string label, Apply apply = Apply::Live) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Choice;
    p.fallback = std::move(fallback);
    p.choices = std::move(choices);
    p.label = std::move(label);
    p.apply = apply;
    p.none_ok = std::find(p.choices.begin(), p.choices.end(), std::string()) != p.choices.end();
    return p;
}

Pref colour(std::string path, std::string key, std::string fallback, std::string label) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Colour;
    p.fallback = std::move(fallback);
    p.label = std::move(label);
    return p;
}

Pref text(std::string path, std::string key, Kind kind, std::string fallback, std::string label, Apply apply = Apply::Live) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = kind;
    p.fallback = std::move(fallback);
    p.label = std::move(label);
    p.apply = apply;
    p.none_ok = true;
    return p;
}

// (what the tools and the windows remember: kept as written)
Pref memory(std::string path, std::string key) {
    Pref p;
    p.path = std::move(path);
    p.key = std::move(key);
    p.kind = Kind::Data;
    p.apply = Apply::Never;
    p.none_ok = true;
    return p;
}

// (none is its value when nothing is chosen: the computer's own, the book's)
Pref no_default(Pref p) {
    p.none_ok = true;
    p.fallback.clear();
    return p;
}

Strings paper_keys() {
    Strings out{""};  // (前回と同じ: B4 投稿用)
    for (const PaperPreset& preset : paper_presets()) out.emplace_back(preset.key);
    return out;
}

std::vector<Pref> make_table() {
    const Strings held{"", "picker", "select", "move", "eraser"};
    const Strings per_tool{"", "none", "picker", "select", "move", "eraser", "pen", "fill"};
    const Strings policies{"preserve", "convert", "off"};
    std::vector<Pref> t{
        // interface (インターフェイス)
        choice("interface.theme", "ui/theme", "system", {"system", "light", "dark"}, "画面の色"),
        integer("interface.panelBrightness", "ui/brightness", 0, -2, 2, "パネルの明るさ"),
        choice("interface.canvasColor", "ui/surround", "auto", {"auto", "58", "128", "190", "20"}, "原稿のまわり"),
        choice("interface.uiFont", "ui/ui_font", "genko", {"genko", "system"}, "画面の書体"),
        integer("interface.uiFontSize", "ui/font_pt", 0, 0, 24, "画面の文字の大きさ（0: パソコンの設定のまま）"),
        boolean("interface.monoIcons", "ui/mono_icons", false, "アイコンをグレーだけで描く"),
        boolean("interface.showHints", "ui/hints", false, "パネルに説明の文を出す"),
        spelled(boolean("interface.toolNames", "ui/tool_names", false, "道具の名前を表示", Apply::NewWindow), "1", "0"),
        spelled(boolean("interface.nextSteps", "ui/next_steps", true, "次の作業の案内を表示", Apply::NewWindow), "on", "off"),
        // workspace (ワークスペース)
        text("workspace.current", "ui/workspace", Kind::Text, "", "今のワークスペース", Apply::NewWindow),
        text("workspace.stage", "ui/stage", Kind::Text, "", "作業の段階", Apply::NewWindow),
        text("workspace.quickAccess", "ui/quick", Kind::Text, "", "クイックアクセスに並べるコマンド（タブ区切り）", Apply::NewWindow),
        text("workspace.commandBar", "ui/commandbar", Kind::Text, "", "コマンドバーに並べるコマンド（タブ区切り）", Apply::NewWindow),
        // tools (ツール)
        boolean("tools.radialMenu", "ui/radial", true, "描く道具の右クリックで円形のメニュー"),
        choice("tools.altTool", "keys/alt_tool", "picker", held, "Alt を押している間"),
        choice("tools.ctrlTool", "keys/ctrl_tool", "select", held, "Ctrl を押している間"),
        boolean("tools.holdToSwitch", "keys/hold_swap", true, "道具のキーを押している間だけ持ち替える"),
        choice("tools.modifiers.*.*", "keys/*/*", "", per_tool, "道具ごとの修飾キー"),
        // fileHandling (ファイル管理)
        integer("fileHandling.recentFileCount", "files/recent_count", 20, 0, 100, "最近使用したファイルの数"),
        boolean("fileHandling.lowercaseExtension", "files/lowercase_extension", true, "拡張子を小文字にする"),
        boolean("fileHandling.ignoreExifProfileTag", "files/ignore_exif_profile", false, "EXIF のプロファイルのタグを無視"),
        // export (書き出し)
        choice("export.quickExportFormat", "export/quick_format", "png", {"png", "jpg", "gif", "webp"}, "クイック書き出しの形式"),
        choice("export.quickExportLocation", "export/quick_location", "ask", {"ask", "sameFolder", "exports"}, "クイック書き出しの場所"),
        integer("export.jpegQuality", "export/jpeg_quality", 85, 1, 100, "JPG の画質"),
        choice("export.metadata", "export/metadata", "copyright", {"none", "copyright", "all"}, "メタデータ"),
        boolean("export.convertToSrgb", "export/srgb", true, "sRGB に変換"),
        // performance (パフォーマンス)
        boolean("performance.useGpu", "ui/gpu_canvas", false, "原稿の表示にグラフィックボードを使う", Apply::NewWindow),
        // (PC5 H-a, render:GPU-CPU-parity: the most the page's tiles take on the card)
        integer("performance.gpuMemoryMb", "ui/gpu_memory_mb", 512, 64, 1024, "グラフィックボードの記憶の上限（MB）", Apply::NewWindow),
        // cursors (カーソル)
        choice("cursors.painting", "ui/cursor", "circle", {"circle", "circle_cross", "cross", "dot", "tip"}, "ペンのカーソル"),
        boolean("cursors.showOnlyCrosshairWhilePainting", "ui/cross_drawing", false, "描いている間は十字だけにする"),
        // transparencyAndGamut (透明部分・色域)
        choice("transparencyAndGamut.gridSize", "view/checker_size", "medium", {"none", "small", "medium", "large"}, "市松の大きさ"),
        choice("transparencyAndGamut.gridColors", "view/checker_colors", "light",
               {"light", "medium", "dark", "red", "orange", "green", "blue", "purple", "custom"}, "市松の色"),
        colour("transparencyAndGamut.customLight", "view/checker_light", "#ffffff", "カスタムの色（明るい方）"),
        colour("transparencyAndGamut.customDark", "view/checker_dark", "#cccccc", "カスタムの色（暗い方）"),
        colour("transparencyAndGamut.gamutWarningColor", "view/gamut_color", "#808080", "色域外警告の色"),
        integer("transparencyAndGamut.gamutWarningOpacity", "view/gamut_opacity", 100, 1, 100, "色域外警告の不透明度（%）"),
        // unitsAndRulers (単位・定規 — PC5 H-b: the ruler's unit is 定規's, the guides', the grid's, 情報's and the size windows';
        // 文字 the type's (節 G); a column and its gutter (points) are 画像解像度's and カンバスサイズ's 列)
        choice("unitsAndRulers.rulers", "units/ruler", "mm", {"px", "in", "cm", "mm", "pt", "pica", "percent"}, "定規の単位"),
        choice("unitsAndRulers.typeUnits", "units/type", "pt", {"pt", "px", "mm"}, "文字の単位"),
        number("unitsAndRulers.columnWidth", "units/column_width", "180", 1, 10000, "段組の幅（pt）"),
        number("unitsAndRulers.gutter", "units/gutter", "12", 0, 10000, "段組の間隔（pt）"),
        number("unitsAndRulers.printResolution", "units/print_resolution", "300", 1, 10000, "新規の印刷の解像度（ppi）"),
        number("unitsAndRulers.screenResolution", "units/screen_resolution", "96", 1, 10000, "画面の解像度（ppi。プリントサイズ）"),
        choice("unitsAndRulers.pointSize", "units/point_size", "postscript", {"postscript", "traditional"}, "ポイント/パイカの単位"),
        // guidesGridAndSlices (ガイド・グリッド・スライス — PC5 H-b; slices' two are 節 J's)
        colour("guidesGridAndSlices.guideColor", "guides/guide_color", "#4affff", "ガイドの色"),
        choice("guidesGridAndSlices.guideStyle", "guides/guide_style", "lines", {"lines", "dashedLines", "dots"}, "ガイドの線種"),
        colour("guidesGridAndSlices.smartGuideColor", "guides/smart_color", "#e64980", "スマートガイドの色"),
        colour("guidesGridAndSlices.gridColor", "guides/grid_color", "#508cdc", "グリッドの色"),
        choice("guidesGridAndSlices.gridStyle", "guides/grid_style", "lines", {"lines", "dashedLines", "dots"}, "グリッドの線種"),
        // (the thin lines' step, what snaps — Genko's own path: Photoshop's グリッド線 is the thick lines' step in gridUnit, which
        // the screen shows as this × subdivisions; the review's P2-3, the coordinator's decision: not a path of its own here)
        number("guidesGridAndSlices.gridSpacingMm", "guides/grid_mm", "5", 0.5, 100, "グリッドの細い線の間隔（mm。グリッド線 ÷ 分割数）"),
        integer("guidesGridAndSlices.subdivisions", "guides/grid_subdivisions", 5, 1, 100, "分割数（太い線の間の細い線の数）"),
        choice("guidesGridAndSlices.gridUnit", "guides/grid_unit", "mm", {"mm", "cm", "in", "pt", "pica"}, "グリッド線の単位"),
        // (PC5 J-b: the slices' two — their lines' colour and their numbers shown)
        colour("guidesGridAndSlices.sliceColor", "guides/slice_color", "#38b5ff", "スライスの線の色"),
        boolean("guidesGridAndSlices.showSliceNumbers", "guides/slice_numbers", true, "スライス番号を表示"),
        // rawDefaults (Camera Raw)
        choice("rawDefaults.colorSpace", "raw/space", "adobe-rgb-compat", {"prophoto-compat", "adobe-rgb-compat", "srgb", "display-p3"}, "色空間"),
        choice("rawDefaults.bitDepth", "raw/depth", "16", {"8", "16"}, "ビット数"),
        number("rawDefaults.resolution", "raw/resolution", "300", 1, 100000, "解像度（ppi）"),
        choice("rawDefaults.sharpenFor", "raw/sharpen", "none", {"none", "screen", "glossy", "matte"}, "出力のシャープ"),
        boolean("rawDefaults.openAsSmartObject", "raw/smart_object", false, "スマートオブジェクトとして開く"),
        boolean("rawDefaults.applyAutoTone", "raw/auto_tone", false, "自動補正を適用"),
        // tablet (ペンタブレット)
        no_default(number("tablet.pressureGamma", "tablet/gamma", "", 0.3, 3, "自分に合わせた筆圧の曲線（γ。なし: ふつう）")),
        choice("tablet.penButton", "tablet/button", "menu", {"menu", "picker", "hand"}, "ペンのサイドボタン"),
        // comfort (長い時間の作業)
        integer("comfort.restMinutes", "ui/rest_minutes", 0, 0, 1440, "休憩の案内（分。0: 知らせない）"),
        no_default(boolean("comfort.reduceMotion", "ui/reduce_motion", false, "動きを減らす（なし: パソコンの設定に合わせる）")),
        boolean("comfort.raiseRequests", "ui/raise_requests", true, "承認の依頼が届いたら、承認箱を前に出す"),
        // book (原稿と保存)
        choice("book.newPaper", "new/paper", "", paper_keys(), "新しい原稿の用紙", Apply::NewWindow),
        integer("book.saveAfterMs", "save/after_ms", 2000, 500, 10000, "変更を保存するまで（ミリ秒）"),
        text("book.backupFolder", "backup/folder", Kind::Path, "", "バックアップの置き場所"),
        number("book.backupMinutes", "backup/minutes", "30", 1, 1440, "バックアップの間隔（分）"),
        integer("book.backupKeep", "backup/keep", 10, 1, 200, "バックアップの数"),
        // colorSettings (編集 → カラー設定…)
        text("colorSettings.workingRgb", "color/workingRgb", Kind::Text, "srgb", "作業用の RGB"),
        text("colorSettings.workingCmyk", "color/workingCmyk", Kind::Text, "coated-cmyk", "作業用の CMYK"),
        text("colorSettings.workingGray", "color/workingGray", Kind::Text, "sgray", "作業用のグレー"),
        choice("colorSettings.policyRgb", "color/policyRgb", "preserve", policies, "RGB のカラーマネジメントポリシー"),
        choice("colorSettings.policyCmyk", "color/policyCmyk", "preserve", policies, "CMYK のカラーマネジメントポリシー"),
        choice("colorSettings.policyGray", "color/policyGray", "preserve", policies, "グレーのカラーマネジメントポリシー"),
        boolean("colorSettings.askOnMismatch", "color/askOnMismatch", true, "プロファイルの不一致: 開くときに確認"),
        boolean("colorSettings.askOnPaste", "color/askOnPaste", true, "プロファイルの不一致: ペーストするときに確認"),
        boolean("colorSettings.askOnMissing", "color/askOnMissing", false, "プロファイルなし: 開くときに確認"),
        choice("colorSettings.intent", "color/intent", "relative", {"perceptual", "relative", "saturation", "absolute"}, "マッチング方法"),
        boolean("colorSettings.blackPointCompensation", "color/bpc", true, "黒点の補正を使用"),
        boolean("colorSettings.dither", "color/dither", false, "ディザの使用"),
        text("colorSettings.monitorProfile", "color/monitorProfile", Kind::Text, "auto", "モニターのプロファイル"),
        text("colorSettings.exportCmykProfile", "color/icc", Kind::Path, "", "書き出しの CMYK プロファイル"),
        // shortcuts (a command's keys, by its name — the words, with its place when another command has them)
        text("shortcuts.*", "shortcuts/*", Kind::Keys, "", "ショートカット"),
        // keyboardShortcuts (キーボードショートカット: the key set — PC5 H-0, the user's decision 8)
        choice("keyboardShortcuts.keySet", "ui/key_set", "photoshop", {"photoshop", "genko"}, "キーの組"),
        // view (the view's switches kept for the next window)
        boolean("view.brushPreview", "ui/brush_preview", true, "ブラシプレビュー"),
        no_default(boolean("view.phoneView", "ui/phone_view", false, "スマホの画面の範囲を表示（なし: 縦読みの原稿だけ）")),
        boolean("view.snapToDrawingRulers", "guides/snap", true, "描画定規にスナップ", Apply::NewWindow),
        boolean("view.showDrawingRulers", "guides/show", true, "描画定規を表示", Apply::NewWindow),
        boolean("view.grid", "guides/grid", false, "グリッドを表示", Apply::NewWindow),
        boolean("view.snapToGrid", "guides/grid_snap", false, "グリッドにスナップ", Apply::NewWindow),
        boolean("view.snapTo.*", "snap/*", true, "スナップ先", Apply::NewWindow),
        // (PC5 H-b: エクストラ and 表示・非表示 ▸'s own switches, ガイドをロック — this computer's, never the book's)
        boolean("view.extras", "view/extras", true, "エクストラ", Apply::NewWindow),
        boolean("view.show.guides", "view/show_guides", true, "ガイド", Apply::NewWindow),
        boolean("view.show.canvasGuides", "view/show_canvas_guides", true, "カンバスガイド", Apply::NewWindow),
        boolean("view.show.smartGuides", "view/show_smart_guides", true, "スマートガイド", Apply::NewWindow),
        boolean("view.show.layerEdges", "view/show_layer_edges", false, "レイヤーエッジ", Apply::NewWindow),
        boolean("view.show.mesh", "view/show_mesh", true, "メッシュ", Apply::NewWindow),
        boolean("view.show.editPins", "view/show_edit_pins", true, "編集ピン", Apply::NewWindow),
        boolean("view.lockGuides", "guides/lock", false, "ガイドをロック", Apply::NewWindow),
        // (PC5 J-c・J-d1: 表示 → 表示・非表示 ▸ 注釈・カウント)
        boolean("view.show.notes", "view/show_notes", true, "注釈", Apply::NewWindow),
        boolean("view.show.count", "view/show_count", true, "カウント", Apply::NewWindow),
        // (PC5 J-b: 表示 → 表示・非表示 ▸ スライス)
        boolean("view.show.slices", "view/show_slices", true, "スライス", Apply::NewWindow),
        // state (what the tools and the windows remember)
        memory("state.3d.*", "3d/*"),
        memory("state.ai.*", "ai/*"),
        memory("state.align.*", "align/*"),
        memory("state.brush.*", "brush/*"),
        memory("state.colour.*", "colour/*"),
        memory("state.eraser.*", "eraser/*"),
        memory("state.fill.*", "fill/*"),
        memory("state.gradient.*", "gradient/*"),
        memory("state.gutter.*", "gutter_*"),
        memory("state.guideLayouts.*", "guide_layouts/*"),  // (PC5 H-b: 新規ガイドレイアウトを作成…'s presets)
        memory("state.layerDetails", "ui/layer_details"),
        memory("state.keySetTold", "ui/key_set_told"),  // (PC5 H-0: the change of the default key set said once)
        memory("state.migration.*", "migration/*"),
        memory("state.move.*", "move/*"),
        memory("state.namesCurrent.*", "names_current/*"),
        memory("state.patterns.*", "patterns/*"),
        memory("state.pencil.*", "pencil/*"),
        memory("state.picker.*", "picker/*"),
        memory("state.raw.*", "raw/remembered*"),
        memory("state.shapes.*", "shapes/*"),
        memory("state.shortcutsUnread.*", "shortcuts_unread/*"),
        memory("state.stroke.*", "stroke/*"),
        memory("state.subview.*", "subview/*"),
        memory("state.toolPresets.*", "tool_presets/*"),
        memory("state.warp.*", "warp/*"),
        memory("state.workspaces.*", "workspaces/*"),
    };
    return t;
}

// --- patterns ------------------------------------------------------------------------------------------------------

std::vector<std::string> split(std::string_view text, char by) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t next = text.find(by, at);
        out.emplace_back(text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos) return out;
        at = next + 1;
    }
}

bool is_pattern(std::string_view text) { return text.find('*') != std::string_view::npos; }

// Whether `text` fits `pattern` (parts split by `by`; a part "*" or "prefix*" fits one part — the last pattern part
// takes the rest, its own separators too), the names the stars stand for in `names`.
bool fits(std::string_view pattern, std::string_view text, char by, std::vector<std::string>* names) {
    const auto want = split(pattern, by);
    const auto got = split(text, by);
    if (got.size() < want.size()) return false;
    std::vector<std::string> found;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string& w = want[i];
        std::string part = got[i];
        if (i + 1 == want.size()) {  // (the last: the rest)
            for (std::size_t j = i + 1; j < got.size(); ++j) part += by + got[j];
        }
        const std::size_t star = w.find('*');
        if (star == std::string::npos) {
            if (part != w) return false;
            continue;
        }
        const std::string prefix = w.substr(0, star);
        if (part.size() <= prefix.size() || part.compare(0, prefix.size(), prefix) != 0) return false;
        found.push_back(part.substr(prefix.size()));
    }
    if (got.size() > want.size() && want.back().find('*') == std::string::npos) return false;
    if (names != nullptr) *names = std::move(found);
    return true;
}

// The pattern with its stars filled by `names` in order.
std::string fill(std::string_view pattern, const std::vector<std::string>& names) {
    std::string out;
    std::size_t n = 0;
    for (const char c : pattern) {
        if (c == '*' && n < names.size()) {
            out += names[n++];
        } else {
            out += c;
        }
    }
    return out;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) out += (out.empty() ? "" : "/") + name;
    return out;
}

const Pref* lookup(std::string_view text, bool by_key_side, std::string* name) {
    const char by = by_key_side ? '/' : '.';
    for (const Pref& p : table()) {
        const std::string& side = by_key_side ? p.key : p.path;
        if (!is_pattern(side) && side == text) {
            if (name != nullptr) name->clear();
            return &p;
        }
    }
    for (const Pref& p : table()) {
        const std::string& side = by_key_side ? p.key : p.path;
        std::vector<std::string> names;
        if (is_pattern(side) && fits(side, text, by, &names)) {
            if (name != nullptr) *name = joined(names);
            return &p;
        }
    }
    return nullptr;
}

std::string section_of(std::string_view path) { return std::string(path.substr(0, path.find('.'))); }

bool is_section(std::string_view path) {
    const auto& all = sections();
    return std::find(all.begin(), all.end(), path) != all.end();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool hex_colour(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') return false;
    return std::all_of(text.begin() + 1, text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string number_text(double value) {
    if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15) return std::to_string(static_cast<long long>(value));
    return QString::number(value, 'g', 15).toStdString();
}

std::string describe(const Pref& p) {
    switch (p.kind) {
    case Kind::Boolean: return "true か false";
    case Kind::Integer: return "整数（" + number_text(p.min) + "〜" + number_text(p.max) + "）";
    case Kind::Number: return "数（" + number_text(p.min) + "〜" + number_text(p.max) + "）";
    case Kind::Choice: {
        std::string out;
        for (const std::string& c : p.choices) out += (out.empty() ? "" : "・") + (c.empty() ? std::string("\"\"") : c);
        return out + " のどれか";
    }
    case Kind::Colour: return "#rrggbb の色";
    case Kind::Keys: return "キー（Ctrl+K など。いくつかは | で区切る）";
    default: return "文字列";
    }
}

// The file's keys a pattern's entry has now.
std::vector<std::string> present(QSettings& settings, const Pref& p) {
    std::vector<std::string> out;
    for (const QString& key : settings.allKeys()) {
        const std::string text = key.toStdString();
        if (by_key(text) == &p) out.push_back(text);
    }
    return out;
}

// Each entry a path names: the entries of a section, the one of a path (a pattern's with its name).
struct Named {
    const Pref* pref;
    std::string path;  // the path with the pattern's name filled
    std::string key;
};

Outcome refused(std::string code, std::string path, std::string message) {
    Outcome out;
    out.ok = false;
    out.code = std::move(code);
    out.path = std::move(path);
    out.message = std::move(message);
    return out;
}

Outcome unknown(std::string_view path) {
    return refused("unknown_pref", std::string(path), "「" + std::string(path) + "」という環境設定はありません");
}

// The entries a path names in the file: every entry ("" — the app's memories left out), a section's, or one.
std::optional<std::vector<Named>> named(QSettings& settings, std::string_view path) {
    std::vector<Named> out;
    const auto add = [&](const Pref& p) {
        if (!is_pattern(p.key)) {
            out.push_back(Named{&p, p.path, p.key});
            return;
        }
        for (const std::string& key : present(settings, p)) {
            std::vector<std::string> names;
            (void)fits(p.key, key, '/', &names);
            out.push_back(Named{&p, fill(p.path, names), key});
        }
    };
    if (path.empty()) {
        for (const Pref& p : table()) {
            if (section_of(p.path) != "state") add(p);
        }
        return out;
    }
    if (is_section(path)) {
        for (const Pref& p : table()) {
            if (section_of(p.path) == path) add(p);
        }
        return out;
    }
    std::string name;
    const Pref* p = by_path(path, &name);
    if (p == nullptr) return std::nullopt;
    if (is_pattern(p->path)) {
        std::vector<std::string> names;
        (void)fits(p->path, path, '.', &names);
        out.push_back(Named{p, std::string(path), fill(p->key, names)});
    } else {
        out.push_back(Named{p, p->path, p->key});
    }
    return out;
}

}  // namespace

const std::vector<std::string>& sections() {
    static const std::vector<std::string> all{
        // (upstream's, in Photoshop's order of 環境設定's panes)
        "general", "interface", "workspace", "tools", "historyLog", "fileHandling", "export", "performance", "scratchDisks", "cursors",
        "transparencyAndGamut", "unitsAndRulers", "guidesGridAndSlices", "plugIns", "type", "enhancedControls", "rawDefaults", "integrations",
        // (Genko's)
        "tablet", "comfort", "book", "colorSettings", "shortcuts", "keyboardShortcuts", "menus", "toolbar", "view", "state"};
    return all;
}

const std::vector<Pref>& table() {
    static const std::vector<Pref> all = [] {
        std::vector<Pref> t = make_table();
        // (in the sections' order)
        std::stable_sort(t.begin(), t.end(), [](const Pref& a, const Pref& b) {
            const auto& s = sections();
            return std::find(s.begin(), s.end(), section_of(a.path)) < std::find(s.begin(), s.end(), section_of(b.path));
        });
        return t;
    }();
    return all;
}

const Pref* by_path(std::string_view path, std::string* name) { return lookup(path, false, name); }

const Pref* by_key(std::string_view key, std::string* name) { return lookup(key, true, name); }

std::optional<std::string> key_of(std::string_view path) {
    const Pref* p = by_path(path);
    if (p == nullptr) return std::nullopt;
    if (!is_pattern(p->path)) return p->key;
    std::vector<std::string> names;
    (void)fits(p->path, path, '.', &names);
    return fill(p->key, names);
}

std::optional<std::string> path_of(std::string_view key) {
    const Pref* p = by_key(key);
    if (p == nullptr) return std::nullopt;
    if (!is_pattern(p->key)) return p->path;
    std::vector<std::string> names;
    (void)fits(p->key, key, '/', &names);
    return fill(p->path, names);
}

std::optional<std::string> check(const Pref& p, const Json& value) {
    const auto wrong = [&p] { return std::optional<std::string>("「" + p.path + "」は " + describe(p) + " です"); };
    if (value.is_string() && value.get<std::string>().empty() && p.none_ok) return std::nullopt;
    switch (p.kind) {
    case Kind::Boolean:
        return value.is_boolean() ? std::nullopt : wrong();
    case Kind::Integer:
    case Kind::Number: {
        if (!value.is_number()) return wrong();
        const double v = value.get<double>();
        if (!std::isfinite(v)) return wrong();
        if (p.kind == Kind::Integer && v != std::floor(v)) return wrong();
        if (p.min != p.max && (v < p.min || v > p.max)) return wrong();
        return std::nullopt;
    }
    case Kind::Choice:
        if (!value.is_string()) return wrong();
        return std::find(p.choices.begin(), p.choices.end(), value.get<std::string>()) != p.choices.end() ? std::nullopt : wrong();
    case Kind::Colour:
        return value.is_string() && hex_colour(value.get<std::string>()) ? std::nullopt : wrong();
    case Kind::Keys: {
        if (!value.is_string()) return wrong();
        for (const std::string& one : split(value.get<std::string>(), '|')) {
            if (one.empty()) return wrong();
        }
        return std::nullopt;
    }
    case Kind::Text:
    case Kind::Path:
    case Kind::Data:
        return value.is_string() ? std::nullopt : wrong();
    }
    return wrong();
}

Json decode(const Pref& p, const QVariant& stored) {
    const bool has = stored.isValid() && !stored.isNull();
    if (p.kind == Kind::Data) {
        if (!has) return Json();
        if (stored.typeId() == QMetaType::QByteArray) return stored.toByteArray().toBase64().toStdString();
        if (stored.typeId() == QMetaType::QStringList) {
            Json list = Json::array();
            for (const QString& one : stored.toStringList()) list.push_back(one.toStdString());
            return list;
        }
        return stored.toString().toStdString();
    }
    // (none kept: the fallback; kept but empty: none chosen where that is a value, else the fallback)
    if (!has || stored.toString().isEmpty()) {
        if (has && p.none_ok) return std::string();
        if (p.fallback.empty()) return std::string();
        return decode(p, QString::fromStdString(p.fallback));
    }
    const std::string written = stored.toString().toStdString();
    const auto fallback = [&p]() -> Json { return decode(p, QVariant()); };
    switch (p.kind) {
    case Kind::Boolean: {
        const std::string w = lower(written);
        if (w == lower(p.on) || w == "1" || w == "true" || w == "yes") return true;
        if (w == lower(p.off) || w == "0" || w == "false" || w == "no") return false;
        return fallback();
    }
    case Kind::Integer:
    case Kind::Number: {
        bool ok = false;
        const double v = QString::fromStdString(written).toDouble(&ok);
        if (!ok || !std::isfinite(v)) return fallback();
        double out = p.kind == Kind::Integer ? std::round(v) : v;
        if (p.min != p.max) out = std::clamp(out, p.min, p.max);
        if (p.kind == Kind::Integer) return static_cast<long long>(out);
        return out;
    }
    case Kind::Choice:
        if (std::find(p.choices.begin(), p.choices.end(), written) != p.choices.end()) return written;
        return fallback();
    case Kind::Colour: {
        const std::string w = lower(written);
        if (hex_colour(w)) return w;
        return fallback();
    }
    default:
        return written;
    }
}

QVariant encode(const Pref& p, const Json& value) {
    if (value.is_string() && value.get<std::string>().empty()) return QString();
    switch (p.kind) {
    case Kind::Boolean: return QString::fromStdString(value.get<bool>() ? p.on : p.off);
    case Kind::Integer: return static_cast<qlonglong>(std::llround(value.get<double>()));
    case Kind::Number: return value.get<double>();
    case Kind::Colour: return QString::fromStdString(lower(value.get<std::string>()));
    default: return QString::fromStdString(value.get<std::string>());
    }
}

const std::vector<std::string>& reused_names() {
    static const std::vector<std::string> all{"グラデーション", "書き出し…"};
    return all;
}

std::optional<std::string> mark_of(std::string_view key) {
    if (key == "ui/quick" || key == "ui/commandbar") return "names_current/" + std::string(key);
    if (key.substr(0, 10) == "shortcuts/") {
        const std::string name(key.substr(10));
        const auto& reused = reused_names();
        if (std::find(reused.begin(), reused.end(), name) != reused.end()) return "names_current/" + std::string(key);
    }
    return std::nullopt;
}

std::unique_ptr<QSettings> store() {
    return std::make_unique<QSettings>(QString::fromStdString(path_to_utf8(config_dir() / "settings.ini")), QSettings::IniFormat);
}

Json Outcome::json() const {
    if (ok) return Json{{"ok", true}, {"values", values}};
    return Json{{"ok", false}, {"error", code}, {"path", path}, {"message", message}};
}

Outcome get(QSettings& settings, std::string_view path) {
    const auto entries = named(settings, path);
    if (!entries) return unknown(path);
    Outcome out;
    for (const Named& n : *entries) out.values[n.path] = decode(*n.pref, settings.value(QString::fromStdString(n.key)));
    return out;
}

Outcome set(QSettings& settings, const Json& values) {
    if (!values.is_object()) return refused("bad_value", "", "値は {\"section.key\": 値} の形で渡します");
    // (a section's object: its keys)
    Json flat = Json::object();
    for (const auto& [path, value] : values.items()) {
        if (is_section(path) && value.is_object()) {
            for (const auto& [key, inner] : value.items()) flat[path + "." + key] = inner;
        } else {
            flat[path] = value;
        }
    }
    std::vector<std::pair<std::string, QVariant>> writes;
    Outcome out;
    for (const auto& [path, value] : flat.items()) {
        const Pref* p = by_path(path);
        if (p == nullptr || (is_pattern(p->path) && path.find('*') != std::string::npos)) return unknown(path);
        if (p->kind == Kind::Data) return refused("bad_value", path, "「" + path + "」はアプリが覚えている値で、ここからは変えられません");
        if (const auto wrong = check(*p, value)) return refused("bad_value", path, *wrong);
        writes.emplace_back(*key_of(path), encode(*p, value));
        out.values[path] = value;
    }
    for (const auto& [key, stored] : writes) {
        settings.setValue(QString::fromStdString(key), stored);
        // (a list or a key that may hold a reused name: read as this build's names — the mark the app writes too)
        if (const auto mark = mark_of(key)) settings.setValue(QString::fromStdString(*mark), stored);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) return refused("config_unwritable", "", "環境設定のファイルに書き込めません");
    return out;
}

Outcome reset(QSettings& settings, std::string_view path) {
    const auto entries = named(settings, path);
    if (!entries) return unknown(path);
    // (the app's memories — the workspaces kept, the tools' settings, migration/done — are not preferences: never reset
    // from here, as set never writes them)
    const auto memory = [](const Pref* p) { return p->kind == Kind::Data; };
    if (section_of(path) == "state" || std::any_of(entries->begin(), entries->end(), [&](const Named& n) { return memory(n.pref); })) {
        return refused("bad_value", std::string(path), "「" + std::string(path) + "」はアプリが覚えている値で、ここからは戻せません");
    }
    Outcome out;
    for (const Named& n : *entries) {
        settings.remove(QString::fromStdString(n.key));
        if (!is_pattern(n.pref->key)) out.values[n.path] = decode(*n.pref, QVariant());
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) return refused("config_unwritable", "", "環境設定のファイルに書き込めません");
    return out;
}

Json value(QSettings& settings, std::string_view path) {
    const Pref* p = by_path(path);
    if (p == nullptr) return Json();
    return decode(*p, settings.value(QString::fromStdString(*key_of(path))));
}

Json value(std::string_view path) {
    const auto settings = store();
    return value(*settings, path);
}

}  // namespace genko::core::prefs
