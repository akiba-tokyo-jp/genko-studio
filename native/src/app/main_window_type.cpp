// PC5 節 G (文字; type@1, SPEC.md TEXT-02, design section G 5.1): the 書式 menu — Photoshop's, after レイヤー — and the type
// layers in the window. G-0 made the menu with the type commands this build had (ワープテキスト…, 作業用パスを作成, シェイプ
// に変換: the same commands as ページ → 台詞); G-a adds アンチエイリアス ▸, 方向 ▸, テキストレイヤーをラスタライズ, ポイント
// テキストに変換・段落テキストに変換, すべてのテキストレイヤーを更新, Lorem Ipsum をペースト, the 横書き文字ツール, the text
// tools' type layers — a click a point text, a drag a box text, a click on the target path a text on it, a click on a
// type layer's text typing over it —, and the chosen type layer's box on the canvas. The later groups add theirs in
// Photoshop's order: パネル ▸, OpenType ▸ (G-b, G-d), フォントプレビューのサイズ ▸, 言語オプション ▸, the fonts' commands and
// 初期設定の書式 (G-d).

#include <QActionGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QMenu>

#include <algorithm>
#include <cmath>
#include <map>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/ime.hpp"
#include "app/layer_panel.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/tool_settings.hpp"
#include "core/ids.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "core/vector.hpp"
#include "render/not_yet_ported.hpp"
#include "render/text/lettering.hpp"
#include "render/text/shaping.hpp"
#include "render/type_layer.hpp"

namespace genko::app {

using core::Json;
using core::Num;

namespace {

constexpr int kFrameDpi = 150;  // (the chosen type layer's box on the canvas: where its letters are, measured this finely)

// Lorem Ipsum をペースト: Photoshop's Latin, or (down) a Japanese dummy of Genko's own words.
const char* kLorem =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.";
const char* kDummy = "ここに文章が入ります。文字の大きさや行の送り、字の並びを確かめるための仮の文です。あとで本当の文に置き換えます。";

// PC5 G-b: 書式 → OpenType ▸ in Photoshop's order (its Japanese names), and — 東アジア言語の機能 — プロポーショナルメトリクス.
const std::vector<std::pair<QString, QString>>& opentype_names() {
    static const std::vector<std::pair<QString, QString>> names{
        {QStringLiteral("liga"), QStringLiteral("標準合字")},          {QStringLiteral("calt"), QStringLiteral("前後関係に依存する字形")},
        {QStringLiteral("dlig"), QStringLiteral("任意の合字")},        {QStringLiteral("swsh"), QStringLiteral("スワッシュ字形")},
        {QStringLiteral("salt"), QStringLiteral("デザインのバリエーション")}, {QStringLiteral("titl"), QStringLiteral("タイトル用字形")},
        {QStringLiteral("ornm"), QStringLiteral("飾り文字")},          {QStringLiteral("ordn"), QStringLiteral("序数表記")},
        {QStringLiteral("frac"), QStringLiteral("分数字")},            {QStringLiteral("onum"), QStringLiteral("オールドスタイル")},
        {QStringLiteral("palt"), QStringLiteral("プロポーショナルメトリクス")}};
    return names;
}

// The settings' key of 言語オプション ▸ (default, east_asian or middle_eastern; Genko's default: East Asian, as Photoshop's
// Japanese edition).
const QString kLanguageFeatures = QStringLiteral("type/language_features");

// The first subpath of a path as the points a text on a path follows (each curve sampled), or none.
std::vector<QPointF> path_points(const core::vector::Path& path) {
    std::vector<QPointF> out;
    if (path.subpaths.empty() || path.subpaths.front().knots.size() < 2) return out;
    for (const auto& seg : path.subpaths.front().segments()) {
        for (int i = out.empty() ? 0 : 1; i <= 8; ++i) {
            const double t = i / 8.0, u = 1 - t;
            const double x = u * u * u * seg[0].x + 3 * u * u * t * seg[1].x + 3 * u * t * t * seg[2].x + t * t * t * seg[3].x;
            const double y = u * u * u * seg[0].y + 3 * u * u * t * seg[1].y + 3 * u * t * t * seg[2].y + t * t * t * seg[3].y;
            out.emplace_back(x, y);
        }
    }
    return out;
}

// How far a point is from a polyline (mm).
double distance_to(const std::vector<QPointF>& points, const QPointF& p) {
    double best = 1e300;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i], b = points[i + 1];
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        const double len = dx * dx + dy * dy;
        const double t = len > 0 ? std::clamp(((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy)));
    }
    return best;
}

}  // namespace

void MainWindow::build_type_actions() {
    // 横書き文字ツール (Photoshop's; テキスト — act_text — sets down or across as its options say)
    make("act_text_h", QStringLiteral("横書き文字ツール"), [this] { choose_tool(QStringLiteral("text_h")); }, {QKeySequence(QStringLiteral("Shift+T"))},
         QStringLiteral("クリックした所に横書きの文字を入れます（テキストレイヤーならドラッグで段落テキスト）"), true);
    QAction* across = action("act_text_h");
    tools_->addAction(across);
    across->setAutoRepeat(false);
    tool_actions_[QStringLiteral("text_h")] = across;
    // 書式 → アンチエイリアス ▸ (the chosen type layer's or line's; checked: its own)
    auto* methods = new QActionGroup(this);
    methods->setExclusive(true);
    for (const auto& [key, label] : antialias_names()) {
        const std::string method = key.toStdString();
        QAction* act = make(QStringLiteral("act_type_aa_") + key, label, [this, method] { type_command(Json{{"antialias", method}}); }, {},
                            QStringLiteral("選んだテキストレイヤー・台詞の文字の縁の描き方（「なし」は縁をぼかさない。ほかは滑らかに描き、PSD に名前を書く）"), true);
        methods->addAction(act);
    }
    // 書式 → 方向 ▸
    auto* ways = new QActionGroup(this);
    ways->setExclusive(true);
    ways->addAction(make("act_type_horizontal", QStringLiteral("横書き"), [this] { type_direction(false); }, {},
                         QStringLiteral("選んだテキストレイヤー・台詞を横書きにします"), true));
    ways->addAction(make("act_type_vertical", QStringLiteral("縦書き"), [this] { type_direction(true); }, {},
                         QStringLiteral("選んだテキストレイヤー・台詞を縦書きにします"), true));
    make("act_type_rasterize", QStringLiteral("テキストレイヤーをラスタライズ"), [this] { rasterize_type(); }, {},
         QStringLiteral("選んだテキストレイヤー（または台詞）を、見えているとおりの画素のレイヤーにします（文字としては直せなくなります）"));
    make("act_type_to_point", QStringLiteral("ポイントテキストに変換"), [this] { type_command(Json{{"point", true}}); }, {},
         QStringLiteral("選んだテキストを、枠で折り返さないポイントテキストにします（文字の位置はそのまま）"));
    make("act_type_to_box", QStringLiteral("段落テキストに変換"), [this] { type_command(Json{{"box", true}}); }, {},
         QStringLiteral("選んだテキストを、枠の中で折り返す段落テキストにします（枠は今の文字を囲む大きさ）"));
    make("act_type_update_all", QStringLiteral("すべてのテキストレイヤーを更新"), [this] { update_all_type(); }, {},
         QStringLiteral("組んだ文字の覚えを捨てて、テキストレイヤーと台詞を組み直して描き直します（書体を入れ替えた後など）"));
    make("act_type_lorem", QStringLiteral("Lorem Ipsum をペースト"), [this] { paste_lorem(); }, {},
         QStringLiteral("仮の文を入れます（横書きは Lorem ipsum、縦書きは和文の仮の文）。打っている途中なら、その場所に入れます"));
    // PC5 G-b: 書式 → OpenType ▸ (the chosen text's OpenType features, the whole of it; one its font has not is grey)
    for (const auto& [tag, label] : opentype_names()) {
        const std::string feature = tag.toStdString();
        const QString id = QStringLiteral("act_type_ot_") + tag;
        QAction* act = make(id, label, [this, feature, id] { type_feature(feature, action(id)->isChecked()); }, {},
                            QStringLiteral("選んだテキストレイヤー・台詞の文字に OpenType 機能（%1）を使う・使わない（書体にある機能だけ選べます）").arg(tag), true);
        act->setChecked(false);
    }
    // 書式 → 言語オプション ▸: which features the menus show (Photoshop's 初期設定・東アジア言語・中東言語および南アジア言語の機能), the paragraph's
    // composer (中東および南アジア言語用 Composer: words broken at spaces, no kinsoku) and — 中東言語 — its direction
    auto* languages = new QActionGroup(this);
    languages->setExclusive(true);
    for (const auto& [key, label] : std::initializer_list<std::pair<const char*, QString>>{
             {"default", QStringLiteral("初期設定の機能")}, {"east_asian", QStringLiteral("東アジア言語の機能")}, {"middle_eastern", QStringLiteral("中東言語および南アジア言語の機能")}}) {
        const QString which = QString::fromLatin1(key);
        languages->addAction(make(QStringLiteral("act_type_lang_") + which, label, [this, which] { type_language_features(which); }, {},
                                  QStringLiteral("書式のメニューに出す文字の機能を選びます（%1）").arg(label), true));
    }
    make("act_type_composer_world", QStringLiteral("中東および南アジア言語用 Composer"),
         [this] { type_set(Json{{"composer", action("act_type_composer_world")->isChecked() ? "world" : "japanese"}}); }, {},
         QStringLiteral("選んだテキストの段落を、語の切れ目（空白）で折り返し、禁則・約物の詰めをしない組み方にします（右から左の文字・南アジアの文字向け）"), true);
    make("act_type_rtl", QStringLiteral("右から左の段落"), [this] { type_set(Json{{"direction", action("act_type_rtl")->isChecked() ? "rtl" : "auto"}}); }, {},
         QStringLiteral("選んだテキストの段落を右から左に組みます（オフ: 最初の文字の向きに従う）"), true);
    // PC5 G-c: 書式 → 初期設定の書式を読み込み・初期設定の書式を保存 (the formatting kept in the machine's settings, not the book)
    make("act_type_load_defaults", QStringLiteral("初期設定の書式を読み込み"), [this] { load_default_type_styles(); }, {},
         QStringLiteral("「初期設定の書式を保存」で覚えた書式（文字と段落の属性）を、選んだテキストレイヤー・台詞の文字全体に当てます（文字スタイル・段落スタイルは外れます）"));
    make("act_type_save_defaults", QStringLiteral("初期設定の書式を保存"), [this] { save_default_type_styles(); }, {},
         QStringLiteral("選んだテキストレイヤー・台詞の始めの文字の書式（文字と段落の属性）を、このパソコンの設定に覚えます（原稿には入りません）"));
    // (Genko's own: a line and a type layer each other's — 書式's last)
    make("act_line_to_type_layer", QStringLiteral("台詞をテキストレイヤーに"), [this] { line_to_type_layer(); }, {},
         QStringLiteral("選んだ台詞をテキストレイヤーにします（話者・読む順は外れ、レイヤーの並びの中＝コマ枠の下に描かれます）"));
    make("act_type_layer_to_line", QStringLiteral("テキストレイヤーを台詞に"), [this] { type_layer_to_line(); }, {},
         QStringLiteral("選んだテキストレイヤーを台詞にします（話者なし・読む順は最後。すべてのレイヤーとコマ枠の上に描かれます）"));
    // what the canvas's text tools make: type layers
    connect(text_settings_->make, &QComboBox::currentIndexChanged, this, [this] { canvas_->type_layers = text_settings_->line_fields().type_layer; });
    connect(canvas_, &PageCanvas::typePointRequested, this, [this](double x, double y) { type_new_layer(x, y, std::nullopt); });
    connect(canvas_, &PageCanvas::typeBoxRequested, this, [this](const QRectF& box) { type_new_layer(box.x(), box.y(), box); });
    connect(canvas_, &PageCanvas::typeLayerClicked, this, [this](const QString& id) {
        set_target_layer(id.toStdString());
        edit_type_layer_inline(id.toStdString());
    });
}

void MainWindow::build_type_menu(QMenu* menu) {
    type_menu_ = menu;
    menu->setObjectName(QStringLiteral("menu_type"));
    // (Photoshop's 書式: アンチエイリアス ▸, 方向 ▸; 作業用パスを作成, シェイプに変換, テキストレイヤーをラスタライズ, the conversions;
    // ワープテキスト…; すべてのテキストレイヤーを更新; Lorem Ipsum をペースト)
    QMenu* methods = menu->addMenu(QStringLiteral("アンチエイリアス"));
    methods->setObjectName(QStringLiteral("menu_type_antialias"));
    for (const auto& [key, label] : antialias_names()) methods->addAction(action(QStringLiteral("act_type_aa_") + key));
    QMenu* ways = menu->addMenu(QStringLiteral("方向"));
    ways->setObjectName(QStringLiteral("menu_type_direction"));
    for (const char* name : {"act_type_horizontal", "act_type_vertical"}) ways->addAction(action(name));
    QMenu* opentype = menu->addMenu(QStringLiteral("OpenType"));  // (PC5 G-b)
    opentype->setObjectName(QStringLiteral("menu_type_opentype"));
    for (const auto& [tag, label] : opentype_names()) {
        if (tag == QLatin1String("palt")) opentype->addSeparator();  // (東アジア言語の機能)
        opentype->addAction(action(QStringLiteral("act_type_ot_") + tag));
    }
    connect(menu, &QMenu::aboutToShow, this, [this] { refresh_type_commands(); });
    menu->addSeparator();
    for (const char* name : {"act_line_work_path", "act_line_to_shape", "act_type_rasterize", "act_type_to_point", "act_type_to_box"}) {
        if (QAction* act = action(name)) menu->addAction(act);
    }
    menu->addSeparator();
    if (QAction* act = action("act_warp_text")) menu->addAction(act);
    menu->addSeparator();
    QMenu* languages = menu->addMenu(QStringLiteral("言語オプション"));  // (PC5 G-b)
    languages->setObjectName(QStringLiteral("menu_type_language"));
    for (const char* name : {"act_type_lang_default", "act_type_lang_east_asian", "act_type_lang_middle_eastern"}) languages->addAction(action(name));
    languages->addSeparator();
    for (const char* name : {"act_type_composer_world", "act_type_rtl"}) languages->addAction(action(name));
    type_language_features(type_language_features());  // (the menus as the settings say)
    menu->addSeparator();
    menu->addAction(action("act_type_update_all"));
    menu->addSeparator();
    menu->addAction(action("act_type_lorem"));
    menu->addSeparator();
    for (const char* name : {"act_type_load_defaults", "act_type_save_defaults"}) menu->addAction(action(name));  // (PC5 G-c)
    menu->addSeparator();
    for (const char* name : {"act_line_to_type_layer", "act_type_layer_to_line"}) menu->addAction(action(name));
}

void MainWindow::line_to_type_layer() {
    const core::StoryLine* line = selected_line_or_say();
    if (line == nullptr) return;
    // (the line's own id when the page has no layer of it: the balloon's pen follows its id)
    std::string id = line->id;
    if (const core::Page* page = current_page(); page == nullptr || std::any_of(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == id; }))
        id = core::new_id();
    if (!apply_ops(Json::array({Json{{"op", "line_to_type_layer"}, {"line", line->id}, {"new_id", id}}}))) return;
    canvas_->selected_line_id.reset();
    set_target_layer(id);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
    flash(QStringLiteral("台詞をテキストレイヤーにしました。レイヤーの並びの中（コマ枠の下）に描かれます"), 5000);
}

void MainWindow::type_layer_to_line() {
    const core::Layer* layer = target_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr || layer->kind != core::LayerKind::Type) {
        flash(QStringLiteral("先にテキストレイヤーを選びます（レイヤー パネル）"), 4000);
        return;
    }
    // (the layer's id when no line has it: the balloon's pen follows its id)
    std::string id = layer->id;
    if (const auto& story = book().story; std::any_of(story.begin(), story.end(), [&](const core::StoryLine& l) { return l.id == id; })) id = core::new_id();
    if (!apply_ops(Json::array({Json{{"op", "type_layer_to_line"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"new_id", id}}}))) return;
    canvas_->selected_line_id = id;
    on_line_selected(id, false);
    if (layer_panel_ != nullptr) layer_panel_->refresh();
    flash(QStringLiteral("テキストレイヤーを台詞にしました。すべてのレイヤーとコマ枠の上に描かれます"), 5000);
}

// --- the text the commands act on ------------------------------------------------------------------------------------------

std::optional<Json> MainWindow::type_target(bool say) {
    if (const core::Layer* layer = target_layer(); layer != nullptr && layer->kind == core::LayerKind::Type && layer->type) {
        const core::Page* page = current_page();
        if (page != nullptr) return Json{{"page", page->index.json()}, {"layer_id", layer->id}};
    }
    if (canvas_->selected_line_id && line_by_id(*canvas_->selected_line_id) != nullptr) return Json{{"line", *canvas_->selected_line_id}};
    if (say) flash(QStringLiteral("先にテキストレイヤー（レイヤー パネル）か台詞を選びます"), 4000);
    return std::nullopt;
}

const core::StoryLine* MainWindow::type_record(const Json& target) const {
    if (target.contains("line")) return line_by_id(target["line"].get<std::string>());
    const core::Layer* layer = target_layer();
    return layer != nullptr && layer->type ? layer->type.get() : nullptr;
}

void MainWindow::type_command(const Json& change) {
    const std::optional<Json> target = type_target(true);
    if (!target) return;
    Json op{{"op", "edit_type"}};
    for (const auto& [key, value] : target->items()) op[key] = value;
    for (const auto& [key, value] : change.items()) op[key] = value;
    apply_ops(Json::array({op}));
    refresh_type_commands();
}

void MainWindow::type_direction(bool vertical) {
    const std::optional<Json> target = type_target(true);
    if (!target) return;
    const core::StoryLine* record = type_record(*target);
    if (record == nullptr || (record->wrap == "vertical") == vertical) return;
    if (target->contains("line") && render::text::shape_of(*record).empty()) {
        toggle_selected_wrap();  // (a line in its balloon: its box fitted again, as ページ → 台詞 does)
        refresh_type_commands();
        return;
    }
    type_command(Json{{"wrap", vertical ? "vertical" : "horizontal"}});
}

void MainWindow::refresh_type_commands() {
    // (the checks and what can be done follow the chosen text)
    const std::optional<Json> target = type_target(false);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    const std::string method = record != nullptr && record->native_type && record->native_type->contains("antialias")
                                   ? (*record->native_type)["antialias"].get<std::string>()
                                   : std::string("smooth");
    for (const auto& [key, label] : antialias_names()) {
        QAction* act = action(QStringLiteral("act_type_aa_") + key);
        act->setEnabled(record != nullptr);
        act->setChecked(record != nullptr && key.toStdString() == method);
    }
    for (const char* name : {"act_type_horizontal", "act_type_vertical", "act_type_rasterize", "act_type_lorem"}) action(name)->setEnabled(record != nullptr);
    if (record != nullptr) {
        action("act_type_vertical")->setChecked(record->wrap == "vertical");
        action("act_type_horizontal")->setChecked(record->wrap != "vertical");
    }
    const std::string shape = record != nullptr ? render::text::shape_of(*record) : std::string();
    const bool loose = record != nullptr && record->balloon == "none";  // (point and box text have no balloon)
    action("act_type_to_point")->setEnabled(loose && shape != "point");
    action("act_type_to_box")->setEnabled(loose && shape != "box");
    if (canvas_->editor() != nullptr) action("act_type_lorem")->setEnabled(true);
    // PC5 G-b: OpenType ▸ — what the whole text's first letters take, the features its font has; 言語オプション ▸
    Json chars = Json::object(), para = Json::object();
    std::vector<std::string> has;
    if (record != nullptr) {
        const Json* styles = book().type_styles ? &*book().type_styles : nullptr;
        try {
            chars = core::type_text::resolve_chars(*record, styles).front().attrs;
            para = core::type_text::resolve_para(*record, styles, 0);
        } catch (const std::exception&) {
        }
        static std::map<std::string, std::vector<std::string>> known;  // (a font's features, read once)
        const std::string font = render::text::line_face(*record, std::optional<std::string>(book().font_path)).first.other;
        auto it = known.find(font);
        if (it == known.end()) {
            std::vector<std::string> tags;
            try {
                tags = render::text::font_info(font).features;
            } catch (const std::exception&) {
            }
            it = known.emplace(font, std::move(tags)).first;
        }
        has = it->second;
    }
    const Json features = chars.value("features", Json::object());
    for (const auto& [tag, label] : opentype_names()) {
        QAction* act = action(QStringLiteral("act_type_ot_") + tag);
        const std::string t = tag.toStdString();
        const Json value = features.is_object() ? features.value(t, Json()) : Json();
        bool on = value.is_number_integer() && value.get<std::int64_t>() > 0;
        if (t == "liga") on = chars.value("ligatures", Json(true)) != Json(false) && value != Json(0);
        if (t == "calt") on = value != Json(0);
        if (t == "palt") on = chars.value("proportional", Json(false)) == Json(true);
        act->setEnabled(record != nullptr && std::binary_search(has.begin(), has.end(), t));
        act->setChecked(record != nullptr && act->isEnabled() && on);
    }
    action("act_type_composer_world")->setEnabled(record != nullptr);
    action("act_type_composer_world")->setChecked(record != nullptr && para.value("composer", Json()) == Json("world"));
    action("act_type_rtl")->setEnabled(record != nullptr && record->wrap != "vertical");  // (down: Japanese alone)
    action("act_type_rtl")->setChecked(record != nullptr && para.value("direction", Json()) == Json("rtl"));
    // PC5 G-c: 初期設定の書式 — kept from a text, laid on one
    action("act_type_save_defaults")->setEnabled(record != nullptr);
    action("act_type_load_defaults")->setEnabled(record != nullptr && preferences::default_type_styles().has_value());
}

// --- PC5 G-c: the styles (type_style) and the default formatting ----------------------------------------------------------

std::optional<Json> MainWindow::type_style(const std::string& kind, const std::string& what, const Json& args, bool on_text) {
    Json op{{"op", "type_style"}, {"kind", kind}, {"action", what}};
    if (on_text) {
        const std::optional<Json> target = type_target(true);
        if (!target) return std::nullopt;
        for (const auto& [key, value] : target->items()) op[key] = value;
    }
    for (const auto& [key, value] : args.items()) op[key] = value;
    if (!apply_ops(Json::array({op}))) return std::nullopt;
    refresh_type_commands();
    refresh_type_frame();
    for (const Json& result : last_results_) {
        if (result.is_object() && result.value("op", Json()) == Json("type_style")) {
            Json out = result;
            out.erase("index");
            out.erase("op");
            return out;
        }
    }
    return Json::object();
}

void MainWindow::save_default_type_styles() {
    const std::optional<Json> target = type_target(true);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    if (record == nullptr) return;
    const Json* styles = book().type_styles ? &*book().type_styles : nullptr;
    preferences::set_default_type_styles(core::type_styles::defaults_of(*record, styles));
    refresh_type_commands();
    flash(QStringLiteral("初期設定の書式を保存しました（このパソコンの設定。原稿には入りません）"), 4000);
}

void MainWindow::load_default_type_styles() {
    const std::optional<Json> target = type_target(true);
    if (!target) return;
    const std::optional<Json> kept = preferences::default_type_styles();
    if (!kept) {
        flash(QStringLiteral("先に「初期設定の書式を保存」で書式を覚えます"), 4000);
        return;
    }
    // (one step: the styles taken away and the text's own attributes cleared — 基本段落 and なし —, then the formatting laid on
    // the whole text, as Photoshop's is: the text looks as the one it was kept from)
    Json clear{{"op", "type_style"}, {"kind", "paragraph"}, {"action", "apply"}, {"clear_overrides", true}};
    Json none{{"op", "type_style"}, {"kind", "character"}, {"action", "apply"}};
    Json set{{"op", "set_type"}, {"char", (*kept)["char"]}, {"para", (*kept)["para"]}};
    for (Json* op : {&clear, &none, &set}) {
        for (const auto& [key, value] : target->items()) (*op)[key] = value;
    }
    apply_ops(Json::array({clear, none, set}));
    refresh_type_commands();
    refresh_type_frame();
}

void MainWindow::type_set(const Json& attrs) {
    const std::optional<Json> target = type_target(true);
    if (!target) {
        refresh_type_commands();
        return;
    }
    Json op{{"op", "set_type"}};
    for (const auto& [key, value] : target->items()) op[key] = value;
    for (const auto& [key, value] : attrs.items()) op[key] = value;
    apply_ops(Json::array({op}));
    refresh_type_commands();
}

void MainWindow::type_feature(const std::string& tag, bool on) {
    const std::optional<Json> target = type_target(true);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    if (record == nullptr) {
        refresh_type_commands();
        return;
    }
    if (tag == "liga") return type_set(Json{{"ligatures", on}});
    if (tag == "palt") return type_set(Json{{"proportional", on}});
    // (the whole text's features as they are, this one on or off: calt is on unless turned off, the others off unless on)
    Json features = Json::object();
    try {
        const Json* styles = book().type_styles ? &*book().type_styles : nullptr;
        features = core::type_text::resolve_chars(*record, styles).front().attrs.value("features", Json::object());
    } catch (const std::exception&) {
    }
    if (!features.is_object()) features = Json::object();
    if (tag == "calt") features[tag] = on ? 1 : 0;
    else if (on) features[tag] = 1;
    else features.erase(tag);
    std::map<std::string, Json> sorted;  // (the tags in order, as the record keeps them)
    for (const auto& [key, value] : features.items()) sorted[key] = value;
    Json ordered = Json::object();
    for (auto& [key, value] : sorted) ordered[key] = std::move(value);
    type_set(Json{{"features", ordered}});
}

QString MainWindow::type_language_features() const {
    const QString which = settings()->value(kLanguageFeatures, QStringLiteral("east_asian")).toString();
    return which == QLatin1String("default") || which == QLatin1String("middle_eastern") ? which : QStringLiteral("east_asian");
}

void MainWindow::type_language_features(const QString& which) {
    const QString chosen = which == QLatin1String("default") || which == QLatin1String("middle_eastern") ? which : QStringLiteral("east_asian");
    if (chosen != type_language_features()) settings()->setValue(kLanguageFeatures, chosen);
    for (const char* key : {"default", "east_asian", "middle_eastern"}) {
        if (QAction* act = action(QStringLiteral("act_type_lang_") + QString::fromLatin1(key))) act->setChecked(chosen == QLatin1String(key));
    }
    // (東アジア言語の機能: プロポーショナルメトリクス in OpenType ▸; 中東言語および南アジア言語の機能: the paragraph's direction)
    if (QAction* act = action("act_type_ot_palt")) act->setVisible(chosen == QLatin1String("east_asian"));
    if (QAction* act = action("act_type_rtl")) act->setVisible(chosen == QLatin1String("middle_eastern"));
}

// --- the canvas: type layers made and typed over --------------------------------------------------------------------------

void MainWindow::type_new_layer(double x_mm, double y_mm, std::optional<QRectF> box) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto origin = session_;
    const std::string page_id = page->id;
    const TextToolSettings::LineFields fields = text_settings_->line_fields();
    const bool vertical = canvas_->tool() == QLatin1String("text_h") ? false : fields.vertical;
    // (a click on the target path: the text follows it — 文字をパスに沿わせる, as a line's text_path)
    std::vector<QPointF> along;
    if (!box) {
        if (const auto path = canvas_->target_path_now()) {
            along = path_points(*path);
            const double reach = std::max(1.5, 8.0 / std::max(0.1, canvas_->scale()));  // (8 screen pixels, 1.5 mm at least)
            if (along.size() < 2 || distance_to(along, QPointF(x_mm, y_mm)) > reach) along.clear();
        }
    }
    canvas_->open_editor(x_mm, y_mm, QString(), [this, origin, page_id, x_mm, y_mm, box, fields, vertical, along](std::optional<QString> typed) {
        if (!typed || typed->isEmpty() || closed_) return;
        const core::Page* target = nullptr;
        for (const auto& p : book().pages) {
            if (p->id == page_id) target = p.get();
        }
        if (session_ != origin || target == nullptr) {
            flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
            return;
        }
        const lettering::Marks marks = lettering::parse_marks(*typed);
        Json op{{"op", "add_type_layer"}, {"page", target->index.json()}, {"text", marks.text}, {"wrap", vertical ? "vertical" : "horizontal"}};
        Json style = fields.style;
        if (!along.empty()) {  // (its box round the path; the path from the box's corner, as a line's)
            double x0 = along[0].x(), y0 = along[0].y(), x1 = x0, y1 = y0;
            for (const QPointF& p : along) {
                x0 = std::min(x0, p.x());
                y0 = std::min(y0, p.y());
                x1 = std::max(x1, p.x());
                y1 = std::max(y1, p.y());
            }
            Json points = Json::array();
            for (const QPointF& p : along) points.push_back(Json::array({core::py_round(p.x() - x0, 2), core::py_round(p.y() - y0, 2)}));
            style["text_path"] = points;
            op["box"] = Json::array({core::py_round(x0, 2), core::py_round(y0, 2), core::py_round(std::max(1.0, x1 - x0), 2), core::py_round(std::max(1.0, y1 - y0), 2)});
        } else if (box) {
            op["box"] = Json::array({box->x(), box->y(), box->width(), box->height()});
        } else {
            op["point"] = Json::array({x_mm, y_mm});
        }
        if (!style.empty()) op["style"] = style;
        if (fields.antialias != "smooth") op["antialias"] = fields.antialias;
        lettering::put_ruby(op, marks, nullptr, false);
        if (!marks.emphasis_runs.empty()) op["emphasis_runs"] = marks.emphasis_runs;
        if (!marks.style_runs.empty()) op["style_runs"] = marks.style_runs;
        const std::string id = core::new_id();
        op["new_id"] = id;
        if (const core::Layer* above = target_layer(); above != nullptr && current_page() == target) op["above"] = above->id;  // (Photoshop's: over the chosen layer)
        if (apply_ops(Json::array({op}))) {
            set_target_layer(id);
            if (layer_panel_ != nullptr) layer_panel_->refresh();
            refresh_type_frame();
        }
    });
}

void MainWindow::edit_type_layer_inline(const std::string& layer_id) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Layer* layer = nullptr;
    for (const core::Layer& l : page->layers) {
        if (l.id == layer_id) layer = &l;
    }
    if (layer == nullptr || !layer->type) return;
    if (layer->locked) {
        flash(QStringLiteral("このテキストレイヤーはロックされています"), 3000);
        return;
    }
    const core::StoryLine record = core::type_text::line_of(*layer, page->index);
    // (review P3-5: where its text shows — its corner through its matrix)
    QPointF at(record.x_mm.value(), record.y_mm.value());
    if (const auto t = core::type_text::transform_of(record)) {
        const auto& m = *t;
        at = QPointF(m[0] * at.x() + m[2] * at.y() + m[4], m[1] * at.x() + m[3] * at.y() + m[5]);
    }
    const auto origin = session_;
    const std::string page_id = page->id;
    canvas_->open_editor(at.x(), at.y(), lettering::with_marks(record), [this, origin, page_id, layer_id](std::optional<QString> typed) {
        if (!typed || closed_) return;
        const core::Page* target = nullptr;
        for (const auto& p : book().pages) {
            if (p->id == page_id) target = p.get();
        }
        if (session_ != origin || target == nullptr) {
            flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
            return;
        }
        const core::Layer* now = nullptr;
        for (const core::Layer& l : target->layers) {
            if (l.id == layer_id && l.type) now = &l;
        }
        if (now == nullptr) return;
        if (typed->isEmpty()) {  // (all its text taken away: the type layer goes, as Photoshop's — review P3-5)
            apply_ops(Json::array({Json{{"op", "delete_layer"}, {"page", target->index.json()}, {"id", layer_id}}}));
            if (layer_panel_ != nullptr) layer_panel_->refresh();
            refresh_type_frame();
            return;
        }
        const core::StoryLine current = core::type_text::line_of(*now, target->index);
        const lettering::Marks marks = lettering::parse_marks(*typed);
        if (lettering::same_marks(marks, current)) return;
        Json op{{"op", "edit_type"},
                {"page", target->index.json()},
                {"layer_id", layer_id},
                {"text", marks.text},
                {"ruby_runs", marks.ruby_runs},
                {"emphasis_runs", marks.emphasis_runs},
                {"style_runs", marks.style_runs}};
        lettering::put_ruby(op, marks, &current, true);  // (TEXT-01: by place when the runs would not say where)
        apply_ops(Json::array({op}));
        refresh_type_frame();
    });
}

void MainWindow::refresh_type_frame() {
    const core::Layer* layer = target_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr || layer->kind != core::LayerKind::Type || !layer->type) {
        if (canvas_->type_frame) {
            canvas_->type_frame.reset();
            canvas_->update();
        }
        return;
    }
    const core::StoryLine& record = *layer->type;
    PageCanvas::TypeFrame frame;
    frame.layer_id = layer->id;
    frame.box = QRectF(record.x_mm.value(), record.y_mm.value(), record.w_mm.value(), record.h_mm.value());
    frame.transform = core::type_text::transform_of(record);
    frame.box_text = render::text::shape_of(record) == "box";
    if (frame.box_text) {
        try {
            frame.overflow = render::text::placement(core::type_text::line_of(*layer, page->index), kFrameDpi, std::optional<std::string>(book().font_path)).overflow;
        } catch (const render::NotYetPorted&) {
            frame.overflow = false;
        }
    }
    canvas_->type_frame = frame;
    canvas_->update();
}

// --- 書式's other commands -------------------------------------------------------------------------------------------------

void MainWindow::update_all_type() {
    render::text::clear_layout_cache();  // (set again from the fonts as they are now)
    render::type_layer::forget_extents();  // (and where their letters are found again — review P3-6)
    canvas_->renderer().clear();
    show_page();
    refresh_type_frame();
    flash(QStringLiteral("テキストレイヤーと台詞を組み直しました"), 2500);
}

void MainWindow::paste_lorem() {
    if (InlineEditor* editor = canvas_->editor()) {  // (typing: at the caret, as Photoshop's)
        editor->insertPlainText(QString::fromUtf8(text_settings_->line_fields().vertical && canvas_->tool() != QLatin1String("text_h") ? kDummy : kLorem));
        return;
    }
    const std::optional<Json> target = type_target(true);
    if (!target) return;
    const core::StoryLine* record = type_record(*target);
    if (record == nullptr) return;
    const std::int64_t end = core::ruby::count(record->text);
    type_command(Json{{"insert", Json{{"at", end}, {"text", std::string(record->wrap == "vertical" ? kDummy : kLorem)}}}});
}

}  // namespace genko::app
