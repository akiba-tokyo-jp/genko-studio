#include "app/story_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRawFont>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/icons.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/text_style.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/text/lettering.hpp"

namespace genko::app {

using core::Json;

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

// A slot's work, as PySide runs a Python slot: an error (a style written by hand, say) is logged and the panel carries
// on. (An exception must never leave a Qt slot: it would end the program.)
template <class Work>
void carry_on(const char* slot, Work&& work) {
    try {
        work();
    } catch (const std::exception& error) {
        qWarning("StoryPanel.%s: %s", slot, error.what());
    }
}

// QColor(*rgb), the colour Python's panel opens its colour dialog with: one number is a QRgb (0xRRGGBB, solid), three or
// four are red, green, blue (and alpha). What Python cannot make a colour of — two numbers, five, words, a number past
// a C int (its TypeError or OverflowError, logged by PySide: no dialog) — starts from the default colour here.
QColor start_colour(const Json& rgb, const QColor& fallback) {
    if (!core::py_truthy(rgb) || !rgb.is_array() || rgb.size() == 2 || rgb.size() > 4) return fallback;
    std::vector<std::int64_t> v;
    for (const Json& x : rgb) {
        if (x.is_boolean()) {
            v.push_back(x.get<bool>() ? 1 : 0);
        } else if (x.is_number_integer() && !(x.is_number_unsigned() && x.get<std::uint64_t>() > 0xFFFFFFFFULL)) {
            v.push_back(x.get<std::int64_t>());
        } else {
            return fallback;
        }
    }
    if (v.size() == 1) return v[0] >= 0 && v[0] <= 0xFFFFFFFFLL ? QColor::fromRgb(static_cast<QRgb>(v[0])) : fallback;
    for (const std::int64_t c : v) {
        if (c < std::numeric_limits<int>::min() || c > std::numeric_limits<int>::max()) return fallback;
    }
    return QColor(static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2]), v.size() == 4 ? static_cast<int>(v[3]) : 255);
}

// theme.empty_note: quiet words in an empty list, saying what will appear there and how (they go with the first row)
class EmptyNote : public QObject {
public:
    EmptyNote(QListWidget* list, QLabel* note) : QObject(list), list_(list), note_(note) {
        list->viewport()->installEventFilter(this);
        follow();
    }
    void follow() {
        if (note_.isNull()) return;
        note_->setGeometry(list_->viewport()->rect().adjusted(8, 8, -8, -8));
        note_->setVisible(list_->count() == 0);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) follow();
        return QObject::eventFilter(watched, event);
    }

private:
    QListWidget* list_;
    QPointer<QLabel> note_;
};

QPushButton* iconic(const QString& text, const char* icon, const QString& tip) {
    // (flat pictures beside the list)
    auto* button = new QPushButton(text);
    button->setIcon(icons::icon(icon));
    button->setText(QString());
    button->setToolTip(tip);
    button->setFixedSize(28, 26);
    button->setProperty("iconbtn", true);
    return button;
}

// fonts.system_fonts(): [{name, style, path}] of the computer's fonts that can draw Japanese, kept in the config
// folder (fonts.json) once looked for.
Json system_fonts() {
    const auto cache = config_dir() / "fonts.json";
    try {
        std::ifstream in(cache, std::ios::binary);
        if (in) {
            const Json found = Json::parse(in);
            if (found.is_array()) return found;
        }
    } catch (const std::exception&) {  // (not read: looked for again)
    }
    QStringList dirs;
#if defined(Q_OS_WIN)
    dirs << qEnvironmentVariable("WINDIR", QStringLiteral("C:\\Windows")) + QStringLiteral("/Fonts")
         << qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/Microsoft/Windows/Fonts");
#elif defined(Q_OS_MACOS)
    dirs << QStringLiteral("/System/Library/Fonts") << QStringLiteral("/Library/Fonts") << QDir::homePath() + QStringLiteral("/Library/Fonts");
#else
    dirs << QStringLiteral("/usr/share/fonts") << QStringLiteral("/usr/local/share/fonts") << QDir::homePath() + QStringLiteral("/.fonts")
         << QDir::homePath() + QStringLiteral("/.local/share/fonts");
#endif
    Json found = Json::array();
    for (const QString& folder : dirs) {
        if (!QFileInfo(folder).isDir()) continue;
        QStringList paths;
        QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) paths << it.next();
        std::sort(paths.begin(), paths.end());
        for (const QString& path : paths) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix != QLatin1String("ttf") && suffix != QLatin1String("otf") && suffix != QLatin1String("ttc")) continue;
            const QRawFont font(path, 24);
            if (!font.isValid() || !font.supportsCharacter(QChar(0x3042)) || !font.supportsCharacter(QChar(0x6F22))) continue;
            found.push_back(Json{{"name", font.familyName().toStdString()}, {"style", font.styleName().toStdString()}, {"path", path.toStdString()}});
        }
    }
    try {
        std::filesystem::create_directories(cache.parent_path());
        std::ofstream(cache, std::ios::binary) << found.dump(1);
    } catch (const std::exception&) {
    }
    return found;
}

}  // namespace

StoryPanel::StoryPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("story_panel"));
    list = new QListWidget;
    list->setObjectName(QStringLiteral("storyList"));
    connect(list, &QListWidget::currentRowChanged, this, [this](int) { picked(); });  // (picked carries on by itself)
    empty_note = new QLabel(QStringLiteral("このページにはまだ台詞がありません。\nテキストの道具（T）で、置きたい所をクリックします"), list->viewport());
    empty_note->setWordWrap(true);
    empty_note->setAlignment(Qt::AlignCenter);
    theme::role(empty_note, "empty");
    empty_note->setAttribute(Qt::WA_TransparentForMouseEvents);
    new EmptyNote(list, empty_note);  // (its place follows the list's size; refresh says whether it shows)
    up_button = iconic(QStringLiteral("↑ 前へ"), "up", QStringLiteral("選んだ台詞を読み順で前へ"));
    up_button->setObjectName(QStringLiteral("story_up"));
    connect(up_button, &QPushButton::clicked, this, [this] { carry_on("move", [this] { move(-1); }); });
    down_button = iconic(QStringLiteral("↓ 後へ"), "down", QStringLiteral("選んだ台詞を読み順で後ろへ"));
    down_button->setObjectName(QStringLiteral("story_down"));
    connect(down_button, &QPushButton::clicked, this, [this] { carry_on("move", [this] { move(1); }); });
    speaker = new QLineEdit;
    speaker->setObjectName(QStringLiteral("story_speaker"));
    speaker->setPlaceholderText(QStringLiteral("話者（空でもよい）"));
    text = new QPlainTextEdit;
    text->setObjectName(QStringLiteral("story_text"));
    text->setPlaceholderText(QStringLiteral("台詞を打つ（改行で次の列へ）"));
    text->setToolTip(QStringLiteral("ルビは ｜約束《やくそく》、傍点は 《《強調》》、一部を大きく {大|…}・太く {太|…}・赤く {赤|…}、"
                                    "好きな色 {#3060c0|…}・大きさ {×1.3|…}・縦中横 {縦中横|12}（重ねるときは {大、赤|…}）"));
    text_style::install(text);  // (or choose the characters and right-click: 選んだ文字を)
    text->setMaximumHeight(80);
    kind = new QComboBox;
    kind->setObjectName(QStringLiteral("story_kind"));
    for (const auto& [key, label] : lettering::kinds()) kind->addItem(label, key);
    vertical = new QCheckBox(QStringLiteral("縦書き"));
    vertical->setObjectName(QStringLiteral("story_vertical"));
    vertical->setChecked(true);
    add_button = new QPushButton(QStringLiteral("コマに追加"));
    add_button->setObjectName(QStringLiteral("story_add"));
    add_button->setIcon(icons::icon("add"));
    add_button->setToolTip(QStringLiteral("上の欄の台詞を、選んだコマに加えます"));
    connect(add_button, &QPushButton::clicked, this, [this] { carry_on("add", [this] { add(); }); });
    apply_button = new QPushButton(QStringLiteral("台詞を直す"));
    apply_button->setObjectName(QStringLiteral("story_apply"));
    apply_button->setToolTip(QStringLiteral("選んだ台詞を、上の欄の言葉・話者・形に直します"));
    connect(apply_button, &QPushButton::clicked, this, [this] { carry_on("apply_edit", [this] { apply_edit(); }); });
    delete_button = iconic(QStringLiteral("消す"), "delete", QStringLiteral("選んだ台詞を消す"));
    delete_button->setObjectName(QStringLiteral("story_delete"));
    connect(delete_button, &QPushButton::clicked, this, [this] { carry_on("remove", [this] { remove(); }); });
    // lettering style of the selected line (applied at once)
    font = new QComboBox;
    font->setObjectName(QStringLiteral("story_font"));
    for (const auto& [key, label] : lettering::bundled_fonts()) font->addItem(label, key);
    font->addItem(QStringLiteral("パソコンの書体を選ぶ…"), QStringLiteral("__pick__"));
    connect(font, &QComboBox::activated, this, [this](int) { carry_on("font_changed", [this] { font_changed(); }); });
    const auto spin = [this](double lo, double hi, double step, const QString& suffix, const QString& special = {}) {
        auto* box = new QDoubleSpinBox;
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setDecimals(2);
        box->setSuffix(suffix);
        if (!special.isEmpty()) box->setSpecialValueText(special);
        connect(box, &QDoubleSpinBox::editingFinished, this, [this] { carry_on("style_changed", [this] { style_changed(); }); });
        return box;
    };
    const auto choice = [this](std::initializer_list<std::pair<const char*, const char*>> items) {
        auto* combo = new QComboBox;
        for (const auto& [label, key] : items) combo->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
        connect(combo, &QComboBox::activated, this, [this](int) { carry_on("style_changed", [this] { style_changed(); }); });
        return combo;
    };
    const auto check = [this](const QString& label) {
        auto* box = new QCheckBox(label);
        connect(box, &QCheckBox::clicked, this, [this](bool) { carry_on("style_changed", [this] { style_changed(); }); });
        return box;
    };
    size = spin(0, 40, 0.5, QStringLiteral(" mm"), QStringLiteral("自動"));
    tracking = spin(-0.3, 1.0, 0.05, QStringLiteral(" 字"));
    leading = spin(-0.3, 2.0, 0.05, QStringLiteral(" 字"));
    outline = spin(0, 5, 0.1, QStringLiteral(" mm"), QStringLiteral("なし"));
    border = spin(0, 3, 0.05, QStringLiteral(" mm"));
    align = choice({{"上", "top"}, {"中央", "center"}, {"下", "bottom"}, {"左", "left"}, {"右", "right"}, {"均等（列・行いっぱいに）", "justify"}});
    fill = choice({{"白", "white"}, {"塗らない（透明）", "none"}});
    tcy = check(QStringLiteral("数字と !? を縦中横にする"));
    rotate = spin(-180, 180, 5, QStringLiteral("°"));
    rotate->setDecimals(0);
    rotate->setToolTip(QStringLiteral("フキダシごと回します（選択ツールで、フキダシの上の○をドラッグしても回せます）"));
    skew = spin(-60, 60, 5, QStringLiteral("°"));
    skew->setDecimals(0);
    skew->setToolTip(QStringLiteral("文字を傾けます（描き文字・効果音に）"));
    scale_x = spin(0.3, 3, 0.05, QString());
    scale_x->setToolTip(QStringLiteral("長体・平体: 1 より小さいと細長い字（長体）、大きいと平たい字（平体）"));
    gradient = new QPushButton(QStringLiteral("文字のグラデーション…"));
    gradient->setToolTip(QStringLiteral("文字を上から下へ 2 色で塗り分けます（もう一度押すと外す）"));
    connect(gradient, &QPushButton::clicked, this, [this] { carry_on("pick_gradient", [this] { pick_gradient(); }); });
    yakumono = check(QStringLiteral("約物を詰める（」「 などを半分に）"));
    arc = spin(-1, 1, 0.1, QString());
    arc->setToolTip(QStringLiteral("文字を弓なりに曲げます（1 で真ん中が大きく持ち上がる。マイナスで逆向き）"));
    latin = choice({{"4 文字以上は寝かせる", "rotate"}, {"1 文字ずつ立てる", "upright"}});
    latin->setToolTip(QStringLiteral("縦書きの中の半角の英数字の組み方"));
    mark = choice({{"ゴマ（﹅）", "sesame"}, {"黒丸（・）", "dot"}});
    mark->setToolTip(QStringLiteral("《《強調》》と書いた所に付く傍点の形"));
    weight = choice({{"標準", "normal"}, {"太", "bold"}, {"極太", "heavy"}});
    weight->setToolTip(QStringLiteral("文字の太さ。書体に太い字がないときは、字の線を太らせて作ります"));
    italic = check(QStringLiteral("斜体"));
    outline_colour = new QPushButton(QStringLiteral("フチの色…"));
    outline_colour->setToolTip(QStringLiteral("白フチの色（黒フチなど）"));
    connect(outline_colour, &QPushButton::clicked, this, [this] { carry_on("pick_outline_colour", [this] { pick_outline_colour(); }); });
    wobble = spin(0, 1, 0.1, QString());
    wobble->setToolTip(QStringLiteral("フキダシの線を手描きのように揺らす（0 でまっすぐ）"));
    double_line = check(QStringLiteral("二重線"));
    spikes = new QSpinBox;
    spikes->setRange(0, 80);
    spikes->setSpecialValueText(QStringLiteral("自動"));
    spikes->setToolTip(QStringLiteral("叫びのフキダシのトゲの数"));
    connect(spikes, &QSpinBox::editingFinished, this, [this] { carry_on("style_changed", [this] { style_changed(); }); });
    spike_depth = spin(0.05, 0.6, 0.05, QString());
    spike_depth->setToolTip(QStringLiteral("叫びのフキダシのトゲの長さ（大きいほど鋭い）"));
    color = new QPushButton(QStringLiteral("文字の色…"));
    connect(color, &QPushButton::clicked, this, [this] { carry_on("pick_color", [this] { pick_color(); }); });
    line_colour = new QPushButton(QStringLiteral("フキダシの線の色…"));
    connect(line_colour, &QPushButton::clicked, this, [this] {
        carry_on("pick_style_colour", [this] { pick_style_colour("line_rgb", QStringLiteral("フキダシの線の色"), QColor(20, 20, 20)); });
    });
    fill_colour = new QPushButton(QStringLiteral("フキダシの中の色…"));
    connect(fill_colour, &QPushButton::clicked, this, [this] {
        carry_on("pick_style_colour", [this] { pick_style_colour("fill_rgb", QStringLiteral("フキダシの中の色"), QColor(255, 255, 255)); });
    });
    fill_cover = new QSpinBox;
    fill_cover->setRange(0, 100);
    fill_cover->setSuffix(QStringLiteral(" %"));
    fill_cover->setToolTip(QStringLiteral("フキダシの中の塗りの濃さ（下の絵が透ける）"));
    connect(fill_cover, &QSpinBox::editingFinished, this, [this] { carry_on("style_changed", [this] { style_changed(); }); });
    text_dx = spin(-40, 40, 0.5, QStringLiteral(" mm"));
    text_dy = spin(-40, 40, 0.5, QStringLiteral(" mm"));
    for (QDoubleSpinBox* box : {text_dx, text_dy}) box->setToolTip(QStringLiteral("文字だけをフキダシの中でずらします（フキダシは動かない）"));
    tail_width = spin(0, 30, 0.5, QStringLiteral(" mm"), QStringLiteral("自動"));
    tail_width->setToolTip(QStringLiteral("しっぽの付け根の幅"));
    connect(tail_width, &QDoubleSpinBox::editingFinished, this, [this] { carry_on("set_tail_width", [this] { set_tail_width(); }); });  // (after its style: as Python's)
    path_curve = check(QStringLiteral("手で描いたフキダシを曲線にする"));
    ruby_scale = spin(0.25, 0.8, 0.05, QStringLiteral(" 字"));
    ruby_scale->setToolTip(QStringLiteral("ルビの大きさ（本文の何字ぶんか。既定 0.5）"));
    mono_ruby = check(QStringLiteral("モノルビ（1 字ずつに振る）"));
    mono_ruby->setToolTip(QStringLiteral("読みが字数で割り切れるとき、1 字ずつの真横に振ります（つかない時はまとめて）"));
    layer_order = new QComboBox;
    layer_order->setToolTip(QStringLiteral("テキストの重ね順: このレイヤーの下に台詞を描きます（上のレイヤーの絵がフキダシに重なる）"));
    connect(layer_order, &QComboBox::activated, this, [this](int) {
        carry_on("layer_order", [this] {
            const QVariant data = layer_order->currentData();
            style(Json{{"below_layer", data.isValid() && !data.isNull() ? Json(data.toString().toStdString()) : Json()}});
        });
    });
    reset = new QPushButton(QStringLiteral("既定の設定に戻す"));
    reset->setToolTip(QStringLiteral("この台詞の文字とフキダシの設定を既定に戻します"));
    connect(reset, &QPushButton::clicked, this, [this] { carry_on("reset_style", [this] { reset_style(); }); });
    // the names the tests and the screen readers use
    for (const auto& [widget, name] : std::initializer_list<std::pair<QWidget*, const char*>>{
             {font, "font"}, {size, "size"}, {tracking, "tracking"}, {leading, "leading"}, {outline, "outline"}, {border, "border"},
             {align, "align"}, {fill, "fill"}, {tcy, "tcy"}, {rotate, "rotate"}, {skew, "skew"}, {scale_x, "scale_x"},
             {gradient, "gradient"}, {yakumono, "yakumono"}, {arc, "arc"}, {latin, "latin"}, {mark, "mark"}, {weight, "weight"},
             {italic, "italic"}, {outline_colour, "outline_colour"}, {wobble, "wobble"}, {double_line, "double"}, {spikes, "spikes"},
             {spike_depth, "spike_depth"}, {color, "color"}, {line_colour, "line_colour"}, {fill_colour, "fill_colour"},
             {fill_cover, "fill_cover"}, {text_dx, "text_dx"}, {text_dy, "text_dy"}, {tail_width, "tail_width"},
             {path_curve, "path_curve"}, {ruby_scale, "ruby_scale"}, {mono_ruby, "mono_ruby"}, {layer_order, "layer_order"},
             {reset, "reset"}})
        widget->setObjectName(QStringLiteral("story_") + QString::fromLatin1(name));

    auto* order = new QHBoxLayout;
    order->setSpacing(2);
    auto* heading = new QLabel(QStringLiteral("台詞（読み順）"));
    theme::role(heading, "section");
    order->addWidget(heading, 1);
    order->addWidget(up_button);
    order->addWidget(down_button);
    order->addWidget(delete_button);
    auto* row = new QHBoxLayout;
    row->addWidget(kind, 1);
    row->addWidget(vertical);
    connect(text, &QPlainTextEdit::textChanged, this, [this] { carry_on("main_button", [this] { main_button(); }); });  // (the accent only once there are words to add)
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add_button, 1);
    buttons->addWidget(apply_button, 1);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("書体"), font);
    form->addRow(QStringLiteral("文字の大きさ"), size);
    form->addRow(QStringLiteral("字間"), tracking);
    form->addRow(QStringLiteral("行間"), leading);
    form->addRow(QStringLiteral("揃え"), align);
    form->addRow(QStringLiteral("白フチ"), outline);
    form->addRow(QStringLiteral("フキダシの線"), border);
    form->addRow(QStringLiteral("フキダシの中"), fill);
    form->addRow(QString(), line_colour);  // (the balloon's colours beside its line and inside, not at the bottom)
    form->addRow(QString(), fill_colour);
    form->addRow(QString(), tcy);
    form->addRow(QStringLiteral("欧文"), latin);
    form->addRow(QStringLiteral("傍点"), mark);
    auto* faces = new QHBoxLayout;
    faces->addWidget(weight);
    faces->addWidget(italic);
    form->addRow(QStringLiteral("太さ"), faces);
    form->addRow(QString(), outline_colour);
    form->addRow(QStringLiteral("線の揺れ"), wobble);
    form->addRow(QString(), double_line);
    form->addRow(QStringLiteral("トゲの数"), spikes);
    form->addRow(QStringLiteral("トゲの長さ"), spike_depth);
    form->addRow(QStringLiteral("回転"), rotate);
    form->addRow(QStringLiteral("傾き"), skew);
    form->addRow(QStringLiteral("弓なり"), arc);
    form->addRow(QStringLiteral("長体・平体"), scale_x);
    form->addRow(QString(), yakumono);
    form->addRow(QString(), color);
    form->addRow(QString(), gradient);
    form->addRow(QStringLiteral("中の塗りの濃さ"), fill_cover);
    auto* shift = new QHBoxLayout;
    shift->addWidget(text_dx);
    shift->addWidget(text_dy);
    form->addRow(QStringLiteral("文字のずれ（横・縦）"), shift);
    form->addRow(QStringLiteral("しっぽの幅"), tail_width);
    form->addRow(QString(), path_curve);
    form->addRow(QStringLiteral("ルビの大きさ"), ruby_scale);
    form->addRow(QString(), mono_ruby);
    form->addRow(QStringLiteral("重ね順"), layer_order);
    auto* hint = new QLabel(QStringLiteral("フキダシはダブルクリックで打ち直し、四隅で大きさ、●でしっぽの先、◇でしっぽの曲がり、上の○で回転。"
                                           "右クリックで形・しっぽ・結合。"));
    hint->setWordWrap(true);
    theme::hint(hint);
    // the lettering and balloon settings of the chosen line sit beside the tool (ツールの設定), where there is room;
    // this panel keeps the list and the words
    style_box = new QWidget;
    style_box->setObjectName(QStringLiteral("story_style_box"));
    style_title = new QLabel;
    theme::role(style_title, "heading");
    style_title->setWordWrap(true);
    auto* sl = new QVBoxLayout(style_box);
    sl->setContentsMargins(0, 6, 0, 0);
    sl->addWidget(style_title);
    // (hidden while no line is chosen: a column of greyed-out fields only says "not here")
    style_body = new QWidget;
    auto* bl = new QVBoxLayout(style_body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addLayout(form);
    bl->addWidget(reset);
    bl->addWidget(hint);
    sl->addWidget(style_body);
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(4);
    layout->addLayout(order);
    layout->addWidget(list, 1);
    layout->addWidget(speaker);
    layout->addWidget(text);
    layout->addLayout(row);
    layout->addLayout(buttons);
    auto* more = new QLabel(QStringLiteral("文字・フキダシの設定は左の「ツールの設定」に出ます"));
    more->setWordWrap(true);
    theme::hint(more);
    layout->addWidget(more);
    picked();
}

std::vector<const core::StoryLine*> StoryPanel::lines() const {
    const core::Page* page = window_->current_page();
    return page != nullptr ? window_->book().story_for_page(page->index) : std::vector<const core::StoryLine*>{};
}

const core::StoryLine* StoryPanel::line() const {
    const auto id = current_id();
    if (!id) return nullptr;
    for (const core::StoryLine* l : lines()) {
        if (l->id == *id) return l;
    }
    return nullptr;
}

std::optional<std::string> StoryPanel::current_id() const {
    const int row = list->currentRow();
    return row >= 0 && row < static_cast<int>(line_ids_.size()) ? std::optional<std::string>(line_ids_[static_cast<std::size_t>(row)]) : std::nullopt;
}

void StoryPanel::select(const std::optional<std::string>& line_id) {
    if (!line_id) return;
    const auto it = std::find(line_ids_.begin(), line_ids_.end(), *line_id);
    if (it != line_ids_.end()) list->setCurrentRow(static_cast<int>(it - line_ids_.begin()));
}

void StoryPanel::refresh() {
    const auto current = current_id();
    {
        const QSignalBlocker quiet(list);
        list->clear();
        line_ids_.clear();
        int n = 0;
        for (const core::StoryLine* l : lines()) {
            const QString who = l->speaker.empty() ? QString() : qs(l->speaker) + QStringLiteral(": ");
            const QString label = lettering::kind_label(l->balloon);
            const QString words = qs(l->text).replace(QLatin1Char('\n'), QLatin1Char(' '));
            // (the kind first: a long line is cut at its end, not its kind)
            auto* item = new QListWidgetItem(QStringLiteral("%1.〔%2〕%3%4").arg(++n).arg(label.section(QStringLiteral("（"), 0, 0), who, words));
            item->setToolTip(QStringLiteral("%1%2\n%3").arg(who, words, label));
            list->addItem(item);
            line_ids_.push_back(l->id);
        }
        const auto it = current ? std::find(line_ids_.begin(), line_ids_.end(), *current) : line_ids_.end();
        list->setCurrentRow(it != line_ids_.end() ? static_cast<int>(it - line_ids_.begin()) : -1);
    }
    empty_note->setGeometry(list->viewport()->rect().adjusted(8, 8, -8, -8));
    empty_note->setVisible(list->count() == 0);
    picked();
}

void StoryPanel::main_button() {
    theme::role_prop(add_button, "primary", !lettering::strip(text->toPlainText()).isEmpty() && !current_id());
}

void StoryPanel::picked() {
    // (Python's _picked stops at a value it cannot show, its slot's error logged and _loading left on; here every field
    // that can be shown is, and the panel goes on taking edits)
    try {
        show_picked();
    } catch (const std::exception& error) {
        loading_ = false;
        qWarning("StoryPanel.picked: %s", error.what());
    }
}

void StoryPanel::show_picked() {
    const core::StoryLine* l = line();
    for (QWidget* widget : {static_cast<QWidget*>(apply_button), static_cast<QWidget*>(delete_button)}) widget->setEnabled(l != nullptr);
    theme::role_prop(apply_button, "primary", l != nullptr);
    main_button();
    style_body->setVisible(l != nullptr);
    theme::role(style_title, l != nullptr ? "heading" : "hint");
    if (l != nullptr) {
        const QString words = qs(l->text);
        const qsizetype length = static_cast<qsizetype>(render::text::u32(l->text).size());
        const QString first = qs(render::text::utf8(render::text::u32(l->text).substr(0, 12)));
        style_title->setText(QStringLiteral("選んだ台詞の文字とフキダシ: 「%1%2」").arg(first, length > 12 ? QStringLiteral("…") : QString()));
    } else {
        style_title->setText(QStringLiteral("台詞をクリックすると設定が出ます"));
    }
    window_->canvas()->selected_line_id = l != nullptr ? std::optional<std::string>(l->id) : std::nullopt;
    window_->canvas()->update();
    if (l == nullptr) return;
    loading_ = true;
    speaker->setText(qs(l->speaker));
    text->setPlainText(lettering::with_marks(*l));
    kind->setCurrentIndex(std::max(0, kind->findData(qs(l->balloon))));
    vertical->setChecked(l->wrap == "vertical");
    const Json st = render::text::style_of(*l);
    // (a value that is not a number — written into the book by hand — or more than its field holds shows as the
    // field's default, or as much as the field holds)
    const auto read = [](auto&& value, auto fallback) -> decltype(fallback) {
        try {
            return value();
        } catch (const std::exception&) {
            return fallback;
        }
    };
    const auto f = [&st, &read](const char* key, double fallback) {
        return read([&] { return core::py_truthy(st[key]) ? core::to_float(st[key]) : fallback; }, fallback);
    };
    const QString face = core::py_truthy(st["font"]) ? qs(core::py_str(st["font"])) : (l->balloon == "sfx" ? QStringLiteral("sfx") : QStringLiteral("antique"));
    int index = font->findData(face);
    if (index < 0) {
        font->insertItem(font->count() - 1, QFileInfo(face).completeBaseName(), face);
        index = font->findData(face);
    }
    font->setCurrentIndex(index);
    size->setValue(f("size_mm", 0));
    tracking->setValue(f("tracking", 0));
    leading->setValue(f("leading", 0));
    outline->setValue(f("outline_mm", 0));
    border->setValue(read([&] { return st["border_mm"].is_null() ? 0.35 : core::to_float(st["border_mm"]); }, 0.35));
    align->setCurrentIndex(std::max(0, align->findData(qs(core::py_str(st["align"])))));
    fill->setCurrentIndex(std::max(0, fill->findData(qs(core::py_str(st["fill"])))));
    tcy->setChecked(core::py_truthy(st["tcy"]));
    rotate->setValue(f("rotate_deg", 0));
    skew->setValue(f("skew_deg", 0));
    arc->setValue(f("arc", 0));
    scale_x->setValue(f("scale_x", 1.0));
    yakumono->setChecked(core::py_truthy(st["yakumono"]));
    gradient->setText(core::py_truthy(st["gradient"]) ? QStringLiteral("文字のグラデーションを外す") : QStringLiteral("文字のグラデーション…"));
    latin->setCurrentIndex(std::max(0, latin->findData(qs(core::py_str(st["latin"])))));
    mark->setCurrentIndex(std::max(0, mark->findData(qs(core::py_str(st["emphasis_mark"])))));
    weight->setCurrentIndex(read([&] { return render::text::line_weight(st); }, 0));
    italic->setChecked(core::py_truthy(st["italic"]));
    wobble->setValue(f("wobble", 0));
    double_line->setChecked(core::py_truthy(st["double"]));
    spikes->setValue(read([&] { return core::py_truthy(st["spikes"]) ? static_cast<int>(std::clamp<std::int64_t>(core::py_int(st["spikes"]), 0, 80)) : 0; }, 0));
    spike_depth->setValue(f("spike_depth", 0.2));
    fill_cover->setValue(read(
        [&] {
            const double percent = 100 * (st["fill_opacity"].is_null() ? 1.0 : core::to_float(st["fill_opacity"]));
            return std::isnan(percent) ? 100 : static_cast<int>(core::py_round_int(std::clamp(percent, 0.0, 100.0)));
        },
        100));
    text_dx->setValue(f("text_dx_mm", 0));
    text_dy->setValue(f("text_dy_mm", 0));
    const Json first_width = !l->tails.empty() && l->tails.front().is_object() ? core::get_or(l->tails.front(), "width_mm", Json()) : Json();
    tail_width->setValue(read([&] { return core::py_truthy(first_width) ? core::to_float(first_width) : 0.0; }, 0.0));
    tail_width->setEnabled(!l->tails.empty());
    path_curve->setVisible(l->path.has_value() && !l->path->empty());
    path_curve->setChecked(core::py_truthy(st["path_curve"]));
    ruby_scale->setValue(f("ruby_scale", 0.5));
    mono_ruby->setChecked(core::py_truthy(st["mono_ruby"]));
    layer_order->clear();
    layer_order->addItem(QStringLiteral("いちばん上（既定）"), QVariant());
    if (const core::Page* page = window_->current_page()) {
        for (auto it = page->layers.rbegin(); it != page->layers.rend(); ++it)
            layer_order->addItem(QStringLiteral("「%1」の下").arg(wording::layer_label(*it)), qs(it->id));
    }
    const int below = st["below_layer"].is_string() ? layer_order->findData(qs(st["below_layer"].get<std::string>())) : 0;
    layer_order->setCurrentIndex(std::max(0, below));
    loading_ = false;
}

void StoryPanel::style(const Json& change) {
    const core::StoryLine* l = line();
    if (l != nullptr && !loading_) window_->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", l->id}, {"style", change}}}));
}

void StoryPanel::style_changed() {
    if (loading_) return;
    const auto or_none = [](double v) { return v != 0.0 ? Json(v) : Json(); };
    const auto on_or_none = [](bool on) { return on ? Json(true) : Json(); };
    const auto data = [](QComboBox* box) { return Json(box->currentData().toString().toStdString()); };
    Json change = Json::object();
    change["size_mm"] = or_none(size->value());
    change["tracking"] = tracking->value();
    change["leading"] = leading->value();
    change["outline_mm"] = or_none(outline->value());
    change["border_mm"] = border->value();
    change["align"] = data(align);
    change["fill"] = data(fill);
    change["tcy"] = tcy->isChecked();
    change["rotate_deg"] = or_none(rotate->value());
    change["skew_deg"] = or_none(skew->value());
    change["arc"] = or_none(arc->value());
    change["latin"] = data(latin);
    change["emphasis_mark"] = data(mark);
    change["weight"] = weight->currentIndex() != 0 ? data(weight) : Json();
    change["bold"] = nullptr;
    change["italic"] = on_or_none(italic->isChecked());
    change["wobble"] = or_none(wobble->value());
    change["double"] = on_or_none(double_line->isChecked());
    change["spikes"] = spikes->value() != 0 ? Json(spikes->value()) : Json();
    change["spike_depth"] = std::abs(spike_depth->value() - 0.2) > 1e-6 ? Json(spike_depth->value()) : Json();
    change["scale_x"] = std::abs(scale_x->value() - 1) > 1e-3 ? Json(scale_x->value()) : Json();
    change["yakumono"] = yakumono->isChecked() ? Json() : Json(false);
    change["fill_opacity"] = fill_cover->value() < 100 ? Json(fill_cover->value() / 100.0) : Json();
    change["text_dx_mm"] = or_none(text_dx->value());
    change["text_dy_mm"] = or_none(text_dy->value());
    change["path_curve"] = on_or_none(path_curve->isChecked());
    change["ruby_scale"] = std::abs(ruby_scale->value() - 0.5) > 1e-3 ? Json(ruby_scale->value()) : Json();
    change["mono_ruby"] = on_or_none(mono_ruby->isChecked());
    style(change);
}

void StoryPanel::pick_style_colour(const char* key, const QString& title, const QColor& fallback) {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    const auto chosen = ask::colour(this, start_colour(render::text::style_of(*l)[key], fallback), title);
    if (chosen) style(Json{{key, Json::array({chosen->red(), chosen->green(), chosen->blue()})}});
}

void StoryPanel::set_tail_width() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_ || l->tails.empty()) return;
    const double width = tail_width->value();
    Json tails = Json::array();
    for (const Json& t : l->tails) {
        Json out = t;
        if (width != 0.0) {
            out["width_mm"] = width;
        } else if (out.is_object()) {
            out.erase("width_mm");
        }
        tails.push_back(out);
    }
    window_->apply_ops(Json::array({Json{{"op", "move_line"}, {"id", l->id}, {"tails", tails}}}));
}

void StoryPanel::font_changed() {
    if (loading_) return;
    QString key = font->currentData().toString();
    if (key == QLatin1String("__pick__")) {
        const auto picked_font = pick_system_font();
        if (!picked_font) {
            picked();
            return;
        }
        key = qs(*picked_font);
    }
    style(Json{{"font", key.toStdString()}});
    picked();
}

std::optional<std::string> StoryPanel::pick_system_font() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Json found;
    try {
        found = system_fonts();
    } catch (...) {
        QApplication::restoreOverrideCursor();
        throw;
    }
    QApplication::restoreOverrideCursor();
    if (found.empty()) {
        window_->flash(QStringLiteral("日本語を表示できる書体が、このパソコンに見つかりませんでした"), 6000);
        return std::nullopt;
    }
    QStringList names;
    for (const Json& item : found) names << QStringLiteral("%1 %2").arg(qs(core::py_str(item["name"])), qs(core::py_str(item["style"])));
    const auto name = ask::get_item(this, QStringLiteral("パソコンの書体"), QStringLiteral("書体"), names, 0, false);
    if (!name || !names.contains(*name)) return std::nullopt;
    return core::py_str(found[static_cast<std::size_t>(names.indexOf(*name))]["path"]);
}

void StoryPanel::pick_gradient() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    if (core::py_truthy(render::text::style_of(*l)["gradient"])) {
        style(Json{{"gradient", nullptr}});
        return;
    }
    const auto top = ask::colour(this, QColor(250, 200, 0), QStringLiteral("グラデーションの上の色"));
    if (!top) return;
    const auto bottom = ask::colour(this, QColor(200, 0, 0), QStringLiteral("グラデーションの下の色"));
    if (bottom) {
        style(Json{{"gradient", Json{{"rgb_from", Json::array({top->red(), top->green(), top->blue()})},
                                     {"rgb_to", Json::array({bottom->red(), bottom->green(), bottom->blue()})},
                                     {"angle", 90}}}});
    }
}

void StoryPanel::pick_color() {
    pick_style_colour("rgb", QStringLiteral("文字の色"), QColor(10, 10, 10));
}

void StoryPanel::pick_outline_colour() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    const auto chosen = ask::colour(this, start_colour(render::text::style_of(*l)["outline_rgb"], QColor(255, 255, 255)), QStringLiteral("フチの色"));
    if (!chosen) return;
    // (re-read: the dialog may have let the book change)
    const core::StoryLine* again = line();
    if (again == nullptr) return;
    const Json outline_mm = render::text::style_of(*again)["outline_mm"];
    style(Json{{"outline_rgb", Json::array({chosen->red(), chosen->green(), chosen->blue()})},
               {"outline_mm", core::py_truthy(outline_mm) ? outline_mm : Json(0.6)}});
}

void StoryPanel::reset_style() {
    if (line() == nullptr) return;
    Json keep = Json::object();
    for (const std::string& key : core::line_style_keys()) {
        if (key != "group") keep[key] = nullptr;
    }
    style(keep);
    picked();
}

void StoryPanel::move(int delta) {
    const core::Page* page = window_->current_page();
    const auto line_id = current_id();
    if (page == nullptr || !line_id) return;
    std::vector<std::string> order = line_ids_;
    const auto i = static_cast<std::ptrdiff_t>(std::find(order.begin(), order.end(), *line_id) - order.begin());
    const std::ptrdiff_t j = i + delta;
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(order.size())) return;
    std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
    Json ids = Json::array();
    for (const std::string& id : order) ids.push_back(id);
    if (window_->apply_ops(Json::array({Json{{"op", "reorder_lines"}, {"page", page->index.json()}, {"order", ids}}}))) {
        refresh();
        select(line_id);
    }
}

void StoryPanel::add() {
    const core::Page* page = window_->current_page();
    const QString typed = lettering::strip(text->toPlainText());
    if (page == nullptr || typed.isEmpty()) {
        window_->flash(QStringLiteral("台詞を書いてから追加します"), 6000);
        return;
    }
    const core::Frame* frame = window_->selected_frame();
    if (frame == nullptr) {
        window_->flash(QStringLiteral("先に編集画面でコマをクリックして選びます（テキストツール T なら、置きたい所をクリック）"), 6000);
        return;
    }
    const lettering::Marks marks = lettering::parse_marks(typed);
    const std::string balloon = kind->currentData().toString().toStdString();
    const Json box = lettering::place_new(window_->book(), *page, *frame, marks.text, balloon, vertical->isChecked());
    std::vector<std::string> before;
    for (const core::StoryLine* l : lines()) before.push_back(l->id);
    Json op{{"op", "add_line"}, {"page", page->index.json()}, {"text", marks.text}, {"speaker", lettering::strip(speaker->text()).toStdString()},
            {"frame_id", frame->id}, {"balloon", balloon}};
    for (const auto& [key, value] : box.items()) op[key] = value;
    if (!marks.ruby_runs.empty()) op["ruby_runs"] = marks.ruby_runs;
    if (!marks.emphasis_runs.empty()) op["emphasis_runs"] = marks.emphasis_runs;
    if (!marks.style_runs.empty()) op["style_runs"] = marks.style_runs;
    if (window_->apply_ops(Json::array({op}))) {
        text->clear();
        std::optional<std::string> added;
        for (const core::StoryLine* l : lines()) {
            if (std::find(before.begin(), before.end(), l->id) == before.end()) {
                added = l->id;
                break;
            }
        }
        refresh();
        select(added);
    }
}

void StoryPanel::apply_edit() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    const QString typed = lettering::strip(text->toPlainText());
    if (typed.isEmpty()) {
        window_->flash(QStringLiteral("台詞が空です。消すときは「削除」を押します"), 6000);
        return;
    }
    const lettering::Marks marks = lettering::parse_marks(typed);
    const std::string balloon = kind->currentData().toString().toStdString();
    const bool down = vertical->isChecked();
    Json ops = Json::array({Json{{"op", "edit_line"},
                                 {"id", l->id},
                                 {"text", marks.text},
                                 {"speaker", lettering::strip(speaker->text()).toStdString()},
                                 {"balloon", balloon},
                                 {"wrap", down ? "vertical" : "horizontal"},
                                 {"ruby_runs", marks.ruby_runs},
                                 {"emphasis_runs", marks.emphasis_runs},
                                 {"style_runs", marks.style_runs}}});
    if (marks.text != l->text || balloon != l->balloon || down != (l->wrap == "vertical")) {
        Json op{{"op", "move_line"}, {"id", l->id}};
        const Json size = lettering::refit(*l, window_->frame_by_id(l->frame_id), marks.text, balloon, down);
        for (const auto& [key, value] : size.items()) op[key] = value;
        ops.push_back(op);
    }
    window_->apply_ops(ops);
}

void StoryPanel::remove() {
    const auto line_id = current_id();
    if (line_id && window_->apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", *line_id}}}))) refresh();
}

}  // namespace genko::app
