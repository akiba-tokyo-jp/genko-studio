#include "app/book_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <set>

#include "app/lettering.hpp"
#include "app/look.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"

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
