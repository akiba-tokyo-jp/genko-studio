// PC5 G-d: 文字・段落・文字スタイル・段落スタイル・字形 panels (app/type_panels.hpp). Upstream: PhotoCraft v0.2.0's
// ui-egui/src/type_panels_ui.rs (the styles' and the glyphs' panels' behaviour) and text/src/glyphs.rs (the glyphs'
// categories), MIT — native/third_party/photocraft/NOTICE.md.

#include "app/type_panels.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/view_units.hpp"
#include "core/prefs_table.hpp"
#include "core/pyconv.hpp"
#include "core/ruby.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "render/draw.hpp"
#include "render/text/lettering.hpp"
#include "render/text/opentype.hpp"
#include "render/text/shaping.hpp"
#include "service/briefs.hpp"

namespace genko::app {

using core::Json;
namespace ts = core::type_styles;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const Json& at(const Json& o, const char* key) {
    static const Json none;
    if (!o.is_object()) return none;
    const auto it = o.find(key);
    return it == o.end() ? none : *it;
}

double num(const Json& v, double fallback) { return v.is_number() ? v.get<double>() : fallback; }

QToolButton* toggle(const QString& text, const QString& tip, const QString& name) {
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setObjectName(name);
    b->setAutoRaise(true);
    return b;
}

// The units of 文字の単位 and this page's.
struct Lengths {
    core::units::Unit unit;
    core::units::Basis basis;
};
Lengths lengths(MainWindow* window) { return Lengths{view_units::type_unit(), view_units::basis(window->current_page(), true)}; }

// The range the window says is chosen, as the styles take it.
std::optional<ts::Range> range_of(const MainWindow* window) {
    const auto r = window->type_range();
    if (!r) return std::nullopt;
    return ts::Range{r->first, r->second};
}

// The record's characters at the range chosen (its start: the attributes there), and its paragraph's.
struct Shown {
    const core::StoryLine* record = nullptr;
    Json chars = Json::object();
    Json para = Json::object();
    std::optional<ts::Range> range;
};
Shown shown(MainWindow* window) {
    Shown out;
    const std::optional<Json> target = window->type_target(false);
    if (!target) return out;
    out.record = window->type_record(*target);
    if (out.record == nullptr) return out;
    out.range = range_of(window);
    const Json* styles = window->book().type_styles ? &*window->book().type_styles : nullptr;
    try {
        const std::int64_t at_letter = out.range ? out.range->start : 0;
        for (const core::type_text::CharRun& run : core::type_text::resolve_chars(*out.record, styles)) {
            if (run.start <= at_letter && (at_letter < run.end || run.end == run.start)) {
                out.chars = run.attrs;
                break;
            }
            out.chars = run.attrs;
        }
        const std::vector<std::int64_t> paragraphs = ts::paragraphs_of(out.record->text, at_letter, at_letter);
        out.para = core::type_text::resolve_para(*out.record, styles, paragraphs.empty() ? 0 : paragraphs.front());
    } catch (const std::exception&) {
    }
    return out;
}

// The fonts of the computer (service::briefs::type_fonts) as the lists take them, read once a run.
struct FontFace {
    QString family;
    QString style;
    QString path;
    int index = 0;
    QString postscript;
};
const std::vector<FontFace>& system_faces(std::map<QString, QString>* japanese = nullptr) {
    static std::vector<FontFace> faces;
    static std::map<QString, QString> names;  // (family → its Japanese name)
    static bool read = false;
    if (!read) {
        read = true;
        try {
            for (const Json& family : service::briefs::type_fonts()) {
                const QString name = qs(family.value("family", std::string()));
                if (family.contains("family_ja") && family["family_ja"].is_string()) names[name] = qs(family["family_ja"].get<std::string>());
                for (const Json& face : family.value("styles", Json::array())) {
                    FontFace f;
                    f.family = name;
                    f.style = face.value("style", Json()).is_string() ? qs(face["style"].get<std::string>()) : QStringLiteral("Regular");
                    f.path = qs(face.value("path", std::string()));
                    f.index = face.value("index", Json(0)).is_number_integer() ? face["index"].get<int>() : 0;
                    f.postscript = face.value("postscript", Json()).is_string() ? qs(face["postscript"].get<std::string>()) : QString();
                    faces.push_back(std::move(f));
                }
            }
        } catch (const std::exception&) {
        }
    }
    if (japanese != nullptr) *japanese = names;
    return faces;
}

// The family a font file (and face) is in, as the list knows it.
const FontFace* face_of_path(const QString& path, int index) {
    for (const FontFace& f : system_faces()) {
        if (f.path == path && f.index == index) return &f;
    }
    return nullptr;
}

// The preview's pixel size for 書式 → フォントプレビューのサイズ (0: none).
int preview_px(const QString& size) {
    if (size == QLatin1String("small")) return 10;
    if (size == QLatin1String("medium")) return 13;
    if (size == QLatin1String("large")) return 17;
    if (size == QLatin1String("extraLarge")) return 22;
    if (size == QLatin1String("huge")) return 28;
    return 0;
}

// A Qt family that draws a font file (added to the application once), or "" when it cannot.
QString qt_family(const QString& font) {
    static std::map<QString, QString> added;
    if (const auto it = added.find(font); it != added.end()) return it->second;
    QString family;
    try {
        const auto bytes = render::font_bytes(font.toStdString());
        const int id = QFontDatabase::addApplicationFontFromData(*bytes);
        if (id >= 0) {
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) family = families.front();
        }
    } catch (const std::exception&) {
    }
    added.emplace(font, family);
    return family;
}

}  // namespace

void FontBox::showPopup() {
    emit aboutToShowPopup();
    QComboBox::showPopup();
}

// --- 文字 ------------------------------------------------------------------------------------------------------------------

CharacterPanel::CharacterPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("character_panel"));
    title = new QLabel;
    title->setWordWrap(true);
    theme::hint(title);
    family = new FontBox;
    family->setObjectName(QStringLiteral("type_family"));
    family->setToolTip(QStringLiteral("フォントファミリー（▼ を開くとパソコンの書体も並びます）"));
    for (const auto& [key, label] : lettering::bundled_fonts()) family->addItem(label, key);
    connect(family, &FontBox::aboutToShowPopup, this, [this] { fill_fonts(); });
    connect(family, &QComboBox::activated, this, [this](int) { family_chosen(); });
    style = new QComboBox;
    style->setObjectName(QStringLiteral("type_font_style"));
    style->setToolTip(QStringLiteral("フォントスタイル"));
    connect(style, &QComboBox::activated, this, [this](int) { style_chosen(); });
    const auto length_box = [](const QString& name, const QString& tip, const QString& special = {}) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(name);
        box->setToolTip(tip);
        box->setKeyboardTracking(false);
        if (!special.isEmpty()) box->setSpecialValueText(special);
        return box;
    };
    size = length_box(QStringLiteral("type_size"), QStringLiteral("フォントサイズ（自動: 台詞はフキダシに合わせる）"), QStringLiteral("自動"));
    leading = length_box(QStringLiteral("type_leading"), QStringLiteral("行送り（自動: 文字の大きさの 120%）"), QStringLiteral("自動"));
    baseline = length_box(QStringLiteral("type_baseline"), QStringLiteral("ベースラインシフト（上へ）"));
    line_leading = new QDoubleSpinBox;
    line_leading->setObjectName(QStringLiteral("type_line_leading"));
    line_leading->setRange(-0.5, 3);
    line_leading->setSingleStep(0.05);
    line_leading->setDecimals(2);
    line_leading->setKeyboardTracking(false);
    line_leading->setToolTip(QStringLiteral("行間（台詞の書式: 行と行の間に足す、文字の大きさに対する割合）"));
    kerning = new QComboBox;
    kerning->setObjectName(QStringLiteral("type_kerning"));
    kerning->setToolTip(QStringLiteral("カーニング"));
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>>{{"metrics", "メトリクス"}, {"optical", "オプティカル"}, {"none", "0"}})
        kerning->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    tracking = new QSpinBox;
    tracking->setObjectName(QStringLiteral("type_tracking"));
    tracking->setRange(-1000, 10000);
    tracking->setSingleStep(10);
    tracking->setKeyboardTracking(false);
    tracking->setToolTip(QStringLiteral("トラッキング（1/1000 em）"));
    const auto percent = [](const QString& name, const QString& tip) {
        auto* box = new QSpinBox;
        box->setObjectName(name);
        box->setRange(1, 1000);
        box->setSuffix(QStringLiteral(" %"));
        box->setKeyboardTracking(false);
        box->setToolTip(tip);
        return box;
    };
    scale_y = percent(QStringLiteral("type_scale_y"), QStringLiteral("垂直比率"));
    scale_x = percent(QStringLiteral("type_scale_x"), QStringLiteral("水平比率（長体・平体）"));
    color = new QPushButton(QStringLiteral("カラー…"));
    color->setObjectName(QStringLiteral("type_color"));
    color->setToolTip(QStringLiteral("文字のカラー"));
    connect(color, &QPushButton::clicked, this, [this] { pick_color(); });
    weight = new QComboBox;
    weight->setObjectName(QStringLiteral("type_weight"));
    weight->setToolTip(QStringLiteral("疑似ボールド（線を太らせる）"));
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>>{{"normal", "疑似ボールドなし"}, {"bold", "疑似ボールド"}, {"heavy", "疑似ボールド（極太）"}})
        weight->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    italic = toggle(QStringLiteral("T̸"), QStringLiteral("疑似斜体"), QStringLiteral("type_italic"));
    italic->setText(QStringLiteral("斜"));
    all_caps = toggle(QStringLiteral("TT"), QStringLiteral("オールキャップス"), QStringLiteral("type_all_caps"));
    small_caps = toggle(QStringLiteral("Tᴛ"), QStringLiteral("スモールキャップス"), QStringLiteral("type_small_caps"));
    superscript = toggle(QStringLiteral("T¹"), QStringLiteral("上付き文字"), QStringLiteral("type_superscript"));
    subscript = toggle(QStringLiteral("T₁"), QStringLiteral("下付き文字"), QStringLiteral("type_subscript"));
    underline = toggle(QStringLiteral("T̲"), QStringLiteral("下線"), QStringLiteral("type_underline"));
    strikethrough = toggle(QStringLiteral("T̶"), QStringLiteral("打ち消し線"), QStringLiteral("type_strikethrough"));
    for (const auto& [tag, label, tip] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
             {"liga", "fi", "標準合字"}, {"dlig", "st", "任意の合字"}, {"swsh", "𝒜", "スワッシュ字形"}, {"salt", "aa", "デザインのバリエーション"},
             {"titl", "T", "タイトル用字形"}, {"ornm", "❦", "飾り文字"}, {"ordn", "1st", "序数表記"}, {"frac", "½", "分数字"}}) {
        QToolButton* b = toggle(QString::fromUtf8(label), QString::fromUtf8(tip), QStringLiteral("type_ot_") + QString::fromLatin1(tag));
        features.push_back(b);
        const std::string feature = tag;
        connect(b, &QToolButton::clicked, this, [this, feature](bool on) {
            if (loading_) return;
            if (feature == "liga") {
                set(Json{{"ligatures", on}});
                return;
            }
            // (the features as they are at the range, this one on or off)
            Json now = shown(window_).chars.value("features", Json::object());
            if (!now.is_object()) now = Json::object();
            if (on) now[feature] = 1;
            else now.erase(feature);
            set(Json{{"features", now}});
        });
    }
    language = new QComboBox;
    language->setObjectName(QStringLiteral("type_language"));
    language->setToolTip(QStringLiteral("言語（ハイフネーション・字形の選び方）"));
    for (const auto& [tag, label] : std::initializer_list<std::pair<const char*, const char*>>{
             {"ja", "日本語"}, {"en-US", "英語：アメリカ"}, {"en-GB", "英語：イギリス"}, {"de", "ドイツ語"}, {"fr", "フランス語"}, {"es", "スペイン語"},
             {"it", "イタリア語"}, {"ko", "韓国語"}, {"zh-Hans", "中国語（簡体字）"}, {"zh-Hant", "中国語（繁体字）"}, {"ru", "ロシア語"}, {"ar", "アラビア語"},
             {"he", "ヘブライ語"}, {"hi", "ヒンディー語"}, {"th", "タイ語"}})
        language->addItem(QString::fromUtf8(label), QString::fromLatin1(tag));
    antialias = new QComboBox;
    antialias->setObjectName(QStringLiteral("type_antialias"));
    antialias->setToolTip(QStringLiteral("アンチエイリアスの種類（テキストレイヤー・台詞全体）"));
    for (const auto& [key, label] : antialias_names()) antialias->addItem(label, key);
    tcy = new QCheckBox(QStringLiteral("縦中横"));
    tcy->setObjectName(QStringLiteral("type_tcy"));
    tcy->setToolTip(QStringLiteral("縦書きで、選んだ文字（全体なら 2〜3 桁の数字）を横に並べて 1 字に収めます"));
    proportional = new QCheckBox(QStringLiteral("プロポーショナルメトリクス"));
    proportional->setObjectName(QStringLiteral("type_proportional"));

    // changes: each field its attribute (and, a line of before on its whole, its style's key)
    connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (loading_) return;
        const Lengths l = lengths(window_);
        const double mm = core::units::to_mm(v, l.unit, l.basis);
        set(Json{{"size_mm", v > 0 ? Json(mm) : Json()}}, Json{{"size_mm", v > 0 ? Json(mm) : Json()}});
    });
    connect(leading, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (loading_) return;
        const Lengths l = lengths(window_);
        set(Json{{"leading_mm", v > 0 ? Json(core::units::to_mm(v, l.unit, l.basis)) : Json("auto")}});
    });
    connect(baseline, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (loading_) return;
        const Lengths l = lengths(window_);
        set(Json{{"baseline_shift_mm", core::units::to_mm(v, l.unit, l.basis)}});
    });
    connect(line_leading, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (loading_) return;
        const std::optional<Json> target = window_->type_target(true);
        const core::StoryLine* record = target ? window_->type_record(*target) : nullptr;
        if (record != nullptr) window_->type_style_key(Json{{"leading", v}});
    });
    connect(kerning, &QComboBox::activated, this, [this](int) { set(Json{{"kerning", kerning->currentData().toString().toStdString()}}); });
    connect(tracking, &QSpinBox::valueChanged, this, [this](int v) {
        if (!loading_) set(Json{{"tracking", v / 1000.0}}, Json{{"tracking", v / 1000.0}});
    });
    connect(scale_y, &QSpinBox::valueChanged, this, [this](int v) {
        if (!loading_) set(Json{{"scale_y", v / 100.0}});
    });
    connect(scale_x, &QSpinBox::valueChanged, this, [this](int v) {
        if (!loading_) set(Json{{"scale_x", v / 100.0}}, Json{{"scale_x", v / 100.0}});
    });
    connect(weight, &QComboBox::activated, this, [this](int) {
        const QString w = weight->currentData().toString();
        set(Json{{"faux_bold", w.toStdString()}}, Json{{"weight", w == QLatin1String("normal") ? Json() : Json(w.toStdString())}, {"bold", nullptr}});
    });
    connect(italic, &QToolButton::clicked, this, [this](bool on) { set(Json{{"faux_italic", on}}, Json{{"italic", on ? Json(true) : Json()}}); });
    connect(all_caps, &QToolButton::clicked, this, [this](bool on) { set(Json{{"caps", on ? "all" : "normal"}}); });
    connect(small_caps, &QToolButton::clicked, this, [this](bool on) { set(Json{{"caps", on ? "small" : "normal"}}); });
    connect(superscript, &QToolButton::clicked, this, [this](bool on) { script(true, on); });
    connect(subscript, &QToolButton::clicked, this, [this](bool on) { script(false, on); });
    connect(underline, &QToolButton::clicked, this, [this](bool on) { set(Json{{"underline", on}}); });
    connect(strikethrough, &QToolButton::clicked, this, [this](bool on) { set(Json{{"strikethrough", on}}); });
    connect(language, &QComboBox::activated, this, [this](int) { set(Json{{"language", language->currentData().toString().toStdString()}}); });
    connect(antialias, &QComboBox::activated, this, [this](int) {
        if (!loading_) window_->type_command(Json{{"antialias", antialias->currentData().toString().toStdString()}});
    });
    connect(tcy, &QCheckBox::clicked, this, [this](bool on) { set(Json{{"tcy", on}}, Json{{"tcy", on}}); });
    connect(proportional, &QCheckBox::clicked, this, [this](bool on) { set(Json{{"proportional", on}}); });

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    form->addRow(QStringLiteral("フォント"), family);
    form->addRow(QStringLiteral("スタイル"), style);
    form->addRow(QStringLiteral("サイズ"), size);
    form->addRow(QStringLiteral("行送り"), leading);
    form->addRow(QStringLiteral("カーニング"), kerning);
    form->addRow(QStringLiteral("トラッキング"), tracking);
    form->addRow(QStringLiteral("垂直比率"), scale_y);
    form->addRow(QStringLiteral("水平比率"), scale_x);
    form->addRow(QStringLiteral("ベースラインシフト"), baseline);
    form->addRow(QStringLiteral("カラー"), color);
    form->addRow(QStringLiteral("疑似ボールド"), weight);
    auto* toggles = new QHBoxLayout;
    toggles->setSpacing(1);
    for (QToolButton* b : {italic, all_caps, small_caps, superscript, subscript, underline, strikethrough}) toggles->addWidget(b);
    toggles->addStretch(1);
    auto* opentype = new QHBoxLayout;
    opentype->setSpacing(1);
    for (QToolButton* b : features) opentype->addWidget(b);
    opentype->addStretch(1);
    form->addRow(QString(), toggles);
    form->addRow(QStringLiteral("OpenType"), opentype);
    form->addRow(QStringLiteral("言語"), language);
    form->addRow(QStringLiteral("アンチエイリアス"), antialias);
    form->addRow(QString(), tcy);
    form->addRow(QString(), proportional);
    form->addRow(QStringLiteral("行間"), line_leading);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(title);
    layout->addLayout(form);
    layout->addStretch(1);
    refresh();
}

void CharacterPanel::fill_fonts() {
    // (the computer's families after the bundled ones — the fonts used last first —, each shown in its own face as
    // 書式 → フォントプレビューのサイズ says; their names in Japanese unless 環境設定 → テキスト says English)
    const QString chosen = family->currentData().toString();
    const QSignalBlocker quiet(family);
    std::map<QString, QString> japanese;
    const std::vector<FontFace>& faces = system_faces(&japanese);
    const auto store = settings();
    const bool english = core::prefs::value(*store, "type.showFontNamesInEnglish") == Json(true);
    const int px = preview_px(window_->font_preview());
    family->clear();
    const auto add = [&](const QString& label, const QString& data, const QString& font_file) {
        family->addItem(label, data);
        if (px > 0 && !font_file.isEmpty()) {
            const QString qt = qt_family(font_file);
            if (!qt.isEmpty()) {
                QFont f(qt);
                f.setPixelSize(px);
                family->setItemData(family->count() - 1, f, Qt::FontRole);
            }
        }
    };
    // (the fonts used last, as many as 環境設定 → テキスト says)
    const int recent = static_cast<int>(num(core::prefs::value(*store, "type.recentFonts"), 10));
    const QStringList used = store->value(QStringLiteral("type/recent_fonts_used")).toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    std::set<QString> listed;
    for (const QString& data : used.mid(0, recent)) {
        if (data.startsWith(QLatin1String("family:"))) {
            const QString name = data.mid(7);
            const auto it = std::find_if(faces.begin(), faces.end(), [&](const FontFace& f) { return f.family == name; });
            if (it == faces.end()) continue;
            add((english || !japanese.contains(name) ? name : japanese[name]) + QStringLiteral("（最近）"), data, it->path);
            listed.insert(data);
        }
    }
    if (!listed.empty()) family->insertSeparator(family->count());
    for (const auto& [key, label] : lettering::bundled_fonts()) add(label, key, key == QLatin1String("antique") ? QStringLiteral("mincho") : key);
    family->insertSeparator(family->count());
    std::set<QString> seen;
    for (const FontFace& f : faces) {
        if (!seen.insert(f.family).second) continue;
        add(english || !japanese.contains(f.family) ? f.family : japanese[f.family], QStringLiteral("family:") + f.family, f.path);
    }
    fonts_read_ = true;
    const int again = family->findData(chosen);
    if (again >= 0) family->setCurrentIndex(again);
}

void CharacterPanel::refresh() {
    const Shown s = shown(window_);
    setEnabled(s.record != nullptr);
    loading_ = true;
    const QSignalBlocker q1(size), q2(leading), q3(baseline), q4(tracking), q5(scale_y), q6(scale_x), q7(line_leading);
    title->setText(s.record == nullptr ? QStringLiteral("テキストレイヤーか台詞を選ぶと、その文字の設定が出ます")
                   : s.range         ? QStringLiteral("選んだ文字（%1〜%2 字目）").arg(s.range->start + 1).arg(s.range->end)
                                     : QStringLiteral("文字全体"));
    const Lengths l = lengths(window_);
    const QString suffix = QStringLiteral(" ") + qs(core::units::suffix_of(l.unit));
    const int decimals = std::max(1, core::units::decimals_of(l.unit));
    for (QDoubleSpinBox* box : {size, leading, baseline}) {
        box->setSuffix(suffix);
        box->setDecimals(decimals);
    }
    size->setRange(0, core::units::from_mm(1000, l.unit, l.basis));
    leading->setRange(0, core::units::from_mm(1000, l.unit, l.basis));
    baseline->setRange(-core::units::from_mm(1000, l.unit, l.basis), core::units::from_mm(1000, l.unit, l.basis));
    const Json& c = s.chars;
    // (the font: a bundled key or a file — the family it is in, as the list knows it)
    const Json& font = at(c, "font");
    QString data = font.is_string() ? qs(font.get<std::string>()) : (s.record != nullptr && s.record->balloon == "sfx" ? QStringLiteral("sfx") : QStringLiteral("antique"));
    const int face = static_cast<int>(num(at(c, "face_index"), 0));
    style->clear();
    if (const FontFace* f = font.is_string() ? face_of_path(data, face) : nullptr) {
        if (!fonts_read_) fill_fonts();
        data = QStringLiteral("family:") + f->family;
        for (const FontFace& other : system_faces()) {
            if (other.family != f->family) continue;
            style->addItem(other.style, other.path);
            style->setItemData(style->count() - 1, other.index, Qt::UserRole + 1);
            if (other.path == f->path && other.index == f->index) style->setCurrentIndex(style->count() - 1);
        }
    } else {
        style->addItem(at(c, "font_style").is_string() ? qs(at(c, "font_style").get<std::string>()) : QStringLiteral("Regular"), data);
    }
    int index = family->findData(data);
    if (index < 0) {
        family->addItem(QFileInfo(data).completeBaseName(), data);
        index = family->count() - 1;
    }
    {
        const QSignalBlocker quiet(family);
        family->setCurrentIndex(index);
    }
    size->setValue(core::units::from_mm(num(at(c, "size_mm"), 0), l.unit, l.basis));
    leading->setValue(at(c, "leading_mm").is_number() ? core::units::from_mm(at(c, "leading_mm").get<double>(), l.unit, l.basis) : 0);
    baseline->setValue(core::units::from_mm(num(at(c, "baseline_shift_mm"), 0), l.unit, l.basis));
    kerning->setCurrentIndex(std::max(0, kerning->findData(at(c, "kerning").is_string() ? qs(at(c, "kerning").get<std::string>()) : QStringLiteral("metrics"))));
    tracking->setValue(static_cast<int>(std::lround(num(at(c, "tracking"), 0) * 1000)));
    scale_y->setValue(static_cast<int>(std::lround(num(at(c, "scale_y"), 1) * 100)));
    scale_x->setValue(static_cast<int>(std::lround(num(at(c, "scale_x"), 1) * 100)));
    const Json& bold = at(c, "faux_bold");
    weight->setCurrentIndex(std::max(0, weight->findData(bold.is_string() ? qs(bold.get<std::string>()) : QStringLiteral("normal"))));
    italic->setChecked(at(c, "faux_italic") == Json(true));
    all_caps->setChecked(at(c, "caps") == Json("all"));
    small_caps->setChecked(at(c, "caps") == Json("small"));
    const double shift = num(at(c, "baseline_shift_mm"), 0), scale = num(at(c, "scale"), 1);
    superscript->setChecked(scale < 0.99 && shift > 0);
    subscript->setChecked(scale < 0.99 && shift < 0);
    underline->setChecked(at(c, "underline") == Json(true));
    strikethrough->setChecked(at(c, "strikethrough") == Json(true));
    const Json& own = at(c, "features");
    for (QToolButton* b : features) {
        const std::string tag = b->objectName().mid(8).toStdString();
        b->setChecked(tag == "liga" ? at(c, "ligatures") != Json(false) : own.is_object() && own.value(tag, Json(0)).is_number_integer() && own[tag].get<int>() > 0);
    }
    language->setCurrentIndex(std::max(0, language->findData(at(c, "language").is_string() ? qs(at(c, "language").get<std::string>()) : QStringLiteral("ja"))));
    const std::string method = s.record != nullptr && s.record->native_type && s.record->native_type->contains("antialias")
                                   ? (*s.record->native_type)["antialias"].get<std::string>()
                                   : std::string("smooth");
    antialias->setCurrentIndex(std::max(0, antialias->findData(qs(method))));
    // (縦中横: a range's own — a range of the text set in one cell —, else the whole text's own, else its style's: 2 or 3 digits)
    bool across = s.record != nullptr && core::py_truthy(at(render::text::style_of(*s.record), "tcy"));
    if (s.record != nullptr && s.record->native_type && s.record->native_type->is_object()) {
        const Json& nt = *s.record->native_type;
        if (at(at(nt, "char"), "tcy").is_boolean()) across = at(at(nt, "char"), "tcy").get<bool>();
        if (s.range) {
            across = false;
            if (at(nt, "runs").is_array()) {
                for (const Json& run : nt["runs"]) {
                    if (num(at(run, "start"), 0) <= s.range->start && s.range->start < num(at(run, "end"), 0) && at(at(run, "char"), "tcy") == Json(true)) across = true;
                }
            }
        }
    }
    tcy->setChecked(across);
    proportional->setChecked(at(c, "proportional") == Json(true));
    proportional->setVisible(window_->type_language_features() == QLatin1String("east_asian"));
    line_leading->setValue(s.record != nullptr ? num(at(render::text::style_of(*s.record), "leading"), 0) : 0);  // (its style as drawn: the default too)
    loading_ = false;
}

void CharacterPanel::set(const Json& attrs, const Json& style_keys) {
    if (loading_) return;
    window_->type_attrs(attrs, style_keys);
}

void CharacterPanel::pick_color() {
    const Shown s = shown(window_);
    if (s.record == nullptr) return;
    const Json& rgb = at(s.chars, "rgb");
    const QColor now = rgb.is_array() && rgb.size() == 3 ? QColor(rgb[0].get<int>(), rgb[1].get<int>(), rgb[2].get<int>()) : QColor(10, 10, 10);
    const auto chosen = ask::colour(this, now, QStringLiteral("文字のカラー"));
    if (!chosen) return;
    const Json value = Json::array({chosen->red(), chosen->green(), chosen->blue()});
    set(Json{{"rgb", value}}, Json{{"rgb", value}});
}

void CharacterPanel::script(bool super, bool on) {
    // (上付き・下付き: Photoshop's — the size at 58 % and the baseline moved by a third of the size up, a sixth down)
    const Shown s = shown(window_);
    if (s.record == nullptr) return;
    const double size_mm = num(at(s.chars, "size_mm"), num(at(s.record->style, "size_mm"), 5.0));
    if (!on) {
        set(Json{{"scale", nullptr}, {"baseline_shift_mm", nullptr}});
        return;
    }
    set(Json{{"scale", 0.58}, {"baseline_shift_mm", core::py_round(super ? size_mm * 0.33 : -size_mm * 0.17, 4)}});
}

void CharacterPanel::family_chosen() {
    const QString data = family->currentData().toString();
    if (data.startsWith(QLatin1String("family:"))) {
        const QString name = data.mid(7);
        // (its Regular — else its first style — as Photoshop picks one)
        const FontFace* pick = nullptr;
        for (const FontFace& f : system_faces()) {
            if (f.family != name) continue;
            if (pick == nullptr || f.style == QLatin1String("Regular")) pick = &f;
            if (f.style == QLatin1String("Regular")) break;
        }
        if (pick == nullptr) return;
        window_->remember_font(data);
        Json attrs{{"font", pick->path.toStdString()}, {"face_index", pick->index > 0 ? Json(pick->index) : Json()}, {"family", pick->family.toStdString()},
                   {"font_style", pick->style.toStdString()}, {"postscript", pick->postscript.isEmpty() ? Json() : Json(pick->postscript.toStdString())}};
        set(attrs, pick->index > 0 ? Json() : Json{{"font", pick->path.toStdString()}});
        return;
    }
    set(Json{{"font", data.toStdString()}, {"face_index", nullptr}, {"family", nullptr}, {"font_style", nullptr}, {"postscript", nullptr}},
        Json{{"font", data == QLatin1String("antique") ? Json() : Json(data.toStdString())}});
}

void CharacterPanel::style_chosen() {
    const QString path = style->currentData().toString();
    const int index = style->currentData(Qt::UserRole + 1).toInt();
    const FontFace* f = face_of_path(path, index);
    if (f == nullptr) return;
    Json attrs{{"font", f->path.toStdString()}, {"face_index", f->index > 0 ? Json(f->index) : Json()}, {"family", f->family.toStdString()},
               {"font_style", f->style.toStdString()}, {"postscript", f->postscript.isEmpty() ? Json() : Json(f->postscript.toStdString())}};
    set(attrs, f->index > 0 ? Json() : Json{{"font", f->path.toStdString()}});
}

// --- 段落 ------------------------------------------------------------------------------------------------------------------

ParagraphPanel::ParagraphPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("paragraph_panel"));
    title = new QLabel;
    title->setWordWrap(true);
    theme::hint(title);
    auto* align_row = new QHBoxLayout;
    align_row->setSpacing(1);
    for (const auto& [value, label, tip] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
             {"left", "⫷", "左揃え（縦書きは上揃え）"}, {"center", "≡", "中央揃え"}, {"right", "⫸", "右揃え（縦書きは下揃え）"},
             {"justify", "☰", "均等配置（最終行左揃え）"}, {"justify_center", "☷", "均等配置（最終行中央揃え）"},
             {"justify_right", "☶", "均等配置（最終行右揃え）"}, {"justify_all", "▤", "両端揃え"}}) {
        QToolButton* b = toggle(QString::fromUtf8(label), QString::fromUtf8(tip), QStringLiteral("type_align_") + QString::fromLatin1(value));
        aligns.push_back(b);
        align_row->addWidget(b);
        const std::string v = value;
        connect(b, &QToolButton::clicked, this, [this, v] {
            // (a line of before keeps its style's alignment — left, center, right, justify —, down: top, center, bottom)
            const std::optional<Json> target = window_->type_target(false);
            const core::StoryLine* record = target ? window_->type_record(*target) : nullptr;
            std::string legacy = v;
            if (record != nullptr && record->wrap == "vertical") legacy = v == "left" ? "top" : v == "right" ? "bottom" : v == "justify_all" ? "justify" : v;
            const bool known = legacy == "left" || legacy == "center" || legacy == "right" || legacy == "top" || legacy == "bottom" || legacy == "justify";
            set(Json{{"align", v}}, known ? Json{{"align", legacy}} : Json());
        });
    }
    align_row->addStretch(1);
    const auto length_box = [](const QString& name, const QString& tip) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(name);
        box->setToolTip(tip);
        box->setKeyboardTracking(false);
        return box;
    };
    start_indent = length_box(QStringLiteral("type_start_indent"), QStringLiteral("左インデント（縦書きは上）"));
    end_indent = length_box(QStringLiteral("type_end_indent"), QStringLiteral("右インデント（縦書きは下）"));
    first_indent = length_box(QStringLiteral("type_first_indent"), QStringLiteral("1 行目左インデント"));
    space_before = length_box(QStringLiteral("type_space_before"), QStringLiteral("段落前のアキ"));
    space_after = length_box(QStringLiteral("type_space_after"), QStringLiteral("段落後のアキ"));
    for (const auto& [box, attr] : std::initializer_list<std::pair<QDoubleSpinBox*, const char*>>{
             {start_indent, "start_indent_mm"}, {end_indent, "end_indent_mm"}, {first_indent, "first_indent_mm"}, {space_before, "space_before_mm"},
             {space_after, "space_after_mm"}}) {
        const std::string key = attr;
        QDoubleSpinBox* b = box;
        connect(b, &QDoubleSpinBox::valueChanged, this, [this, key, b](double v) {
            if (loading_) return;
            const Lengths l = lengths(window_);
            (void)b;
            set(Json{{key, core::py_round(core::units::to_mm(v, l.unit, l.basis), 4)}});
        });
    }
    kinsoku = new QComboBox;
    kinsoku->setObjectName(QStringLiteral("type_kinsoku"));
    kinsoku->setToolTip(QStringLiteral("禁則処理"));
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>>{{"genko", "Genko の禁則"}, {"none", "なし"}, {"weak", "弱い禁則"}, {"strong", "強い禁則"}})
        kinsoku->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    connect(kinsoku, &QComboBox::activated, this, [this](int) { set(Json{{"kinsoku", kinsoku->currentData().toString().toStdString()}}); });
    yakumono = new QCheckBox(QStringLiteral("約物を詰める（文字組み）"));
    yakumono->setObjectName(QStringLiteral("type_yakumono"));
    connect(yakumono, &QCheckBox::clicked, this, [this](bool on) { set(Json{{"yakumono", on}}, Json{{"yakumono", on}}); });
    hyphenate = new QCheckBox(QStringLiteral("ハイフネーション"));
    hyphenate->setObjectName(QStringLiteral("type_hyphenate"));
    hyphenate->setToolTip(QStringLiteral("覚えるだけで、まだ語を分けません"));
    connect(hyphenate, &QCheckBox::clicked, this, [this](bool on) { set(Json{{"hyphenate", on}}); });
    direction = new QComboBox;
    direction->setObjectName(QStringLiteral("type_direction"));
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>>{{"auto", "最初の文字に従う"}, {"ltr", "左から右"}, {"rtl", "右から左"}})
        direction->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    connect(direction, &QComboBox::activated, this, [this](int) { set(Json{{"direction", direction->currentData().toString().toStdString()}}); });
    composer = new QComboBox;
    composer->setObjectName(QStringLiteral("type_composer"));
    for (const auto& [value, label] : std::initializer_list<std::pair<const char*, const char*>>{{"japanese", "日本語の組み方"}, {"world", "中東および南アジア言語用 Composer"}})
        composer->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    connect(composer, &QComboBox::activated, this, [this](int) { set(Json{{"composer", composer->currentData().toString().toStdString()}}); });
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    form->addRow(QStringLiteral("揃え"), align_row);
    form->addRow(QStringLiteral("左インデント"), start_indent);
    form->addRow(QStringLiteral("右インデント"), end_indent);
    form->addRow(QStringLiteral("1 行目"), first_indent);
    form->addRow(QStringLiteral("段落前のアキ"), space_before);
    form->addRow(QStringLiteral("段落後のアキ"), space_after);
    form->addRow(QStringLiteral("禁則処理"), kinsoku);
    form->addRow(QString(), yakumono);
    form->addRow(QString(), hyphenate);
    form->addRow(QStringLiteral("方向"), direction);
    form->addRow(QStringLiteral("組み方"), composer);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(title);
    layout->addLayout(form);
    layout->addStretch(1);
    refresh();
}

void ParagraphPanel::refresh() {
    const Shown s = shown(window_);
    setEnabled(s.record != nullptr);
    loading_ = true;
    title->setText(s.record == nullptr ? QStringLiteral("テキストレイヤーか台詞を選ぶと、その段落の設定が出ます")
                   : s.range         ? QStringLiteral("選んだ文字のある段落")
                                     : QStringLiteral("段落すべて"));
    const Lengths l = lengths(window_);
    const QString suffix = QStringLiteral(" ") + qs(core::units::suffix_of(l.unit));
    const double top = core::units::from_mm(1000, l.unit, l.basis);
    const Json& p = s.para;
    for (const auto& [box, attr] : std::initializer_list<std::pair<QDoubleSpinBox*, const char*>>{
             {start_indent, "start_indent_mm"}, {end_indent, "end_indent_mm"}, {first_indent, "first_indent_mm"}, {space_before, "space_before_mm"},
             {space_after, "space_after_mm"}}) {
        const QSignalBlocker quiet(box);
        box->setSuffix(suffix);
        box->setDecimals(std::max(1, core::units::decimals_of(l.unit)));
        box->setRange(std::string_view(attr).starts_with("space") ? 0.0 : -top, top);
        box->setValue(core::units::from_mm(num(at(p, attr), 0), l.unit, l.basis));
    }
    // (the alignment: a line's style's top and bottom are left and right here)
    std::string align = at(p, "align").is_string() ? at(p, "align").get<std::string>() : std::string();
    if (align.empty()) align = s.record != nullptr && render::text::shape_of(*s.record).empty() ? "center" : "left";
    if (align == "top" || align == "start") align = "left";
    if (align == "bottom") align = "right";
    for (QToolButton* b : aligns) b->setChecked(b->objectName().mid(11).toStdString() == align);
    kinsoku->setCurrentIndex(std::max(0, kinsoku->findData(at(p, "kinsoku").is_string() ? qs(at(p, "kinsoku").get<std::string>()) : QStringLiteral("genko"))));
    yakumono->setChecked(at(p, "yakumono") != Json(false));
    hyphenate->setChecked(at(p, "hyphenate") == Json(true));
    direction->setCurrentIndex(std::max(0, direction->findData(at(p, "direction").is_string() ? qs(at(p, "direction").get<std::string>()) : QStringLiteral("auto"))));
    direction->setEnabled(s.record != nullptr && s.record->wrap != "vertical");
    composer->setCurrentIndex(std::max(0, composer->findData(at(p, "composer").is_string() ? qs(at(p, "composer").get<std::string>()) : QStringLiteral("japanese"))));
    const bool middle = window_->type_language_features() == QLatin1String("middle_eastern");
    direction->setVisible(middle);
    composer->setVisible(middle);
    if (auto* form = qobject_cast<QFormLayout*>(layout()->itemAt(1)->layout())) {
        for (QWidget* w : {static_cast<QWidget*>(direction), static_cast<QWidget*>(composer)}) {
            if (QWidget* label = form->labelForField(w)) label->setVisible(middle);
        }
    }
    loading_ = false;
}

void ParagraphPanel::set(const Json& attrs, const Json& style_keys) {
    if (loading_) return;
    window_->type_attrs(attrs, style_keys);
}

// --- 文字スタイル・段落スタイル --------------------------------------------------------------------------------------------

TypeStylesPanel::TypeStylesPanel(MainWindow* window, bool paragraph) : window_(window), paragraph_(paragraph) {
    setObjectName(paragraph ? QStringLiteral("paragraph_styles_panel") : QStringLiteral("character_styles_panel"));
    list = new QListWidget;
    list->setObjectName(paragraph ? QStringLiteral("type_paragraph_styles") : QStringLiteral("type_character_styles"));
    connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        apply_row(item, (QApplication::keyboardModifiers() & Qt::AltModifier) != 0);
    });
    connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { options(); });
    const auto button = [this](const QString& text, const QString& tip, const QString& name) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        b->setAutoRaise(true);
        (void)this;
        return b;
    };
    const QString what = paragraph ? QStringLiteral("段落スタイル") : QStringLiteral("文字スタイル");
    clear_button = button(QStringLiteral("⌫"), QStringLiteral("上書きを消去"), QStringLiteral("type_style_clear"));
    redefine_button = button(QStringLiteral("✓"), QStringLiteral("選択範囲で%1を再定義").arg(what), QStringLiteral("type_style_redefine"));
    new_button = button(QStringLiteral("＋"), QStringLiteral("新規%1を作成").arg(what), QStringLiteral("type_style_new"));
    delete_button = button(QStringLiteral("🗑"), QStringLiteral("%1を削除").arg(what), QStringLiteral("type_style_delete"));
    menu_button = button(QStringLiteral("≡"), QStringLiteral("パネルメニュー"), QStringLiteral("type_style_menu"));
    connect(clear_button, &QToolButton::clicked, this, [this] { clear_override(); });
    connect(redefine_button, &QToolButton::clicked, this, [this] { redefine(); });
    connect(new_button, &QToolButton::clicked, this, [this] { new_style(); });
    connect(delete_button, &QToolButton::clicked, this, [this] { remove(); });
    auto* menu = new QMenu(menu_button);
    menu->addAction(QStringLiteral("新規%1…").arg(what), this, [this] { new_style(); });
    menu->addAction(QStringLiteral("スタイルを複製"), this, [this] { duplicate(); });
    menu->addAction(QStringLiteral("スタイルを削除"), this, [this] { remove(); });
    menu->addAction(QStringLiteral("スタイル名を変更…"), this, [this] {
        const auto name = ask::get_text(this, QStringLiteral("スタイル名を変更"), QStringLiteral("新しい名前"), list->currentItem() != nullptr ? list->currentItem()->data(Qt::UserRole + 1).toString() : QString());
        if (name) rename(*name);
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("スタイルを再定義"), this, [this] { redefine(); });
    menu->addAction(QStringLiteral("上書きを消去"), this, [this] { clear_override(); });
    menu->addAction(QStringLiteral("スタイルオプション…"), this, [this] { options(); });
    menu_button->setMenu(menu);
    menu_button->setPopupMode(QToolButton::InstantPopup);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(1);
    buttons->addWidget(menu_button);
    buttons->addStretch(1);
    for (QToolButton* b : {clear_button, redefine_button, new_button, delete_button}) buttons->addWidget(b);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(list, 1);
    layout->addLayout(buttons);
    refresh();
}

Json TypeStylesPanel::range_args() const {
    const std::optional<ts::Range> range = range_of(window_);
    if (!range) return Json::object();
    return Json{{"range", Json::array({range->start, range->end})}};
}

void TypeStylesPanel::refresh() {
    loading_ = true;
    const QSignalBlocker quiet(list);
    const std::optional<std::string> was = chosen();
    const bool had = list->currentItem() != nullptr;
    list->clear();
    const core::Document& doc = window_->book();
    const ts::Kind k = paragraph_ ? ts::Kind::Paragraph : ts::Kind::Character;
    Json styles = Json::array();
    try {
        styles = ts::list(doc, k);
    } catch (const std::exception&) {
    }
    // (the one the chosen text takes: ▶; its overrides: +)
    Json current = Json::object();
    const std::optional<Json> target = window_->type_target(false);
    const core::StoryLine* record = target ? window_->type_record(*target) : nullptr;
    if (record != nullptr) {
        try {
            current = ts::current(*record, doc.type_styles ? &*doc.type_styles : nullptr, range_of(window_));
        } catch (const std::exception&) {
        }
    }
    const std::string key = paragraph_ ? "paragraph" : "character";
    const Json takes = current.value(key, Json());
    const bool override = current.value(key + "_override", Json(false)) == Json(true);
    const bool mixed = current.value(key + "_mixed", Json(false)) == Json(true);
    for (const Json& s : styles) {
        const Json id = s.value("id", Json());
        const QString name = qs(s.value("name", std::string()));
        const bool on = record != nullptr && !mixed && takes == id;
        auto* item = new QListWidgetItem(QStringLiteral("%1%2%3").arg(on ? QStringLiteral("▶ ") : QStringLiteral("　 "), name, on && override ? QStringLiteral("+") : QString()));
        item->setData(Qt::UserRole, id.is_string() ? qs(id.get<std::string>()) : QString());
        item->setData(Qt::UserRole + 1, name);
        item->setToolTip(QStringLiteral("%1（%2 か所で使用）").arg(name).arg(s.value("texts", Json(0)).get<std::int64_t>()));
        list->addItem(item);
        if (had && was && ((id.is_string() && id.get<std::string>() == *was) || (id.is_null() && was->empty()))) list->setCurrentItem(item);
    }
    const bool editable = record != nullptr;
    for (QToolButton* b : {clear_button, redefine_button}) b->setEnabled(editable);
    loading_ = false;
}

std::optional<std::string> TypeStylesPanel::chosen() const {
    const QListWidgetItem* item = list->currentItem();
    if (item == nullptr) return std::nullopt;
    return item->data(Qt::UserRole).toString().toStdString();
}

void TypeStylesPanel::apply_row(QListWidgetItem* item, bool clear_overrides) {
    if (item == nullptr || loading_) return;
    list->setCurrentItem(item);
    Json args = range_args();
    const QString id = item->data(Qt::UserRole).toString();
    if (!id.isEmpty()) args["id"] = id.toStdString();
    if (clear_overrides) args["clear_overrides"] = true;
    window_->type_style(kind(), "apply", args, true);
    window_->refresh_type_panels();
}

void TypeStylesPanel::new_style() {
    // (from the text chosen, as Photoshop's; none chosen: an empty style)
    const std::optional<Json> target = window_->type_target(false);
    Json args = range_args();
    args["from_selection"] = target.has_value();
    const auto made = window_->type_style(kind(), "new", args, target.has_value());
    if (made && made->contains("id")) {
        window_->refresh_type_panels();
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString().toStdString() == (*made)["id"].get<std::string>()) list->setCurrentRow(i);
        }
    }
}

void TypeStylesPanel::redefine() {
    Json args = range_args();
    if (const auto id = chosen(); id && !id->empty() && *id != ts::kBasic) args["id"] = *id;
    if (paragraph_ && chosen() && *chosen() == ts::kBasic) args["id"] = std::string(ts::kBasic);
    window_->type_style(kind(), "redefine", args, true);
    window_->refresh_type_panels();
}

void TypeStylesPanel::clear_override() {
    window_->type_style(kind(), "clear_override", range_args(), true);
    window_->refresh_type_panels();
}

void TypeStylesPanel::remove() {
    const auto id = chosen();
    if (!id || id->empty() || *id == ts::kBasic) {
        window_->flash(QStringLiteral("削除するスタイルを一覧で選びます（[なし]・[基本段落] は削除できません）"), 4000);
        return;
    }
    window_->type_style(kind(), "delete", Json{{"id", *id}}, false);
    window_->refresh_type_panels();
}

void TypeStylesPanel::duplicate() {
    const auto id = chosen();
    if (!id || id->empty() || *id == ts::kBasic) return;
    window_->type_style(kind(), "duplicate", Json{{"id", *id}}, false);
    window_->refresh_type_panels();
}

void TypeStylesPanel::rename(const QString& name) {
    const auto id = chosen();
    if (!id || id->empty() || *id == ts::kBasic) return;
    window_->type_style(kind(), "rename", Json{{"id", *id}, {"name", name.toStdString()}}, false);
    window_->refresh_type_panels();
}

void TypeStylesPanel::options() {
    // スタイルオプション…: the style's attributes as Photoshop's dialog sets them — here the 文字 and 段落 panels' attributes of
    // the text chosen laid on it (再定義 from the text), or its name
    const auto id = chosen();
    if (!id) return;
    const auto name = ask::get_text(this, QStringLiteral("スタイルオプション"), QStringLiteral("スタイル名"), list->currentItem()->data(Qt::UserRole + 1).toString());
    if (!name || id->empty() || *id == ts::kBasic) return;
    if (*name != list->currentItem()->data(Qt::UserRole + 1).toString()) rename(*name);
}

// --- 字形 ------------------------------------------------------------------------------------------------------------------

namespace {

// 表示 (upstream glyphs.rs's categories, then the Japanese ones)
struct Category {
    const char* key;
    const char* label;
};
constexpr Category kCategories[] = {
    {"all", "フォント全体"},          {"basic_latin", "ラテン（基本）"},  {"latin1", "ラテン 1"},       {"latin_a", "ラテン拡張 A"},
    {"latin_b", "ラテン拡張 B"},      {"greek", "ギリシャ文字"},         {"cyrillic", "キリル文字"},    {"punctuation", "句読点"},
    {"numbers", "数字"},              {"currency", "通貨記号"},          {"symbols", "記号"},           {"math", "数学記号"},
    {"arrows", "矢印"},               {"ornaments", "飾り文字"},         {"hiragana", "ひらがな"},      {"katakana", "カタカナ"},
    {"cjk_symbols", "和文の記号・約物"}, {"kanji", "漢字"},              {"fullwidth", "全角英数"},
};

bool in_category(char32_t c, std::string_view category) {
    const auto u = static_cast<std::uint32_t>(c);
    const auto in = [u](std::uint32_t a, std::uint32_t b) { return u >= a && u <= b; };
    const auto any = [u](std::initializer_list<std::uint32_t> list) { return std::find(list.begin(), list.end(), u) != list.end(); };
    if (category == "basic_latin") return in(0x20, 0x7E);
    if (category == "latin1") return in(0xA0, 0xFF);
    if (category == "latin_a") return in(0x100, 0x17F);
    if (category == "latin_b") return in(0x180, 0x24F);
    if (category == "greek") return in(0x370, 0x3FF) || in(0x1F00, 0x1FFF);
    if (category == "cyrillic") return in(0x400, 0x52F);
    if (category == "punctuation") {
        const bool ascii = (in(0x21, 0x2F) || in(0x3A, 0x40) || in(0x5B, 0x60) || in(0x7B, 0x7E)) && !any({'$', '+', '<', '=', '>', '^', '`', '|', '~'});
        return ascii || any({0xA1, 0xA7, 0xAB, 0xB6, 0xB7, 0xBB, 0xBF}) || in(0x2010, 0x2027) || in(0x2030, 0x205E) || in(0x2E00, 0x2E7F) || in(0x3000, 0x303F);
    }
    if (category == "numbers") return QChar::isNumber(c) || in(0x2150, 0x218F);
    if (category == "currency") return any({'$', 0xA2, 0xA3, 0xA4, 0xA5}) || in(0x20A0, 0x20CF);
    if (category == "symbols")
        return any({0xA9, 0xAE, 0xB0, 0xA6, 0xA8, 0xAF, 0xB4, 0xB8, 0x2122, 0x2120, 0x2116, 0x212E, '^', '`', '|', '~'}) || in(0x2100, 0x214F) ||
               in(0x2300, 0x23FF) || in(0x2500, 0x25FF);
    if (category == "math")
        return any({'+', '<', '=', '>', 0xB1, 0xD7, 0xF7, 0xAC, 0xB5, 0xB9, 0xB2, 0xB3, 0xBC, 0xBD, 0xBE}) || in(0x2070, 0x209F) || in(0x2200, 0x22FF) ||
               in(0x27C0, 0x27EF) || in(0x2980, 0x2AFF);
    if (category == "arrows") return in(0x2190, 0x21FF) || in(0x27F0, 0x27FF) || in(0x2900, 0x297F) || in(0x2B00, 0x2BFF);
    if (category == "ornaments") return in(0x2600, 0x27BF) || in(0x1F300, 0x1FAFF) || in(0xE000, 0xF8FF);
    if (category == "hiragana") return in(0x3040, 0x309F);
    if (category == "katakana") return in(0x30A0, 0x30FF) || in(0x31F0, 0x31FF) || in(0xFF65, 0xFF9F);
    if (category == "cjk_symbols") return in(0x3000, 0x303F) || in(0xFE30, 0xFE4F) || in(0x3200, 0x33FF);
    if (category == "kanji") return in(0x4E00, 0x9FFF) || in(0x3400, 0x4DBF) || in(0x20000, 0x3FFFF) || in(0xF900, 0xFAFF);
    if (category == "fullwidth") return in(0xFF01, 0xFF5E);
    return true;
}

QString text_of(char32_t c) { return QString::fromUcs4(&c, 1); }

}  // namespace

GlyphsPanel::GlyphsPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("glyphs_panel"));
    font = new QComboBox;
    font->setObjectName(QStringLiteral("glyphs_font"));
    font->setToolTip(QStringLiteral("字形を見る書体（選んだ文字の書体が出ます）"));
    for (const auto& [key, label] : lettering::bundled_fonts()) {
        if (key != QLatin1String("antique")) font->addItem(label, key);
    }
    connect(font, &QComboBox::activated, this, [this](int) { show_font(font->currentData().toString(), 0); });
    category = new QComboBox;
    category->setObjectName(QStringLiteral("glyphs_category"));
    for (const Category& c : kCategories) category->addItem(QString::fromUtf8(c.label), QString::fromLatin1(c.key));
    connect(category, &QComboBox::activated, this, [this](int) { fill(); });
    const auto grid = [](const QString& name) {
        auto* g = new QListWidget;
        g->setObjectName(name);
        g->setViewMode(QListView::IconMode);
        g->setResizeMode(QListView::Adjust);
        g->setMovement(QListView::Static);
        g->setGridSize(QSize(34, 34));
        g->setUniformItemSizes(true);
        g->setFocusPolicy(Qt::NoFocus);  // (a double click inserts while the editor keeps the keys)
        return g;
    };
    recent = grid(QStringLiteral("glyphs_recent"));
    recent->setMaximumHeight(40);
    recent->setFlow(QListView::LeftToRight);
    recent->setWrapping(false);
    glyphs = grid(QStringLiteral("glyphs_grid"));
    variants = grid(QStringLiteral("glyphs_variants"));
    variants->setMaximumHeight(40);
    variants->setToolTip(QStringLiteral("異体字（選んだ字の、書体が持つ異体字セレクター付きの字形）"));
    for (QListWidget* g : {recent, glyphs, variants}) {
        connect(g, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { insert(item->data(Qt::UserRole).toString()); });
    }
    connect(glyphs, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) { chosen(item); });
    auto* top = new QHBoxLayout;
    top->addWidget(font, 1);
    top->addWidget(category, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(new QLabel(QStringLiteral("最近使用した字形")));
    layout->addWidget(recent);
    layout->addLayout(top);
    layout->addWidget(glyphs, 1);
    layout->addWidget(new QLabel(QStringLiteral("異体字")));
    layout->addWidget(variants);
    refresh();
}

void GlyphsPanel::refresh() {
    // the recent glyphs (this computer's), and the font of the chosen text at its caret
    recent->clear();
    const QStringList used = settings()->value(QStringLiteral("type/recent_glyphs")).toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    for (const QString& g : used) {
        auto* item = new QListWidgetItem(g);
        item->setData(Qt::UserRole, g);
        item->setTextAlignment(Qt::AlignCenter);
        recent->addItem(item);
    }
    const Shown s = shown(window_);
    QString chosen_font = font_.isEmpty() ? QStringLiteral("mincho") : font_;
    int face = face_;
    if (s.record != nullptr) {
        const Json& f = at(s.chars, "font");
        chosen_font = f.is_string() ? qs(f.get<std::string>()) : (s.record->balloon == "sfx" ? QStringLiteral("sfx") : QStringLiteral("mincho"));
        if (chosen_font == QLatin1String("antique")) chosen_font = QStringLiteral("mincho");  // (アンチック: its kana's 明朝)
        face = static_cast<int>(num(at(s.chars, "face_index"), 0));
    }
    if (chosen_font != font_ || face != face_ || glyphs->count() == 0) show_font(chosen_font, face);
}

void GlyphsPanel::show_font(const QString& file, int face_index) {
    font_ = file;
    face_ = face_index;
    int at_font = font->findData(file);
    if (at_font < 0) {
        font->addItem(QFileInfo(file).completeBaseName(), file);
        at_font = font->count() - 1;
    }
    font->setCurrentIndex(at_font);
    mapped_.clear();
    try {
        mapped_ = render::text::charmap(file.toStdString(), static_cast<unsigned>(face_index));
    } catch (const std::exception&) {
    }
    family_ = qt_family(file);
    fill();
}

void GlyphsPanel::fill() {
    glyphs->clear();
    variants->clear();
    const std::string which = category->currentData().toString().toStdString();
    QFont shown_in = family_.isEmpty() ? QFont() : QFont(family_);
    shown_in.setPixelSize(20);
    int n = 0;
    for (const char32_t c : mapped_) {
        if (!in_category(c, which)) continue;
        auto* item = new QListWidgetItem(text_of(c));
        item->setData(Qt::UserRole, text_of(c));
        item->setFont(shown_in);
        item->setTextAlignment(Qt::AlignCenter);
        item->setToolTip(QStringLiteral("U+%1").arg(static_cast<std::uint32_t>(c), 4, 16, QLatin1Char('0')).toUpper());
        glyphs->addItem(item);
        if (++n >= 30000) break;  // (a huge font: the first thirty thousand)
    }
}

void GlyphsPanel::chosen(QListWidgetItem* item) {
    variants->clear();
    if (item == nullptr) return;
    const QString text = item->data(Qt::UserRole).toString();
    const QList<uint> ucs = text.toUcs4();
    if (ucs.size() != 1) return;
    std::vector<char32_t> selectors;
    try {
        selectors = render::text::variants_of(font_.toStdString(), static_cast<char32_t>(ucs.front()), static_cast<unsigned>(face_));
    } catch (const std::exception&) {
    }
    QFont shown_in = family_.isEmpty() ? QFont() : QFont(family_);
    shown_in.setPixelSize(20);
    for (const char32_t vs : selectors) {
        const char32_t pair[2] = {static_cast<char32_t>(ucs.front()), vs};
        const QString seq = QString::fromUcs4(pair, 2);
        auto* v = new QListWidgetItem(seq);
        v->setData(Qt::UserRole, seq);
        v->setFont(shown_in);
        v->setTextAlignment(Qt::AlignCenter);
        v->setToolTip(QStringLiteral("U+%1 U+%2").arg(ucs.front(), 4, 16, QLatin1Char('0')).arg(static_cast<std::uint32_t>(vs), 4, 16, QLatin1Char('0')).toUpper());
        variants->addItem(v);
    }
}

void GlyphsPanel::insert(const QString& glyph) {
    if (glyph.isEmpty()) return;
    window_->insert_glyph(glyph);
    // (the recent ones: this one first, 20 at most)
    QStringList used = settings()->value(QStringLiteral("type/recent_glyphs")).toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    used.removeAll(glyph);
    used.prepend(glyph);
    settings()->setValue(QStringLiteral("type/recent_glyphs"), used.mid(0, 20).join(QLatin1Char('\t')));
    refresh();
}

}  // namespace genko::app
