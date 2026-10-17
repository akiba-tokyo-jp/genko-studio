#include "app/book_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <set>

#include "app/ask.hpp"
#include "app/lettering.hpp"
#include "app/look.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/covers.hpp"
#include "core/ink.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "render/ink.hpp"

namespace genko::app {

using core::Json;

namespace {

// nombre.POSITIONS with nombre.LABELS
const std::vector<std::pair<const char*, const char*>>& nombre_positions() {
    static const std::vector<std::pair<const char*, const char*>> list = {
        {"bottom_center", "下の真ん中"}, {"bottom_outside", "下の外側"}, {"top_outside", "上の外側"}, {"side_outside", "外側の真ん中"}};
    return list;
}

// covers.KINDS with covers.LABELS
const std::vector<std::pair<const char*, const char*>>& cover_kinds() {
    static const std::vector<std::pair<const char*, const char*>> list = {
        {"front", "表紙"}, {"back", "裏表紙"}, {"jacket", "カバー（表紙・背・裏表紙・袖）"}, {"obi", "帯（表・背・裏・袖、低い紙）"}};
    return list;
}

// QComboBox.findData(value) as PySide does it for a value of the book: a str is found among the keys; anything else is
// no key (-1), and max(0, …) chooses the first.
int index_of(const QComboBox* box, const Json& value) {
    return std::max(0, value.is_string() ? box->findData(QString::fromStdString(value.get<std::string>())) : -1);
}

// Python's QFormLayout(dialog), its buttons the last row: here the rows in a body that scrolls when the screen is small
// (1024 × 640 at 200 % is 512 × 320), and the buttons under it, always in sight (SPEC UX-02); the dialog as large as its
// rows, within the screen.
void lay_out(QDialog* dialog, QFormLayout* form, QDialogButtonBox* buttons) {
    auto* rows = new QWidget;
    form->setContentsMargins(0, 0, 0, 0);
    rows->setLayout(form);
    auto* body = new QScrollArea;
    body->setFrameShape(QFrame::NoFrame);
    body->setWidgetResizable(true);
    body->setWidget(rows);
    auto* outer = new QVBoxLayout(dialog);
    outer->addWidget(body, 1);
    outer->addWidget(buttons);
    const QMargins m = outer->contentsMargins();
    const QSize inside = rows->sizeHint();
    const QSize wanted(std::max(inside.width() + body->verticalScrollBar()->sizeHint().width(), buttons->sizeHint().width()) + m.left() + m.right(),
                       inside.height() + outer->spacing() + buttons->sizeHint().height() + m.top() + m.bottom());
    look::fit_to_screen(dialog, wanted);
}

// Python's str.isspace() (and so what str.strip() and int() take away): the Unicode white space and the separators
// \x1c–\x1f.
bool py_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

QString py_strip(const QString& text) {
    qsizetype a = 0;
    qsizetype b = text.size();
    while (a < b && py_space(text[a].unicode())) ++a;
    while (b > a && py_space(text[b - 1].unicode())) --b;
    return text.mid(a, b - a);
}

// Python's int(text): spaces around it, a sign, decimal digits of any script with single underscores between them. Its
// value (held at kFar past it: Python's int has no bound, and no page is numbered so), or nothing where Python raises
// ValueError.
constexpr std::int64_t kFar = 1'000'000'000'000'000;

std::optional<std::int64_t> py_int_text(const QString& text) {
    const QList<uint> chars = py_strip(text).toUcs4();
    qsizetype i = 0;
    bool negative = false;
    if (i < chars.size() && (chars[i] == '+' || chars[i] == '-')) {
        negative = chars[i] == '-';
        ++i;
    }
    std::int64_t value = 0;
    bool digit = false;  // (the last character was a digit)
    for (; i < chars.size(); ++i) {
        const char32_t c = chars[i];
        if (c == '_') {
            if (!digit || i + 1 == chars.size()) return std::nullopt;
            digit = false;
            continue;
        }
        const int d = QChar::category(c) == QChar::Number_DecimalDigit ? QChar::digitValue(c) : -1;
        if (d < 0) return std::nullopt;
        value = value > kFar ? value : value * 10 + d;
        digit = true;
    }
    if (!digit) return std::nullopt;
    return negative ? -value : value;
}

}  // namespace

void lay_out_dialog(QDialog* dialog, QFormLayout* form, QDialogButtonBox* buttons) { lay_out(dialog, form, buttons); }

// --- ノンブルの設定 ---------------------------------------------------------------------------------------------------

NombreDialog::NombreDialog(QWidget* parent, const core::Document& book) : QDialog(parent) {
    setObjectName(QStringLiteral("nombre_dialog"));
    // nombre.settings(episode): nombre.DEFAULTS under the book's own
    Json cfg = Json::object({{"position", "bottom_center"}, {"font", "gothic"}, {"size_mm", 3.0}, {"start", 1}, {"hidden", false},
                             {"hidden_size_mm", 2.0}, {"show", true}});
    if (book.nombre.is_object()) {
        for (const auto& [key, value] : book.nombre.items()) cfg[key] = value;
    }
    // (Python's float() and int() of them, which fail before its dialog opens; and PySide's QSpinBox.setValue, which
    // takes no number past a C int)
    const double size_value = core::py_float(cfg["size_mm"]);
    const std::int64_t first = core::py_int(cfg["start"]);
    if (first < INT_MIN || first > INT_MAX) throw core::Error("value", "start is too large for a spin box");
    const double hidden_value = core::py_float(cfg["hidden_size_mm"]);
    setWindowTitle(QStringLiteral("ノンブルの設定"));
    shown = new QCheckBox(QStringLiteral("ノンブルを入れる"));
    shown->setObjectName(QStringLiteral("nombre_show"));
    shown->setChecked(core::py_truthy(cfg["show"]));
    position = new QComboBox;
    position->setObjectName(QStringLiteral("nombre_position"));
    for (const auto& [key, label] : nombre_positions()) position->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    position->setCurrentIndex(index_of(position, cfg["position"]));
    face = new QComboBox;
    face->setObjectName(QStringLiteral("nombre_font"));
    for (const auto& [key, label] : lettering::bundled_fonts()) face->addItem(label, key);
    face->setCurrentIndex(index_of(face, cfg["font"]));
    size_mm = new QDoubleSpinBox;
    size_mm->setObjectName(QStringLiteral("nombre_size"));
    size_mm->setRange(1, 20);
    size_mm->setSingleStep(0.5);
    size_mm->setSuffix(QStringLiteral(" mm"));
    size_mm->setValue(size_value);
    start = new QSpinBox;
    start->setObjectName(QStringLiteral("nombre_start"));
    start->setRange(0, 9999);
    start->setValue(static_cast<int>(first));
    start->setToolTip(QStringLiteral("1 ページ目の番号（前の話から続けるとき）"));
    hidden = new QCheckBox(QStringLiteral("隠しノンブルを入れる（のど側の下、製本で見えなくなる所）"));
    hidden->setObjectName(QStringLiteral("nombre_hidden"));
    hidden->setChecked(core::py_truthy(cfg["hidden"]));
    hidden_mm = new QDoubleSpinBox;
    hidden_mm->setObjectName(QStringLiteral("nombre_hidden_size"));
    hidden_mm->setRange(1, 10);
    hidden_mm->setSuffix(QStringLiteral(" mm"));
    hidden_mm->setValue(hidden_value);
    auto* form = new QFormLayout;
    form->addRow(QString(), shown);
    form->addRow(QStringLiteral("位置"), position);
    form->addRow(QStringLiteral("書体"), face);
    form->addRow(QStringLiteral("大きさ"), size_mm);
    form->addRow(QStringLiteral("始まりの番号"), start);
    form->addRow(QString(), hidden);
    form->addRow(QStringLiteral("隠しノンブルの大きさ"), hidden_mm);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay_out(this, form, buttons);
}

Json NombreDialog::op() const {
    return Json::object({{"op", "set_nombre"},
                         {"show", shown->isChecked()},
                         {"position", position->currentData().toString().toStdString()},
                         {"font", face->currentData().toString().toStdString()},
                         {"size_mm", size_mm->value()},
                         {"start", start->value()},
                         {"hidden", hidden->isChecked()},
                         {"hidden_size_mm", hidden_mm->value()}});
}

// --- 表紙・カバーを足す ------------------------------------------------------------------------------------------------

CoverDialog::CoverDialog(QWidget* parent, const core::Document& book) : QDialog(parent) {
    setObjectName(QStringLiteral("cover_dialog"));
    std::set<std::string> have;
    for (const auto& page : book.pages) {
        if (const Json* cover = core::cover_of(*page)) have.insert(cover->at("kind").get<std::string>());
    }
    setWindowTitle(QStringLiteral("表紙・カバーを足す"));
    kind = new QComboBox;
    kind->setObjectName(QStringLiteral("cover_kind"));
    for (const auto& [key, label] : cover_kinds()) {
        if (!have.contains(key)) kind->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    }
    spine = new QDoubleSpinBox;
    spine->setObjectName(QStringLiteral("cover_spine"));
    spine->setRange(1, 100);
    spine->setValue(10);
    spine->setSuffix(QStringLiteral(" mm"));
    spine->setToolTip(QStringLiteral("背幅（ページ数と紙の厚さで決まります。印刷所に聞きます）"));
    flap = new QDoubleSpinBox;
    flap->setObjectName(QStringLiteral("cover_flap"));
    flap->setRange(0, 200);
    flap->setValue(70);
    flap->setSuffix(QStringLiteral(" mm"));
    band = new QDoubleSpinBox;
    band->setObjectName(QStringLiteral("cover_band"));
    band->setRange(15, 200);
    band->setValue(50);
    band->setSuffix(QStringLiteral(" mm"));
    band->setToolTip(QStringLiteral("帯の高さ"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("種類"), kind);
    form->addRow(QStringLiteral("背幅（カバー・帯）"), spine);
    form->addRow(QStringLiteral("袖（カバー・帯）"), flap);
    form->addRow(QStringLiteral("帯の高さ"), band);
    connect(kind, &QComboBox::currentIndexChanged, this, [this] { enable(); });
    enable();
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay_out(this, form, buttons);
}

bool CoverDialog::all_there() const { return kind->count() == 0; }

namespace {

// The profiles of a space a dialog offers: the built-in ones, then the book's (its pages' and its own modes'), each by
// its description; a built-in's data is its id, the book's its ref.
void add_profiles(QComboBox* box, const core::Document& book, core::ProfileSpace space, bool with_none) {
    for (const core::BuiltinProfile& p : core::builtin_profiles()) {
        if (p.space != space) continue;
        if (p.id == "srgb" && !with_none) continue;
        const QString name = p.id == "srgb" ? QStringLiteral("sRGB（プロファイルなし）") : QString::fromUtf8(p.description.data(), static_cast<qsizetype>(p.description.size()));
        box->addItem(name, QString::fromUtf8(p.id.data(), static_cast<qsizetype>(p.id.size())));
    }
    std::map<std::string, core::Bytes> held;
    const auto keep = [&](const std::optional<core::PageColor>& inks, const std::optional<core::RgbMode>& rgb) {
        if (inks && inks->icc && inks->space != core::InkSpace::Lab &&
            (inks->space == core::InkSpace::Cmyk ? core::ProfileSpace::Cmyk : core::ProfileSpace::Gray) == space) {
            held.emplace(inks->profile, inks->icc);
        }
        if (rgb && rgb->icc && space == core::ProfileSpace::Rgb) held.emplace(rgb->profile, rgb->icc);
    };
    keep(book.color, book.rgb);
    for (const auto& p : book.pages) keep(p->color, p->rgb);
    for (const auto& [ref, bytes] : held) {
        if (core::builtin_profile_by_ref(ref) != nullptr) continue;  // (a built-in one: listed above)
        const std::string name = render::ink::profile_description(*bytes);
        box->addItem(QString::fromStdString(name.empty() ? ref.substr(7, 12) : name), QString::fromStdString(ref));
    }
}

// The space a mode's profile is of.
std::optional<core::ProfileSpace> space_of_mode(const QString& mode) {
    if (mode == QLatin1String("rgb")) return core::ProfileSpace::Rgb;
    if (mode == QLatin1String("gray")) return core::ProfileSpace::Gray;
    if (mode == QLatin1String("cmyk")) return core::ProfileSpace::Cmyk;
    return std::nullopt;
}

// A profile file chosen as a person picks it: read, checked for the space (any of RGB, grey, CMYK when none), its ref.
std::optional<std::pair<std::string, std::string>> profile_file(QWidget* parent, std::optional<core::ProfileSpace> space) {
    const QString path = ask::open_path(parent, QStringLiteral("カラープロファイル"), QStringLiteral("カラープロファイル (*.icc *.icm)"));
    if (path.isEmpty()) return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > static_cast<qint64>(core::kProfileMaxBytes)) {
        ask::warning(parent, QStringLiteral("Genko"), QStringLiteral("プロファイルを読めません（16 MB まで）"));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    const std::string icc(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const std::optional<core::ProfileSpace> its = render::ink::profile_space(icc);
    const core::ProfileSpace wanted = space ? *space : its.value_or(core::ProfileSpace::Cmyk);
    std::optional<std::string> why = render::ink::profile_problem(icc, wanted);
    if (!its || (space && *its != *space) || *its == core::ProfileSpace::Lab) {
        why = space == core::ProfileSpace::Rgb ? std::string("not an RGB profile") : space == core::ProfileSpace::Gray ? std::string("not a grey profile")
                                                                                                                       : std::string("not a CMYK profile");
    }
    if (why) {
        const QString kind = wanted == core::ProfileSpace::Rgb ? QStringLiteral("RGB") : wanted == core::ProfileSpace::Gray ? QStringLiteral("グレー") : QStringLiteral("CMYK");
        ask::warning(parent, QStringLiteral("Genko"), QStringLiteral("このプロファイルは %1 のページに使えません: %2").arg(kind, QString::fromStdString(*why)));
        return std::nullopt;
    }
    return std::make_pair(core::bytes_ref(icc), icc);
}

// The profile a combo holds (its data: a built-in's id, or a ref) into an op: "profile", or "icc" for a file chosen in
// the dialog.
void put_profile(Json& op, const QComboBox* box, const std::map<std::string, std::string>& chosen) {
    const std::string data = box->currentData().toString().toStdString();
    if (const auto it = chosen.find(data); it != chosen.end()) op["icc"] = core::b64encode(it->second);
    else op["profile"] = data;
}

}  // namespace

ColorModeDialog::ColorModeDialog(QWidget* parent, const core::Document& book, const core::Page* page, const QString& preset)
    : QDialog(parent), book_(book), page_(page) {
    setObjectName(QStringLiteral("color_mode_dialog"));
    setWindowTitle(QStringLiteral("ページの色モード"));
    scope = new QComboBox;
    scope->setObjectName(QStringLiteral("color_mode_scope"));
    if (page != nullptr) scope->addItem(QStringLiteral("このページ（%1 ページ）").arg(QString::fromStdString(page->index.repr())), QStringLiteral("page"));
    scope->addItem(QStringLiteral("すべてのページと、これから足すページ"), QStringLiteral("all"));
    mode = new QComboBox;
    mode->setObjectName(QStringLiteral("color_mode_mode"));
    mode->addItem(QStringLiteral("RGB"), QStringLiteral("rgb"));
    mode->addItem(QStringLiteral("グレースケール"), QStringLiteral("gray"));
    mode->addItem(QStringLiteral("CMYK（印刷のインキ）"), QStringLiteral("cmyk"));
    mode->addItem(QStringLiteral("Lab"), QStringLiteral("lab"));
    depth = new QComboBox;
    depth->setObjectName(QStringLiteral("color_mode_depth"));
    depth->addItem(QStringLiteral("8 bit"), 8);
    depth->addItem(QStringLiteral("16 bit"), 16);
    depth->addItem(QStringLiteral("32 bit（浮動小数）"), 32);
    profile = new QComboBox;
    profile->setObjectName(QStringLiteral("color_mode_profile"));
    pick = new QPushButton(QStringLiteral("ファイルを選ぶ…"));
    pick->setObjectName(QStringLiteral("color_mode_pick"));
    connect(pick, &QPushButton::clicked, this, [this] { choose_file(); });
    auto* profile_row = new QWidget;
    auto* profile_line = new QHBoxLayout(profile_row);
    profile_line->setContentsMargins(0, 0, 0, 0);
    profile_line->addWidget(profile, 1);
    profile_line->addWidget(pick);
    intent = new QComboBox;
    intent->setObjectName(QStringLiteral("color_mode_intent"));
    intent->addItem(QStringLiteral("相対的な色域を維持"), QStringLiteral("relative"));
    intent->addItem(QStringLiteral("知覚的"), QStringLiteral("perceptual"));
    intent->addItem(QStringLiteral("彩度"), QStringLiteral("saturation"));
    intent->addItem(QStringLiteral("絶対的な色域を維持"), QStringLiteral("absolute"));
    bpc = new QCheckBox(QStringLiteral("黒点の補正"));
    bpc->setObjectName(QStringLiteral("color_mode_bpc"));
    bpc->setChecked(true);
    bpc->setToolTip(QStringLiteral("変換するとき、黒の濃さをプロファイルの黒に合わせます"));
    gray = new QComboBox;
    gray->setObjectName(QStringLiteral("color_mode_gray"));
    gray->addItem(QStringLiteral("プロファイルで変換"), QStringLiteral("profile"));
    gray->addItem(QStringLiteral("K 版だけ（線画向け）"), QStringLiteral("k"));
    gray->setToolTip(QStringLiteral("画素レイヤーのちょうど灰色（R=G=B）の画素を、K 版だけ（グレースケールでは同じ明るさ）にします"));
    dither = new QCheckBox(QStringLiteral("8 bit に下げるときディザをかける"));
    dither->setObjectName(QStringLiteral("color_mode_dither"));
    clip_hdr = new QCheckBox(QStringLiteral("0〜1 を超える HDR の値を切る"));
    clip_hdr->setObjectName(QStringLiteral("color_mode_clip_hdr"));
    clip_hdr->setToolTip(QStringLiteral("32 bit から下げるときなど、残せない明るさの値を切って変換します（切らなければ変換しません）"));
    // what is there now: the page's mode (or the book's)
    const std::optional<core::PageColor>& now = page != nullptr ? page->color : book.color;
    const std::optional<core::RgbMode>& now_rgb = page != nullptr ? page->rgb : book.rgb;
    QString start = preset;
    if (start.isEmpty()) start = now ? QString::fromUtf8(core::to_string(now->space).data()) : QStringLiteral("rgb");
    mode->setCurrentIndex(std::max(0, mode->findData(start)));
    fill_profiles();
    if (now) {
        if (preset.isEmpty() || preset == QString::fromUtf8(core::to_string(now->space).data())) {
            depth->setCurrentIndex(std::max(0, depth->findData(now->depth)));
        }
        if (const int at = intent->findData(QString::fromStdString(now->intent)); at >= 0) intent->setCurrentIndex(at);
        bpc->setChecked(now->bpc);
    } else if (now_rgb) {
        if (start == QLatin1String("rgb")) depth->setCurrentIndex(std::max(0, depth->findData(now_rgb->depth)));
        if (const int at = intent->findData(QString::fromStdString(now_rgb->intent)); at >= 0) intent->setCurrentIndex(at);
        bpc->setChecked(now_rgb->bpc);
    }
    auto* note = new QLabel(QStringLiteral("画素レイヤーを新しい色に変換します（取り消せます）。線・台詞・塗りの色は RGB のまま持ち、"
                                           "ページの色に当てて描きます（墨は K 100%）。プロファイルは原稿に埋め込みます。"));
    note->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("色モード"), mode);
    form->addRow(QStringLiteral("精度"), depth);
    form->addRow(QStringLiteral("プロファイル"), profile_row);
    form->addRow(QStringLiteral("変換の意図"), intent);
    form->addRow(QString(), bpc);
    form->addRow(QStringLiteral("灰色の画素"), gray);
    form->addRow(QString(), dither);
    form->addRow(QString(), clip_hdr);
    form->addRow(note);
    connect(mode, &QComboBox::currentIndexChanged, this, [this] {
        // (back to RGB: 8 bit — the paint layers as PNG —, unless 16 or 32 is chosen again: precise pixels, kept only by
        // layers that do not clip to their panels)
        if (mode->currentData().toString() == QLatin1String("rgb")) depth->setCurrentIndex(0);
        fill_profiles();
        enable();
    });
    connect(profile, &QComboBox::currentIndexChanged, this, [this] { enable(); });
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    enable();
    lay_out(this, form, buttons);
}

// The profiles of the chosen mode's space (none for Lab): RGB's begins at sRGB, grey's at sGray; CMYK's with none
// chosen unless the page (or the book) is CMYK already — a printer's profile is chosen, the built-in one offered.
void ColorModeDialog::fill_profiles() {
    const QSignalBlocker quiet(profile);
    profile->clear();
    const QString chosen = mode->currentData().toString();
    const std::optional<core::ProfileSpace> space = space_of_mode(chosen);
    if (!space) return;
    add_profiles(profile, book_, *space, *space == core::ProfileSpace::Rgb);
    for (const auto& [ref, bytes] : chosen_) {
        if (render::ink::profile_space(bytes) != space || profile->findData(QString::fromStdString(ref)) >= 0) continue;
        const std::string name = render::ink::profile_description(bytes);
        profile->addItem(QString::fromStdString(name.empty() ? ref.substr(7, 12) : name), QString::fromStdString(ref));
    }
    const std::optional<core::PageColor>& now = page_ != nullptr ? page_->color : book_.color;
    const std::optional<core::RgbMode>& now_rgb = page_ != nullptr ? page_->rgb : book_.rgb;
    std::string current;
    if (now && now->space != core::InkSpace::Lab && core::to_string(now->space) == chosen.toStdString()) current = now->profile;
    if (!now && now_rgb && chosen == QLatin1String("rgb")) current = now_rgb->profile;
    int at = -1;
    if (!current.empty()) {
        at = profile->findData(QString::fromStdString(current));
        if (at < 0) {
            if (const core::BuiltinProfile* b = core::builtin_profile_by_ref(current)) at = profile->findData(QString::fromUtf8(b->id.data(), static_cast<qsizetype>(b->id.size())));
        }
    } else if (chosen == QLatin1String("rgb")) {
        at = profile->findData(QStringLiteral("srgb"));
    } else if (chosen == QLatin1String("gray")) {
        at = profile->findData(QStringLiteral("sgray"));
    }
    profile->setCurrentIndex(at);
}

void ColorModeDialog::enable() {
    const QString chosen = mode->currentData().toString();
    const bool cmyk = chosen == QLatin1String("cmyk");
    const bool lab = chosen == QLatin1String("lab");
    profile->setEnabled(!lab);
    pick->setEnabled(!lab);
    gray->setEnabled(cmyk || chosen == QLatin1String("gray"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(problem().isEmpty());
}

void ColorModeDialog::choose_file() {
    const auto file = profile_file(this, space_of_mode(mode->currentData().toString()));
    if (!file) return;
    const auto& [ref, icc] = *file;
    chosen_[ref] = icc;
    const std::string name = render::ink::profile_description(icc);
    int at = profile->findData(QString::fromStdString(ref));
    if (at < 0) {
        profile->addItem(QString::fromStdString(name.empty() ? ref.substr(7, 12) : name), QString::fromStdString(ref));
        at = profile->count() - 1;
    }
    profile->setCurrentIndex(at);
}

QString ColorModeDialog::problem() const {
    if (mode->currentData().toString() == QLatin1String("cmyk") && profile->currentIndex() < 0) {
        return QStringLiteral("CMYK にするには、印刷所が指定する CMYK のプロファイル（.icc）か内蔵のものを選びます");
    }
    return {};
}

core::Json ColorModeDialog::op() const {
    Json op = Json::object({{"op", "set_color_mode"}, {"mode", mode->currentData().toString().toStdString()}, {"depth", depth->currentData().toInt()}});
    if (scope->currentData().toString() == QLatin1String("page") && page_ != nullptr) op["page"] = page_->index.json();
    const QString chosen = mode->currentData().toString();
    const std::string how = intent->currentData().toString().toStdString();
    if (chosen != QLatin1String("rgb") || how != "relative") op["intent"] = how;
    if (chosen == QLatin1String("cmyk")) {
        put_profile(op, profile, chosen_);
        op["gray"] = gray->currentData().toString().toStdString();
    } else if (chosen == QLatin1String("gray")) {
        put_profile(op, profile, chosen_);
        if (gray->currentData().toString() == QLatin1String("k")) op["gray"] = "k";
    } else if (chosen == QLatin1String("rgb") && profile->currentIndex() >= 0 && profile->currentData().toString() != QLatin1String("srgb")) {
        put_profile(op, profile, chosen_);
    }
    if (!bpc->isChecked()) op["bpc"] = false;
    if (dither->isChecked()) op["dither"] = true;
    if (clip_hdr->isChecked()) op["clip_hdr"] = true;
    return op;
}

ProfileDialog::ProfileDialog(QWidget* parent, const core::Document& book, const core::Page* page, bool convert)
    : QDialog(parent), page_(page), convert_(convert) {
    setObjectName(convert ? QStringLiteral("convert_profile_dialog") : QStringLiteral("assign_profile_dialog"));
    setWindowTitle(convert ? QStringLiteral("プロファイルに変換") : QStringLiteral("プロファイルの指定"));
    scope = new QComboBox;
    scope->setObjectName(QStringLiteral("profile_scope"));
    if (page != nullptr) scope->addItem(QStringLiteral("このページ（%1 ページ）").arg(QString::fromStdString(page->index.repr())), QStringLiteral("page"));
    scope->addItem(QStringLiteral("すべてのページ"), QStringLiteral("all"));
    profile = new QComboBox;
    profile->setObjectName(QStringLiteral("profile_choice"));
    pick = new QPushButton(QStringLiteral("ファイルを選ぶ…"));
    pick->setObjectName(QStringLiteral("profile_pick"));
    connect(pick, &QPushButton::clicked, this, [this] { choose_file(); });
    auto* profile_row = new QWidget;
    auto* profile_line = new QHBoxLayout(profile_row);
    profile_line->setContentsMargins(0, 0, 0, 0);
    profile_line->addWidget(profile, 1);
    profile_line->addWidget(pick);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("対象"), scope);
    form->addRow(QStringLiteral("プロファイル"), profile_row);
    if (convert) {
        for (const core::ProfileSpace space : {core::ProfileSpace::Rgb, core::ProfileSpace::Gray, core::ProfileSpace::Cmyk, core::ProfileSpace::Lab}) {
            add_profiles(profile, book, space, true);
        }
        intent = new QComboBox;
        intent->setObjectName(QStringLiteral("profile_intent"));
        intent->addItem(QStringLiteral("相対的な色域を維持"), QStringLiteral("relative"));
        intent->addItem(QStringLiteral("知覚的"), QStringLiteral("perceptual"));
        intent->addItem(QStringLiteral("彩度"), QStringLiteral("saturation"));
        intent->addItem(QStringLiteral("絶対的な色域を維持"), QStringLiteral("absolute"));
        bpc = new QCheckBox(QStringLiteral("黒点の補正"));
        bpc->setObjectName(QStringLiteral("profile_bpc"));
        bpc->setChecked(true);
        dither = new QCheckBox(QStringLiteral("8 bit でディザをかける"));
        dither->setObjectName(QStringLiteral("profile_dither"));
        clip_hdr = new QCheckBox(QStringLiteral("0〜1 を超える HDR の値を切る"));
        clip_hdr->setObjectName(QStringLiteral("profile_clip_hdr"));
        form->addRow(QStringLiteral("変換の意図"), intent);
        form->addRow(QString(), bpc);
        form->addRow(QString(), dither);
        form->addRow(QString(), clip_hdr);
    } else {
        // (the page's space: its pixels are taken as the profile's — RGB, grey or CMYK; none: sRGB or sGray)
        const std::optional<core::PageColor>& now = page != nullptr ? page->color : book.color;
        space_ = !now ? core::ProfileSpace::Rgb
                 : now->space == core::InkSpace::Gray ? std::optional(core::ProfileSpace::Gray)
                 : now->space == core::InkSpace::Cmyk ? std::optional(core::ProfileSpace::Cmyk)
                                                      : std::nullopt;
        if (space_) {
            if (*space_ != core::ProfileSpace::Cmyk) {
                profile->addItem(*space_ == core::ProfileSpace::Rgb ? QStringLiteral("なし（sRGB）") : QStringLiteral("なし（sGray）"), QStringLiteral("none"));
            }
            add_profiles(profile, book, *space_, false);
            // (the page's own profile chosen to begin with)
            const std::optional<core::RgbMode>& now_rgb = page != nullptr ? page->rgb : book.rgb;
            const std::string current = now ? now->profile : now_rgb ? now_rgb->profile : std::string();
            int at = current.empty() ? 0 : profile->findData(QString::fromStdString(current));
            if (const core::BuiltinProfile* b = core::builtin_profile_by_ref(current); at < 0 && b != nullptr) {
                at = profile->findData(QString::fromUtf8(b->id.data(), static_cast<qsizetype>(b->id.size())));
            }
            profile->setCurrentIndex(at);
        }
        pick->setEnabled(space_.has_value());
    }
    auto* note = new QLabel(convert ? QStringLiteral("画素をプロファイルの色に変換します（取り消せます）。色モードはプロファイルの色空間になります。")
                                    : QStringLiteral("画素の値はそのままで、どのプロファイルの色として見るかを決めます（見え方が変わります）。"));
    note->setWordWrap(true);
    form->addRow(note);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(profile, &QComboBox::currentIndexChanged, this, [this] { buttons->button(QDialogButtonBox::Ok)->setEnabled(problem().isEmpty()); });
    buttons->button(QDialogButtonBox::Ok)->setEnabled(problem().isEmpty());
    lay_out(this, form, buttons);
}

void ProfileDialog::choose_file() {
    const auto file = profile_file(this, space_);
    if (!file) return;
    const auto& [ref, icc] = *file;
    chosen_[ref] = icc;
    const std::string name = render::ink::profile_description(icc);
    int at = profile->findData(QString::fromStdString(ref));
    if (at < 0) {
        profile->addItem(QString::fromStdString(name.empty() ? ref.substr(7, 12) : name), QString::fromStdString(ref));
        at = profile->count() - 1;
    }
    profile->setCurrentIndex(at);
}

QString ProfileDialog::problem() const {
    if (!convert_ && !space_) return QStringLiteral("Lab のページにはプロファイルを指定できません");
    if (profile->currentIndex() < 0) return QStringLiteral("プロファイルを選びます");
    return {};
}

core::Json ProfileDialog::op() const {
    Json op = Json::object({{"op", convert_ ? "convert_profile" : "assign_profile"}});
    if (scope->currentData().toString() == QLatin1String("page") && page_ != nullptr) op["page"] = page_->index.json();
    put_profile(op, profile, chosen_);
    if (convert_) {
        op["intent"] = intent->currentData().toString().toStdString();
        if (!bpc->isChecked()) op["bpc"] = false;
        if (dither->isChecked()) op["dither"] = true;
        if (clip_hdr->isChecked()) op["clip_hdr"] = true;
    }
    return op;
}

void CoverDialog::enable() {
    const QString chosen = kind->currentData().toString();
    const bool wrap = chosen == QLatin1String("jacket") || chosen == QLatin1String("obi");
    spine->setEnabled(wrap);
    flap->setEnabled(wrap);
    band->setEnabled(chosen == QLatin1String("obi"));
}

Json CoverDialog::op() const {
    const std::string chosen = kind->currentData().toString().toStdString();
    Json op = Json::object({{"op", "add_cover"}, {"kind", chosen}});
    if (core::is_wrap_kind(chosen)) {
        op["spine_mm"] = spine->value();
        op["flap_mm"] = flap->value();
    }
    if (chosen == "obi") op["height_mm"] = band->value();
    return op;
}

// --- 作品の結合 -------------------------------------------------------------------------------------------------------

std::optional<std::vector<std::int64_t>> page_list(const QString& given, std::int64_t count) {
    QString text = given;
    text.replace(QStringLiteral("、"), QStringLiteral(",")).replace(QStringLiteral("〜"), QStringLiteral("-")).replace(QStringLiteral("～"), QStringLiteral("-"));
    text = py_strip(text);
    if (text.isEmpty()) return std::nullopt;
    const auto unreadable = [&given] { return core::Error("value", given.toStdString()); };
    std::vector<std::int64_t> out;
    for (const QString& piece : text.split(QLatin1Char(','))) {
        const QString part = py_strip(piece);
        if (part.isEmpty()) continue;
        const qsizetype dash = part.indexOf(QLatin1Char('-'));
        if (dash >= 0) {
            const auto a = py_int_text(part.left(dash));
            const auto b = py_int_text(part.mid(dash + 1));
            if (!a || !b) throw unreadable();
            const std::int64_t lo = std::min(*a, *b);
            const std::int64_t hi = std::max(*a, *b);
            // (any page of it outside the book: Python's ValueError once it has made the whole range — or, for a range
            // past any book, its OverflowError or MemoryError before that)
            if (lo < 1 || hi > count) throw unreadable();
            for (std::int64_t p = lo; p <= hi; ++p) out.push_back(p);
        } else {
            const auto p = py_int_text(part);
            if (!p) throw unreadable();
            out.push_back(*p);
        }
    }
    if (out.empty() || std::any_of(out.begin(), out.end(), [count](std::int64_t p) { return p < 1 || p > count; })) throw unreadable();
    std::vector<std::int64_t> once;  // list(dict.fromkeys(out))
    for (const std::int64_t p : out) {
        if (std::find(once.begin(), once.end(), p) == once.end()) once.push_back(p);
    }
    return once;
}

std::string python_path_text(const QString& folder) {
    QString text = folder;
#ifdef Q_OS_WIN
    text.replace(QLatin1Char('\\'), QLatin1Char('/'));  // (pathlib on Windows takes either)
#endif
    // the root: "/" (or exactly "//", which POSIX keeps), then the parts without empty and "." ones
    QString root;
    if (text.startsWith(QStringLiteral("//")) && !text.startsWith(QStringLiteral("///"))) {
        root = QStringLiteral("//");
    } else if (text.startsWith(QLatin1Char('/'))) {
        root = QStringLiteral("/");
    }
    QStringList parts;
    for (const QString& part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part != QLatin1String(".")) parts << part;
    }
    QString out = root + parts.join(QLatin1Char('/'));
#ifdef Q_OS_WIN
    if (root.isEmpty() && parts.size() == 1 && parts[0].size() == 2 && parts[0][1] == QLatin1Char(':') && text.size() > 2) out += QLatin1Char('/');
    out = QDir::toNativeSeparators(out);
#endif
    if (out.isEmpty()) out = QStringLiteral(".");
    return out.toStdString();
}

Json import_op(const std::string& from, const std::optional<std::vector<std::int64_t>>& pages, std::optional<std::int64_t> after) {
    Json op = Json::object({{"op", "import_pages"}, {"from", from}});
    if (pages && !pages->empty()) {
        Json list = Json::array();
        for (const std::int64_t p : *pages) list.push_back(p);
        op["pages"] = list;
    }
    if (after) op["after"] = *after;
    return op;
}

}  // namespace genko::app
