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

bool is_white(const Json& colour) {
    if (!colour.is_array() || colour.size() < 3) return false;
    return std::all_of(colour.begin(), colour.begin() + 3, [](const Json& c) { return c.is_number() && core::to_float(c) >= 255; });
}

}  // namespace

const std::vector<BalloonPreset>& balloon_presets() {
    // (the kinds and the keys as SPEC BALLOON-01 names them; the names as the app says them — フキダシ, しっぽ, 台詞)
    static const std::vector<BalloonPreset> all = {
        {"oval", "ふつう", "ふつうの台詞（楕円）", "speech", Json::object(), Json::array(), false, false},
        {"oval_tail", "しっぽつき", "しっぽのついた、ふつうの台詞。置いたあと ● をドラッグして話し手へ向けます", "speech", Json::object(),
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
        {"flash", "フラッシュ", "放射線で囲む、ひらめきや衝撃のひとこと", "flash", Json::object(), Json::array(), false, false},
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
        {"black_flash", "ベタフラ", "黒いフキダシを放射線で囲む（ベタフラッシュ）", "flash", black(), Json::array(), false, false},
        {"dotted_box", "点線の囲み", "点線の四角。見出しやメモに", "dotted_box", Json::object(), Json::array(), false, false},
        {"tone_box", "トーンの囲み", "薄いトーンを敷いた四角。見出しや解説に", "tone_box", Json::object(), Json::array(), false, false},
        {"fancy_box", "飾り枠", "二重線と四隅の飾りの四角。題目やテーマに", "fancy_box", Json::object(), Json::array(), false, false},
        {"inward_tail", "内向きしっぽ", "フキダシに切れ込むしっぽ。コマの外にいる人の声に", "speech", Json::object(), Json::array({tail(0.27, 0.17, "inward")}),
         true, false},
        {"two_tails", "しっぽ 2 本", "しっぽが 2 本。2 人が同時に言う台詞に", "speech", Json::object(), Json::array({tail(-0.5, 0.85), tail(0.5, 0.85)}), true,
         false},
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
    static const std::vector<std::string> keys = {"border_mm", "fill",        "fill_rgb",     "line_rgb",  "fill_opacity", "double",
                                                  "spikes",    "spike_depth", "spike_jitter", "spike_alt", "bumps",        "corner_mm",
                                                  "squircle",  "sides",       "wave_mm",      "dash_mm",   "wobble"};
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

Json preset_restyle_op(const BalloonPreset& preset, const core::StoryLine& line) {
    Json style = Json::object();
    for (const std::string& key : balloon_look_keys()) {
        if (line.style.contains(key)) style[key] = nullptr;
    }
    // (white letters go with the black they were on: a line turned from ベタ to a white balloon gets its letters back)
    if (line.style.contains("rgb") && is_white(line.style["rgb"]) && !preset.style.contains("rgb")) style["rgb"] = nullptr;
    for (const auto& [key, value] : preset.style.items()) style[key] = value;
    Json op{{"op", "edit_line"}, {"id", line.id}, {"balloon", preset.kind}};
    if (!style.empty()) op["style"] = std::move(style);
    if (preset.about_tails) {
        const Json now = PageCanvas::tails_of(line);
        const Json& first = preset.tails.front();
        const std::string kind = first.contains("kind") ? first["kind"].get<std::string>() : std::string("wedge");
        const bool inward_now = !now.empty() && std::all_of(now.begin(), now.end(), [](const Json& t) { return t.is_object() && t.value("kind", "") == "inward"; });
        const double w = line.w_mm.truthy() ? line.w_mm.value() : 40.0;
        const double h = line.h_mm.truthy() ? line.h_mm.value() : 20.0;
        if (now.empty() || now.size() != preset.tails.size() || (kind == "inward") != inward_now) {
            // (none, another number of them, or tails that point the other way — in or out: the preset's own)
            op["tails"] = preset_tails(preset, line.x_mm.value(), line.y_mm.value(), w, h);
        } else {
            Json tails = Json::array();  // (where they point kept, each of the preset's kind)
            for (std::size_t i = 0; i < now.size(); ++i) {
                Json t = now[i];
                const Json& from = preset.tails[i];
                if (from.contains("kind")) {
                    t["kind"] = from["kind"];
                } else {
                    t.erase("kind");
                }
                tails.push_back(std::move(t));
            }
            if (tails != now) op["tails"] = std::move(tails);
        }
    }
    return op;
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
    }
    if (tailed) h = std::min(h, 20.0);
    core::StoryLine line;
    line.id = std::string("preset_") + preset.key;
    line.text = preset.across ? "ひとこと" : kind == "narration" ? "その頃" : "セリフ";
    line.balloon = kind;
    line.wrap = preset.across ? "horizontal" : "vertical";
    line.x_mm = core::Num((kPaperW - w) / 2 + (tailed ? 2.0 : 0.0));
    line.y_mm = core::Num(tailed ? 3.5 : (kPaperH - h) / 2 - (kind == "thought" ? 2.0 : 0.0));
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
                                           "台詞を選んでいないとき（と文字ツールを持っているとき）は、次に置く台詞の形になります。"));
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

void BalloonPalette::mark(const QString& key) {
    QListWidgetItem* wanted = item(key);
    const QSignalBlocker quiet(list_);
    if (wanted == nullptr) {
        list_->clearSelection();
        list_->setCurrentItem(nullptr);
    } else {
        list_->setCurrentItem(wanted);
        wanted->setSelected(true);
    }
    list_->setProperty("marked", key);
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
        const QString key = at->data(kKey).toString();
        QMenu menu(this);
        menu.addAction(QStringLiteral("選んだ台詞のフキダシをこの形にする"), this, [this, key] { emit picked(key, QStringLiteral("line")); });
        menu.addAction(QStringLiteral("次に置く台詞をこの形にする"), this, [this, key] { emit picked(key, QStringLiteral("next")); });
        menu.exec(asked->globalPos());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace genko::app
