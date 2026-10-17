#include "app/adjust_forms.hpp"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"

namespace genko::app::adjust_forms {

using core::Json;

namespace {

// The value at a key's path ("native.blend") in `json`, or null.
Json at_path(const Json& json, const std::string& key) {
    const Json* here = &json;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        const std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!here->is_object() || !here->contains(part)) return Json();
        here = &(*here)[part];
        if (dot == std::string::npos) return *here;
        start = dot + 1;
    }
}

void put_path(Json& json, const std::string& key, const Json& value) {
    Json* here = &json;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        const std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (dot == std::string::npos) {
            (*here)[part] = value;
            return;
        }
        if (!here->contains(part) || !(*here)[part].is_object()) (*here)[part] = Json::object();
        here = &(*here)[part];
        start = dot + 1;
    }
}

double rounded(double v, int places) {
    const double k = std::pow(10.0, places);
    return std::round(v * k) / k;
}

int places_for(double lo, double hi) { return hi - lo <= 2 ? 3 : hi - lo <= 20 ? 2 : 0; }

QString colour_text(const std::array<int, 3>& rgb) { return QColor(rgb[0], rgb[1], rgb[2]).name(); }

std::array<int, 3> rgb_of(const Json& value, std::array<int, 3> fallback) {
    if (!value.is_array() || value.size() != 3) return fallback;
    std::array<int, 3> out{};
    for (std::size_t c = 0; c < 3; ++c) {
        if (!value[c].is_number()) return fallback;
        out[c] = std::clamp(static_cast<int>(std::lround(value[c].get<double>())), 0, 255);
    }
    return out;
}

}  // namespace

Form::Form(QWidget* parent) : QWidget(parent) {
    form_ = new QFormLayout(this);
    form_->setContentsMargins(0, 0, 0, 0);
    setObjectName(QStringLiteral("adjust_form"));
}

void Form::add(const QString& label, QWidget* widget, Field field) {
    widget->setObjectName(QStringLiteral("field_%1").arg(QString::fromStdString(field.key)));
    form_->addRow(label, widget);
    fields_.push_back(std::move(field));
}

void Form::number(const std::string& key, const QString& label, double lo, double hi, double fallback, int decimals) {
    auto* spin = new QDoubleSpinBox;
    const int places = decimals >= 0 ? decimals : places_for(lo, hi);
    spin->setRange(lo, hi);
    spin->setDecimals(places);
    spin->setSingleStep(places >= 3 ? 0.01 : places == 2 ? 0.1 : 1);
    spin->setValue(fallback);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!quiet_) emit changed();
    });
    add(label, spin,
        Field{key, Json(fallback), [spin, places] { return places == 0 ? Json(static_cast<std::int64_t>(std::lround(spin->value()))) : Json(rounded(spin->value(), places)); },
              [spin, fallback](const Json& v) { spin->setValue(v.is_number() ? v.get<double>() : fallback); }});
}

void Form::choice(const std::string& key, const QString& label, std::vector<std::pair<QString, Json>> options, const Json& fallback) {
    auto* combo = new QComboBox;
    for (const auto& [words, value] : options) combo->addItem(words);
    const auto index_of = [options](const Json& v) {
        for (std::size_t i = 0; i < options.size(); ++i)
            if (options[i].second == v) return static_cast<int>(i);
        return -1;
    };
    combo->setCurrentIndex(std::max(0, index_of(fallback)));
    connect(combo, &QComboBox::currentIndexChanged, this, [this] {
        if (!quiet_) emit changed();
    });
    add(label, combo,
        Field{key, fallback, [combo, options] { return options.empty() ? Json() : options[static_cast<std::size_t>(std::max(0, combo->currentIndex()))].second; },
              [combo, index_of, fallback](const Json& v) {
                  const int at = index_of(v);
                  combo->setCurrentIndex(std::max(0, at >= 0 ? at : index_of(fallback)));
              }});
}

void Form::check(const std::string& key, const QString& label, bool fallback) {
    auto* box = new QCheckBox;
    box->setChecked(fallback);
    connect(box, &QCheckBox::toggled, this, [this] {
        if (!quiet_) emit changed();
    });
    add(label, box,
        Field{key, Json(fallback), [box] { return Json(box->isChecked()); },
              [box, fallback](const Json& v) { box->setChecked(v.is_boolean() ? v.get<bool>() : fallback); }});
}

void Form::colour(const std::string& key, const QString& label, std::array<int, 3> fallback) {
    auto* button = new QPushButton;
    auto now = std::make_shared<std::array<int, 3>>(fallback);
    const auto show = [button, now] {
        button->setText(colour_text(*now));
        button->setStyleSheet(QStringLiteral("background: %1;").arg(colour_text(*now)));
    };
    show();
    connect(button, &QPushButton::clicked, this, [this, button, now, show, label] {
        const auto chosen = ask::colour(button, QColor((*now)[0], (*now)[1], (*now)[2]), label);
        if (!chosen) return;
        *now = {chosen->red(), chosen->green(), chosen->blue()};
        show();
        if (!quiet_) emit changed();
    });
    add(label, button,
        Field{key, Json::array({fallback[0], fallback[1], fallback[2]}), [now] { return Json::array({(*now)[0], (*now)[1], (*now)[2]}); },
              [now, show, fallback](const Json& v) {
                  *now = rgb_of(v, fallback);
                  show();
              }});
}

void Form::custom(const std::string& key, const QString& label, QWidget* widget, std::function<Json()> get, std::function<void(const Json&)> set,
                  const Json& fallback) {
    add(label, widget, Field{key, fallback, std::move(get), std::move(set)});
}

void Form::from_table(const core::AdjustKind& kind, const std::map<std::string, QString>& labels,
                      const std::map<std::string, std::vector<std::pair<QString, Json>>>& choices) {
    using Type = core::AdjustParam::Type;
    const auto field = [&](const core::AdjustParam& p, const std::string& key) {
        const auto label = labels.find(key);
        if (label == labels.end()) return;
        switch (p.type) {
            case Type::Number: number(key, label->second, p.lo, p.hi, p.fallback); break;
            case Type::Whole: number(key, label->second, p.lo, p.hi, p.fallback, 0); break;
            case Type::Bool: check(key, label->second, p.fallback != 0); break;
            case Type::Choice: {
                std::vector<std::pair<QString, Json>> options;
                if (const auto words = choices.find(key); words != choices.end()) {
                    options = words->second;
                } else {
                    for (const std::string_view c : p.choices) options.emplace_back(QString::fromUtf8(c.data(), static_cast<qsizetype>(c.size())), Json(std::string(c)));
                }
                choice(key, label->second, std::move(options), Json(std::string(p.text)));
                break;
            }
            case Type::Rgb:
                colour(key, label->second, p.numbers.size() == 3 ? std::array<int, 3>{static_cast<int>(p.numbers[0]), static_cast<int>(p.numbers[1]), static_cast<int>(p.numbers[2])}
                                                                 : std::array<int, 3>{0, 0, 0});
                break;
            default: break;  // (lists, groups and a group's own: its custom fields)
        }
    };
    for (const core::AdjustParam& p : kind.params) field(p, std::string(p.key));
    for (const core::AdjustParam& p : kind.native) field(p, "native." + std::string(p.key));
}

void Form::note(const QString& text) {
    auto* words = new QLabel(text);
    words->setWordWrap(true);
    words->setProperty("role", "hint");
    form_->addRow(QString(), words);
}

Json Form::values() const {
    Json out = Json::object();
    for (const Field& f : fields_) put_path(out, f.key, f.get());
    return out;
}

void Form::set_values(const Json& now) {
    quiet_ = true;
    for (const Field& f : fields_) {
        const Json given = at_path(now, f.key);
        f.set(given.is_null() ? f.fallback : given);
    }
    quiet_ = false;
    emit changed();
}

void Form::reset() { set_values(Json::object()); }

// --- the kinds with a form -------------------------------------------------------------------------------------------

const std::vector<FormKind>& form_kinds() {
    static const std::vector<FormKind> all = [] {
        std::vector<FormKind> out = tone_forms();
        for (FormKind& each : color_forms()) out.push_back(std::move(each));
        return out;
    }();
    return all;
}

const FormKind* form_kind(std::string_view kind) {
    for (const FormKind& each : form_kinds()) {
        if (each.kind == kind) return &each;
    }
    return nullptr;
}

std::optional<Json> adjust_dialog(QWidget* parent, const std::string& kind, const Json& now, const FormContext& context, FilterPreview preview) {
    const FormKind* found = form_kind(kind);
    if (found == nullptr || !found->build) return std::nullopt;
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("adjust_dialog"));
    dialog.setWindowTitle(found->title);
    auto* layout = new QFormLayout(&dialog);
    auto* form = new Form;
    found->build(*form, context);
    const Json given = now.is_object() ? now : Json::object();
    form->set_values(found->prepare ? found->prepare(given) : given);
    layout->addRow(form);
    auto* row = new QDialogButtonBox;
    QPushButton* reset = row->addButton(QStringLiteral("既定に戻す"), QDialogButtonBox::ResetRole);
    reset->setObjectName(QStringLiteral("adjust_reset"));
    QObject::connect(reset, &QPushButton::clicked, form, &Form::reset);
    if (context.automatic) {
        QPushButton* automatic = row->addButton(QStringLiteral("自動"), QDialogButtonBox::ActionRole);
        automatic->setObjectName(QStringLiteral("adjust_auto"));
        QObject::connect(automatic, &QPushButton::clicked, form, [form, &context] {
            if (const auto settings = context.automatic()) form->set_values(*settings);
        });
    }
    layout->addRow(row);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    QCheckBox* check = nullptr;
    if (preview) {
        check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
        check->setObjectName(QStringLiteral("filter_preview"));
        check->setChecked(true);
        layout->addRow(QString(), check);
        QObject::connect(form, &Form::changed, &dialog, [timer] { timer->start(); });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [timer] { timer->start(); });
        QObject::connect(timer, &QTimer::timeout, &dialog, [&] {
            try {
                preview(check->isChecked() ? std::optional<Json>(form->values()) : std::nullopt);
            } catch (const std::exception&) {
                preview(std::nullopt);  // (a preview that cannot be made must not stop the window)
            }
        });
        timer->start(0);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("かける"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    const bool ok = ask::exec(&dialog) == QDialog::Accepted;
    timer->stop();
    const Json result = form->values();
    if (preview) preview(std::nullopt);
    if (!ok) return std::nullopt;
    return result;
}

}  // namespace genko::app::adjust_forms
