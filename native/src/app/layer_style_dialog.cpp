// PC4 E6 (layer.layerStyle.*, SPEC.md COLOR-09「レイヤースタイル」): the レイヤースタイル window (app/layer_style_dialog.hpp).

#include "app/layer_style_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/layer_dialogs.hpp"
#include "app/patterns_panel.hpp"
#include "core/ink.hpp"
#include "core/layer_basics.hpp"
#include "core/layer_styles.hpp"
#include "core/patterns.hpp"
#include "render/gradient_presets.hpp"
#include "render/pattern_library.hpp"
#include "render/preset_store.hpp"
#include "render/style_presets.hpp"

namespace genko::app {

using core::Json;

namespace {

// The kinds Photoshop's window takes several of (＋).
bool takes_several(const std::string& kind) {
    return kind == "stroke" || kind == "inner_shadow" || kind == "color_overlay" || kind == "gradient_overlay" || kind == "drop_shadow";
}

Json at_path(const Json& e, const std::string& key) {
    const auto dot = key.find('.');
    if (dot == std::string::npos) return e.contains(key) ? e[key] : Json(nullptr);
    const std::string head = key.substr(0, dot);
    if (!e.contains(head) || !e[head].is_object()) return nullptr;
    return at_path(e[head], key.substr(dot + 1));
}

void put_path(Json& e, const std::string& key, const Json& value) {
    const auto dot = key.find('.');
    if (dot == std::string::npos) {
        e[key] = value;
        return;
    }
    Json& inner = e[key.substr(0, dot)];
    if (!inner.is_object()) inner = Json::object();
    put_path(inner, key.substr(dot + 1), value);
}

QString object_name(const std::string& key) {
    std::string n = "style_" + key;
    std::replace(n.begin(), n.end(), '.', '_');
    return QString::fromStdString(n);
}

QColor colour_of(const Json& rgb) {
    if (!rgb.is_array() || rgb.size() != 3) return Qt::black;
    return QColor(rgb[0].get<int>(), rgb[1].get<int>(), rgb[2].get<int>());
}

void paint_button(QPushButton* button, const QColor& c) {
    QPixmap swatch(28, 14);
    swatch.fill(c);
    button->setIcon(QIcon(swatch));
    button->setText(c.name());
}

// A pattern fill in the book's form ({"id", …}) as the window holds it ({"choice": "book:<id>", …}).
Json with_choice(const Json& fill) {
    if (!fill.is_object() || !fill.contains("id")) return fill;
    Json out = fill;
    out["choice"] = "book:" + fill["id"].get<std::string>();
    out.erase("id");
    return out;
}

// The effects with their patterns as `turn` gives them (none: the effect left out, a texture taken off).
Json patterns_turned(const Json& effects, const std::function<std::optional<Json>(const Json&)>& turn) {
    Json out = Json::array();
    for (Json e : effects) {
        if (e.contains("pattern")) {
            const auto p = e["pattern"].is_object() ? turn(e["pattern"]) : std::nullopt;
            if (!p) continue;
            e["pattern"] = *p;
        }
        if (e.contains("texture") && e["texture"].is_object() && e["texture"].contains("pattern")) {
            const auto p = e["texture"]["pattern"].is_object() ? turn(e["texture"]["pattern"]) : std::nullopt;
            if (p) {
                e["texture"]["pattern"] = *p;
            } else {
                e.erase("texture");
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

const std::vector<std::pair<QString, std::string>>& gradient_styles() {
    static const std::vector<std::pair<QString, std::string>> s{{QStringLiteral("線形"), "linear"},
                                                                {QStringLiteral("円形"), "radial"},
                                                                {QStringLiteral("円錐形"), "angle"},
                                                                {QStringLiteral("反射"), "reflected"},
                                                                {QStringLiteral("菱形"), "diamond"}};
    return s;
}

}  // namespace

QString effect_title(const std::string& kind) {
    static const std::map<std::string, QString> titles{{"bevel", QStringLiteral("ベベルとエンボス")},
                                                       {"stroke", QStringLiteral("境界線")},
                                                       {"inner_shadow", QStringLiteral("シャドウ（内側）")},
                                                       {"inner_glow", QStringLiteral("光彩（内側）")},
                                                       {"satin", QStringLiteral("サテン")},
                                                       {"color_overlay", QStringLiteral("カラーオーバーレイ")},
                                                       {"gradient_overlay", QStringLiteral("グラデーションオーバーレイ")},
                                                       {"pattern_overlay", QStringLiteral("パターンオーバーレイ")},
                                                       {"outer_glow", QStringLiteral("光彩（外側）")},
                                                       {"drop_shadow", QStringLiteral("ドロップシャドウ")}};
    const auto it = titles.find(kind);
    return it != titles.end() ? it->second : QStringLiteral("混合オプション");
}

LayerStyleDialog::LayerStyleDialog(std::shared_ptr<const core::Document> book, const core::Page& page, const core::Layer& layer,
                                   const std::string& open_at, QWidget* parent)
    : QDialog(parent), book_(std::move(book)) {
    setObjectName(QStringLiteral("layer_style_dialog"));
    setWindowTitle(QStringLiteral("レイヤースタイル"));
    page_space_ = "rgb";
    if (page.color) {
        switch (page.color->space) {
            case core::InkSpace::Cmyk: page_space_ = "cmyk"; break;
            case core::InkSpace::Lab: page_space_ = "lab"; break;
            default: page_space_ = "gray"; break;
        }
    }
    blend_ = layer.blend.empty() ? std::string("normal") : layer.blend;
    opacity_ = layer.opacity;
    light_ = core::global_light(*book_);
    const Json style = layer.style ? *layer.style : Json{{"effects", Json::array()}};
    if (style.contains("fill_opacity")) fill_ = style["fill_opacity"];
    if (style.contains("blend_if")) blend_if_ = style["blend_if"];
    shown_ = !(style.contains("enabled") && style["enabled"] == Json(false));
    channels_.assign(core::blend_channel_count(page), true);  // (PC5 I-a, blend-channels@1)
    if (layer.blend_channels.size() == channels_.size()) channels_ = layer.blend_channels;
    for (const std::string_view kind : core::kEffectKinds) {
        bool any = false;
        for (const Json& e : style.value("effects", Json::array())) {
            if (e.value("kind", std::string()) != kind) continue;
            Json own = e;
            if (own.contains("pattern")) own["pattern"] = with_choice(own["pattern"]);
            if (own.contains("texture") && own["texture"].is_object() && own["texture"].contains("pattern"))
                own["texture"]["pattern"] = with_choice(own["texture"]["pattern"]);
            rows_.push_back({std::string(kind), own});
            any = true;
        }
        if (!any) rows_.push_back({std::string(kind), nullptr});
    }

    auto* outer = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    outer->addLayout(body, 1);
    auto* left = new QVBoxLayout;
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("layer_style_list"));
    list_->setMinimumWidth(200);
    left->addWidget(list_, 1);
    auto* buttons_row = new QHBoxLayout;
    more_ = new QPushButton(QStringLiteral("＋"));
    more_->setObjectName(QStringLiteral("layer_style_more"));
    more_->setToolTip(QStringLiteral("選んだ効果をもう 1 つ加えます"));
    less_ = new QPushButton(QStringLiteral("−"));
    less_->setObjectName(QStringLiteral("layer_style_less"));
    less_->setToolTip(QStringLiteral("選んだ効果を消します"));
    buttons_row->addWidget(more_);
    buttons_row->addWidget(less_);
    buttons_row->addStretch(1);
    left->addLayout(buttons_row);
    body->addLayout(left);
    page_ = new QScrollArea;
    page_->setWidgetResizable(true);
    page_->setMinimumWidth(360);
    body->addWidget(page_, 1);
    auto* right = new QVBoxLayout;
    swatch_ = new QLabel;
    swatch_->setObjectName(QStringLiteral("layer_style_swatch"));
    swatch_->setFixedSize(96, 96);
    swatch_->setToolTip(QStringLiteral("スタイルの見本"));
    right->addWidget(swatch_);
    right->addStretch(1);
    body->addLayout(right);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(box);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { show_row(row); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (filling_) return;
        const int row = list_->row(item) - 1;
        if (row < 0 || row >= static_cast<int>(rows_.size())) return;
        if (item->checkState() == Qt::Checked) {
            switched_on(row)["enabled"] = true;
        } else if (!rows_[static_cast<std::size_t>(row)].effect.is_null()) {
            rows_[static_cast<std::size_t>(row)].effect["enabled"] = false;
        }
        changed();
    });
    connect(more_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow() - 1;
        if (row < 0 || !takes_several(rows_[static_cast<std::size_t>(row)].kind)) return;
        Row added{rows_[static_cast<std::size_t>(row)].kind, core::default_effect(rows_[static_cast<std::size_t>(row)].kind)};
        rows_.insert(rows_.begin() + row, std::move(added));  // (the new one listed first: on top, as Photoshop's)
        fill_list();
        list_->setCurrentRow(row + 1);
        changed();
    });
    connect(less_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow() - 1;
        if (row < 0) return;
        const std::string kind = rows_[static_cast<std::size_t>(row)].kind;
        const auto same = std::count_if(rows_.begin(), rows_.end(), [&](const Row& r) { return r.kind == kind; });
        if (same > 1) {
            rows_.erase(rows_.begin() + row);
        } else {
            rows_[static_cast<std::size_t>(row)].effect = nullptr;
        }
        fill_list();
        list_->setCurrentRow(std::min(row + 1, list_->count() - 1));
        changed();
    });
    later_.setSingleShot(true);
    later_.setInterval(120);
    connect(&later_, &QTimer::timeout, this, [this] { preview(); });
    fill_list();
    if (open_at.empty()) {
        list_->setCurrentRow(0);
    } else {
        choose(open_at);
    }
    preview();
}

LayerStyleDialog::~LayerStyleDialog() {
    if (worker_ != nullptr) worker_->waitForFinished();
}

void LayerStyleDialog::fill_list() {
    filling_ = true;
    list_->clear();
    list_->addItem(QStringLiteral("混合オプション"));
    for (const Row& r : rows_) {
        auto* item = new QListWidgetItem(effect_title(r.kind) + (takes_several(r.kind) ? QStringLiteral("  ＋") : QString()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(!r.effect.is_null() && r.effect.value("enabled", true) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, QString::fromStdString(r.kind));
        list_->addItem(item);
    }
    filling_ = false;
}

int LayerStyleDialog::row_of(const std::string& kind) const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].kind == kind) return static_cast<int>(i);
    return -1;
}

Json& LayerStyleDialog::switched_on(int row) {
    Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.effect.is_null()) {
        r.effect = core::default_effect(r.kind);
        if (r.kind == "pattern_overlay") {  // (the chosen pattern: the book's first, else the library's)
            QComboBox* box = pattern_combo(*book_, {});
            r.effect["pattern"] = Json{{"choice", box->currentData().toString().toStdString()}, {"scale", 100}, {"angle", 0}, {"link", true}};
            delete box;
        }
        filling_ = true;
        list_->item(row + 1)->setCheckState(Qt::Checked);
        filling_ = false;
    }
    return r.effect;
}

void LayerStyleDialog::choose(const std::string& kind) {
    const int row = row_of(kind);
    if (row < 0) {
        list_->setCurrentRow(0);
        return;
    }
    switched_on(row)["enabled"] = true;
    filling_ = true;
    list_->item(row + 1)->setCheckState(Qt::Checked);
    filling_ = false;
    list_->setCurrentRow(row + 1);
    show_row(row + 1);
    changed();
}

Json LayerStyleDialog::effect(const std::string& kind) const {
    const int row = row_of(kind);
    return row >= 0 ? rows_[static_cast<std::size_t>(row)].effect : Json(nullptr);
}

void LayerStyleDialog::set(const std::string& kind, const std::string& key, const Json& value) {
    const int row = row_of(kind);
    if (row < 0) return;
    put_path(switched_on(row), key, value);
    show_row(list_->currentRow());
    changed();
}

void LayerStyleDialog::set_fill_opacity(double fill) {
    fill_ = fill;
    changed();
}

void LayerStyleDialog::set_blend_if(const Json& blend_if) {
    blend_if_ = blend_if;
    changed();
}

void LayerStyleDialog::show_row(int row) {
    auto* holder = new QWidget;
    auto* form = new QFormLayout(holder);
    if (row <= 0) {
        more_->setEnabled(false);
        less_->setEnabled(false);
        build_blending(form);
    } else if (row - 1 < static_cast<int>(rows_.size())) {
        Row& r = rows_[static_cast<std::size_t>(row - 1)];
        more_->setEnabled(takes_several(r.kind));
        less_->setEnabled(!r.effect.is_null());
        auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(effect_title(r.kind)));
        form->addRow(title);
        if (r.effect.is_null()) {
            form->addRow(new QLabel(QStringLiteral("左のボックスにチェックを入れると、この効果を加えます")));
        } else {
            build_effect(form, r);
        }
    }
    page_->setWidget(holder);
}

void LayerStyleDialog::build_blending(QFormLayout* form) {
    form->addRow(new QLabel(QStringLiteral("<b>混合オプション</b>")));
    auto* mode = new QComboBox;
    mode->setObjectName(QStringLiteral("style_layer_blend"));
    for (const auto& [id, label] : blend_modes()) mode->addItem(label, QString::fromStdString(id));
    mode->setCurrentIndex(std::max(0, mode->findData(QString::fromStdString(blend_))));
    connect(mode, &QComboBox::currentIndexChanged, this, [this, mode] {
        blend_ = mode->currentData().toString().toStdString();
        changed();
    });
    form->addRow(QStringLiteral("描画モード"), mode);
    auto* opacity = new QSpinBox;
    opacity->setObjectName(QStringLiteral("style_layer_opacity"));
    opacity->setRange(0, 100);
    opacity->setSuffix(QStringLiteral(" %"));
    opacity->setValue(static_cast<int>(std::lround(opacity_ * 100)));
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { opacity_ = v / 100.0; });
    form->addRow(QStringLiteral("不透明度"), opacity);
    auto* fill = new QSpinBox;
    fill->setObjectName(QStringLiteral("style_fill_opacity"));
    fill->setRange(0, 100);
    fill->setSuffix(QStringLiteral(" %"));
    fill->setValue(static_cast<int>(std::lround(fill_.get<double>() * 100)));
    connect(fill, &QSpinBox::valueChanged, this, [this](int v) {
        fill_ = v / 100.0;
        changed();
    });
    form->addRow(QStringLiteral("塗りの不透明度"), fill);
    // (PC5 I-a, blend-channels@1: 高度な合成 → チャンネル — the page's colour channels, one left out keeping what is under the layer)
    if (!channels_.empty()) {
        std::vector<QString> names{QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")};
        if (page_space_ == "cmyk") names = {QStringLiteral("C"), QStringLiteral("M"), QStringLiteral("Y"), QStringLiteral("K")};
        if (page_space_ == "lab") names = {QStringLiteral("L"), QStringLiteral("a"), QStringLiteral("b")};
        if (page_space_ == "gray") names = {QStringLiteral("グレー")};
        auto* row = new QHBoxLayout;
        for (std::size_t c = 0; c < channels_.size() && c < names.size(); ++c) {
            auto* box = new QCheckBox(names[c]);
            box->setObjectName(QStringLiteral("style_channel_%1").arg(c));
            box->setChecked(channels_[c]);
            connect(box, &QCheckBox::toggled, this, [this, c](bool on) { channels_[c] = on; });
            row->addWidget(box);
        }
        row->addStretch(1);
        form->addRow(QStringLiteral("チャンネル"), row);
    }
    auto* shown = new QCheckBox(QStringLiteral("効果を表示"));
    shown->setObjectName(QStringLiteral("style_shown"));
    shown->setChecked(shown_);
    connect(shown, &QCheckBox::toggled, this, [this](bool on) {
        shown_ = on;
        changed();
    });
    form->addRow(shown);
    // ブレンド条件 (Blend If): the channel, then this layer's and the underlying layer's black and white points, each split
    std::vector<std::pair<QString, std::string>> channels{{QStringLiteral("グレー"), "gray"}};
    if (page_space_ == "rgb") channels.insert(channels.end(), {{QStringLiteral("レッド"), "red"}, {QStringLiteral("グリーン"), "green"}, {QStringLiteral("ブルー"), "blue"}});
    if (page_space_ == "cmyk")
        channels.insert(channels.end(), {{QStringLiteral("シアン"), "cyan"}, {QStringLiteral("マゼンタ"), "magenta"}, {QStringLiteral("イエロー"), "yellow"}, {QStringLiteral("ブラック"), "black"}});
    if (page_space_ == "lab") channels.insert(channels.end(), {{QStringLiteral("明度"), "lightness"}, {QStringLiteral("a"), "a"}, {QStringLiteral("b"), "b"}});
    auto* channel = new QComboBox;
    channel->setObjectName(QStringLiteral("style_blend_if_channel"));
    for (const auto& [label, id] : channels) channel->addItem(label, QString::fromStdString(id));
    form->addRow(QStringLiteral("ブレンド条件"), channel);
    std::array<QSpinBox*, 8> spins{};
    for (int side = 0; side < 2; ++side) {
        auto* row = new QHBoxLayout;
        for (int k = 0; k < 4; ++k) {
            auto* s = new QSpinBox;
            s->setObjectName(QStringLiteral("style_blend_if_%1_%2").arg(side == 0 ? QStringLiteral("this") : QStringLiteral("under")).arg(k));
            s->setRange(0, 255);
            row->addWidget(s);
            spins[static_cast<std::size_t>(side * 4 + k)] = s;
        }
        form->addRow(side == 0 ? QStringLiteral("これらのレイヤー") : QStringLiteral("下になっているレイヤー"), row);
    }
    const auto load = [this, channel, spins] {
        const std::string id = channel->currentData().toString().toStdString();
        Json now{{"this", Json::array({0, 0, 255, 255})}, {"under", Json::array({0, 0, 255, 255})}};
        for (const Json& e : blend_if_)
            if (e.value("channel", std::string()) == id) now = e;
        for (int k = 0; k < 8; ++k) {
            const QSignalBlocker hold(spins[static_cast<std::size_t>(k)]);
            spins[static_cast<std::size_t>(k)]->setValue(now[k < 4 ? "this" : "under"][static_cast<std::size_t>(k % 4)].get<int>());
        }
    };
    const auto store = [this, channel, spins] {
        const std::string id = channel->currentData().toString().toStdString();
        Json one{{"channel", id}, {"this", Json::array()}, {"under", Json::array()}};
        for (int k = 0; k < 8; ++k) {
            one[k < 4 ? "this" : "under"].push_back(spins[static_cast<std::size_t>(k)]->value());
        }
        for (const char* side : {"this", "under"}) {  // (kept in order: black low ≤ black high ≤ white low ≤ white high)
            Json& r = one[side];
            for (std::size_t k = 1; k < 4; ++k)
                if (r[k].get<int>() < r[k - 1].get<int>()) r[k] = r[k - 1];
        }
        Json kept = Json::array();
        for (const Json& e : blend_if_)
            if (e.value("channel", std::string()) != id) kept.push_back(e);
        if (one["this"] != Json::array({0, 0, 255, 255}) || one["under"] != Json::array({0, 0, 255, 255})) kept.push_back(one);
        blend_if_ = std::move(kept);
        changed();
    };
    load();
    connect(channel, &QComboBox::currentIndexChanged, this, load);
    for (QSpinBox* s : spins) connect(s, &QSpinBox::valueChanged, this, store);
}

void LayerStyleDialog::build_effect(QFormLayout* form, Row& r) {
    const std::size_t index = static_cast<std::size_t>(&r - rows_.data());
    const auto e = [this, index]() -> Json& { return rows_[index].effect; };
    const auto number = [&](const QString& label, const std::string& key, double lo, double hi, int decimals, const QString& suffix, double factor) {
        auto* s = new QDoubleSpinBox;
        s->setObjectName(object_name(key));
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        const Json v = at_path(e(), key);
        s->setValue(v.is_number() ? v.get<double>() * factor : lo);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, e, key, factor](double value) {
            put_path(e(), key, value / factor);
            changed();
        });
        form->addRow(label, s);
    };
    const auto percent = [&](const QString& label, const std::string& key) { number(label, key, 0, 100, 0, QStringLiteral(" %"), 100.0); };
    const auto mm = [&](const QString& label, const std::string& key) {
        number(label, key, 0, core::kEffectMaxMm, 2, QStringLiteral(" mm"), 1.0);
    };
    const auto choice = [&](const QString& label, const std::string& key, const std::vector<std::pair<QString, std::string>>& options, bool rebuild = false) {
        auto* box = new QComboBox;
        box->setObjectName(object_name(key));
        for (const auto& [text, id] : options) box->addItem(text, QString::fromStdString(id));
        const Json v = at_path(e(), key);
        box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(v.is_string() ? v.get<std::string>() : std::string()))));
        connect(box, &QComboBox::currentIndexChanged, this, [this, e, key, box, rebuild] {
            put_path(e(), key, box->currentData().toString().toStdString());
            changed();
            if (rebuild) QTimer::singleShot(0, this, [this] { show_row(list_->currentRow()); });
        });
        form->addRow(label, box);
    };
    const auto blend = [&](const QString& label, const std::string& key) {
        std::vector<std::pair<QString, std::string>> modes;
        for (const auto& [id, text] : blend_modes()) modes.emplace_back(text, id);
        choice(label, key, modes);
    };
    const auto check = [&](const QString& label, const std::string& key, bool rebuild = false) {
        auto* box = new QCheckBox(label);
        box->setObjectName(object_name(key));
        box->setChecked(at_path(e(), key) == Json(true));
        connect(box, &QCheckBox::toggled, this, [this, e, key, rebuild](bool on) {
            put_path(e(), key, on);
            changed();
            if (rebuild) QTimer::singleShot(0, this, [this] { show_row(list_->currentRow()); });
        });
        form->addRow(box);
    };
    const auto colour = [&](const QString& label, const std::string& key) {
        auto* button = new QPushButton;
        button->setObjectName(object_name(key));
        paint_button(button, colour_of(at_path(e(), key)));
        connect(button, &QPushButton::clicked, this, [this, e, key, button, label] {
            const auto picked = ask::colour(this, colour_of(at_path(e(), key)), label);
            if (!picked) return;
            put_path(e(), key, Json::array({picked->red(), picked->green(), picked->blue()}));
            paint_button(button, *picked);
            changed();
        });
        form->addRow(label, button);
    };
    // the angle: the book's global light while the effect uses it (changing it changes the light: Photoshop's 包括光源を使用)
    const auto angle = [&](bool altitude) {
        const bool global = e().value("global_light", false);
        auto* s = new QDoubleSpinBox;
        s->setObjectName(object_name(altitude ? "altitude" : "angle"));
        s->setRange(altitude ? 0 : -360, altitude ? 90 : 360);
        s->setDecimals(1);
        s->setSuffix(QStringLiteral("°"));
        const std::string key = altitude ? "altitude" : "angle";
        s->setValue(global ? light_[altitude ? 1 : 0] : e().value(key, 0.0));
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, e, key, altitude](double v) {
            if (e().value("global_light", false)) {
                light_[altitude ? 1 : 0] = v;
            } else {
                e()[key] = v;
            }
            changed();
        });
        form->addRow(altitude ? QStringLiteral("高度") : QStringLiteral("角度"), s);
    };
    const auto pattern = [&](const std::string& key) {
        Json now = at_path(e(), key);
        if (!now.is_object()) now = Json{{"scale", 100}, {"angle", 0}, {"link", true}};
        QComboBox* box = pattern_combo(*book_, QString::fromStdString(now.value("choice", std::string())));
        box->setObjectName(object_name(key + ".choice"));
        if (!now.contains("choice") && box->count() > 0) {
            now["choice"] = box->currentData().toString().toStdString();
            put_path(e(), key, now);
        }
        connect(box, &QComboBox::currentIndexChanged, this, [this, e, key, box] {
            put_path(e(), key + ".choice", box->currentData().toString().toStdString());
            changed();
        });
        form->addRow(QStringLiteral("パターン"), box);
        number(QStringLiteral("比率"), key + ".scale", core::kPatternScaleMin, core::kPatternScaleMax, 0, QStringLiteral(" %"), 1.0);
        number(QStringLiteral("角度"), key + ".angle", -360, 360, 1, QStringLiteral("°"), 1.0);
        check(QStringLiteral("レイヤーにリンク"), key + ".link");
    };
    const auto gradient = [&](const std::string& key) {
        auto* box = new QComboBox;
        box->setObjectName(object_name(key + ".preset"));
        box->addItem(QStringLiteral("（今のグラデーション）"), QString());
        const render::preset_store::Library lib = render::preset_store::load(config_dir(), render::gradient_presets::kind());
        for (const render::preset_store::Group& g : lib.groups)
            for (const Json& item : g.items) box->addItem(QString::fromStdString(item.value("name", std::string())), QString::fromStdString(item.value("name", std::string())));
        connect(box, &QComboBox::currentIndexChanged, this, [this, e, key, box] {
            const std::string name = box->currentData().toString().toStdString();
            if (name.empty()) return;
            const render::preset_store::Library now = render::preset_store::load(config_dir(), render::gradient_presets::kind());
            const Json* item = render::preset_store::find(now.groups, name);
            if (item == nullptr) return;
            const Json made = render::gradient_presets::stops_for(*item, Json::array({0, 0, 0}), Json::array({255, 255, 255}), true);
            Json stops = Json::array();
            for (const Json& s : made["stops"]) stops.push_back(Json::array({s[0], s[1]}));
            Json g = at_path(e(), key);
            if (!g.is_object()) g = Json::object();
            g["stops"] = std::move(stops);
            if (made.contains("transparency")) {
                g["transparency"] = made["transparency"];
            } else {
                g.erase("transparency");
            }
            put_path(e(), key, g);
            changed();
        });
        form->addRow(QStringLiteral("グラデーション"), box);
        check(QStringLiteral("逆方向"), key + ".reverse");
        choice(QStringLiteral("スタイル"), key + ".style", gradient_styles());
        check(QStringLiteral("レイヤーに揃える"), key + ".align");
        number(QStringLiteral("角度"), key + ".angle", -360, 360, 1, QStringLiteral("°"), 1.0);
        number(QStringLiteral("比率"), key + ".scale", 10, 150, 0, QStringLiteral(" %"), 100.0);
    };
    // the paint of a stroke or a glow: a colour, a gradient or (a stroke's) a pattern
    const auto paint = [&](bool patterned) {
        const std::string now = e().contains("gradient") ? "gradient" : e().contains("pattern") ? "pattern" : "rgb";
        auto* box = new QComboBox;
        box->setObjectName(QStringLiteral("style_paint"));
        box->addItem(QStringLiteral("カラー"), QStringLiteral("rgb"));
        box->addItem(QStringLiteral("グラデーション"), QStringLiteral("gradient"));
        if (patterned) box->addItem(QStringLiteral("パターン"), QStringLiteral("pattern"));
        box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(now))));
        connect(box, &QComboBox::currentIndexChanged, this, [this, e, box] {
            const std::string to = box->currentData().toString().toStdString();
            Json& eff = e();
            for (const char* k : {"rgb", "gradient", "pattern"}) eff.erase(k);
            if (to == "rgb") eff["rgb"] = Json::array({0, 0, 0});
            if (to == "gradient") eff["gradient"] = core::default_effect("gradient_overlay")["gradient"];
            if (to == "pattern") {
                QComboBox* first = pattern_combo(*book_, {});
                eff["pattern"] = Json{{"choice", first->currentData().toString().toStdString()}, {"scale", 100}, {"angle", 0}, {"link", true}};
                delete first;
            }
            changed();
            QTimer::singleShot(0, this, [this] { show_row(list_->currentRow()); });
        });
        form->addRow(QStringLiteral("塗りつぶしタイプ"), box);
        if (now == "rgb") colour(QStringLiteral("カラー"), "rgb");
        if (now == "gradient") gradient("gradient");
        if (now == "pattern") pattern("pattern");
    };

    const std::string& kind = r.kind;
    if (kind == "drop_shadow" || kind == "inner_shadow") {
        blend(QStringLiteral("描画モード"), "blend");
        colour(QStringLiteral("カラー"), "rgb");
        percent(QStringLiteral("不透明度"), "opacity");
        angle(false);
        check(QStringLiteral("包括光源を使用"), "global_light", true);
        mm(QStringLiteral("距離"), "distance_mm");
        percent(kind == "drop_shadow" ? QStringLiteral("スプレッド") : QStringLiteral("チョーク"), kind == "drop_shadow" ? "spread" : "choke");
        mm(QStringLiteral("サイズ"), "size_mm");
        percent(QStringLiteral("ノイズ"), "noise");
        if (kind == "drop_shadow") check(QStringLiteral("レイヤーがドロップシャドウをノックアウト"), "knocks_out");
    } else if (kind == "outer_glow" || kind == "inner_glow") {
        blend(QStringLiteral("描画モード"), "blend");
        percent(QStringLiteral("不透明度"), "opacity");
        percent(QStringLiteral("ノイズ"), "noise");
        paint(false);
        choice(QStringLiteral("テクニック"), "technique", {{QStringLiteral("さらにソフトに"), "softer"}, {QStringLiteral("精細"), "precise"}});
        if (kind == "inner_glow") choice(QStringLiteral("ソース"), "source", {{QStringLiteral("エッジ"), "edge"}, {QStringLiteral("中央"), "center"}});
        percent(kind == "outer_glow" ? QStringLiteral("スプレッド") : QStringLiteral("チョーク"), kind == "outer_glow" ? "spread" : "choke");
        mm(QStringLiteral("サイズ"), "size_mm");
        percent(QStringLiteral("範囲"), "range");
        percent(QStringLiteral("適用度"), "jitter");
    } else if (kind == "bevel") {
        choice(QStringLiteral("スタイル"), "style",
               {{QStringLiteral("ベベル（内側）"), "inner"}, {QStringLiteral("ベベル（外側）"), "outer"}, {QStringLiteral("エンボス"), "emboss"},
                {QStringLiteral("ピローエンボス"), "pillow"}, {QStringLiteral("エンボスの境界線"), "stroke"}});
        choice(QStringLiteral("テクニック"), "technique",
               {{QStringLiteral("滑らかに"), "smooth"}, {QStringLiteral("シゼルハード"), "chisel_hard"}, {QStringLiteral("シゼルソフト"), "chisel_soft"}});
        number(QStringLiteral("深さ"), "depth", 1, 1000, 0, QStringLiteral(" %"), 100.0);
        choice(QStringLiteral("方向"), "direction", {{QStringLiteral("上へ"), "up"}, {QStringLiteral("下へ"), "down"}});
        mm(QStringLiteral("サイズ"), "size_mm");
        mm(QStringLiteral("ソフト"), "soften_mm");
        angle(false);
        angle(true);
        check(QStringLiteral("包括光源を使用"), "global_light", true);
        blend(QStringLiteral("ハイライトのモード"), "highlight.blend");
        colour(QStringLiteral("ハイライトのカラー"), "highlight.rgb");
        percent(QStringLiteral("ハイライトの不透明度"), "highlight.opacity");
        blend(QStringLiteral("シャドウのモード"), "shadow.blend");
        colour(QStringLiteral("シャドウのカラー"), "shadow.rgb");
        percent(QStringLiteral("シャドウの不透明度"), "shadow.opacity");
        // テクスチャ: on with the chosen pattern, its depth and 反転
        auto* textured = new QCheckBox(QStringLiteral("テクスチャ"));
        textured->setObjectName(QStringLiteral("style_texture"));
        textured->setChecked(e().contains("texture"));
        connect(textured, &QCheckBox::toggled, this, [this, e](bool on) {
            if (on) {
                QComboBox* first = pattern_combo(*book_, {});
                e()["texture"] = Json{{"pattern", Json{{"choice", first->currentData().toString().toStdString()}, {"scale", 100}, {"angle", 0}, {"link", true}}},
                                      {"depth", 1},
                                      {"invert", false}};
                delete first;
            } else {
                e().erase("texture");
            }
            changed();
            QTimer::singleShot(0, this, [this] { show_row(list_->currentRow()); });
        });
        form->addRow(textured);
        if (e().contains("texture")) {
            pattern("texture.pattern");
            number(QStringLiteral("テクスチャの深さ"), "texture.depth", -1000, 1000, 0, QStringLiteral(" %"), 100.0);
            check(QStringLiteral("テクスチャを反転"), "texture.invert");
        }
    } else if (kind == "satin") {
        blend(QStringLiteral("描画モード"), "blend");
        colour(QStringLiteral("カラー"), "rgb");
        percent(QStringLiteral("不透明度"), "opacity");
        number(QStringLiteral("角度"), "angle", -360, 360, 1, QStringLiteral("°"), 1.0);
        mm(QStringLiteral("距離"), "distance_mm");
        mm(QStringLiteral("サイズ"), "size_mm");
        check(QStringLiteral("反転"), "invert");
    } else if (kind == "color_overlay") {
        blend(QStringLiteral("描画モード"), "blend");
        colour(QStringLiteral("カラー"), "rgb");
        percent(QStringLiteral("不透明度"), "opacity");
    } else if (kind == "gradient_overlay") {
        blend(QStringLiteral("描画モード"), "blend");
        percent(QStringLiteral("不透明度"), "opacity");
        gradient("gradient");
        check(QStringLiteral("ディザ"), "dither");
    } else if (kind == "pattern_overlay") {
        blend(QStringLiteral("描画モード"), "blend");
        percent(QStringLiteral("不透明度"), "opacity");
        pattern("pattern");
    } else if (kind == "stroke") {
        mm(QStringLiteral("サイズ"), "size_mm");
        choice(QStringLiteral("位置"), "position", {{QStringLiteral("外側"), "outside"}, {QStringLiteral("内側"), "inside"}, {QStringLiteral("中央"), "center"}});
        blend(QStringLiteral("描画モード"), "blend");
        percent(QStringLiteral("不透明度"), "opacity");
        paint(true);
    }
}

void LayerStyleDialog::changed() {
    if (filling_) return;
    later_.start();
}

Json LayerStyleDialog::style() const {
    Json effects = Json::array();
    for (const Row& r : rows_)
        if (!r.effect.is_null()) effects.push_back(r.effect);
    // (a pattern: the book's by its id, a library's as its picture with its placement — set_layer_style puts it in)
    effects = patterns_turned(effects, [this](const Json& fill) -> std::optional<Json> {
        const QString choice = QString::fromStdString(fill.value("choice", std::string()));
        Json place = fill;
        place.erase("choice");
        if (choice.startsWith(QLatin1String("book:"))) {
            place["id"] = choice.mid(5).toStdString();
            return place;
        }
        const auto chosen = pattern_chosen(*book_, choice);
        if (!chosen) return std::nullopt;
        Json put = render::pattern_library::add_op(chosen->first, chosen->second);
        put.erase("op");
        for (const auto& [key, value] : place.items()) put[key] = value;
        return put;
    });
    Json out{{"effects", std::move(effects)}};
    if (fill_.get<double>() < 1.0) out["fill_opacity"] = fill_;
    if (!blend_if_.empty()) out["blend_if"] = blend_if_;
    if (!shown_) out["enabled"] = false;
    return out;
}

std::vector<bool> LayerStyleDialog::channels() const {
    if (std::all_of(channels_.begin(), channels_.end(), [](bool on) { return on; })) return {};
    return channels_;
}

void LayerStyleDialog::set_channel(std::size_t channel, bool on) {
    if (channel < channels_.size()) channels_[channel] = on;
}

void LayerStyleDialog::preview() {
    if (worker_ != nullptr && worker_->isRunning()) {
        later_.start();  // (one at a time: again when this one is done)
        return;
    }
    // (the swatch's own book: the patterns the style names, the book's and the library's)
    auto mini = std::make_shared<core::Document>();
    Json effects = Json::array();
    for (const Row& r : rows_)
        if (!r.effect.is_null()) effects.push_back(r.effect);
    effects = patterns_turned(effects, [this, &mini](const Json& fill) -> std::optional<Json> {
        const auto chosen = pattern_chosen(*book_, QString::fromStdString(fill.value("choice", std::string())));
        if (!chosen) return std::nullopt;
        try {
            core::Pattern p = chosen->first;
            p.id = core::next_pattern_id(*mini);
            Json place = fill;
            place.erase("choice");
            place["id"] = core::carry_pattern(*mini, p, chosen->second).first;
            return place;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    });
    Json style{{"effects", std::move(effects)}};
    if (fill_.get<double>() < 1.0) style["fill_opacity"] = fill_;
    if (!shown_) style["enabled"] = false;
    if (worker_ == nullptr) {
        worker_ = new QFutureWatcher<QImage>(this);
        connect(worker_, &QFutureWatcher<QImage>::finished, this, [this] { swatch_->setPixmap(QPixmap::fromImage(worker_->result())); });
    }
    worker_->setFuture(QtConcurrent::run([mini, style, blend = blend_, light = light_]() -> QImage {
        try {
            const render::Image image = render::style_presets::swatch(style, mini.get(), 96, blend, light);
            const std::string bytes = image.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 4, QImage::Format_RGBA8888).copy();
        } catch (const std::exception&) {
            return QImage();
        }
    }));
}

}  // namespace genko::app
