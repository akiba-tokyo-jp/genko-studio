// PC2a (階調の補正・3D LUT・補正のパネル, SPEC.md COLOR-04): the forms of the tone corrections (levels, curve,
// brightness_contrast, exposure, posterize, threshold, invert, gradient_map, color_lookup) and their commands in
// レイヤー → 色調補正 (app/adjust_forms.hpp) — each a destructive correction of the layer drawn on, inside the selection,
// its result shown on the page while it changes — with 平均化, the three 自動 and ファイル → カラールックアップテーブルを
// 書き出す…. A correction made new starts from PhotoCraft's form (the user's decision 3: levels and a tone curve by its
// formula, brightness_contrast modern, posterize floor); one that exists keeps its own.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/layer_dialogs.hpp"
#include "app/main_window.hpp"
#include "core/adjust_tone.hpp"
#include "core/lut3d.hpp"
#include "core/paths.hpp"
#include "formats/exporting.hpp"
#include "render/page.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::adjust_forms {

namespace {

using core::Json;

// The channels a page's records are in (its mode: RGB, CMYK, Lab, grey), with their words.
std::vector<std::pair<std::string, QString>> channel_words(std::string_view space) {
    if (space == "cmyk") return {{"cyan", QStringLiteral("シアン")}, {"magenta", QStringLiteral("マゼンタ")}, {"yellow", QStringLiteral("イエロー")}, {"black", QStringLiteral("ブラック")}};
    if (space == "lab") return {{"lightness", QStringLiteral("明度（L）")}, {"a", QStringLiteral("a")}, {"b", QStringLiteral("b")}};
    if (space == "gray") return {{"gray", QStringLiteral("グレー")}};
    return {{"red", QStringLiteral("赤")}, {"green", QStringLiteral("緑")}, {"blue", QStringLiteral("青")}};
}

QString space_words(std::string_view space) {
    if (space == "cmyk") return QStringLiteral("CMYK（版の明るさ）");
    if (space == "lab") return QStringLiteral("Lab");
    if (space == "gray") return QStringLiteral("グレー");
    return QStringLiteral("RGB");
}

QPixmap histogram_pixmap(const std::vector<int>& counts, int width = 202, int height = 56) {
    QPixmap pixmap(width, height);
    pixmap.fill(QColor(250, 250, 250));
    std::vector<int> sorted = counts;
    std::sort(sorted.begin(), sorted.end());
    const int top = std::max(1, sorted.size() > 1 ? sorted[sorted.size() - 2] : (sorted.empty() ? 1 : sorted.back()));
    QPainter p(&pixmap);
    p.setPen(QPen(QColor(90, 90, 90), 1));
    for (std::size_t i = 0; i < counts.size() && i < 256; ++i) {
        const double x = 1 + static_cast<double>(i) * (width - 2) / 255;
        const int h = std::min(height - 2, static_cast<int>(std::lround((height - 2) * static_cast<double>(counts[i]) / top)));
        if (h > 0) p.drawLine(QPointF(x, height - 1), QPointF(x, height - 1 - h));
    }
    return pixmap;
}

void add_histogram(Form& form, const FormContext& context) {
    if (!context.histogram) return;
    auto* chart = new QLabel;
    chart->setObjectName(QStringLiteral("adjust_histogram"));
    chart->setPixmap(histogram_pixmap(*context.histogram));
    chart->setToolTip(QStringLiteral("このレイヤーの明るさの分布（左が黒、右が白）"));
    form.layout_form()->addRow(QStringLiteral("分布"), chart);
}

// The formula's choice of levels and a tone curve (the user's decision 4): PhotoCraft's (Photoshop's) or Genko's.
void method_field(Form& form) {
    form.choice("native.method", QStringLiteral("式"),
                {{QStringLiteral("Photoshop 型（上流の式）"), Json("upstream")}, {QStringLiteral("Genko 型（今までの式）"), Json()}}, Json());
}

// The records' space: the page's (shown; its value only while there are records).
struct Records {
    std::string space = "rgb";
    Json records = Json::object();
    std::function<void()> shown;  // (the channel widget made again for the space)
};

void space_field(Form& form, const std::shared_ptr<Records>& state, std::string_view page_space) {
    auto* label = new QLabel(space_words(page_space));
    label->setProperty("role", "hint");
    const std::string fallback(page_space);
    form.custom("native.space", QStringLiteral("チャンネルの色空間"), label,
                [state] { return state->records.empty() ? Json() : Json(state->space); },
                [state, label, fallback](const Json& v) {
                    state->space = v.is_string() ? v.get<std::string>() : fallback;
                    label->setText(space_words(state->space));
                    if (state->shown) state->shown();
                },
                Json());
}

// レベル補正: the composite (Python's keys) and each channel's record (native.channels).
void levels_form(Form& form, const FormContext& context) {
    add_histogram(form, context);
    method_field(form);
    form.number("black", QStringLiteral("黒くする所（入力の黒）"), 0, 253, 0, 0);
    form.number("white", QStringLiteral("白くする所（入力の白）"), 2, 255, 255, 0);
    form.number("gamma", QStringLiteral("中間（1 より大きいと明るく）"), 0.01, 9.99, 1.0, 2);
    form.number("out_black", QStringLiteral("出す暗さの下限"), 0, 255, 0, 0);
    form.number("out_white", QStringLiteral("出す明るさの上限"), 0, 255, 255, 0);
    auto state = std::make_shared<Records>();
    const std::string page_space(core::default_tone_space(context.page));
    space_field(form, state, page_space);
    // the channel switch and the chosen channel's record
    auto* box = new QWidget;
    auto* rows = new QFormLayout(box);
    rows->setContentsMargins(0, 0, 0, 0);
    auto* which = new QComboBox;
    which->setObjectName(QStringLiteral("adjust_channel"));
    rows->addRow(QStringLiteral("色"), which);
    struct Spin {
        const char* key;
        double lo, hi, fallback;
        int places;
        QDoubleSpinBox* box = nullptr;
    };
    auto spins = std::make_shared<std::vector<Spin>>(std::vector<Spin>{
        {"black", 0, 253, 0, 0}, {"white", 2, 255, 255, 0}, {"gamma", 0.01, 9.99, 1.0, 2}, {"out_black", 0, 255, 0, 0}, {"out_white", 0, 255, 255, 0}});
    const std::vector<QString> words{QStringLiteral("黒"), QStringLiteral("白"), QStringLiteral("中間"), QStringLiteral("出す下限"), QStringLiteral("出す上限")};
    for (std::size_t i = 0; i < spins->size(); ++i) {
        Spin& s = (*spins)[i];
        s.box = new QDoubleSpinBox;
        s.box->setObjectName(QStringLiteral("adjust_channel_%1").arg(QString::fromLatin1(s.key)));
        s.box->setRange(s.lo, s.hi);
        s.box->setDecimals(s.places);
        s.box->setSingleStep(s.places > 0 ? 0.1 : 1);
        s.box->setValue(s.fallback);
        rows->addRow(words[i], s.box);
    }
    auto loading = std::make_shared<bool>(false);
    const auto show = [state, which, spins, loading] {
        *loading = true;
        const QString was = which->currentData().toString();
        which->clear();
        for (const auto& [name, label] : channel_words(state->space)) which->addItem(label, QString::fromStdString(name));
        which->setCurrentIndex(std::max(0, which->findData(was)));
        const std::string name = which->currentData().toString().toStdString();
        const Json record = state->records.contains(name) ? state->records[name] : Json::object();
        for (Spin& s : *spins) s.box->setValue(record.value(s.key, s.fallback));
        *loading = false;
    };
    state->shown = show;
    QObject::connect(which, &QComboBox::currentIndexChanged, box, [show, loading] {
        if (!*loading) show();
    });
    for (Spin& s : *spins) {
        QObject::connect(s.box, &QDoubleSpinBox::valueChanged, &form, [&form, state, which, spins, loading] {
            if (*loading) return;
            const std::string name = which->currentData().toString().toStdString();
            Json record = Json::object();
            bool identity = true;
            for (const Spin& each : *spins) {
                const double v = each.places == 0 ? std::round(each.box->value()) : std::round(each.box->value() * 100) / 100;
                record[each.key] = each.places == 0 ? Json(static_cast<std::int64_t>(v)) : Json(v);
                identity = identity && v == each.fallback;
            }
            if (identity) {
                state->records.erase(name);
            } else {
                state->records[name] = record;
            }
            emit form.changed();
        });
    }
    form.custom("native.channels", QStringLiteral("チャンネルごと"), box,
                [state] { return state->records.empty() ? Json() : state->records; },
                [state, show](const Json& v) {
                    state->records = v.is_object() ? v : Json::object();
                    show();
                },
                Json());
    show();
    form.note(QStringLiteral("合成は全部の色に、チャンネルごとの組はその色だけにかかります（レベル補正は合成の後にチャンネル）"));
}

// トーンカーブ: the composite's points and each channel's curve (native.channels).
void curve_form(Form& form, const FormContext& context) {
    add_histogram(form, context);
    method_field(form);
    auto* composite = new CurveEditor;
    QObject::connect(composite, &CurveEditor::changed, &form, [&form] { emit form.changed(); });
    form.custom("points", QStringLiteral("合成の曲線"), composite,
                [composite] {
                    Json points = Json::array();
                    for (const auto& p : composite->points()) points.push_back(Json::array({static_cast<int>(p[0]), static_cast<int>(p[1])}));
                    return points;
                },
                [composite](const Json& v) {
                    std::vector<std::array<double, 2>> points;
                    if (v.is_array())
                        for (const Json& p : v)
                            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) points.push_back({p[0].get<double>(), p[1].get<double>()});
                    composite->set_points(points);
                },
                Json::array({Json::array({0, 0}), Json::array({255, 255})}));
    composite->setObjectName(QStringLiteral("curve_editor"));  // (after custom: a field is named field_<key> there)
    auto state = std::make_shared<Records>();
    const std::string page_space(core::default_tone_space(context.page));
    space_field(form, state, page_space);
    auto* box = new QWidget;
    auto* rows = new QVBoxLayout(box);
    rows->setContentsMargins(0, 0, 0, 0);
    auto* which = new QComboBox;
    which->setObjectName(QStringLiteral("adjust_channel"));
    auto* own = new CurveEditor;
    own->setObjectName(QStringLiteral("adjust_channel_curve"));
    rows->addWidget(which);
    rows->addWidget(own);
    auto loading = std::make_shared<bool>(false);
    const auto show = [state, which, own, loading] {
        *loading = true;
        const QString was = which->currentData().toString();
        which->clear();
        for (const auto& [name, label] : channel_words(state->space)) which->addItem(label, QString::fromStdString(name));
        which->setCurrentIndex(std::max(0, which->findData(was)));
        const std::string name = which->currentData().toString().toStdString();
        std::vector<std::array<double, 2>> points;
        if (state->records.contains(name))
            for (const Json& p : state->records[name]) points.push_back({p[0].get<double>(), p[1].get<double>()});
        own->set_points(points);
        *loading = false;
    };
    state->shown = show;
    QObject::connect(which, &QComboBox::currentIndexChanged, box, [show, loading] {
        if (!*loading) show();
    });
    QObject::connect(own, &CurveEditor::changed, &form, [&form, state, which, own, loading] {
        if (*loading) return;
        const std::string name = which->currentData().toString().toStdString();
        const auto& points = own->points();
        const bool straight = points.size() == 2 && points[0] == std::array<double, 2>{0, 0} && points[1] == std::array<double, 2>{255, 255};
        if (straight) {
            state->records.erase(name);
        } else {
            Json list = Json::array();
            for (const auto& p : points) list.push_back(Json::array({static_cast<int>(p[0]), static_cast<int>(p[1])}));
            state->records[name] = list;
        }
        emit form.changed();
    });
    form.custom("native.channels", QStringLiteral("チャンネルごとの曲線"), box,
                [state] { return state->records.empty() ? Json() : state->records; },
                [state, show](const Json& v) {
                    state->records = v.is_object() ? v : Json::object();
                    show();
                },
                Json());
    show();
    // (the curves drawn by the formula chosen)
    if (auto* method = form.findChild<QComboBox*>(QStringLiteral("field_native.method"))) {
        const auto drawn = [method, composite, own] {
            const bool upstream = method->currentIndex() == 0;
            composite->set_upstream(upstream);
            own->set_upstream(upstream);
        };
        QObject::connect(method, &QComboBox::currentIndexChanged, &form, drawn);
        drawn();
    }
    form.note(QStringLiteral("横が元の明るさ、縦がかけた後。チャンネルごとの曲線を先に、合成の曲線を後にかけます"));
}

void brightness_form(Form& form, const FormContext&) {
    form.choice("native.method", QStringLiteral("式"),
                {{QStringLiteral("Photoshop 型"), Json("modern")}, {QStringLiteral("従来（legacy）"), Json("legacy")}, {QStringLiteral("Genko 型（今までの式）"), Json()}},
                Json());
    form.number("brightness", QStringLiteral("明るさ"), -150, 150, 0, 0);
    form.number("contrast", QStringLiteral("コントラスト"), -100, 100, 0, 0);
    form.note(QStringLiteral("Photoshop 型: 明るさ −150〜150・コントラスト −50〜100（黒と白は動きません）。従来と Genko 型は直線の式です"));
}

void exposure_form(Form& form, const FormContext&) {
    form.number("exposure", QStringLiteral("露光量（段）"), -20, 20, 0, 2);
    form.number("offset", QStringLiteral("オフセット"), -0.5, 0.5, 0, 3);
    form.number("gamma", QStringLiteral("ガンマ"), 0.01, 9.99, 1.0, 2);
}

void posterize_form(Form& form, const FormContext&) {
    form.choice("native.method", QStringLiteral("段の区切り"),
                {{QStringLiteral("Photoshop 型（2〜255）"), Json("floor")}, {QStringLiteral("Genko 型（2〜64）"), Json()}}, Json());
    form.number("levels", QStringLiteral("段階の数"), 2, 255, 4, 0);
}

void threshold_form(Form& form, const FormContext&) { form.number("threshold", QStringLiteral("しきい値（0〜255）"), 0, 255, 128, 0); }

void invert_form(Form& form, const FormContext&) { form.note(QStringLiteral("色を反転します（設定はありません）")); }

// A check whose value is only there when it is on (off: Python's form, no native).
void flag(Form& form, const std::string& key, const QString& label) {
    auto* box = new QCheckBox;
    QObject::connect(box, &QCheckBox::toggled, &form, [&form] { emit form.changed(); });
    form.custom(key, label, box, [box] { return box->isChecked() ? Json(true) : Json(); },
                [box](const Json& v) { box->setChecked(v == Json(true)); }, Json());
}

void gradient_form(Form& form, const FormContext& context) {
    auto* editor = new StopsEditor({}, false);
    QObject::connect(editor, &StopsEditor::changed, &form, [&form] { emit form.changed(); });
    const std::array<int, 3> pen = context.pen;
    form.custom("stops", QStringLiteral("色（暗い所 → 明るい所）"), editor,
                [editor] {
                    Json out = Json::array();
                    for (const Stop& s : editor->stops())
                        out.push_back(Json::array({std::round(s.at * 10000) / 10000, Json::array({s.rgb[0], s.rgb[1], s.rgb[2]})}));
                    return out;
                },
                [editor, pen](const Json& v) {
                    std::vector<Stop> stops;
                    if (v.is_array()) {
                        for (const Json& s : v) {
                            if (!s.is_array() || s.size() < 2 || !s[0].is_number() || !s[1].is_array() || s[1].size() < 3) continue;
                            stops.push_back(Stop{s[0].get<double>(), {s[1][0].get<int>(), s[1][1].get<int>(), s[1][2].get<int>()}, 1.0});
                        }
                    }
                    if (stops.size() < 2) stops = {Stop{0.0, pen, 1.0}, Stop{1.0, {255, 255, 255}, 1.0}};
                    editor->set_stops(stops);
                },
                Json());
    flag(form, "native.reverse", QStringLiteral("逆にする"));
    flag(form, "native.dither", QStringLiteral("ディザ（帯を目立たなく）"));
}

// A gradient map's colours (Python's evenly placed "colors") as the stops the form holds.
Json gradient_prepare(const Json& now) {
    if (!now.is_object() || (now.contains("stops") && now["stops"].is_array() && !now["stops"].empty())) return now;
    const auto it = now.find("colors");
    if (it == now.end() || !it->is_array() || it->size() < 2) return now;
    Json out = now;
    Json stops = Json::array();
    for (std::size_t i = 0; i < it->size(); ++i) stops.push_back(Json::array({static_cast<double>(i) / static_cast<double>(it->size() - 1), (*it)[i]}));
    out["stops"] = stops;
    out.erase("colors");
    return out;
}

// カラールックアップ: a look, or a .cube / .3dl / .look file read here and sent as its text (the op keeps it in the book).
struct Lookup {
    Json look, lut, name, data, file_name;
    QLabel* shown = nullptr;
    QComboBox* looks = nullptr;
    void show() {
        if (shown == nullptr) return;
        if (file_name.is_string()) {
            shown->setText(QStringLiteral("読み込んだ表: %1").arg(QString::fromStdString(file_name.get<std::string>())));
        } else if (lut.is_string()) {
            shown->setText(QStringLiteral("原稿の表: %1").arg(name.is_string() ? QString::fromStdString(name.get<std::string>()) : QStringLiteral("（名前なし）")));
        } else {
            shown->setText(look.is_string() ? QStringLiteral("組み込みの見え方") : QStringLiteral("表なし（色はそのまま）"));
        }
    }
};

void lookup_form(Form& form, const FormContext&) {
    auto state = std::make_shared<Lookup>();
    auto* looks = new QComboBox;
    looks->addItem(QStringLiteral("（なし・ファイルの表）"), QString());
    for (const core::lut3d::Look& look : core::lut3d::looks())
        looks->addItem(QString::fromUtf8(look.label.data(), static_cast<qsizetype>(look.label.size())), QString::fromUtf8(look.id.data(), static_cast<qsizetype>(look.id.size())));
    state->looks = looks;
    QObject::connect(looks, &QComboBox::activated, &form, [&form, state, looks] {
        const QString id = looks->currentData().toString();
        state->look = id.isEmpty() ? Json() : Json(id.toStdString());
        if (!id.isEmpty()) state->lut = state->name = state->data = state->file_name = Json();
        state->show();
        emit form.changed();
    });
    form.custom("look", QStringLiteral("見え方"), looks, [state] { return state->look; },
                [state, looks](const Json& v) {
                    state->look = v.is_string() ? v : Json();
                    looks->setCurrentIndex(std::max(0, looks->findData(v.is_string() ? QString::fromStdString(v.get<std::string>()) : QString())));
                    state->show();
                },
                Json());
    auto* load = new QPushButton(QStringLiteral("ファイルから読み込む…"));
    QObject::connect(load, &QPushButton::clicked, &form, [&form, state, looks, load] {
        const QString path = ask::open_path(load, QStringLiteral("3D LUT を読み込む"), QStringLiteral("3D LUT (*.cube *.3dl *.look)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > static_cast<qint64>(core::lut3d::kMaxFileBytes)) {
            ask::warning(load, QStringLiteral("カラールックアップ"), QStringLiteral("読めないか、32 MiB より大きいファイルです"));
            return;
        }
        const QByteArray bytes = file.readAll();
        const std::string name = QFileInfo(path).fileName().toStdString();
        try {  // (read here first: a file it cannot read is said at once, not in the preview)
            (void)core::lut3d::parse(name, std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        } catch (const core::Error& error) {
            ask::warning(load, QStringLiteral("カラールックアップ"), QStringLiteral("3D LUT として読めません: %1").arg(QString::fromUtf8(error.what())));
            return;
        }
        state->data = std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        state->file_name = name;
        state->look = state->lut = state->name = Json();
        looks->setCurrentIndex(0);
        state->show();
        emit form.changed();
    });
    form.custom("data", QStringLiteral("ファイル"), load, [state] { return state->data; },
                [state](const Json& v) { state->data = v.is_string() ? v : Json(); }, Json());
    load->setObjectName(QStringLiteral("adjust_lut_load"));  // (after custom: a field is named field_<key> there)
    auto* shown = new QLabel;
    shown->setProperty("role", "hint");
    state->shown = shown;
    form.custom("file_name", QString(), shown, [state] { return state->file_name; },
                [state](const Json& v) {
                    state->file_name = v.is_string() ? v : Json();
                    state->show();
                },
                Json());
    shown->setObjectName(QStringLiteral("adjust_lut_name"));
    // (a table the book holds already, and its name: kept while no other is chosen)
    auto* held = new QWidget;
    held->setFixedHeight(0);
    form.custom("lut", QString(), held, [state] { return state->lut; },
                [state](const Json& v) {
                    state->lut = v.is_string() ? v : Json();
                    state->show();
                },
                Json());
    auto* named = new QWidget;
    named->setFixedHeight(0);
    form.custom("name", QString(), named, [state] { return state->lut.is_string() ? state->name : Json(); },
                [state](const Json& v) {
                    state->name = v.is_string() ? v : Json();
                    state->show();
                },
                Json());
    form.from_table(*core::adjust_kind("color_lookup"), {{"interpolation", QStringLiteral("補間")}, {"dither", QStringLiteral("ディザ")}},
                    {{"interpolation", {{QStringLiteral("三線形"), Json("trilinear")}, {QStringLiteral("四面体"), Json("tetrahedral")}}}});
    state->show();
}

FormKind kind_of(std::string_view kind, const QString& title, Builder build, Json fresh = Json::object(), std::function<Json(const Json&)> prepare = nullptr) {
    FormKind out;
    out.kind = kind;
    out.group = "PC2a";
    out.title = title;
    out.build = std::move(build);
    out.fresh = std::move(fresh);
    out.prepare = std::move(prepare);
    return out;
}

// --- the commands -----------------------------------------------------------------------------------------------------

// What the layer drawn on shows (its lightness and its red, green and blue: for the levels' and curves' windows and
// 自動), at 100 dpi.
struct LayerCounts {
    std::vector<int> lightness = std::vector<int>(256, 0);
    std::array<std::array<std::uint64_t, 256>, 3> rgb{};
};
std::optional<LayerCounts> counts_of(const MainWindow& window, const core::Page& page, const core::Layer& layer) {
    try {
        const render::Image image = render::layer_image(page, layer, 100, &window.book(), true).convert("RGBA");
        const std::string bytes = image.tobytes();
        LayerCounts out;
        for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data() + i);
            if (q[3] == 0) continue;
            for (unsigned c = 0; c < 3; ++c) ++out.rgb[c][q[c]];
            ++out.lightness[static_cast<std::size_t>(std::clamp(std::lround((q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000.0), 0L, 255L))];
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The op of a destructive correction: the window's settings (what it leaves out — a null — left out), inside the area.
Json filter_op(const Json& page, const std::string& id, const std::string& kind, const Json& values, const std::optional<Json>& area) {
    Json op{{"op", "filter_raster"}, {"page", page}, {"id", id}, {"kind", kind}};
    for (const auto& [key, value] : values.items()) {
        if (value.is_null()) continue;
        if (key == "native" && value.is_object()) {
            Json native = Json::object();
            for (const auto& [k, v] : value.items())
                if (!v.is_null()) native[k] = v;
            if (!native.empty()) op["native"] = native;
            continue;
        }
        op[key] = value;
    }
    if (area) op["area"] = *area;
    return op;
}

// The layer a destructive correction works on: the one drawn on (none: said).
const core::Layer* target(MainWindow* window) {
    const core::Layer* layer = window->target_layer();
    if (window->current_page() == nullptr || layer == nullptr) {
        window->flash(QStringLiteral("先に色調補正をかけるレイヤーを選びます"), 4000, true);
        return nullptr;
    }
    return layer;
}

void run_window(MainWindow* window, const std::string& kind) {
    const core::Layer* layer = target(window);
    if (layer == nullptr) return;
    const core::Page& page = *window->current_page();
    const FormKind* form = form_kind(kind);
    if (form == nullptr) return;
    const auto asked = window->asking();
    const Json index = page.index.json();
    const std::string id = layer->id;
    const std::optional<Json> area = window->selection_area();
    FormContext context;
    context.page = &page;
    const auto& rgb = window->pen().rgb;
    context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
    if (kind == "levels" || kind == "curve") {
        if (const auto counts = counts_of(*window, page, *layer)) {
            context.histogram = counts->lightness;
            if (kind == "levels") {  // (自動: Auto Tone's records, design 3.3)
                const auto rgb_counts = counts->rgb;
                context.automatic = [rgb_counts]() -> std::optional<Json> {
                    return Json{{"native", Json{{"method", "upstream"}, {"space", "rgb"}, {"channels", core::auto_levels(rgb_counts, "tone")}}}};
                };
            }
        }
    }
    const FilterPreview preview = [window, index, id, kind, area](const std::optional<Json>& values) {
        if (!values) {
            window->preview_ops(std::nullopt);
            return;
        }
        window->preview_ops(Json::array({filter_op(index, id, kind, *values, area)}));
    };
    const auto values = adjust_dialog(window, kind, form->fresh, context, preview);
    if (!values || !window->still(asked)) return;
    window->apply_ops(Json::array({filter_op(index, id, kind, *values, area)}));
}

void run_direct(MainWindow* window, const std::string& kind) {
    const core::Layer* layer = target(window);
    if (layer == nullptr) return;
    const core::Page& page = *window->current_page();
    window->apply_ops(Json::array({filter_op(page.index.json(), layer->id, kind, Json::object(), window->selection_area())}));
}

void run_auto(MainWindow* window, const std::string& mode) {
    const core::Layer* layer = target(window);
    if (layer == nullptr) return;
    const core::Page& page = *window->current_page();
    Json op{{"op", "auto_adjust"}, {"page", page.index.json()}, {"id", layer->id}, {"mode", mode}};
    if (const auto area = window->selection_area()) op["area"] = *area;
    window->apply_ops(Json::array({op}));
}

// ファイル → カラールックアップテーブルを書き出す…: the page's correction layers as a .cube file.
void export_lookup(MainWindow* window) {
    const core::Page* page = window->current_page();
    if (page == nullptr) return;
    const bool any = std::any_of(page->layers.begin(), page->layers.end(), [](const core::Layer& l) { return l.kind == core::LayerKind::Adjust && l.visible; });
    if (!any) {
        window->flash(QStringLiteral("このページには見えている色調補正のレイヤーがありません"), 4000, true);
        return;
    }
    const auto size = ask::get_int(window, QStringLiteral("カラールックアップテーブル"), QStringLiteral("格子の大きさ（2〜256。ふつうは 33）"), 33, 2, 256);
    if (!size) return;
    const QString path = ask::save_path(window, QStringLiteral("カラールックアップテーブルを書き出す"), QStringLiteral("look.cube"), QStringLiteral("3D LUT (*.cube)"));
    if (path.isEmpty()) return;
    try {
        const std::string title = window->book().title.empty() ? std::string("Genko") : window->book().title;
        std::vector<std::string> skipped;
        const std::string cube = formats::bake_cube(window->book(), *page, *size, title, &skipped);
        storage::write_atomic(core::path_from_utf8(path.toStdString()), cube);
        window->flash(skipped.empty() ? QStringLiteral("書き出しました: %1").arg(path)
                                      : QStringLiteral("書き出しました（描けない補正レイヤー %1 枚は入れていません）: %2").arg(skipped.size()).arg(path),
                      5000);
    } catch (const std::exception& error) {
        window->flash(QStringLiteral("書き出せません: %1").arg(QString::fromUtf8(error.what())), 6000, true);
    }
}

}  // namespace

std::vector<FormKind> tone_forms() {
    return {kind_of("levels", QStringLiteral("レベル補正"), levels_form, Json{{"native", Json{{"method", "upstream"}}}}),
            kind_of("curve", QStringLiteral("トーンカーブ"), curve_form, Json{{"native", Json{{"method", "upstream"}}}}),
            kind_of("brightness_contrast", QStringLiteral("明るさ・コントラスト"), brightness_form, Json{{"native", Json{{"method", "modern"}}}}),
            kind_of("exposure", QStringLiteral("露光量"), exposure_form),
            kind_of("posterize", QStringLiteral("階調化"), posterize_form, Json{{"native", Json{{"method", "floor"}}}}),
            kind_of("threshold", QStringLiteral("2 値化"), threshold_form),
            kind_of("invert", QStringLiteral("階調の反転"), invert_form),
            kind_of("gradient_map", QStringLiteral("グラデーションマップ"), gradient_form, Json::object(), gradient_prepare),
            kind_of("color_lookup", QStringLiteral("カラールックアップ"), lookup_form)};
}

void build_tone_menu(QMenu* adjust, const MenuContext& context) {
    MainWindow* window = context.window;
    const auto window_of = [window](const std::string& kind) { return [window, kind] { run_window(window, kind); }; };
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    // (Photoshop's names, the layer panel's 色調補正のレイヤー's the same: the user's decision 8, command_title)
    adjust->addAction(context.make(QStringLiteral("act_adjust_brightness"), command_title("brightness_contrast"), window_of("brightness_contrast"), {}));
    adjust->addAction(context.make(QStringLiteral("act_adjust_levels"), command_title("levels"), window_of("levels"), key("Ctrl+L")));
    adjust->addAction(context.make(QStringLiteral("act_adjust_curves"), command_title("curve"), window_of("curve"), key("Ctrl+M")));
    adjust->addAction(context.make(QStringLiteral("act_adjust_exposure"), command_title("exposure"), window_of("exposure"), {}));
    adjust->addAction(context.make(QStringLiteral("act_adjust_lookup"), command_title("color_lookup"), window_of("color_lookup"), {}));
    adjust->addSeparator();
    adjust->addAction(context.make(QStringLiteral("act_adjust_invert"), command_title("invert"), [window] { run_direct(window, "invert"); }, key("Ctrl+I")));
    adjust->addAction(context.make(QStringLiteral("act_adjust_posterize"), command_title("posterize"), window_of("posterize"), {}));
    adjust->addAction(context.make(QStringLiteral("act_adjust_threshold"), command_title("threshold"), window_of("threshold"), {}));
    adjust->addAction(context.make(QStringLiteral("act_adjust_gradient_map"), command_title("gradient_map"), window_of("gradient_map"), {}));
    adjust->addSeparator();
    adjust->addAction(context.make(QStringLiteral("act_adjust_equalize"), QStringLiteral("平均化（イコライズ）"), [window] { run_direct(window, "equalize"); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_auto_tone"), QStringLiteral("自動トーン補正"), [window] { run_auto(window, "tone"); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_auto_contrast"), QStringLiteral("自動コントラスト"), [window] { run_auto(window, "contrast"); }, {}));
    adjust->addAction(context.make(QStringLiteral("act_auto_color"), QStringLiteral("自動カラー補正"), [window] { run_auto(window, "color"); }, {}));
    // ファイル → カラールックアップテーブルを書き出す… (after 書き出し…)
    QAction* lookup = context.make(QStringLiteral("act_export_lookup"), QStringLiteral("カラールックアップテーブルを書き出す…"), [window] { export_lookup(window); }, {});
    // (ファイル: its own group after 書き出し… and 印刷…, which Python's menu keeps together)
    for (QAction* menu : window->menuBar()->actions()) {
        if (menu->menu() == nullptr || menu->text() != QStringLiteral("ファイル")) continue;
        const QList<QAction*> items = menu->menu()->actions();
        QAction* before = nullptr;
        for (qsizetype i = 0; i < items.size(); ++i) {
            if (items[i]->property("genko_attribute").toString() == QLatin1String("act_print") && i + 2 < items.size() && items[i + 1]->isSeparator())
                before = items[i + 2];
        }
        menu->menu()->insertAction(before, lookup);
        if (before != nullptr) menu->menu()->insertSeparator(before);
        break;
    }
}

}  // namespace genko::app::adjust_forms
