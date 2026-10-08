#include "app/colours.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/config.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;

namespace colours {

namespace {

// Python's float x % 1.0.
double mod1(double x) {
    double m = std::fmod(x, 1.0);
    if (m != 0.0) {
        if (m < 0) m += 1.0;
    } else {
        m = 0.0;
    }
    return m;
}

int to_byte(double c) { return static_cast<int>(core::py_round_int(c * 255)); }

Rgb from_hsv(double h, double s, double v) {
    const auto rgb = hsv_to_rgb(h, s, v);
    return {to_byte(rgb[0]), to_byte(rgb[1]), to_byte(rgb[2])};
}

std::vector<ColourSet> make_built_in() {
    std::vector<ColourSet> out;
    std::vector<Rgb> greys;
    for (const int v : {0, 25, 51, 76, 102, 128, 153, 178, 204, 229, 255}) greys.push_back({v, v, v});
    out.emplace_back("マンガのグレー", std::move(greys));
    std::vector<Rgb> basic;
    for (const auto& [s, v] : {std::pair{1.0, 1.0}, {0.55, 1.0}, {1.0, 0.6}}) {
        for (int h = 0; h < 12; ++h) basic.push_back(from_hsv(h / 12.0, s, v));
    }
    out.emplace_back("基本の色", std::move(basic));
    out.emplace_back("肌・髪", std::vector<Rgb>{{255, 224, 196}, {247, 206, 170}, {234, 184, 146}, {205, 146, 110}, {160, 105, 75},
                                                 {110, 70, 50}, {40, 30, 25}, {90, 60, 40}, {150, 100, 50}, {220, 180, 100},
                                                 {240, 220, 160}, {200, 60, 50}});
    return out;
}

std::filesystem::path sets_path(const std::filesystem::path& config_dir) { return config_dir / "colorsets.json"; }

// Python's int(v) for a value read from the file (nullopt: it would raise, which stops the reading).
std::optional<long long> py_int(const Json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_number_integer()) return v.is_number_unsigned() ? static_cast<long long>(std::min<std::uint64_t>(v.get<std::uint64_t>(), 1u << 30))
                                                               : v.get<long long>();
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        return static_cast<long long>(std::clamp(std::trunc(d), -1e12, 1e12));
    }
    if (v.is_string()) {
        const QString text = QString::fromStdString(v.get<std::string>()).trimmed();
        bool ok = false;
        const long long n = text.toLongLong(&ok, 10);
        if (!ok) return std::nullopt;
        return n;
    }
    return std::nullopt;
}

// One colour of a set as Python reads it (tuple(int(v) for v in c)[:3]); nullopt: the reading stops there.
std::optional<std::vector<long long>> read_colour(const Json& c) {
    std::vector<long long> out;
    const auto take = [&](const Json& v) {
        const auto n = py_int(v);
        if (!n) return false;
        out.push_back(*n);
        return true;
    };
    if (c.is_array()) {
        for (const auto& v : c)
            if (!take(v)) return std::nullopt;
    } else if (c.is_object()) {
        for (const auto& item : c.items())
            if (!take(Json(item.key()))) return std::nullopt;
    } else if (c.is_string()) {
        const QString text = QString::fromStdString(c.get<std::string>());
        for (const QChar ch : text)
            if (!take(Json(QString(ch).toStdString()))) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return out;
}

int byte(long long v) { return static_cast<int>(std::clamp<long long>(v, 0, 255)); }

}  // namespace

std::array<double, 3> rgb_to_hsv(double r, double g, double b) {
    const double maxc = std::max({r, g, b});
    const double minc = std::min({r, g, b});
    const double rangec = maxc - minc;
    const double v = maxc;
    if (minc == maxc) return {0.0, 0.0, v};
    const double s = rangec / maxc;
    const double rc = (maxc - r) / rangec;
    const double gc = (maxc - g) / rangec;
    const double bc = (maxc - b) / rangec;
    double h = 0;
    if (r == maxc) h = bc - gc;
    else if (g == maxc) h = 2.0 + rc - bc;
    else h = 4.0 + gc - rc;
    h = mod1(h / 6.0);
    return {h, s, v};
}

std::array<double, 3> hsv_to_rgb(double h, double s, double v) {
    if (s == 0.0) return {v, v, v};
    int i = static_cast<int>(h * 6.0);
    const double f = (h * 6.0) - i;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    i = ((i % 6) + 6) % 6;
    switch (i) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

const std::vector<ColourSet>& built_in_sets() {
    static const std::vector<ColourSet> sets = make_built_in();
    return sets;
}

bool is_built_in(const std::string& name) {
    const auto& sets = built_in_sets();
    return std::any_of(sets.begin(), sets.end(), [&](const ColourSet& s) { return s.first == name; });
}

std::vector<ColourSet> load_sets(const std::filesystem::path& config_dir) {
    std::vector<ColourSet> out = built_in_sets();
    Json data;
    try {
        const std::string text = storage::read_file(sets_path(config_dir));
        if (core::utf8_error(text)) return out;
        data = core::parse_python_json(text);
    } catch (const core::Error&) {
        return out;
    }
    // (Python stops at a file that is not {"sets": {name: [colour, …]}}; here such a file adds nothing)
    if (!data.is_object()) return out;
    const auto found = data.find("sets");
    if (found == data.end() || !found->is_object()) return out;
    for (const auto& [name, colours] : found->items()) {
        std::vector<Rgb> set;
        if (!colours.is_array() && !colours.is_object() && !colours.is_string()) return out;
        const auto each = [&](const Json& c) {
            const auto read = read_colour(c);
            if (!read) return false;
            if (read->size() >= 3) set.push_back({byte((*read)[0]), byte((*read)[1]), byte((*read)[2])});  // (a shorter one is left out)
            return true;
        };
        bool whole = true;
        if (colours.is_array()) {
            for (const auto& c : colours)
                if (!(whole = each(c))) break;
        } else if (colours.is_object()) {
            for (const auto& item : colours.items())
                if (!(whole = each(Json(item.key())))) break;
        } else {
            for (const QChar ch : QString::fromStdString(colours.get<std::string>()))
                if (!(whole = each(Json(QString(ch).toStdString())))) break;
        }
        if (!whole) return out;
        const auto same = std::find_if(out.begin(), out.end(), [&](const ColourSet& s) { return s.first == name; });
        if (same != out.end()) same->second = std::move(set);
        else out.emplace_back(name, std::move(set));
    }
    return out;
}

void save_set(const std::filesystem::path& config_dir, const std::string& name, const std::optional<std::vector<Rgb>>& colours) {
    const std::filesystem::path path = sets_path(config_dir);
    // (a file that is there but cannot be read is left as it is, rather than written over with this set alone)
    const auto unreadable = [&]() { return core::Error("io", "the colour sets cannot be read, so they are left as they are: " + core::path_to_utf8(path)); };
    Json sets = Json::object();
    std::error_code missing;
    if (std::filesystem::exists(path, missing) || missing) {
        Json data;
        try {
            const std::string text = storage::read_file(path);
            if (core::utf8_error(text)) throw unreadable();
            data = core::parse_python_json(text);
        } catch (const core::Error& error) {
            if (error.code() != "not_found") throw unreadable();
        }
        if (!data.is_null()) {
            if (!data.is_object()) throw unreadable();
            const auto found = data.find("sets");
            if (found != data.end() && core::py_truthy(*found)) {
                if (!found->is_object()) throw unreadable();
                sets = *found;
            }
        }
    }
    if (colours) {
        Json list = Json::array();
        for (const Rgb& c : *colours) list.push_back(Json::array({c[0], c[1], c[2]}));
        sets[name] = std::move(list);
    } else {
        sets.erase(name);
    }
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    storage::write_atomic(path, core::dump(Json{{"sets", sets}}, options));
}

Rgb mix(const Rgb& a, const Rgb& b, double t) {
    Rgb out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(core::py_round_int(a[i] + (b[i] - a[i]) * t));
    return out;
}

std::vector<std::vector<Rgb>> between(const std::array<Rgb, 4>& corners, int n) {
    const auto& [tl, tr, bl, br] = corners;
    std::vector<std::vector<Rgb>> grid;
    for (int row = 0; row < n; ++row) {
        const double v = static_cast<double>(row) / (n - 1);
        const Rgb left = mix(tl, bl, v), right = mix(tr, br, v);
        std::vector<Rgb> line;
        for (int col = 0; col < n; ++col) line.push_back(mix(left, right, static_cast<double>(col) / (n - 1)));
        grid.push_back(std::move(line));
    }
    return grid;
}

std::vector<std::vector<Rgb>> nearby(const Rgb& rgb, int n) {
    const auto [h, s, v] = rgb_to_hsv(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
    const int half = n / 2;
    std::vector<std::vector<Rgb>> rows;
    for (int dy = 0; dy < n; ++dy) {
        std::vector<Rgb> row;
        for (int dx = 0; dx < n; ++dx) {
            const double hh = mod1(h + (dx - half) * 0.03);
            const double ss = core::py_clamp(s + (dy - half) * 0.12, 0.0, 1.0);
            const double vv = core::py_clamp(v - (dy - half) * 0.08 + (dx - half) * 0.02, 0.0, 1.0);
            row.push_back(from_hsv(hh, ss, vv));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace colours

// --- the pieces -----------------------------------------------------------------------------------------------------

Swatch::Swatch(const Rgb& rgb, int size, QWidget* parent) : QPushButton(parent) {
    setFixedSize(size, size);
    set_rgb(rgb);
    connect(this, &QPushButton::clicked, this, [this] { emit picked(rgb_); });
}

void Swatch::set_rgb(const Rgb& rgb) {
    rgb_ = rgb;
    setStyleSheet(QStringLiteral("background: rgb(%1, %2, %3); border: 1px solid %4; border-radius: 3px")
                      .arg(rgb[0])
                      .arg(rgb[1])
                      .arg(rgb[2])
                      .arg(theme::tokens().border));
    setToolTip(QStringLiteral("色（RGB %1, %2, %3）").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
    setAccessibleName(toolTip());
}

SVSquare::SVSquare(QWidget* parent) : QWidget(parent) { setMinimumSize(120, 110); }

void SVSquare::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QLinearGradient across(r.topLeft(), r.topRight());
    across.setColorAt(0, QColor(Qt::white));
    across.setColorAt(1, QColor::fromHsvF(static_cast<float>(hue), 1, 1));
    p.fillRect(r, across);
    QLinearGradient down(r.topLeft(), r.bottomLeft());
    down.setColorAt(0, QColor(0, 0, 0, 0));
    down.setColorAt(1, QColor(0, 0, 0, 255));
    p.fillRect(r, down);
    const double x = r.left() + sat * r.width(), y = r.top() + (1 - val) * r.height();
    p.setPen(QPen(QColor(val < 0.6 ? Qt::white : Qt::black), 1.5));
    p.drawEllipse(QPointF(x, y), 5, 5);
}

void SVSquare::pick(QPointF pos) {
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    sat = core::py_clamp((pos.x() - r.left()) / r.width(), 0.0, 1.0);
    val = core::py_clamp(1 - (pos.y() - r.top()) / r.height(), 0.0, 1.0);
    update();
    emit changed(sat, val);
}

void SVSquare::mousePressEvent(QMouseEvent* event) { pick(event->position()); }

void SVSquare::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) pick(event->position());
}

HueBar::HueBar(QWidget* parent) : QWidget(parent) {
    setFixedWidth(18);  // (a strip to drag along, 110 px tall: not a small target)
    setMinimumHeight(110);
}

void HueBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    for (int k = 0; k < 7; ++k) g.setColorAt(k / 6.0, QColor::fromHsvF(static_cast<float>(std::fmod(k / 6.0, 1.0)), 1, 1));
    p.fillRect(r, g);
    const double y = r.top() + hue * r.height();
    p.setPen(QPen(QColor(Qt::black), 2));
    p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
}

void HueBar::pick(QPointF pos) {
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    hue = core::py_clamp((pos.y() - r.top()) / r.height(), 0.0, 0.999);
    update();
    emit changed(hue);
}

void HueBar::mousePressEvent(QMouseEvent* event) { pick(event->position()); }

void HueBar::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) pick(event->position());
}

namespace {

// Colours in rows, as large as the panel's width lets a wide grid be.
QWidget* swatch_grid(const std::vector<std::vector<Rgb>>& rows, ColourPanel* panel) {
    auto* box = new QWidget;
    auto* grid = new QGridLayout(box);
    grid->setSpacing(2);
    grid->setContentsMargins(0, 0, 0, 0);
    std::size_t columns = 1;
    if (!rows.empty()) {
        columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        columns = std::max<std::size_t>(columns, 1);  // (Python: 200 // 0 would not be reached: rows are never empty)
    }
    const int size = std::max(16, std::min(22, 200 / static_cast<int>(columns) - 2));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t c = 0; c < rows[r].size(); ++c) {
            auto* swatch = new Swatch(rows[r][c], size);
            QObject::connect(swatch, &Swatch::picked, panel, [panel](const Rgb& rgb) { panel->choose(rgb); });
            grid->addWidget(swatch, static_cast<int>(r), static_cast<int>(c));
        }
    }
    return box;
}

void empty(QLayout* layout) {
    while (layout->count() != 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

// "r,g,b" items split by ";" (the ones that are not three whole numbers are passed over).
std::vector<Rgb> read_colours(const QString& text) {
    std::vector<Rgb> out;
    for (const QString& item : text.split(QLatin1Char(';'))) {
        if (item.count(QLatin1Char(',')) != 2) continue;
        const QStringList parts = item.split(QLatin1Char(','));
        Rgb rgb{};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) rgb[static_cast<std::size_t>(i)] = std::clamp(parts[i].trimmed().toInt(&ok), 0, 255);
        if (ok) out.push_back(rgb);
    }
    return out;
}

QString write_colours(const std::vector<Rgb>& colours) {
    QStringList items;
    for (const Rgb& c : colours) items << QStringLiteral("%1,%2,%3").arg(c[0]).arg(c[1]).arg(c[2]);
    return items.join(QLatin1Char(';'));
}

}  // namespace

// --- the panel ------------------------------------------------------------------------------------------------------

ColourPanel::ColourPanel(BrushPanel* brush, QWidget* parent) : QWidget(parent), brush_(brush) {
    {
        const auto store = settings();
        const auto sub_read = read_colours(store->value(QStringLiteral("colour/sub"), QStringLiteral("255,255,255")).toString());
        sub_rgb_ = sub_read.size() == 1 ? sub_read.front() : Rgb{255, 255, 255};
        history_ = read_colours(store->value(QStringLiteral("colour/history"), QString()).toString());
        if (history_.size() > static_cast<std::size_t>(colours::kHistory)) history_.resize(colours::kHistory);
        const auto corners = read_colours(store->value(QStringLiteral("colour/corners"), QString()).toString());
        if (corners.size() == 4) std::copy(corners.begin(), corners.end(), corners_.begin());
        else corners_ = {Rgb{255, 255, 255}, Rgb{230, 60, 60}, Rgb{60, 90, 220}, Rgb{20, 20, 20}};
    }
    // main / sub / transparent
    main = new Swatch(brush_->rgb(), 30);
    main->setToolTip(QStringLiteral("メインの色（今の色）"));
    sub = new Swatch(sub_rgb_, 22);
    sub->setToolTip(QStringLiteral("サブの色（X で入れ替え）"));
    connect(sub, &Swatch::picked, this, [this](const Rgb&) { swap(); });
    swap_button = new QPushButton(QStringLiteral("⇄"));
    swap_button->setFixedWidth(28);
    swap_button->setToolTip(QStringLiteral("メインとサブを入れ替える（X）"));
    swap_button->setAccessibleName(swap_button->toolTip());
    connect(swap_button, &QPushButton::clicked, this, [this] { swap(); });
    transparent = new QCheckBox(QStringLiteral("透明色"));
    transparent->setToolTip(QStringLiteral("透明色で描く: 描いた所が消える（ペンのまま消しゴムになる）"));
    auto* top = new QHBoxLayout;
    top->addWidget(main);
    top->addWidget(sub);
    top->addWidget(swap_button);
    top->addWidget(transparent);
    top->addStretch(1);
    // the square and the numbers
    square = new SVSquare;
    bar = new HueBar;
    connect(square, &SVSquare::changed, this, [this](double, double) { from_square(); });
    connect(bar, &HueBar::changed, this, [this](double h) { from_hue(h); });
    auto* picker = new QHBoxLayout;
    picker->addWidget(square, 1);
    picker->addWidget(bar);
    auto* numbers = new QHBoxLayout;
    numbers->setSpacing(2);
    const char* labels[] = {"R", "G", "B"};
    for (std::size_t i = 0; i < 3; ++i) {
        auto* spin = new QSpinBox;
        spin->setRange(0, 255);
        spin->setMaximumWidth(52);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setAccessibleName(QString::fromLatin1(labels[i]));
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { from_numbers(); });
        numbers->addWidget(new QLabel(QString::fromLatin1(labels[i])));
        numbers->addWidget(spin);
        spins[i] = spin;
    }
    hex = new QLineEdit;
    hex->setMaximumWidth(70);
    hex->setToolTip(QStringLiteral("#RRGGBB"));
    hex->setAccessibleName(QStringLiteral("色（#RRGGBB）"));
    connect(hex, &QLineEdit::editingFinished, this, [this] { from_hex(); });
    numbers->addWidget(hex);
    // sets, history, between, near
    tabs = new QTabWidget;
    set_choice = new QComboBox;
    set_choice->setAccessibleName(QStringLiteral("カラーセット"));
    connect(set_choice, qOverload<int>(&QComboBox::activated), this, [this](int) { fill_set(); });
    set_box = new QWidget;
    set_grid_ = new QGridLayout(set_box);
    set_grid_->setSpacing(2);
    auto* add = new QPushButton(QStringLiteral("今の色を足す"));
    connect(add, &QPushButton::clicked, this, [this] { add_to_set(); });
    auto* fresh = new QPushButton(QStringLiteral("新しいセット…"));
    connect(fresh, &QPushButton::clicked, this, [this] { new_set(); });
    auto* sets_page = new QWidget;
    auto* sl = new QVBoxLayout(sets_page);
    sl->setContentsMargins(2, 2, 2, 2);
    sl->addWidget(set_choice);
    sl->addWidget(set_box);
    auto* row = new QHBoxLayout;
    row->addWidget(add);
    row->addWidget(fresh);
    sl->addLayout(row);
    sl->addStretch(1);
    history_page = new QWidget;
    history_grid_ = new QGridLayout(history_page);
    history_grid_->setSpacing(2);
    between_page = new QWidget;
    between_layout_ = new QVBoxLayout(between_page);
    between_layout_->setContentsMargins(2, 2, 2, 2);
    near_page = new QWidget;
    near_layout_ = new QVBoxLayout(near_page);
    near_layout_->setContentsMargins(2, 2, 2, 2);
    tabs->addTab(sets_page, QStringLiteral("セット"));
    tabs->addTab(history_page, QStringLiteral("履歴"));
    tabs->addTab(between_page, QStringLiteral("中間色"));
    tabs->addTab(near_page, QStringLiteral("近似色"));
    pick_source = new QComboBox;
    pick_source->addItem(QStringLiteral("スポイト: 見えている色"), QStringLiteral("view"));
    pick_source->addItem(QStringLiteral("スポイト: 描く先のレイヤーの色"), QStringLiteral("layer"));
    pick_source->setAccessibleName(QStringLiteral("スポイトで取る色"));
    connect(pick_source, qOverload<int>(&QComboBox::activated), this, [this](int) { emit pick_source_chosen(pick_source->currentData().toString()); });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addLayout(top);
    layout->addLayout(picker);
    layout->addLayout(numbers);
    layout->addWidget(pick_source);
    layout->addWidget(tabs, 1);
    sets_ = colours::load_sets(core::poses::config_dir());
    fill_sets();
    fill_history();
    fill_between();
    show_colour(brush_->rgb());
    connect(brush_, &BrushPanel::changed, this, [this] { show_colour(brush_->rgb()); });
}

// --- the colour -----------------------------------------------------------------------------------------------------

Rgb ColourPanel::rgb() const { return brush_->rgb(); }

void ColourPanel::choose(const Rgb& rgb) {
    brush_->set_colour(rgb);
    transparent->setChecked(false);
}

void ColourPanel::show_colour(const Rgb& rgb) {
    loading_ = true;
    main->set_rgb(rgb);
    const auto [h, s, v] = colours::rgb_to_hsv(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
    if (s > 0 && v > 0) square->hue = bar->hue = h;
    square->sat = s;
    square->val = v;
    square->update();
    bar->update();
    for (std::size_t i = 0; i < 3; ++i) spins[i]->setValue(rgb[i]);
    hex->setText(QStringLiteral("#%1%2%3")
                     .arg(rgb[0], 2, 16, QLatin1Char('0'))
                     .arg(rgb[1], 2, 16, QLatin1Char('0'))
                     .arg(rgb[2], 2, 16, QLatin1Char('0'))
                     .toUpper());
    fill_near(rgb);
    loading_ = false;
}

void ColourPanel::from_square() {
    if (loading_) return;
    const auto c = colours::hsv_to_rgb(bar->hue, square->sat, square->val);
    choose({static_cast<int>(core::py_round_int(c[0] * 255)), static_cast<int>(core::py_round_int(c[1] * 255)),
            static_cast<int>(core::py_round_int(c[2] * 255))});
}

void ColourPanel::from_hue(double h) {
    square->hue = h;
    square->update();
    from_square();
}

void ColourPanel::from_numbers() {
    if (!loading_) choose({spins[0]->value(), spins[1]->value(), spins[2]->value()});
}

void ColourPanel::from_hex() {
    QString text = hex->text().trimmed();
    while (text.startsWith(QLatin1Char('#'))) text.remove(0, 1);
    if (text.size() != 6) return;
    Rgb rgb{};
    for (int i = 0; i < 3; ++i) {
        const QString pair = text.mid(i * 2, 2);
        if (std::any_of(pair.begin(), pair.end(), [](QChar ch) { return !std::isxdigit(ch.unicode() < 128 ? ch.toLatin1() : 'g'); })) return;
        rgb[static_cast<std::size_t>(i)] = pair.toInt(nullptr, 16);
    }
    choose(rgb);
}

void ColourPanel::swap() {
    const Rgb new_main = sub_rgb_;
    sub_rgb_ = rgb();
    sub->set_rgb(sub_rgb_);
    settings()->setValue(QStringLiteral("colour/sub"), write_colours({sub_rgb_}));
    choose(new_main);
}

void ColourPanel::remember(const Rgb& rgb) {
    std::vector<Rgb> history{rgb};
    for (const Rgb& c : history_)
        if (c != rgb) history.push_back(c);
    if (history.size() > static_cast<std::size_t>(colours::kHistory)) history.resize(colours::kHistory);
    history_ = std::move(history);
    settings()->setValue(QStringLiteral("colour/history"), write_colours(history_));
    fill_history();
}

// --- the pages ------------------------------------------------------------------------------------------------------

void ColourPanel::fill_sets() {
    const QString current = set_choice->currentText();
    set_choice->clear();
    for (const auto& [name, _] : sets_) set_choice->addItem(QString::fromStdString(name));
    if (!current.isEmpty()) set_choice->setCurrentText(current);
    fill_set();
}

void ColourPanel::fill_set() {
    empty(set_grid_);
    const std::string chosen = set_choice->currentText().toStdString();
    const auto found = std::find_if(sets_.begin(), sets_.end(), [&](const colours::ColourSet& s) { return s.first == chosen; });
    if (found == sets_.end()) return;
    int n = 0;
    for (const Rgb& rgb : found->second) {
        auto* swatch = new Swatch(rgb);
        connect(swatch, &Swatch::picked, this, [this](const Rgb& c) { choose(c); });
        set_grid_->addWidget(swatch, n / 7, n % 7);  // (7 a row: 22 px swatches fit the panel)
        ++n;
    }
}

bool ColourPanel::keep_set(const std::string& name) {
    const auto found = std::find_if(sets_.begin(), sets_.end(), [&](const colours::ColourSet& s) { return s.first == name; });
    try {
        colours::save_set(core::poses::config_dir(), name, found != sets_.end() ? std::optional(found->second) : std::nullopt);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("カラーセット"),
                     QStringLiteral("カラーセットを保存できませんでした。\n%1").arg(wording::error(QString::fromUtf8(error.what()))));
        return false;
    }
    return true;
}

void ColourPanel::add_to_set() {
    const std::string chosen = set_choice->currentText().toStdString();
    std::string name = chosen;
    const auto at = [this](const std::string& key) -> std::vector<Rgb>& {
        auto found = std::find_if(sets_.begin(), sets_.end(), [&](const colours::ColourSet& s) { return s.first == key; });
        if (found == sets_.end()) {
            sets_.emplace_back(key, std::vector<Rgb>{});
            return sets_.back().second;
        }
        return found->second;
    };
    if (colours::is_built_in(name)) {  // (the built-in sets stay as they are: a copy becomes one's own)
        name += "（自分）";
        const std::vector<Rgb> copy = at(chosen);
        at(name) = copy;
    }
    at(name).push_back(rgb());
    keep_set(name);
    fill_sets();
    set_choice->setCurrentText(QString::fromStdString(name));
    fill_set();
}

void ColourPanel::new_set(const std::optional<QString>& asked) {
    QString name;
    if (asked) {
        name = *asked;
    } else {
        const auto typed = ask::get_text(this, QStringLiteral("カラーセット"), QStringLiteral("新しいセットの名前"));
        if (!typed || typed->trimmed().isEmpty()) return;
        name = *typed;
    }
    const std::string key = name.trimmed().toStdString();
    const auto found = std::find_if(sets_.begin(), sets_.end(), [&](const colours::ColourSet& s) { return s.first == key; });
    if (found != sets_.end()) found->second = {rgb()};
    else sets_.emplace_back(key, std::vector<Rgb>{rgb()});
    keep_set(key);
    fill_sets();
    set_choice->setCurrentText(QString::fromStdString(key));
    fill_set();
}

void ColourPanel::fill_history() {
    empty(history_grid_);
    int n = 0;
    for (const Rgb& rgb : history_) {
        auto* swatch = new Swatch(rgb);
        connect(swatch, &Swatch::picked, this, [this](const Rgb& c) { choose(c); });
        history_grid_->addWidget(swatch, n / 7, n % 7);
        ++n;
    }
}

void ColourPanel::fill_between() {
    empty(between_layout_);
    between_layout_->addWidget(swatch_grid(colours::between(corners_), this));
    auto* holder = new QWidget;
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    const char* labels[] = {"左上", "右上", "左下", "右下"};
    for (int k = 0; k < 4; ++k) {
        auto* button = new QPushButton(QString::fromUtf8(labels[k]));
        button->setMinimumWidth(30);
        button->setToolTip(QStringLiteral("今の色をこの角にする"));
        connect(button, &QPushButton::clicked, this, [this, k] { set_corner(k); });
        row->addWidget(button);
    }
    between_layout_->addWidget(holder);
}

void ColourPanel::set_corner(int i) {
    corners_[static_cast<std::size_t>(i)] = rgb();
    settings()->setValue(QStringLiteral("colour/corners"), write_colours({corners_.begin(), corners_.end()}));
    fill_between();
}

void ColourPanel::fill_near(const Rgb& rgb) {
    empty(near_layout_);
    near_layout_->addWidget(swatch_grid(colours::nearby(rgb), this));
}

}  // namespace genko::app
