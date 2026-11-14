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
#include "service/wording.hpp"
#include "core/command_bus.hpp"
#include "core/fukidashi.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "render/text/balloons.hpp"
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
    theme::iconed(button, icon);
    button->setText(QString());
    button->setToolTip(tip);
    button->setFixedSize(28, 26);
    button->setProperty("iconbtn", true);
    return button;
}

}  // namespace

StoryPanel::StoryPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("story_panel"));
    list = new QListWidget;
    list->setObjectName(QStringLiteral("storyList"));
    connect(list, &QListWidget::currentRowChanged, this, [this](int) { picked(); });  // (picked carries on by itself)
    empty_note = new QLabel(QStringLiteral("このページにはまだ台詞がありません。\n縦書き文字ツール（T）で、置きたい所をクリックします"), list->viewport());
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
    theme::iconed(add_button, "add");
    add_button->setToolTip(QStringLiteral("上の欄の台詞を、選んだコマに加えます"));
    connect(add_button, &QPushButton::clicked, this, [this] { carry_on("add", [this] { add(); }); });
    apply_button = new QPushButton(QStringLiteral("台詞を直す"));
    apply_button->setObjectName(QStringLiteral("story_apply"));
    apply_button->setToolTip(QStringLiteral("選んだ台詞を、上の欄の言葉・話者・形に直します"));
    connect(apply_button, &QPushButton::clicked, this, [this] { carry_on("apply_edit", [this] { apply_edit(); }); });
    delete_button = iconic(QStringLiteral("消す"), "delete", QStringLiteral("選んだ台詞を消す"));
    delete_button->setObjectName(QStringLiteral("story_delete"));
    connect(delete_button, &QPushButton::clicked, this, [this] { carry_on("remove", [this] { remove(); }); });
    // lettering style of the selected line (applied at once) — PC5 G-d (8-5 (a)): its face, size, tracking, rows' room,
    // alignment, colour, weight, slant, width, 縦中横 and 約物の詰め are the 文字 and 段落 panels' now (app/type_panels)
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
    outline = spin(0, 5, 0.1, QStringLiteral(" mm"), QStringLiteral("なし"));
    border = spin(0, 3, 0.05, QStringLiteral(" mm"));
    fill = choice({{"白", "white"}, {"塗らない（透明）", "none"}});
    rotate = spin(-180, 180, 5, QStringLiteral("°"));
    rotate->setDecimals(0);
    rotate->setToolTip(QStringLiteral("フキダシごと回します（選択ツールで、フキダシの上の○をドラッグしても回せます）"));
    skew = spin(-60, 60, 5, QStringLiteral("°"));
    skew->setDecimals(0);
    skew->setToolTip(QStringLiteral("文字を傾けます（描き文字・効果音に）"));
    gradient = new QPushButton(QStringLiteral("文字のグラデーション…"));
    gradient->setToolTip(QStringLiteral("文字を上から下へ 2 色で塗り分けます（もう一度押すと外す）"));
    connect(gradient, &QPushButton::clicked, this, [this] { carry_on("pick_gradient", [this] { pick_gradient(); }); });
    arc = spin(-1, 1, 0.1, QString());
    arc->setToolTip(QStringLiteral("文字を弓なりに曲げます（1 で真ん中が大きく持ち上がる。マイナスで逆向き）"));
    latin = choice({{"4 文字以上は寝かせる", "rotate"}, {"1 文字ずつ立てる", "upright"}});
    latin->setToolTip(QStringLiteral("縦書きの中の半角の英数字の組み方"));
    mark = choice({{"ゴマ（﹅）", "sesame"}, {"黒丸（・）", "dot"}});
    mark->setToolTip(QStringLiteral("《《強調》》と書いた所に付く傍点の形"));
    outline_colour = new QPushButton(QStringLiteral("フチの色…"));
    outline_colour->setToolTip(QStringLiteral("白フチの色（黒フチなど）"));
    connect(outline_colour, &QPushButton::clicked, this, [this] { carry_on("pick_outline_colour", [this] { pick_outline_colour(); }); });
    double_line = check(QStringLiteral("二重線"));
    spikes = new QSpinBox;
    spikes->setRange(0, 80);
    spikes->setSpecialValueText(QStringLiteral("自動"));
    spikes->setToolTip(QStringLiteral("叫びのフキダシのトゲの数"));
    connect(spikes, &QSpinBox::editingFinished, this, [this] { carry_on("style_changed", [this] { style_changed(); }); });
    spike_depth = spin(0.05, 0.6, 0.05, QString());
    spike_depth->setToolTip(QStringLiteral("叫びのフキダシのトゲの長さ（大きいほど鋭い）"));
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
    // fukidashi@1: the balloon's shape (each its own edit_line; shown for the kinds it changes — show_shape_rows)
    const auto own_spin = [this](double lo, double hi, double step, const QString& suffix, const QString& special, const char* slot_name,
                                 void (StoryPanel::*slot)()) {
        auto* box = new QDoubleSpinBox;
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setDecimals(2);
        box->setSuffix(suffix);
        if (!special.isEmpty()) box->setSpecialValueText(special);
        connect(box, &QDoubleSpinBox::editingFinished, this, [this, slot_name, slot] { carry_on(slot_name, [this, slot] { (this->*slot)(); }); });
        return box;
    };
    corner = new QDoubleSpinBox;
    corner->setRange(-0.5, 50);  // (its least: 自動 — the kind's own corners; 0: square)
    corner->setSingleStep(0.5);
    corner->setDecimals(2);
    corner->setSuffix(QStringLiteral(" mm"));
    corner->setSpecialValueText(QStringLiteral("自動"));
    corner->setToolTip(QStringLiteral("角の丸みの半径（0 で四角い角。「自動」は形に合った丸み）"));
    connect(corner, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_shape_key", [this] {
            if (corner->value() < 0) corner->setValue(corner->minimum());  // (below 0: 自動)
            set_shape_key(corner, corner->value(), 0.005, "corner_mm", corner->value() < 0 ? Json() : Json(corner->value()));
        });
    });
    squircle = new QCheckBox(QStringLiteral("角をふっくら丸くする"));
    squircle->setToolTip(QStringLiteral("辺はまっすぐに近く、角を大きく丸くした形にします（角丸のフキダシ）"));
    connect(squircle, &QCheckBox::clicked, this, [this](bool on) {
        carry_on("set_shape_key", [this, on] { set_shape_key(squircle, on ? 1.0 : 0.0, 0.5, "squircle", on ? Json(true) : Json()); });
    });
    sides = new QSpinBox;
    sides->setRange(5, 12);
    sides->setValue(8);
    sides->setToolTip(QStringLiteral("多角形のフキダシの角の数（5〜12。初めは 8）"));
    connect(sides, &QSpinBox::editingFinished, this, [this] {
        carry_on("set_shape_key", [this] { set_shape_key(sides, sides->value(), 0.5, "sides", sides->value() != 8 ? Json(sides->value()) : Json()); });
    });
    wave = own_spin(0, 3, 0.1, QStringLiteral(" mm"), QStringLiteral("なし"), "set_wave", &StoryPanel::set_wave);
    wave->setToolTip(QStringLiteral("フキダシの線を細かく波打たせます（ふるえる声・おびえた声に。0 でなめらか）"));
    spike_alt = new QDoubleSpinBox;
    spike_alt->setRange(0, 0.8);
    spike_alt->setSingleStep(0.1);
    spike_alt->setDecimals(2);
    spike_alt->setSpecialValueText(QStringLiteral("そろえる"));
    spike_alt->setToolTip(QStringLiteral("叫びのトゲを 1 本おきに短くします（大きいほど長短の差がつく）"));
    connect(spike_alt, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_shape_key", [this] {
            set_shape_key(spike_alt, spike_alt->value(), 0.005, "spike_alt", spike_alt->value() > 0 ? Json(spike_alt->value()) : Json());
        });
    });
    dash = new QDoubleSpinBox;
    dash->setRange(0, 10);
    dash->setSingleStep(0.5);
    dash->setDecimals(2);
    dash->setSuffix(QStringLiteral(" mm"));
    dash->setSpecialValueText(QStringLiteral("実線"));
    dash->setToolTip(QStringLiteral("フキダシの線を破線にして、1 本の長さを決めます（0.2〜10 mm）"));
    connect(dash, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_shape_key", [this] {
            if (dash->value() > 0 && dash->value() < 0.2) dash->setValue(0.2);  // (the shortest dash)
            set_shape_key(dash, dash->value(), 0.005, "dash_mm", dash->value() > 0 ? Json(dash->value()) : Json());
        });
    });
    tail_kind = new QComboBox;
    for (const auto& [key, label] : tail_kinds()) tail_kind->addItem(label, key);
    tail_kind->setToolTip(QStringLiteral("しっぽの形。「内向き」は、フキダシに切れ込むしっぽ（コマの外の人の声に）"));
    connect(tail_kind, &QComboBox::activated, this, [this](int) { carry_on("set_tail_kind", [this] { set_tail_kind(); }); });
    tail_fillet = own_spin(-0.5, 20, 0.5, QStringLiteral(" mm"), QStringLiteral("自動"), "set_tail_fillet", &StoryPanel::set_tail_fillet);
    tail_fillet->setToolTip(QStringLiteral("しっぽとフキダシのつなぎ目の丸み（0 で角のまま。「自動」はほどよい丸み）"));
    // fukidashi@2: the pen's line, the drips, a symbol's points, the tone and the blend inside
    const auto key_spin = [this](QAbstractSpinBox* box, auto get, const char* key, double half_step, auto value) {
        connect(box, &QAbstractSpinBox::editingFinished, this, [this, box, get, key, half_step, value] {
            carry_on("set_shape_key", [this, box, get, key, half_step, value] { set_shape_key(box, get(), half_step, key, value()); });
        });
    };
    pen_swell = new QDoubleSpinBox;
    pen_swell->setRange(0, 0.9);
    pen_swell->setSingleStep(0.1);
    pen_swell->setDecimals(2);
    pen_swell->setSpecialValueText(QStringLiteral("なし"));
    pen_swell->setToolTip(QStringLiteral("フキダシの線を、片側で太く反対側で細くします（G ペンや筆の強弱。0〜0.9。線の太さはその平均）"));
    key_spin(pen_swell, [this] { return pen_swell->value(); }, "pen_swell", 0.005, [this] { return pen_swell->value() > 0 ? Json(pen_swell->value()) : Json(); });
    pen_angle = new QSpinBox;
    pen_angle->setRange(0, 360);
    pen_angle->setSingleStep(15);
    pen_angle->setValue(45);
    pen_angle->setSuffix(QStringLiteral("°"));
    pen_angle->setToolTip(QStringLiteral("線の太い側の向き（0° が右、90° が下。初めは 45°: 右下が太い）"));
    key_spin(pen_angle, [this] { return static_cast<double>(pen_angle->value()); }, "pen_angle", 0.5,
             [this] { return pen_angle->value() != 45 ? Json(pen_angle->value()) : Json(); });
    pen_taper = new QDoubleSpinBox;
    pen_taper->setRange(0, 1);
    pen_taper->setSingleStep(0.1);
    pen_taper->setDecimals(2);
    pen_taper->setSpecialValueText(QStringLiteral("なし"));
    pen_taper->setToolTip(QStringLiteral("ひと筆ごとに、両端へ向かって線を細くします（入り抜き。筆の長さのうち細る割合、0〜1）"));
    key_spin(pen_taper, [this] { return pen_taper->value(); }, "pen_taper", 0.005, [this] { return pen_taper->value() > 0 ? Json(pen_taper->value()) : Json(); });
    pen_strokes = new QSpinBox;
    pen_strokes->setRange(1, 8);
    pen_strokes->setValue(2);
    pen_strokes->setSuffix(QStringLiteral(" 筆"));
    pen_strokes->setToolTip(QStringLiteral("角のない丸いフキダシを何筆で描くか（1〜8。初めは 2）。角のある形は角から角までがひと筆"));
    key_spin(pen_strokes, [this] { return static_cast<double>(pen_strokes->value()); }, "pen_strokes", 0.5,
             [this] { return pen_strokes->value() != 2 ? Json(pen_strokes->value()) : Json(); });
    drip_length = new QDoubleSpinBox;
    drip_length->setRange(0, 30);
    drip_length->setSingleStep(1);
    drip_length->setDecimals(1);
    drip_length->setSuffix(QStringLiteral(" mm"));
    drip_length->setSpecialValueText(QStringLiteral("なし"));
    drip_length->setToolTip(QStringLiteral("フキダシの下からしずくを垂らします（ホラー・溶ける声に）。いちばん長いしずくの長さ。フキダシの箱はそのぶん下へ伸びます"));
    connect(drip_length, &QDoubleSpinBox::editingFinished, this, [this] { carry_on("set_drip_length", [this] { set_drip_length(); }); });
    drip_count = new QSpinBox;
    drip_count->setRange(0, 24);
    drip_count->setSpecialValueText(QStringLiteral("自動"));
    drip_count->setToolTip(QStringLiteral("しずくの数（1〜24。「自動」はフキダシの幅に合わせる）"));
    key_spin(drip_count, [this] { return static_cast<double>(drip_count->value()); }, "drips", 0.5,
             [this] { return drip_count->value() > 0 ? Json(drip_count->value()) : Json(); });
    drip_width = new QDoubleSpinBox;
    drip_width->setRange(0, 10);
    drip_width->setSingleStep(0.2);
    drip_width->setDecimals(1);
    drip_width->setSuffix(QStringLiteral(" mm"));
    drip_width->setSpecialValueText(QStringLiteral("自動"));
    drip_width->setToolTip(QStringLiteral("しずくの太さ（0.3〜10 mm。「自動」はフキダシの幅に合わせる）"));
    connect(drip_width, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_shape_key", [this] {
            if (drip_width->value() > 0 && drip_width->value() < 0.3) drip_width->setValue(0.3);  // (the slimmest drip)
            set_shape_key(drip_width, drip_width->value(), 0.05, "drip_width_mm", drip_width->value() > 0 ? Json(drip_width->value()) : Json());
        });
    });
    drip_seed = new QPushButton(QStringLiteral("しずくの並びを変える"));
    drip_seed->setToolTip(QStringLiteral("しずくの並び（場所・長さ・太さ）を別のものにします。同じ並びは何度描いても同じです"));
    connect(drip_seed, &QPushButton::clicked, this, [this] { carry_on("next_drip_arrangement", [this] { next_drip_arrangement(); }); });
    points = new QSpinBox;
    points->setRange(3, 12);
    points->setSpecialValueText(QStringLiteral("自動"));
    points->setToolTip(QStringLiteral("星の先の数・花の花びらの数（4〜12。「自動」は星 5・花 6。星を 4 にしてトゲを長くすると、きらめき）"));
    key_spin(points, [this] { return static_cast<double>(points->value()); }, "points", 0.5,
             [this] { return points->value() >= 4 ? Json(points->value()) : Json(); });
    const auto tone_spin = [this](int lo, int hi, int step, const QString& suffix, const QString& special, const QString& tip) {
        auto* box = new QSpinBox;
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setSuffix(suffix);
        if (!special.isEmpty()) box->setSpecialValueText(special);
        box->setToolTip(tip);
        connect(box, &QSpinBox::editingFinished, this, [this] { carry_on("set_tone", [this] { set_tone(); }); });
        return box;
    };
    tone_density = tone_spin(-5, 90, 5, QStringLiteral(" %"), QStringLiteral("なし"),  // (its least, −5: none)
                             QStringLiteral("フキダシの中にトーンを敷きます（線の色で。文字には白いフチがつきます）。"
                                            "白黒の原稿では、60 線なら 4〜90%、85 線なら 7〜85% が点として印刷に残ります"
                                            "（それより薄いと点が飛び、濃いと白がつぶれます。外れると点検で知らせます）"));
    tone_lpi = tone_spin(20, 85, 5, QStringLiteral(" 線"), QString(), QStringLiteral("トーンの線数（20〜85。初めは 60。大きいほど細かい網）"));
    tone_pattern = new QComboBox;
    tone_pattern->addItem(QStringLiteral("点"), QStringLiteral("dot"));
    tone_pattern->addItem(QStringLiteral("線"), QStringLiteral("line"));
    tone_pattern->setToolTip(QStringLiteral("トーンの柄（網点か、平行な線か）"));
    connect(tone_pattern, &QComboBox::activated, this, [this](int) { carry_on("set_tone", [this] { set_tone(); }); });
    tone_to = tone_spin(-5, 90, 5, QStringLiteral(" %"), QStringLiteral("同じ"),  // (its least, −5: the same all over)
                        QStringLiteral("トーンがだんだん濃く（薄く）なる、先の濃さ（グラデーションのトーン。白黒の原稿でグラデーションにするのはこちら）"));
    tone_dir = tone_spin(0, 359, 45, QStringLiteral("°"), QString(), QStringLiteral("トーンが濃くなっていく向き（0° が右、90° が下、180° が左、270° が上）"));
    fill_blend = new QPushButton(QStringLiteral("中の色のグラデーション…"));
    fill_blend->setToolTip(QStringLiteral("フキダシの中の塗りを、塗りの色から別の色へなめらかに変えます（カラーの原稿向け。"
                                          "白黒の原稿では印刷のとき白か黒になるので、「中のトーン」の「先の濃さ」を使います）"));
    connect(fill_blend, &QPushButton::clicked, this, [this] { carry_on("pick_blend", [this] { pick_blend(); }); });
    path_curve = check(QStringLiteral("手で描いたフキダシを曲線にする"));
    ruby_scale = spin(0.25, 0.8, 0.05, QStringLiteral(" 字"));
    ruby_scale->setToolTip(QStringLiteral("ルビの大きさ（本文の何字ぶんか。既定 0.5）"));
    mono_ruby = check(QStringLiteral("モノルビ（1 字ずつに振る）"));
    mono_ruby->setToolTip(QStringLiteral("読みが字数で割り切れるとき、1 字ずつの真横に振ります（つかない時はまとめて）"));
    // TEXT-01: the chosen line's rubies, those that need review marked, reassigned or cleared; the chosen ruby's own
    // settings (each change an edit_line of the line's ruby_spans: that ruby's entry carries it, the others untouched)
    rubies = new QListWidget;
    rubies->setToolTip(QStringLiteral("要確認のルビは、本文の編集が親文字にかかったもの。元の親文字と読みを残してあります"));
    rubies->setMaximumHeight(72);
    connect(rubies, &QListWidget::currentRowChanged, this, [this](int) { carry_on("show_ruby_settings", [this] { show_ruby_settings(); }); });
    ruby_reassign = new QPushButton(QStringLiteral("振り直す…"));
    ruby_reassign->setToolTip(QStringLiteral("選んだルビの親文字と読みを選び直します（組み方はそのまま）"));
    connect(ruby_reassign, &QPushButton::clicked, this, [this] { carry_on("reassign_ruby", [this] { reassign_ruby(); }); });
    ruby_clear = new QPushButton(QStringLiteral("外す"));
    ruby_clear->setToolTip(QStringLiteral("選んだルビを外します（元に戻すで戻ります）"));
    connect(ruby_clear, &QPushButton::clicked, this, [this] { carry_on("clear_ruby", [this] { clear_ruby(); }); });
    ruby_font = new QComboBox;
    ruby_font->addItem(QStringLiteral("本文と同じ"), QString());
    for (const auto& [key, label] : lettering::bundled_fonts()) ruby_font->addItem(label, key);
    ruby_font->setToolTip(QStringLiteral("このルビだけの書体（本文の書体はそのまま）"));
    connect(ruby_font, &QComboBox::activated, this, [this](int) {
        carry_on("set_ruby", [this] {
            const QString key = ruby_font->currentData().toString();
            set_ruby("font", key.isEmpty() ? Json() : Json(key.toStdString()));
        });
    });
    ruby_ratio = new QDoubleSpinBox;
    ruby_ratio->setRange(0.2, 0.8);  // (0.2, below the least a ruby takes: none of its own, the line's)
    ruby_ratio->setSingleStep(0.05);
    ruby_ratio->setDecimals(2);
    ruby_ratio->setSuffix(QStringLiteral(" 字"));
    ruby_ratio->setSpecialValueText(QStringLiteral("行の設定"));
    ruby_ratio->setToolTip(QStringLiteral("このルビの大きさ（本文の何字ぶんか、0.25〜0.8。「行の設定」: 縦書きは行のルビの大きさ、横書きは半分）"));
    connect(ruby_ratio, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_ruby", [this] {
            // (above 「行の設定」 but under the least a ruby takes: the least)
            if (ruby_ratio->value() > ruby_ratio->minimum() + 1e-9 && ruby_ratio->value() < 0.25) ruby_ratio->setValue(0.25);
            set_ruby("scale", ruby_ratio->value() > ruby_ratio->minimum() + 1e-9 ? Json(ruby_ratio->value()) : Json());
        });
    });
    ruby_align = new QComboBox;
    ruby_align->addItem(QStringLiteral("先頭（親文字の頭から）"), QStringLiteral("head"));
    ruby_align->addItem(QStringLiteral("中央（親文字の真ん中に）"), QStringLiteral("center"));
    ruby_align->addItem(QStringLiteral("末尾（親文字の終わりまで）"), QStringLiteral("tail"));
    ruby_align->setCurrentIndex(1);
    ruby_align->setToolTip(QStringLiteral("このルビを親文字のどこに揃えるか（既定は中央）"));
    connect(ruby_align, &QComboBox::activated, this, [this](int) {
        carry_on("set_ruby", [this] {
            const QString key = ruby_align->currentData().toString();
            set_ruby("align", key == QLatin1String("center") ? Json() : Json(key.toStdString()));
        });
    });
    ruby_gap = new QDoubleSpinBox;
    ruby_gap->setRange(-1, 10);
    ruby_gap->setSingleStep(0.1);
    ruby_gap->setDecimals(2);
    ruby_gap->setSuffix(QStringLiteral(" mm"));
    ruby_gap->setToolTip(QStringLiteral("このルビと本文の間の距離（mm。既定 0）"));
    connect(ruby_gap, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_ruby", [this] { set_ruby("gap_mm", ruby_gap->value() != 0.0 ? Json(ruby_gap->value()) : Json()); });
    });
    ruby_tracking = new QDoubleSpinBox;
    ruby_tracking->setRange(-0.5, 2);
    ruby_tracking->setSingleStep(0.05);
    ruby_tracking->setDecimals(2);
    ruby_tracking->setSuffix(QStringLiteral(" 字"));
    ruby_tracking->setToolTip(QStringLiteral("このルビの字と字の間（ルビ 1 字の何字ぶんか。既定 0）"));
    connect(ruby_tracking, &QDoubleSpinBox::editingFinished, this, [this] {
        carry_on("set_ruby", [this] { set_ruby("tracking", ruby_tracking->value() != 0.0 ? Json(ruby_tracking->value()) : Json()); });
    });
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
             {outline, "outline"}, {border, "border"}, {fill, "fill"}, {rotate, "rotate"}, {skew, "skew"},
             {gradient, "gradient"}, {arc, "arc"}, {latin, "latin"}, {mark, "mark"},
             {outline_colour, "outline_colour"}, {double_line, "double"}, {spikes, "spikes"},
             {corner, "corner"}, {squircle, "squircle"}, {sides, "sides"}, {wave, "wave"}, {spike_alt, "spike_alt"}, {dash, "dash"},
             {tail_kind, "tail_kind"}, {tail_fillet, "tail_fillet"},
             {pen_swell, "pen_swell"}, {pen_angle, "pen_angle"}, {pen_taper, "pen_taper"}, {pen_strokes, "pen_strokes"},
             {drip_length, "drip_length"}, {drip_count, "drip_count"}, {drip_width, "drip_width"}, {drip_seed, "drip_seed"}, {points, "points"},
             {tone_density, "tone_density"}, {tone_lpi, "tone_lpi"}, {tone_pattern, "tone_pattern"}, {tone_to, "tone_to"}, {tone_dir, "tone_dir"},
             {fill_blend, "fill_blend"},
             {spike_depth, "spike_depth"}, {line_colour, "line_colour"}, {fill_colour, "fill_colour"},
             {fill_cover, "fill_cover"}, {text_dx, "text_dx"}, {text_dy, "text_dy"}, {tail_width, "tail_width"},
             {path_curve, "path_curve"}, {ruby_scale, "ruby_scale"}, {mono_ruby, "mono_ruby"}, {layer_order, "layer_order"},
             {reset, "reset"}, {ruby_font, "ruby_font"}, {ruby_ratio, "ruby_ratio"}, {ruby_align, "ruby_align"}, {ruby_gap, "ruby_gap"},
             {ruby_tracking, "ruby_tracking"}, {rubies, "rubies"}, {ruby_reassign, "ruby_reassign"}, {ruby_clear, "ruby_clear"}})
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
    form->addRow(QStringLiteral("白フチ"), outline);
    form->addRow(QStringLiteral("フキダシの線"), border);
    form->addRow(QStringLiteral("フキダシの中"), fill);
    form->addRow(QString(), line_colour);  // (the balloon's colours beside its line and inside, not at the bottom)
    form->addRow(QString(), fill_colour);
    form->addRow(QStringLiteral("欧文"), latin);
    form->addRow(QStringLiteral("傍点"), mark);
    form->addRow(QString(), outline_colour);
    form->addRow(QString(), double_line);
    form->addRow(QStringLiteral("破線の長さ"), dash);
    form->addRow(QStringLiteral("線のふるえ"), wave);
    form->addRow(QStringLiteral("角の丸み"), corner);
    form->addRow(QString(), squircle);
    form->addRow(QStringLiteral("角の数"), sides);
    form->addRow(QStringLiteral("トゲの数"), spikes);
    form->addRow(QStringLiteral("トゲの長さ"), spike_depth);
    form->addRow(QStringLiteral("トゲの長短"), spike_alt);
    form->addRow(QStringLiteral("星の先・花びらの数"), points);
    form->addRow(QStringLiteral("線の強弱"), pen_swell);
    form->addRow(QStringLiteral("太い側の向き"), pen_angle);
    form->addRow(QStringLiteral("入り抜き"), pen_taper);
    form->addRow(QStringLiteral("筆の運び"), pen_strokes);
    form->addRow(QStringLiteral("しずくの長さ"), drip_length);
    form->addRow(QStringLiteral("しずくの数"), drip_count);
    form->addRow(QStringLiteral("しずくの太さ"), drip_width);
    form->addRow(QString(), drip_seed);
    form->addRow(QStringLiteral("中のトーン"), tone_density);
    form->addRow(QStringLiteral("トーンの線数"), tone_lpi);
    form->addRow(QStringLiteral("トーンの柄"), tone_pattern);
    form->addRow(QStringLiteral("先の濃さ"), tone_to);
    form->addRow(QStringLiteral("濃くなる向き"), tone_dir);
    form->addRow(QString(), fill_blend);
    form->addRow(QStringLiteral("回転"), rotate);
    form->addRow(QStringLiteral("傾き"), skew);
    form->addRow(QStringLiteral("弓なり"), arc);
    form->addRow(QString(), gradient);
    form->addRow(QStringLiteral("中の塗りの濃さ"), fill_cover);
    auto* shift = new QHBoxLayout;
    shift->addWidget(text_dx);
    shift->addWidget(text_dy);
    form->addRow(QStringLiteral("文字のずれ（横・縦）"), shift);
    form->addRow(QStringLiteral("しっぽの形"), tail_kind);
    form->addRow(QStringLiteral("しっぽの幅"), tail_width);
    form->addRow(QStringLiteral("しっぽの付け根の丸み"), tail_fillet);
    style_form = form;
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
    ruby_box = new QWidget;
    auto* rl = new QVBoxLayout(ruby_box);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(2);
    auto* ruby_heading = new QLabel(QStringLiteral("ルビ（選ぶと下で組み方を変えられます。要確認は振り直すか外す）"));
    theme::hint(ruby_heading);
    rl->addWidget(ruby_heading);
    rl->addWidget(rubies);
    auto* ruby_buttons = new QHBoxLayout;
    ruby_buttons->addWidget(ruby_reassign, 1);
    ruby_buttons->addWidget(ruby_clear, 1);
    rl->addLayout(ruby_buttons);
    ruby_settings = new QWidget;  // (the chosen ruby's own: none chosen, nothing to set)
    auto* rf = new QFormLayout(ruby_settings);
    rf->setContentsMargins(0, 0, 0, 0);
    rf->setSpacing(2);
    rf->addRow(QStringLiteral("書体"), ruby_font);
    rf->addRow(QStringLiteral("大きさ"), ruby_ratio);
    rf->addRow(QStringLiteral("揃え"), ruby_align);
    rf->addRow(QStringLiteral("本文との距離"), ruby_gap);
    rf->addRow(QStringLiteral("字間"), ruby_tracking);
    ruby_settings->setEnabled(false);
    rl->addWidget(ruby_settings);
    ruby_box->setVisible(false);
    layout->addWidget(ruby_box);
    layout->addLayout(row);
    layout->addLayout(buttons);
    auto* more = new QLabel(QStringLiteral("文字・フキダシの設定は左の「オプション」に出ます"));
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
            // (TEXT-01: a line with a ruby that needs review says so first)
            const QString review = core::ruby::review_count(*l) > 0 ? QStringLiteral("【ルビ要確認】") : QString();
            auto* item = new QListWidgetItem(QStringLiteral("%1.%5〔%2〕%3%4").arg(++n).arg(label.section(QStringLiteral("（"), 0, 0), who, words, review));
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
    show_rubies(l);
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
    outline->setValue(f("outline_mm", 0));
    border->setValue(read([&] { return st["border_mm"].is_null() ? 0.35 : core::to_float(st["border_mm"]); }, 0.35));
    fill->setCurrentIndex(std::max(0, fill->findData(qs(core::py_str(st["fill"])))));
    rotate->setValue(f("rotate_deg", 0));
    skew->setValue(f("skew_deg", 0));
    arc->setValue(f("arc", 0));
    gradient->setText(core::py_truthy(st["gradient"]) ? QStringLiteral("文字のグラデーションを外す") : QStringLiteral("文字のグラデーション…"));
    latin->setCurrentIndex(std::max(0, latin->findData(qs(core::py_str(st["latin"])))));
    mark->setCurrentIndex(std::max(0, mark->findData(qs(core::py_str(st["emphasis_mark"])))));
    double_line->setChecked(core::py_truthy(st["double"]));
    spikes->setValue(read([&] { return core::py_truthy(st["spikes"]) ? static_cast<int>(std::clamp<std::int64_t>(core::py_int(st["spikes"]), 0, 80)) : 0; }, 0));
    spike_depth->setValue(f("spike_depth", l->balloon == "star" ? 0.38 : 0.2));  // (fukidashi@2: a star's own depth)
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
    // fukidashi@1: the balloon's shape (the line's own keys: none of them has a default in the style)
    const auto own = [&](const char* key) { return core::get_or(l->style, key, Json()); };
    const auto g = [&own, &read](const char* key, double fallback) {
        return read([&] { return core::py_truthy(own(key)) ? core::to_float(own(key)) : fallback; }, fallback);
    };
    corner->setValue(read([&] { return own("corner_mm").is_number() ? std::clamp(core::to_float(own("corner_mm")), 0.0, 50.0) : corner->minimum(); },
                          corner->minimum()));
    squircle->setChecked(core::py_truthy(own("squircle")));
    sides->setValue(read([&] { return core::py_truthy(own("sides")) ? static_cast<int>(std::clamp<std::int64_t>(core::py_int(own("sides")), 5, 12)) : 8; }, 8));
    wave->setValue(read([&] { return shown_wave(*l); }, 0.0));
    spike_alt->setValue(g("spike_alt", 0));
    dash->setValue(g("dash_mm", 0));
    dash->setSpecialValueText(l->balloon == "whisper" ? QStringLiteral("自動") : QStringLiteral("実線"));  // (a whisper is dashed by itself)
    const Json tails_now = PageCanvas::tails_of(*l);
    const Json first_tail = !tails_now.empty() && tails_now.front().is_object() ? tails_now.front() : Json::object();
    const Json first_kind = core::get_or(first_tail, "kind", Json());
    tail_kind->setCurrentIndex(std::max(0, tail_kind->findData(qs(core::py_truthy(first_kind) ? core::py_str(first_kind) : std::string("wedge")))));
    tail_kind->setEnabled(!tails_now.empty());
    const Json first_fillet = core::get_or(first_tail, "fillet_mm", Json());
    tail_fillet->setValue(read([&] { return first_fillet.is_number() ? std::clamp(core::to_float(first_fillet), 0.0, 20.0) : tail_fillet->minimum(); },
                               tail_fillet->minimum()));
    tail_fillet->setEnabled(!tails_now.empty());
    // fukidashi@2
    const auto number_of = [&](const Json& value, double fallback, double least, double most) {
        return read([&] { return value.is_number() ? std::clamp(core::to_float(value), least, most) : fallback; }, fallback);
    };
    pen_swell->setValue(number_of(own("pen_swell"), 0, 0, 0.9));
    pen_angle->setValue(static_cast<int>(std::lround(number_of(own("pen_angle"), 45, 0, 360))));
    pen_taper->setValue(number_of(own("pen_taper"), 0, 0, 1));
    pen_strokes->setValue(static_cast<int>(number_of(own("pen_strokes"), 2, 1, 8)));
    drip_length->setValue(number_of(own("drip_mm"), 0, 0, 30));
    drip_count->setValue(static_cast<int>(number_of(own("drips"), 0, 0, 24)));
    drip_width->setValue(number_of(own("drip_width_mm"), 0, 0, 10));
    points->setValue(static_cast<int>(number_of(own("points"), 3, 3, 12)));
    {
        const Json tone = own("fill_tone");
        const bool toned = tone.is_object() && tone.contains("density");
        tone_density->setValue(toned ? static_cast<int>(std::lround(100 * number_of(tone["density"], 0, 0, 0.9))) : tone_density->minimum());
        tone_lpi->setValue(toned ? static_cast<int>(std::lround(number_of(core::get_or(tone, "lpi", Json()), 60, 20, 85))) : 60);
        tone_pattern->setCurrentIndex(toned && core::get_or(tone, "pattern", Json()) == Json("line") ? 1 : 0);
        const Json to = toned ? core::get_or(tone, "to", Json()) : Json();
        tone_to->setValue(to.is_number() ? static_cast<int>(std::lround(100 * number_of(to, 0, 0, 0.9))) : tone_to->minimum());
        const double dir = toned ? number_of(core::get_or(tone, "dir", Json()), 90, -360, 360) : 90;
        tone_dir->setValue(static_cast<int>(std::lround(std::fmod(std::fmod(dir, 360.0) + 360.0, 360.0))) % 360);
        fill_blend->setText(own("fill_gradient").is_object() ? QStringLiteral("中の色のグラデーションを外す") : QStringLiteral("中の色のグラデーション…"));
    }
    show_shape_rows(*l);
    shown_.clear();  // (what each of these fields shows now: one left so makes no edit — left_as_shown)
    for (const QDoubleSpinBox* box : {corner, wave, spike_alt, dash, tail_fillet, tail_width, pen_swell, pen_taper, drip_length, drip_width}) shown_[box] = box->value();
    for (const QSpinBox* box : {pen_angle, pen_strokes, drip_count, points}) shown_[box] = box->value();
    shown_[sides] = sides->value();
    shown_[squircle] = squircle->isChecked() ? 1.0 : 0.0;
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
    change["outline_mm"] = or_none(outline->value());
    change["border_mm"] = border->value();
    change["fill"] = data(fill);
    change["rotate_deg"] = or_none(rotate->value());
    change["skew_deg"] = or_none(skew->value());
    change["arc"] = or_none(arc->value());
    change["latin"] = data(latin);
    change["emphasis_mark"] = data(mark);
    change["double"] = on_or_none(double_line->isChecked());
    change["spikes"] = spikes->value() != 0 ? Json(spikes->value()) : Json();
    {
        const core::StoryLine* l = line();
        const double usual = l != nullptr && l->balloon == "star" ? 0.38 : 0.2;  // (fukidashi@2: a star's own depth)
        change["spike_depth"] = std::abs(spike_depth->value() - usual) > 1e-6 ? Json(spike_depth->value()) : Json();
    }
    change["fill_opacity"] = fill_cover->value() < 100 ? Json(fill_cover->value() / 100.0) : Json();
    change["text_dx_mm"] = or_none(text_dx->value());
    change["text_dy_mm"] = or_none(text_dy->value());
    change["path_curve"] = on_or_none(path_curve->isChecked());
    change["ruby_scale"] = std::abs(ruby_scale->value() - 0.5) > 1e-3 ? Json(ruby_scale->value()) : Json();
    change["mono_ruby"] = on_or_none(mono_ruby->isChecked());
    style(change);
}

void StoryPanel::show_ruby_settings() {
    const core::StoryLine* l = line();
    const std::vector<core::RubySpan> spans = l != nullptr ? lettering::rubies_of(*l) : std::vector<core::RubySpan>{};
    const int row = rubies->currentRow();
    const bool chosen = row >= 0 && row < static_cast<int>(spans.size());
    ruby_settings->setEnabled(chosen);
    const core::RubySpan none;
    const core::RubySpan& span = chosen ? spans[static_cast<std::size_t>(row)] : none;
    const QString face = span.font ? qs(*span.font) : QString();
    int at = ruby_font->findData(face);
    if (at < 0) {  // (a font file: shown by its name; fonts@1: one of the added faces by the catalogue's)
        ruby_font->addItem(lettering::face_label(face), face);
        at = ruby_font->findData(face);
    }
    ruby_font->setCurrentIndex(at);
    ruby_ratio->setValue(span.scale.value_or(ruby_ratio->minimum()));
    ruby_align->setCurrentIndex(std::max(0, ruby_align->findData(qs(span.align.value_or("center")))));
    ruby_gap->setValue(span.gap_mm.value_or(0.0));
    ruby_tracking->setValue(span.tracking.value_or(0.0));
}

void StoryPanel::set_ruby(const char* key, const Json& value) {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_) return;
    std::vector<core::RubySpan> spans = lettering::rubies_of(*l);
    const int row = rubies->currentRow();
    if (row < 0 || row >= static_cast<int>(spans.size())) {
        window_->flash(QStringLiteral("先にルビの一覧で組み方を変えるルビを選びます"), 6000);
        return;
    }
    core::RubySpan& span = spans[static_cast<std::size_t>(row)];
    const core::RubySpan was = span;
    try {
        (void)core::ruby::set_setting(span, key, value);
    } catch (const core::OpError& error) {  // (said, as an op's refusal is; the ruby's settings shown again)
        window_->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        show_ruby_settings();
        return;
    }
    if (span == was) return;  // (nothing changed: no edit)
    window_->apply_ops(Json::array({lettering::rubies_op(*l, spans)}));
}

StoryPanel::RubyFace StoryPanel::set_chosen_ruby_font(const QString& key) {
    const core::StoryLine* l = line();
    if (l == nullptr) return RubyFace::NoRuby;
    const std::vector<core::RubySpan> spans = lettering::rubies_of(*l);
    const int row = rubies->currentRow();
    if (row < 0 || row >= static_cast<int>(spans.size())) return RubyFace::NoRuby;
    const std::optional<std::string> wanted = key.isEmpty() ? std::nullopt : std::optional<std::string>(key.toStdString());
    if (spans[static_cast<std::size_t>(row)].font == wanted) return RubyFace::Same;
    const std::size_t before = window_->session().changes_in_memory();
    set_ruby("font", wanted ? Json(*wanted) : Json());
    return window_->session().changes_in_memory() != before ? RubyFace::Done : RubyFace::Refused;
}

void StoryPanel::show_rubies(const core::StoryLine* l) {
    const int row = rubies->currentRow();
    rubies->clear();
    if (l != nullptr) {
        for (const core::RubySpan& span : lettering::rubies_of(*l)) {
            const QString shown = QStringLiteral("%1《%2》").arg(qs(span.base), qs(span.ruby));
            if (span.review.empty()) {
                rubies->addItem(shown);
                continue;
            }
            const QString why = span.review == "unplaced"    ? QStringLiteral("親文字が本文に見つかりません")
                                : span.review == "ambiguous" ? QStringLiteral("どの語に付くか決められません")
                                                             : QStringLiteral("本文の編集が親文字にかかりました");
            auto* item = new QListWidgetItem(QStringLiteral("要確認: %1（%2）").arg(shown, why));
            item->setToolTip(QStringLiteral("元の親文字と読みを残してあります。振り直すか外します（このルビは描かれず、正式な書き出しは止まります）"));
            rubies->addItem(item);
        }
    }
    if (row >= 0 && row < rubies->count()) rubies->setCurrentRow(row);
    ruby_box->setVisible(rubies->count() > 0);
    show_ruby_settings();
}

void StoryPanel::reassign_ruby() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    const std::string id = l->id;
    const int row = rubies->currentRow();
    const std::vector<core::RubySpan> before = lettering::rubies_of(*l);
    if (row < 0 || row >= static_cast<int>(before.size())) {
        window_->flash(QStringLiteral("先にルビの一覧で振り直すルビを選びます"), 6000);
        return;
    }
    const core::RubySpan old = before[static_cast<std::size_t>(row)];
    const QPointer<StoryPanel> alive(this);
    const auto typed = ask::get_text(this, QStringLiteral("ルビを振り直す"), QStringLiteral("親文字（本文の中の語）"), qs(old.base));
    if (alive.isNull() || !typed || lettering::strip(*typed).isEmpty()) return;
    const std::string base = lettering::strip(*typed).toStdString();
    // (the book may have changed while asking: the line as it is now)
    const core::StoryLine* now = line();
    if (now == nullptr || now->id != id) return;
    const std::u32string text = core::ruby::code_points(now->text);
    const std::u32string words = core::ruby::code_points(base);
    const std::vector<std::size_t> b = core::ruby::bounds(text);
    std::vector<std::pair<std::int64_t, std::int64_t>> places;  // where the base is, whole graphemes
    for (std::size_t at = text.find(words); at != std::u32string::npos && !words.empty(); at = text.find(words, at + 1)) {
        const auto g0 = std::lower_bound(b.begin(), b.end(), at);
        const auto g1 = std::lower_bound(b.begin(), b.end(), at + words.size());
        if (g0 != b.end() && *g0 == at && g1 != b.end() && *g1 == at + words.size()) places.emplace_back(g0 - b.begin(), g1 - b.begin());
    }
    if (places.empty()) {
        window_->flash(QStringLiteral("本文に「%1」が見つかりません（親文字は本文の中の語を書きます）").arg(qs(base)), 6000);
        return;
    }
    std::size_t pick = 0;
    if (places.size() > 1) {
        QStringList items;
        for (std::size_t i = 0; i < places.size(); ++i) {
            const std::string before_text = core::ruby::slice(now->text, std::max<std::int64_t>(0, places[i].first - 3), places[i].first);
            const std::string after_text = core::ruby::slice(now->text, places[i].second, places[i].second + 3);
            items << QStringLiteral("%1 つ目: %2［%3］%4").arg(i + 1).arg(qs(before_text), qs(base), qs(after_text));
        }
        const auto chosen = ask::get_item(this, QStringLiteral("ルビを振り直す"), QStringLiteral("どの「%1」に振りますか").arg(qs(base)), items, 0, false);
        if (alive.isNull() || !chosen || !items.contains(*chosen)) return;
        pick = static_cast<std::size_t>(items.indexOf(*chosen));
    }
    const auto reading = ask::get_text(this, QStringLiteral("ルビを振り直す"), QStringLiteral("「%1」の読み").arg(qs(base)), qs(old.ruby));
    if (alive.isNull() || !reading || lettering::strip(*reading).isEmpty()) return;
    const core::StoryLine* again = line();
    if (again == nullptr || again->id != id || again->text != now->text) return;
    std::vector<core::RubySpan> spans = lettering::rubies_of(*again);
    const auto it = std::find(spans.begin(), spans.end(), old);
    if (it == spans.end()) return;  // (changed meanwhile)
    spans.erase(it);
    core::RubySpan fresh;  // (the ruby's own settings kept)
    fresh.start = places[pick].first;
    fresh.end = places[pick].second;
    fresh.ruby = lettering::strip(*reading).toStdString();
    fresh.base = base;
    core::ruby::take_settings(fresh, old);
    spans.push_back(std::move(fresh));
    std::stable_sort(spans.begin(), spans.end(), [](const core::RubySpan& x, const core::RubySpan& y) { return x.start < y.start; });
    window_->apply_ops(Json::array({lettering::rubies_op(*again, spans)}));
}

void StoryPanel::clear_ruby() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    const int row = rubies->currentRow();
    std::vector<core::RubySpan> spans = lettering::rubies_of(*l);
    if (row < 0 || row >= static_cast<int>(spans.size())) {
        window_->flash(QStringLiteral("先にルビの一覧で外すルビを選びます"), 6000);
        return;
    }
    spans.erase(spans.begin() + row);
    window_->apply_ops(Json::array({lettering::rubies_op(*l, spans)}));
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
    // (left as the first tail's was shown: no edit, and tails of different widths are not made one width)
    if (left_as_shown(tail_width, tail_width->value(), 0.005)) return;
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

// --- fukidashi@1: the balloon's shape --------------------------------------------------------------------------------

namespace {

bool kind_in(const std::string& kind, std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&kind](const char* name) { return kind == name; });
}

}  // namespace

const std::vector<std::pair<QString, QString>>& StoryPanel::tail_kinds() {
    static const std::vector<std::pair<QString, QString>> list = {
        {QStringLiteral("wedge"), QStringLiteral("くさび")}, {QStringLiteral("straight"), QStringLiteral("まっすぐ")},
        {QStringLiteral("zigzag"), QStringLiteral("ギザギザ")},        {QStringLiteral("fade"), QStringLiteral("消える")},
        {QStringLiteral("bubbles"), QStringLiteral("泡（心の声）")},   {QStringLiteral("inward"), QStringLiteral("内向き（切れ込み）")},
    };
    return list;
}

double StoryPanel::shown_wave(const core::StoryLine& l) {
    // (what the renderer trembles the outline by: style.wave_mm, or — a line of before — its wobble as that wave)
    const Json wave_mm = core::get_or(l.style, "wave_mm", Json());
    if (core::py_truthy(wave_mm)) return std::clamp(core::to_float(wave_mm), 0.0, 3.0);
    const Json held = core::get_or(l.style, "wobble", Json());
    const double wobble = core::py_truthy(held) ? core::to_float(held) : 0.0;
    if (!(wobble > 0)) return 0.0;
    const double w = l.w_mm.truthy() ? l.w_mm.value() : 40.0;
    const double h = l.h_mm.truthy() ? l.h_mm.value() : 20.0;
    return std::clamp(core::py_round(render::text::balloon::kWobbleWave * std::min(1.0, wobble) * std::min(w, h), 2), 0.0, 3.0);
}

Json StoryPanel::tails_as(const core::StoryLine& l, const std::string& kind) {
    // every tail of the line given the kind. A tail turned inward has its tip moved inside the balloon (a notch is cut
    // from the outline toward its tip), one turned back out has it moved outside again — on the line from the
    // balloon's middle through the tip, so it keeps pointing the same way.
    const double a = (l.w_mm.truthy() ? l.w_mm.value() : 40.0) / 2;
    const double b = (l.h_mm.truthy() ? l.h_mm.value() : 20.0) / 2;
    const double cx = l.x_mm.value() + a;
    const double cy = l.y_mm.value() + b;
    Json out = Json::array();
    for (Json t : PageCanvas::tails_of(l)) {
        const Json was = core::get_or(t, "kind", Json());
        const bool was_inward = was == Json("inward");
        if (kind == "wedge") {
            t.erase("kind");
        } else {
            t["kind"] = kind;
        }
        if ((kind == "inward") != was_inward && t.contains("to") && t["to"].is_array() && t["to"].size() == 2 && t["to"][0].is_number() &&
            t["to"][1].is_number()) {
            double dx = core::to_float(t["to"][0]) - cx;
            double dy = core::to_float(t["to"][1]) - cy;
            if (dx == 0 && dy == 0) dy = 1;  // (a tip on the middle: downward)
            const double gauge = std::sqrt((dx / a) * (dx / a) + (dy / b) * (dy / b));  // (1 on the box's own ellipse)
            const bool inside = gauge < 1;
            if (kind == "inward" && !inside) {
                t["to"] = Json::array({core::py_round(cx + dx / gauge * 0.55, 2), core::py_round(cy + dy / gauge * 0.55, 2)});
                t.erase("via");
                t.erase("vias");
            } else if (kind != "inward" && inside) {
                t["to"] = Json::array({core::py_round(cx + dx / gauge * 1.6, 2), core::py_round(cy + dy / gauge * 1.6, 2)});
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

void StoryPanel::show_shape_rows(const core::StoryLine& l) {
    // each row only for the kinds it changes (render/text/balloons body_of): nothing greyed out that does nothing
    const std::string kind = l.balloon.empty() ? std::string("speech") : l.balloon;
    const bool drawn = l.path.has_value() && !l.path->empty();  // (a balloon drawn by hand: its outline is its own)
    const bool none = kind_in(kind, {"sfx", "none", "picture"});
    const bool squared = kind_in(kind, {"box", "narration", "dotted_box", "tone_box", "fancy_box"});
    // (a dotted box is drawn as dots along its square edge and a fancy box has its marks at its square corners: no
    // rounded corners for either, and no trembling line for the dots)
    style_form->setRowVisible(corner, !drawn && (kind == "rounded" || (squared && kind != "dotted_box" && kind != "fancy_box") || kind == "polygon" ||
                                                 kind_in(kind, {"heart", "star", "diamond"})));
    style_form->setRowVisible(squircle, !drawn && kind == "rounded");
    style_form->setRowVisible(sides, !drawn && kind == "polygon");
    style_form->setRowVisible(wave, !none && (drawn || !kind_in(kind, {"cloud", "shout", "electric", "flash", "dotted_box", "star", "flower"})));
    style_form->setRowVisible(spike_alt, !drawn && kind == "shout");
    style_form->setRowVisible(dash, !none && !kind_in(kind, {"dotted_box", "flash"}));
    // (a narration, a flash and the framed boxes are drawn without tails: render/text/balloons no_tail)
    const bool tailed = !none && !kind_in(kind, {"narration", "flash", "dotted_box", "tone_box", "fancy_box"});
    style_form->setRowVisible(tail_kind, tailed && kind != "thought");  // (a thought's tails are always bubbles)
    style_form->setRowVisible(tail_fillet, tailed);
    corner->setEnabled(!squircle->isChecked() || kind != "rounded");  // (the fuller box has its own corners)
    // fukidashi@2: what the pen, the drips, the points, the tone and the blend change (render/text/balloons: the pen not on
    // a dotted box or a flash; drips not on a balloon drawn by hand, a dotted box or a flash; points on a star and a
    // flower; the tone and the blend wherever there is a balloon)
    const bool lined = !none && !kind_in(kind, {"dotted_box", "flash"});
    style_form->setRowVisible(pen_swell, lined);
    style_form->setRowVisible(pen_angle, lined && pen_swell->value() > 0);
    style_form->setRowVisible(pen_taper, lined);
    style_form->setRowVisible(pen_strokes, lined && pen_taper->value() > 0);
    const bool drips = !drawn && lined;
    style_form->setRowVisible(drip_length, drips);
    for (QWidget* row : std::initializer_list<QWidget*>{drip_count, drip_width, drip_seed}) style_form->setRowVisible(row, drips && drip_length->value() > 0);
    style_form->setRowVisible(points, !drawn && kind_in(kind, {"star", "flower"}));
    style_form->setRowVisible(tone_density, !none);
    for (QWidget* row : std::initializer_list<QWidget*>{tone_lpi, tone_pattern, tone_to}) style_form->setRowVisible(row, !none && tone_density->value() >= 0);
    style_form->setRowVisible(tone_dir, !none && tone_density->value() >= 0 && tone_to->value() >= 0);
    style_form->setRowVisible(fill_blend, !none);
}

bool StoryPanel::left_as_shown(const QWidget* field, double value, double half_step) {
    // a field left as the panel showed it (within half of what it shows a step of): no edit, no step of 取り消し. What a
    // line holds that the field cannot show exactly — more decimals, a value past its range, a default written out —
    // is not rewritten by walking through the field.
    const auto found = shown_.find(field);
    if (found != shown_.end() && std::abs(value - found->second) < half_step) return true;
    shown_[field] = value;
    return false;
}

void StoryPanel::set_shape_key(const QWidget* field, double shown, double half_step, const char* key, const Json& value) {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_ || left_as_shown(field, shown, half_step)) return;
    style(Json{{key, value}});
}

void StoryPanel::set_wave() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_ || left_as_shown(wave, wave->value(), 0.005)) return;
    Json change{{"wave_mm", wave->value() > 0 ? Json(wave->value()) : Json()}};
    if (l->style.contains("wobble")) {
        // a line of before: its wobble was drawn as this trembling; the amount said here takes its place. Its letters
        // keep the ellipse they were fitted in (a line with a wobble was fitted in its box's own ellipse, whatever
        // its hand said: hand false says so from now)
        change["wobble"] = nullptr;
        if (core::is_oval(*l) && core::py_truthy(core::get_or(l->style, "wobble", Json())) && core::get_or(l->style, "hand", Json()) != Json(false)) {
            change["hand"] = false;
        }
    }
    style(change);
}

void StoryPanel::set_tail_kind() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_ || PageCanvas::tails_of(*l).empty()) return;
    const std::string kind = tail_kind->currentData().toString().toStdString();
    const Json tails = tails_as(*l, kind);
    if (tails == PageCanvas::tails_of(*l)) return;
    window_->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", l->id}, {"tails", tails}}}));
}

void StoryPanel::set_tail_fillet() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_) return;
    if (tail_fillet->value() < 0) tail_fillet->setValue(tail_fillet->minimum());  // (below 0: 自動)
    const Json before = PageCanvas::tails_of(*l);
    // (left as the first tail's was shown: no edit — tails that differ from one another stay as they are)
    if (before.empty() || left_as_shown(tail_fillet, tail_fillet->value(), 0.005)) return;
    Json tails = Json::array();
    for (Json t : before) {
        if (!t.is_object()) continue;
        if (tail_fillet->value() < 0) {
            t.erase("fillet_mm");
        } else {
            t["fillet_mm"] = tail_fillet->value();
        }
        tails.push_back(std::move(t));
    }
    if (tails == before) return;
    window_->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", l->id}, {"tails", tails}}}));
}

void StoryPanel::set_drip_length() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_ || left_as_shown(drip_length, drip_length->value(), 0.05)) return;
    const Json value = drip_length->value() > 0 ? Json(drip_length->value()) : Json();
    Json after = l->style.is_object() ? l->style : Json::object();
    if (value.is_null()) {
        after.erase("drip_mm");
    } else {
        after["drip_mm"] = value;
    }
    // (the box grows by as much as the drips do, its top kept: the body — the box less the drips — stays the size it was)
    const std::string kind = l->balloon.empty() ? std::string("speech") : l->balloon;
    const double grown = lettering::drip_height(kind, after) - lettering::drip_height(kind, l->style);
    Json ops = Json::array({Json{{"op", "edit_line"}, {"id", l->id}, {"style", Json{{"drip_mm", value}}}}});
    if (std::abs(grown) > 1e-9 && !(l->path && !l->path->empty())) {
        const double h = (l->h_mm.truthy() ? l->h_mm.value() : 20.0) + grown;
        if (h > 1) ops.push_back(Json{{"op", "move_line"}, {"id", l->id}, {"h_mm", core::py_round(h, 2)}});
    }
    window_->apply_ops(ops);
}

void StoryPanel::next_drip_arrangement() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_) return;
    const Json now = core::get_or(l->style, "drip_seed", Json());
    const std::int64_t seed = now.is_number() ? static_cast<std::int64_t>(std::clamp(core::to_float(now), 0.0, 999999.0)) : 0;
    style(Json{{"drip_seed", (seed + 1) % 1000000}});
}

void StoryPanel::set_tone() {
    const core::StoryLine* l = line();
    if (l == nullptr || loading_) return;
    const Json was = core::get_or(l->style, "fill_tone", Json());
    Json tone;  // (null: no tone)
    if (tone_density->value() >= 0) {
        tone = Json{{"density", tone_density->value() / 100.0}, {"lpi", static_cast<double>(tone_lpi->value())},
                    {"angle", was.is_object() && core::get_or(was, "angle", Json()).is_number() ? core::get_or(was, "angle", Json()) : Json(45.0)},
                    {"pattern", tone_pattern->currentData().toString().toStdString()}};
        if (tone_to->value() >= 0) {
            tone["to"] = tone_to->value() / 100.0;
            tone["dir"] = static_cast<double>(tone_dir->value());
            if (was.is_object() && core::py_truthy(core::get_or(was, "radial", Json()))) tone["radial"] = true;
        }
        if (was.is_object() && core::get_or(was, "inset_mm", Json()).is_number()) tone["inset_mm"] = core::get_or(was, "inset_mm", Json());
    }
    if (tone == was || (tone.is_null() && !l->style.contains("fill_tone"))) {
        show_shape_rows(*l);  // (the rows that go with a tone, as the fields say)
        return;
    }
    style(Json{{"fill_tone", tone}});
}

void StoryPanel::pick_blend() {
    const core::StoryLine* l = line();
    if (l == nullptr) return;
    if (core::get_or(l->style, "fill_gradient", Json()).is_object()) {
        style(Json{{"fill_gradient", nullptr}});
        return;
    }
    const auto to = ask::colour(this, QColor(150, 200, 255), QStringLiteral("中の色のグラデーションの先の色（元の色はフキダシの中の色）"));
    if (!to) return;
    style(Json{{"fill_gradient", Json{{"to", Json::array({to->red(), to->green(), to->blue()})}, {"dir", 90.0}}}});
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
    lettering::put_ruby(op, marks, nullptr, false);
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
    lettering::put_ruby(ops[0], marks, l, true);  // (TEXT-01: by place when Python's runs would not say where)
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
