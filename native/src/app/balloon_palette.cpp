#include "app/balloon_palette.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/text/balloons.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kKey = Qt::UserRole + 21;
// The pictures: 40 × 34 mm of paper at 56 dpi (88 × 75 px), the balloon in its middle. At this size a 0.35 mm line is
// the renderer's one whole pixel: crisp on a plain screen, and drawn again at the screen's own scale on a dense one.
constexpr double kPaperW = 40.0;
constexpr double kPaperH = 34.0;
constexpr int kDpi = 56;

Json rgb(int r, int g, int b) { return Json::array({r, g, b}); }

Json black() { return Json{{"fill_rgb", rgb(0, 0, 0)}, {"line_rgb", rgb(0, 0, 0)}, {"rgb", rgb(255, 255, 255)}}; }

Json tail(double fx, double fy, const char* kind = nullptr) {
    Json out{{"to", Json::array({fx, fy})}};
    if (kind != nullptr) out["kind"] = kind;
    return out;
}

// fukidashi@2: a tone inside — `density`, growing to `to` (down the page, or from the middle outward), 60 lines at 45°
// (in the form the op keeps it in)
Json tone(double density, double to = -1.0, bool radial = false) {
    Json out{{"density", density}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}};
    if (to >= 0) {
        out["to"] = to;
        out["dir"] = 90.0;
        if (radial) out["radial"] = true;
    }
    return out;
}

bool is_white(const Json& colour) {
    if (!colour.is_array() || colour.size() < 3) return false;
    return std::all_of(colour.begin(), colour.begin() + 3, [](const Json& c) { return c.is_number() && core::to_float(c) >= 255; });
}

// A dark colour (a black fill, black letters): none of red, green and blue above a quarter.
bool is_dark(const Json& colour) {
    if (!colour.is_array() || colour.size() < 3) return false;
    return std::all_of(colour.begin(), colour.begin() + 3, [](const Json& c) { return c.is_number() && core::to_float(c) < 64; });
}

bool is_oval_kind(const std::string& kind) { return kind == "speech" || kind == "thought" || kind == "whisper"; }

Json held(const core::StoryLine& line, const char* key) { return core::get_or(line.style, key, Json()); }

}  // namespace

const std::vector<BalloonPreset>& balloon_presets() {
    // (the kinds and the keys as SPEC BALLOON-01 names them; the names as the app says them — フキダシ, しっぽ, 台詞)
    static const std::vector<BalloonPreset> all = {
        {"oval", "普通", "普通の台詞（楕円）", "speech", Json::object(), Json::array(), false, false},
        {"oval_tail", "しっぽつき", "しっぽのついた、普通の台詞。置いたあと ● をドラッグして話し手へ向けます", "speech", Json::object(),
         Json::array({tail(-0.5, 0.85)}), true, false},
        {"capsule", "縦長", "1 列の短い台詞に合う、縦長のカプセル", "capsule", Json::object(), Json::array(), false, false},
        {"capsule_wide", "横長", "横書きのひとことに合う、横長のカプセル", "capsule", Json::object(), Json::array(), false, true},
        {"rounded", "角丸", "角の丸い四角。説明や落ち着いた声に", "rounded", Json::object(), Json::array(), false, false},
        {"squircle", "ふっくら角丸", "辺がまっすぐに近く、角が大きく丸い形", "rounded", Json{{"squircle", true}}, Json::array(), false, false},
        {"box", "四角", "四角いフキダシ。説明やモノローグに", "box", Json::object(), Json::array(), false, false},
        {"narration", "ナレーション", "ナレーションの四角（しっぽなし）", "narration", Json::object(), Json::array(), false, false},
        {"cloud", "もくもく", "雲の形。やわらかい声や、ぼんやりした気持ちに", "cloud", Json::object(), Json::array(), false, false},
        {"thought_cloud", "心の声（雲）", "雲の形に泡のしっぽ。心の中の声に", "cloud", Json::object(), Json::array({tail(-0.55, 0.85, "bubbles")}), true,
         false},
        {"thought", "心の声（泡）", "楕円に泡が続く、心の中の声", "thought", Json::object(), Json::array(), false, false},
        {"shout", "叫び", "トゲのフキダシ。大声や驚きに", "shout", Json::object(), Json::array(), false, false},
        {"shout_alt", "叫び（長短）", "長いトゲと短いトゲが交互の叫び", "shout", Json{{"spike_alt", 0.5}}, Json::array(), false, false},
        {"shout_sharp", "叫び（鋭い）", "細かく鋭いトゲの叫び。悲鳴や怒鳴り声に", "shout", Json{{"spikes", 24}, {"spike_depth", 0.3}}, Json::array(), false,
         false},
        {"electric", "電子音", "ギザギザの輪郭。電話・テレビ・スピーカーの声に", "electric", Json::object(), Json::array(), false, false},
        {"flash", "フラッシュ", "放射状の線で囲む、ひらめきや衝撃のひとこと", "flash", Json::object(), Json::array(), false, false},
        {"whisper", "ささやき", "破線のフキダシ。小声やひそひそ話に", "whisper", Json::object(), Json::array(), false, false},
        {"long_dash", "長い破線", "長めの破線。弱い声や遠くの声に", "whisper", Json{{"dash_mm", 2.5}}, Json::array(), false, false},
        {"wave", "ふるえ声", "線が細かく波打つフキダシ。おびえた声・ふるえる声に", "speech", Json{{"wave_mm", 0.4}}, Json::array(), false, false},
        {"double", "二重線", "線が二重のフキダシ。強調したい台詞に", "speech", Json{{"double", true}}, Json::array(), false, false},
        {"thick", "太い線", "線の太いフキダシ。力のこもった声に", "speech", Json{{"border_mm", 0.8}}, Json::array(), false, false},
        {"no_line", "線なし", "線のない白いフキダシ。絵の上に文字だけを白く抜くときに", "speech", Json{{"border_mm", 0}}, Json::array(), false, false},
        {"polygon8", "八角形", "八角形のフキダシ。機械の声・アナウンスに", "polygon", Json::object(), Json::array(), false, false},
        {"polygon6", "六角形", "六角形のフキダシ。機械の声・通信の声に", "polygon", Json{{"sides", 6}}, Json::array(), false, false},
        {"black_oval", "ベタ白抜き", "黒いフキダシに白い文字。重い声や不気味な声に", "speech", black(), Json::array(), false, false},
        {"black_shout", "ベタの叫び", "黒いトゲのフキダシに白い文字。怒りや絶叫に", "shout", black(), Json::array(), false, false},
        {"black_flash", "ベタフラ", "黒いフキダシを放射状の線で囲む（ベタフラッシュ）", "flash", black(), Json::array(), false, false},
        {"dotted_box", "点線の囲み", "点線の四角。見出しやメモに", "dotted_box", Json::object(), Json::array(), false, false},
        {"tone_box", "トーンの囲み", "薄いトーンを敷いた四角。見出しや解説に", "tone_box", Json::object(), Json::array(), false, false},
        {"fancy_box", "飾り枠", "二重線と四隅の飾りの四角。題目やテーマに", "fancy_box", Json::object(), Json::array(), false, false},
        {"inward_tail", "内向きしっぽ", "フキダシに切れ込むしっぽ。コマの外にいる人の声に", "speech", Json::object(), Json::array({tail(0.27, 0.17, "inward")}),
         true, false},
        {"two_tails", "しっぽ 2 本", "しっぽが 2 本。2 人が同時に言う台詞に", "speech", Json::object(), Json::array({tail(-0.5, 0.85), tail(0.5, 0.85)}), true,
         false},
        // fukidashi@2 (SPEC BALLOON-02): the pen's line, the drips, the symbols, the tone inside
        {"pen_swell", "強弱の線", "線が片側で太く反対側で細い、手で描いたような線", "speech", Json{{"pen_swell", 0.5}}, Json::array(), false, false},
        {"g_pen", "G ペン風", "強弱と入り抜きのある、G ペンで描いたような線", "speech", Json{{"pen_swell", 0.5}, {"pen_taper", 0.6}, {"border_mm", 0.5}},
         Json::array(), false, false},
        {"brush", "筆風", "ひと筆で描いたような、太く抜けのある線", "speech", Json{{"pen_swell", 0.8}, {"pen_taper", 0.9}, {"pen_strokes", 1}, {"border_mm", 0.8}},
         Json::array(), false, false},
        {"g_pen_shout", "叫び（G ペン風）", "トゲの 1 本ずつが入り抜きのある線の叫び", "shout", Json{{"pen_swell", 0.5}, {"pen_taper", 0.8}, {"border_mm", 0.5}},
         Json::array(), false, false},
        {"drips", "したたる", "下からしずくが垂れるフキダシ。不気味な声・溶けるような声に", "speech", Json{{"drip_mm", 6}}, Json::array(), false, false},
        {"horror_black", "ホラー（ベタ）", "黒いフキダシから黒いしずくが垂れる。ホラーの声に", "speech",
         Json{{"drip_mm", 7}, {"drip_seed", 3}, {"fill_rgb", rgb(0, 0, 0)}, {"line_rgb", rgb(0, 0, 0)}, {"rgb", rgb(255, 255, 255)}}, Json::array(), false, false},
        {"melting_box", "溶ける四角", "下が溶けて垂れる四角。ナレーションや不穏な説明に", "box", Json{{"drip_mm", 6}, {"drip_seed", 1}}, Json::array(), false, false},
        {"horror_wave", "ホラー（ふるえ）", "ふるえる線から垂れるしずく。おびえた声・呪いの声に", "speech", Json{{"drip_mm", 6}, {"drip_seed", 2}, {"wave_mm", 0.35}},
         Json::array(), false, false},
        {"heart", "ハート", "ハートの形。甘い声・好きな気持ちに", "heart", Json::object(), Json::array(), false, false},
        {"star", "星", "星の形。はしゃぐ声・うれしい声に", "star", Json::object(), Json::array(), false, false},
        {"sparkle", "きらめき", "4 つの先がとがったきらめき。キラッとしたひとことに", "star", Json{{"points", 4}, {"spike_depth", 0.6}}, Json::array(), false,
         false},
        {"flower", "花", "花びらの形。やわらかい声・明るい気持ちに", "flower", Json::object(), Json::array(), false, false},
        {"diamond", "ひし形", "ひし形。冷たいひとこと・テレパシー・機械の声に", "diamond", Json::object(), Json::array(), false, false},
        {"tone", "トーン", "中に薄いトーンを敷いたフキダシ。回想や心の声に", "speech", Json{{"fill_tone", tone(0.1)}}, Json::array(), false, false},
        {"gradient_tone", "グラデトーン", "下へだんだん濃くなるトーン。暗い気持ち・沈んだ声に", "speech", Json{{"fill_tone", tone(0.05, 0.5)}}, Json::array(), false,
         false},
        {"tone_shout", "トーンの叫び", "中にトーンを敷いた叫び", "shout", Json{{"fill_tone", tone(0.2)}}, Json::array(), false, false},
        {"gradient_dark", "グラデ（濃い）", "外へ向かって濃くなるトーン。重い気持ち・こもった声に", "speech", Json{{"fill_tone", tone(0.1, 0.6, true)}}, Json::array(),
         false, false},
    };
    return all;
}

const BalloonPreset* balloon_preset(const QString& key) {
    for (const BalloonPreset& preset : balloon_presets()) {
        if (key == QLatin1String(preset.key)) return &preset;
    }
    return nullptr;
}

const std::vector<std::string>& balloon_look_keys() {
    // (wobble: a line of before's, drawn as a trembling outline — part of the look it had)
    // (fukidashi@2: the pen's line, the drips, a symbol's points, the tone and the blend inside are part of the look too)
    static const std::vector<std::string> keys = {"border_mm", "fill",        "fill_rgb",     "line_rgb",  "fill_opacity", "double",
                                                  "spikes",    "spike_depth", "spike_jitter", "spike_alt", "bumps",        "corner_mm",
                                                  "squircle",  "sides",       "wave_mm",      "dash_mm",   "wobble",
                                                  "pen_swell", "pen_angle",   "pen_taper",    "pen_strokes", "drip_mm",    "drips",
                                                  "drip_width_mm", "drip_seed", "points",     "fill_tone", "fill_gradient"};
    return keys;
}

Json preset_tails(const BalloonPreset& preset, double x_mm, double y_mm, double w_mm, double h_mm) {
    Json out = Json::array();
    const double cx = x_mm + w_mm / 2;
    const double cy = y_mm + h_mm / 2;
    for (const Json& t : preset.tails) {
        Json placed{{"to", Json::array({core::py_round(cx + t["to"][0].get<double>() * w_mm, 2), core::py_round(cy + t["to"][1].get<double>() * h_mm, 2)})}};
        if (t.contains("kind")) placed["kind"] = t["kind"];
        out.push_back(std::move(placed));
    }
    return out;
}

Json preset_style_change(const BalloonPreset& preset, const core::StoryLine& line) {
    Json style = Json::object();
    for (const std::string& key : balloon_look_keys()) {
        if (line.style.contains(key)) style[key] = nullptr;
    }
    const Json letters = held(line, "rgb");
    for (const auto& [key, value] : preset.style.items()) {
        // (ベタ: white letters for a line whose letters are the usual black — a colour the person chose is kept)
        if (key == "rgb" && !(letters.is_null() || is_dark(letters))) continue;
        style[key] = value;
    }
    // (white letters that went with a black inside go with it: a line turned from ベタ to a white balloon gets its
    // letters back. White letters on a white or absent inside — 白抜き文字 with their own edge — are the person's)
    if (!preset.style.contains("rgb") && is_white(letters) && is_dark(held(line, "fill_rgb"))) style["rgb"] = nullptr;
    // (a line of before whose wobble goes: its letters were fitted in its box's own ellipse — hand false keeps them
    // there, whatever hand said)
    if (core::py_truthy(held(line, "wobble")) && is_oval_kind(preset.kind) && held(line, "hand") != Json(false)) style["hand"] = false;
    // only what changes
    Json change = Json::object();
    for (const auto& [key, value] : style.items()) {
        const auto now = line.style.find(key);
        if (value.is_null() ? now == line.style.end() : (now != line.style.end() && *now == value)) continue;
        change[key] = value;
    }
    return change;
}

Json preset_restyle_ops(const BalloonPreset& preset, const core::StoryLine& line, const Json& box, const std::vector<JoinedLine>& joined) {
    Json ops = Json::array();
    const auto restyled = [&preset](const core::StoryLine& l) {
        Json op{{"op", "edit_line"}, {"id", l.id}};
        if ((l.balloon.empty() ? std::string("speech") : l.balloon) != preset.kind) op["balloon"] = preset.kind;
        Json style = preset_style_change(preset, l);
        if (!style.empty()) op["style"] = std::move(style);
        return op;
    };
    const auto resized = [](const core::StoryLine& l, const Json& to) {
        Json op{{"op", "move_line"}, {"id", l.id}};
        for (const auto& [key, value] : to.items()) op[key] = value;
        return op;
    };
    Json edit = restyled(line);
    Json tails;  // (null: its tails as they are)
    if (preset.about_tails) {
        const Json now = PageCanvas::tails_of(line);
        const Json& first = preset.tails.front();
        const std::string kind = first.contains("kind") ? first["kind"].get<std::string>() : std::string("wedge");
        const auto inward = [](const Json& t) { return t.is_object() && core::get_or(t, "kind", Json()) == Json("inward"); };
        // (tails that point the other way — any notch under a preset of tails that run out, any tail that runs out
        // under the preset of a notch —, none, or not as many as しっぽ 2 本 says: the preset's own)
        const bool other_way = kind == "inward" ? !std::all_of(now.begin(), now.end(), inward) : std::any_of(now.begin(), now.end(), inward);
        const bool counted = preset.tails.size() > 1 && now.size() != preset.tails.size();
        if (now.empty() || counted || other_way) {
            const bool sized = box.is_object();
            const double x = sized ? core::to_float(box["x_mm"]) : line.x_mm.value();
            const double y = sized ? core::to_float(box["y_mm"]) : line.y_mm.value();
            const double w = sized ? core::to_float(box["w_mm"]) : (line.w_mm.truthy() ? line.w_mm.value() : 40.0);
            const double h = sized ? core::to_float(box["h_mm"]) : (line.h_mm.truthy() ? line.h_mm.value() : 20.0);
            tails = preset_tails(preset, x, y, w, h);
        } else {
            Json kept = Json::array();  // (every tail kept where it points, each of the preset's kind)
            for (std::size_t i = 0; i < now.size(); ++i) {
                Json t = now[i];
                const Json& from = preset.tails[std::min(i, preset.tails.size() - 1)];
                if (from.contains("kind")) {
                    t["kind"] = from["kind"];
                } else {
                    t.erase("kind");
                }
                kept.push_back(std::move(t));
            }
            if (kept != now) tails = std::move(kept);
        }
    }
    // the line: its kind and look, then — refitted — its box, the tails the preset gives said with the box they are for
    // (a move_line that says its tails leaves them as said)
    if (box.is_object()) {
        if (edit.size() > 2) ops.push_back(std::move(edit));
        Json move = resized(line, box);
        if (!tails.is_null()) move["tails"] = std::move(tails);
        ops.push_back(std::move(move));
    } else {
        if (!tails.is_null()) edit["tails"] = std::move(tails);
        if (edit.size() > 2) ops.push_back(std::move(edit));
    }
    // the lines joined with it (style.group): one balloon — its inside, its line, its dashes come from its first line —,
    // so each takes the kind and the look too; their words, places, faces and tails are their own
    for (const JoinedLine& other : joined) {
        Json more = restyled(*other.line);
        if (more.size() > 2) ops.push_back(std::move(more));
        if (other.box.is_object()) ops.push_back(resized(*other.line, other.box));
    }
    return ops;
}

QSize preset_picture_size() {
    return QSize(static_cast<int>(std::lround(kPaperW / 25.4 * kDpi)), static_cast<int>(std::lround(kPaperH / 25.4 * kDpi)));
}

QImage preset_picture(const BalloonPreset& preset, double scale) {
    // (kept by the preset and the scale: drawn once a process)
    static std::map<std::pair<std::string, int>, QImage> kept;
    const int dpi = std::max(kDpi, static_cast<int>(std::lround(kDpi * scale)));
    const auto found = kept.find({preset.key, dpi});
    if (found != kept.end()) return found->second;
    const QSize logical = preset_picture_size();
    const double factor = static_cast<double>(dpi) / kDpi;
    const render::Size size{static_cast<int>(std::lround(logical.width() * factor)), static_cast<int>(std::lround(logical.height() * factor))};
    // the balloon: in the paper's middle, or a little up and to the right when its tail runs out below it
    const std::string kind = preset.kind;
    const bool tailed = !preset.tails.empty() && !(preset.tails.front().contains("kind") && preset.tails.front()["kind"] == "inward");
    double w = 29, h = 22;
    if (kind == "capsule") {
        w = preset.across ? 31 : 12.5;
        h = preset.across ? 12.5 : 27;
    } else if (kind == "shout" || kind == "electric" || kind == "cloud") {
        w = 31;
        h = 25;
    } else if (kind == "flash") {
        w = 25;
        h = 20;
    } else if (kind == "narration") {  // (a narration's box: tall, as it sits in a panel's corner)
        w = 15;
        h = 26;
    } else if (kind == "heart" || kind == "star" || kind == "flower") {  // (fukidashi@2: the symbols, about as wide as tall)
        w = 27;
        h = 26;
    } else if (kind == "diamond") {
        w = 31;
        h = 26;
    }
    if (tailed) h = std::min(h, 20.0);
    // (drips: the body as it would be without them, the drips below it — up a little, so that they are seen whole)
    const double drips = preset.style.contains("drip_mm") && preset.style["drip_mm"].is_number() ? core::to_float(preset.style["drip_mm"]) : 0.0;
    if (drips > 0) h = std::min(h, kPaperH - 4 - drips) + drips;
    core::StoryLine line;
    line.id = std::string("preset_") + preset.key;
    line.text = preset.across ? "ひとこと" : kind == "narration" ? "その頃" : "セリフ";
    line.balloon = kind;
    line.wrap = preset.across ? "horizontal" : "vertical";
    line.x_mm = core::Num((kPaperW - w) / 2 + (tailed ? 2.0 : 0.0));
    line.y_mm = core::Num(tailed ? 3.5 : drips > 0 ? 2.0 : (kPaperH - h) / 2 - (kind == "thought" ? 2.0 : 0.0));
    line.w_mm = core::Num(w);
    line.h_mm = core::Num(h);
    line.style = preset.style;
    line.style["size_mm"] = 4.0;  // (the sample's letters: one column in every shape)
    for (const Json& t : preset_tails(preset, line.x_mm.value(), line.y_mm.value(), w, h)) line.tails.push_back(t);
    // (white paper; a balloon without a line is shown on grey, where its white can be seen)
    const bool lineless = preset.style.contains("border_mm") && preset.style["border_mm"].is_number() && core::to_float(preset.style["border_mm"]) == 0;
    const int ground = lineless ? 176 : 255;
    render::Image paper = render::Image::create("RGB", size, render::Ink{ground, ground, ground});
    try {
        render::text::draw_group(render::text::whole_page(paper), {&line}, dpi, false);
    } catch (const std::exception& error) {  // (a picture that cannot be drawn: the white paper, the name under it says what it is)
        qWarning("BalloonPalette: %s: %s", preset.key, error.what());
    }
    const std::string bytes = paper.tobytes();
    QImage picture = QImage(reinterpret_cast<const uchar*>(bytes.data()), paper.width(), paper.height(), paper.width() * 3, QImage::Format_RGB888).copy();
    picture.setDevicePixelRatio(factor);
    kept[{preset.key, dpi}] = picture;
    return picture;
}

BalloonPalette::BalloonPalette(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("balloon_palette"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* note = new QLabel(QStringLiteral("形をクリックすると、選んだ台詞のフキダシがその形になります。"
                                           "台詞を選んでいないときや、文字ツールを使っているときは、次に置く台詞の形になります。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    theme::hint(note);  // (hidden when hints are off: its words are then this panel's tooltip)
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("balloon_presets"));
    list_->setAccessibleName(QStringLiteral("フキダシの形"));
    list_->setViewMode(QListView::IconMode);
    const QSize picture = preset_picture_size();
    list_->setIconSize(picture);
    list_->setGridSize(QSize(picture.width() + 12, picture.height() + 30));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setWrapping(true);
    list_->setWordWrap(true);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::DefaultContextMenu);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // (the pictures wrap to the panel's width)
    // (until its picture is drawn, each shows the white paper: nothing jumps when the pictures come)
    QPixmap blank(picture);
    blank.fill(Qt::white);
    const QIcon waiting(blank);
    for (const BalloonPreset& preset : balloon_presets()) {
        auto* item = new QListWidgetItem(waiting, QString::fromUtf8(preset.name));
        item->setData(kKey, QString::fromLatin1(preset.key));
        item->setToolTip(QStringLiteral("%1\n%2\nクリックで、選んだ台詞のフキダシがこの形になります（選んでいなければ、次に置く台詞の形に）")
                             .arg(QString::fromUtf8(preset.name), QString::fromUtf8(preset.tip)));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        list_->addItem(item);
    }
    count_ = list_->count();
    layout->addWidget(list_, 1);
    setFocusProxy(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item != nullptr) emit picked(item->data(kKey).toString(), QString());
    });
    list_->installEventFilter(this);
    timer_ = new QTimer(this);
    timer_->setInterval(0);
    connect(timer_, &QTimer::timeout, this, [this] { draw_some(4); });
}

QListWidgetItem* BalloonPalette::item(const QString& key) const {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(kKey).toString() == key) return list_->item(i);
    }
    return nullptr;
}

std::unique_ptr<QMenu> BalloonPalette::item_menu(const QString& key) {
    if (item(key) == nullptr) return nullptr;
    auto menu = std::make_unique<QMenu>(this);
    menu->addAction(QStringLiteral("選んだ台詞のフキダシをこの形にする"), this, [this, key] { emit picked(key, QStringLiteral("line")); });
    menu->addAction(QStringLiteral("次に置く台詞をこの形にする"), this, [this, key] { emit picked(key, QStringLiteral("next")); });
    return menu;
}

void BalloonPalette::mark(const QString& key) {
    // the look the next line takes: its item chosen (the list's highlight), in heavier letters, and the one in hand. A
    // pick that only restyled a line leaves the highlight on the marked look, or on none (the item in hand stays where
    // it was, for the arrow keys).
    QListWidgetItem* wanted = item(key);
    const bool fresh = wanted != nullptr && marked() != key;  // (newly marked: it becomes the item in hand)
    const QSignalBlocker quiet(list_);
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        QFont font = it->font();
        font.setBold(it == wanted);
        it->setFont(font);
        it->setBackground(it == wanted ? QBrush(QColor(theme::tokens().accent_soft)) : QBrush());
    }
    list_->clearSelection();
    if (wanted != nullptr) wanted->setSelected(true);  // (the look's own highlight: the list's chosen item)
    if (fresh) {
        list_->setCurrentItem(wanted, QItemSelectionModel::NoUpdate);
        list_->scrollToItem(wanted);
    }
    list_->setProperty("marked", wanted != nullptr ? key : QString());
}

QString BalloonPalette::marked() const { return list_->property("marked").toString(); }

void BalloonPalette::draw_some(int most) {
    const double scale = devicePixelRatioF();
    for (int n = 0; n < most && next_ < count_; ++n, ++next_) {
        QListWidgetItem* it = list_->item(next_);
        if (const BalloonPreset* preset = balloon_preset(it->data(kKey).toString())) it->setIcon(QIcon(QPixmap::fromImage(preset_picture(*preset, scale))));
    }
    if (next_ >= count_) timer_->stop();
}

void BalloonPalette::draw_pictures() { draw_some(count_); }

bool BalloonPalette::event(QEvent* event) {
    // (moved to a screen of another scale: the pictures drawn again at it, a few at a time as when first shown)
    if (event->type() == QEvent::DevicePixelRatioChange && list_ != nullptr && next_ > 0) {
        next_ = 0;
        if (isVisible() && !timer_->isActive()) timer_->start();
    }
    return QWidget::event(event);
}

void BalloonPalette::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // (the look changed — light, dark, the brightness: the marked item's colour with it)
    if ((event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) && list_ != nullptr && !marked().isEmpty()) mark(marked());
}

void BalloonPalette::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (next_ < count_ && !timer_->isActive()) timer_->start();  // (the first time it is seen: a few between events)
}

bool BalloonPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
            if (const QListWidgetItem* current = list_->currentItem()) emit picked(current->data(kKey).toString(), QString());
            return true;
        }
    }
    if (watched == list_ && event->type() == QEvent::ContextMenu) {
        auto* asked = static_cast<QContextMenuEvent*>(event);
        // (the menu key: at the current item; the mouse: at the item under it)
        QListWidgetItem* at = asked->reason() == QContextMenuEvent::Mouse ? list_->itemAt(list_->viewport()->mapFrom(list_, asked->pos())) : list_->currentItem();
        if (at == nullptr) return true;
        if (const std::unique_ptr<QMenu> menu = item_menu(at->data(kKey).toString())) menu->exec(asked->globalPos());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace genko::app
