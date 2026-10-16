#include "app/color_settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/look.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/paths.hpp"
#include "render/ink.hpp"

namespace genko::app::color_settings {

namespace {

const QString kPick = QStringLiteral("?file");

QString text_of(const QSettings& store, const char* key, const QString& fallback) {
    return store.value(QString::fromLatin1(key), fallback).toString();
}

bool flag_of(const QSettings& store, const char* key, bool fallback) {
    const QString value = store.value(QString::fromLatin1(key), fallback ? QStringLiteral("true") : QStringLiteral("false")).toString();
    return value == QLatin1String("true") || value == QLatin1String("1");
}

bool is_policy(const QString& p) { return p == QLatin1String("preserve") || p == QLatin1String("convert") || p == QLatin1String("off"); }

bool is_intent(const QString& i) {
    return i == QLatin1String("perceptual") || i == QLatin1String("relative") || i == QLatin1String("saturation") || i == QLatin1String("absolute");
}

core::Bytes read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray bytes = file.read(static_cast<qint64>(core::kProfileMaxBytes) + 1);
    if (bytes.isEmpty() || bytes.size() > static_cast<qsizetype>(core::kProfileMaxBytes)) return nullptr;
    return std::make_shared<const std::string>(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

core::ProfileSpace profile_space(Space space) {
    return space == Space::Rgb ? core::ProfileSpace::Rgb : space == Space::Gray ? core::ProfileSpace::Gray : core::ProfileSpace::Cmyk;
}

QString builtin_name(const core::BuiltinProfile& b) { return QString::fromUtf8(b.description.data(), static_cast<qsizetype>(b.description.size())); }

// The built-in profiles of a space in the box, then the files chosen, then ファイル… (and for CMYK the export choice).
void fill(QComboBox* box, Space space, const QString& now) {
    box->clear();
    for (const core::BuiltinProfile& b : core::builtin_profiles()) {
        if (b.space != profile_space(space)) continue;
        box->addItem(builtin_name(b), QString::fromUtf8(b.id.data(), static_cast<qsizetype>(b.id.size())));
    }
    if (space == Space::Cmyk) box->addItem(QStringLiteral("書き出しの ICC・Genko の変換（CMYK で見る）"), QStringLiteral("export"));
    if (box->findData(now) < 0 && !now.isEmpty()) box->addItem(QFileInfo(now).fileName(), now);
    box->addItem(QStringLiteral("ファイル…"), kPick);
    box->setCurrentIndex(std::max(0, box->findData(now)));
}

// ファイル… chosen: an ICC file of the space asked for and put in the box (its own name), or the choice before kept.
void watch_pick(QComboBox* box, Space space, QWidget* parent) {
    auto previous = std::make_shared<int>(box->currentIndex());
    QObject::connect(box, &QComboBox::currentIndexChanged, parent, [box, space, parent, previous](int index) {
        if (box->itemData(index).toString() != kPick) {
            *previous = index;
            return;
        }
        const QString path = ask::open_path(parent, QStringLiteral("ICC プロファイル"), QStringLiteral("ICC プロファイル (*.icc *.icm)"));
        const core::Bytes bytes = path.isEmpty() ? nullptr : read_file(path);
        const auto found = bytes ? render::ink::profile_space(*bytes) : std::nullopt;
        const QSignalBlocker quiet(box);
        if (!found || *found != profile_space(space)) {
            if (!path.isEmpty()) ask::warning(parent, QStringLiteral("カラー設定"), QStringLiteral("このファイルはこの色空間の ICC プロファイルとして使えません。"));
            box->setCurrentIndex(*previous);
            return;
        }
        int at = box->findData(path);
        if (at < 0) {
            box->insertItem(index, QFileInfo(path).fileName(), path);
            at = index;
        }
        box->setCurrentIndex(at);
        *previous = at;
    });
}

QComboBox* policy_box() {
    auto* box = new QComboBox;
    box->addItem(QStringLiteral("埋め込まれたプロファイルを使う"), QStringLiteral("preserve"));
    box->addItem(QStringLiteral("作業用の空間に変換する"), QStringLiteral("convert"));
    box->addItem(QStringLiteral("プロファイルを外す（作業用の空間とみなす）"), QStringLiteral("off"));
    return box;
}

}  // namespace

Settings load() {
    const auto store = settings();
    Settings s;
    const Settings d;
    s.working_rgb = text_of(*store, "color/workingRgb", d.working_rgb);
    s.working_cmyk = text_of(*store, "color/workingCmyk", d.working_cmyk);
    s.working_gray = text_of(*store, "color/workingGray", d.working_gray);
    s.policy_rgb = text_of(*store, "color/policyRgb", d.policy_rgb);
    s.policy_cmyk = text_of(*store, "color/policyCmyk", d.policy_cmyk);
    s.policy_gray = text_of(*store, "color/policyGray", d.policy_gray);
    for (QString* p : {&s.policy_rgb, &s.policy_cmyk, &s.policy_gray})
        if (!is_policy(*p)) *p = QStringLiteral("preserve");
    s.ask_mismatch = flag_of(*store, "color/askOnMismatch", d.ask_mismatch);
    s.ask_paste = flag_of(*store, "color/askOnPaste", d.ask_paste);
    s.ask_missing = flag_of(*store, "color/askOnMissing", d.ask_missing);
    s.intent = text_of(*store, "color/intent", d.intent);
    if (!is_intent(s.intent)) s.intent = d.intent;
    s.bpc = flag_of(*store, "color/bpc", d.bpc);
    s.dither = flag_of(*store, "color/dither", d.dither);
    s.monitor = text_of(*store, "color/monitorProfile", d.monitor);
    return s;
}

void save(const Settings& s) {
    const auto store = settings();
    const auto flag = [](bool on) { return on ? QStringLiteral("true") : QStringLiteral("false"); };
    store->setValue(QStringLiteral("color/workingRgb"), s.working_rgb);
    store->setValue(QStringLiteral("color/workingCmyk"), s.working_cmyk);
    store->setValue(QStringLiteral("color/workingGray"), s.working_gray);
    store->setValue(QStringLiteral("color/policyRgb"), s.policy_rgb);
    store->setValue(QStringLiteral("color/policyCmyk"), s.policy_cmyk);
    store->setValue(QStringLiteral("color/policyGray"), s.policy_gray);
    store->setValue(QStringLiteral("color/askOnMismatch"), flag(s.ask_mismatch));
    store->setValue(QStringLiteral("color/askOnPaste"), flag(s.ask_paste));
    store->setValue(QStringLiteral("color/askOnMissing"), flag(s.ask_missing));
    store->setValue(QStringLiteral("color/intent"), s.intent);
    store->setValue(QStringLiteral("color/bpc"), flag(s.bpc));
    store->setValue(QStringLiteral("color/dither"), flag(s.dither));
    store->setValue(QStringLiteral("color/monitorProfile"), s.monitor);
}

std::optional<std::filesystem::path> export_icc() {
    const QString chosen = settings()->value(QStringLiteral("color/icc")).toString();
    if (chosen.isEmpty() || !QFileInfo(chosen).isFile()) return std::nullopt;
    return core::path_from_utf8(chosen.toStdString());
}

std::string ref_of(const core::Bytes& icc) { return icc ? core::bytes_ref(*icc) : std::string(); }

Working working(Space space, const Settings& s) {
    Working out;
    out.spec = space == Space::Rgb ? s.working_rgb : space == Space::Gray ? s.working_gray : s.working_cmyk;
    const QString fallback = space == Space::Rgb ? QStringLiteral("srgb") : space == Space::Gray ? QStringLiteral("sgray") : QStringLiteral("coated-cmyk");
    if (space == Space::Cmyk && out.spec == QLatin1String("export")) {
        const auto path = export_icc();
        out.icc = path ? read_file(QString::fromStdString(core::path_to_utf8(*path))) : nullptr;
        out.usable = out.icc && render::ink::profile_space(*out.icc) == core::ProfileSpace::Cmyk;
        if (!out.usable) out.icc = nullptr;
        out.name = out.icc ? QString::fromStdString(core::icc_description(*out.icc)) : QStringLiteral("Genko の変換（書き出しの ICC なし）");
        return out;
    }
    const core::BuiltinProfile* b = core::builtin_profile(out.spec.toStdString());
    if (b != nullptr && b->space == profile_space(space)) {
        out.builtin = QString::fromUtf8(b->id.data(), static_cast<qsizetype>(b->id.size()));
        out.icc = b->bytes;
        out.is_default = out.builtin == fallback;
        out.name = builtin_name(*b);
        return out;
    }
    out.icc = read_file(out.spec);
    out.usable = out.icc && render::ink::profile_space(*out.icc) == profile_space(space);
    if (!out.usable) {  // (a file gone or not of its space: the built-in default, said by its name)
        const core::BuiltinProfile* d = core::builtin_profile(fallback.toStdString());
        out.icc = d->bytes;
        out.builtin = fallback;
        out.is_default = true;
        out.name = builtin_name(*d);
        out.usable = true;
        return out;
    }
    out.name = QString::fromStdString(core::icc_description(*out.icc));
    if (out.name.isEmpty()) out.name = QFileInfo(out.spec).fileName();
    return out;
}

void put_profile(core::Json& op, const Working& w) {
    if (!w.builtin.isEmpty()) op["profile"] = w.builtin.toStdString();
    else if (w.icc) op["icc"] = core::b64encode(*w.icc);
}

ColorSettingsDialog::ColorSettingsDialog(QWidget* parent, const Settings& s, std::optional<double> text_gamma, bool book) : QDialog(parent) {
    setObjectName(QStringLiteral("color_settings_dialog"));
    setWindowTitle(QStringLiteral("カラー設定"));
    working_rgb = new QComboBox;
    working_cmyk = new QComboBox;
    working_gray = new QComboBox;
    policy_rgb = policy_box();
    policy_cmyk = policy_box();
    policy_gray = policy_box();
    ask_mismatch = new QCheckBox(QStringLiteral("開くとき、プロファイルが違えば聞く"));
    ask_paste = new QCheckBox(QStringLiteral("画像を読み込むとき、プロファイルが違えば聞く"));
    ask_missing = new QCheckBox(QStringLiteral("画像を読み込むとき、プロファイルが無ければ聞く"));
    intent = new QComboBox;
    intent->addItem(QStringLiteral("相対的な色域を維持"), QStringLiteral("relative"));
    intent->addItem(QStringLiteral("知覚的"), QStringLiteral("perceptual"));
    intent->addItem(QStringLiteral("彩度"), QStringLiteral("saturation"));
    intent->addItem(QStringLiteral("絶対的な色域を維持"), QStringLiteral("absolute"));
    bpc = new QCheckBox(QStringLiteral("黒点の補正"));
    dither = new QCheckBox(QStringLiteral("8 bit に下げるときディザをかける"));
    gamma_on = new QCheckBox(QStringLiteral("この原稿の文字の色をガンマで混ぜる"));
    gamma_on->setToolTip(QStringLiteral("台詞・文字のふちを、下の色とこのガンマで混ぜます（Photoshop の既定は 1.45）。原稿に残り、どこで描いても同じになります"));
    gamma = new QDoubleSpinBox;
    gamma->setRange(1.01, core::kTextGammaMax);
    gamma->setDecimals(2);
    gamma->setSingleStep(0.05);
    gamma->setValue(text_gamma.value_or(1.45));
    gamma_on->setChecked(text_gamma.has_value());
    gamma->setEnabled(book && text_gamma.has_value());
    gamma_on->setEnabled(book);
    connect(gamma_on, &QCheckBox::toggled, gamma, &QWidget::setEnabled);
    monitor = new QComboBox;
    reset = new QPushButton(QStringLiteral("初期設定に戻す"));
    show_settings(s);
    watch_pick(working_rgb, Space::Rgb, this);
    watch_pick(working_cmyk, Space::Cmyk, this);
    watch_pick(working_gray, Space::Gray, this);
    watch_pick(monitor, Space::Rgb, this);
    connect(reset, &QPushButton::clicked, this, [this] { show_settings(Settings{}); });
    for (auto [name, widget] : std::initializer_list<std::pair<const char*, QWidget*>>{
             {"working_rgb", working_rgb}, {"working_cmyk", working_cmyk}, {"working_gray", working_gray}, {"policy_rgb", policy_rgb},
             {"policy_cmyk", policy_cmyk}, {"policy_gray", policy_gray}, {"ask_mismatch", ask_mismatch}, {"ask_paste", ask_paste},
             {"ask_missing", ask_missing}, {"intent", intent}, {"bpc", bpc}, {"dither", dither}, {"gamma_on", gamma_on}, {"gamma", gamma},
             {"monitor", monitor}, {"reset", reset}}) {
        widget->setObjectName(QStringLiteral("color_%1").arg(QString::fromLatin1(name)));
    }
    auto* form = new QFormLayout;
    form->addRow(look::section(QStringLiteral("作業用の空間")));
    form->addRow(QStringLiteral("RGB"), working_rgb);
    form->addRow(QStringLiteral("CMYK"), working_cmyk);
    form->addRow(QStringLiteral("グレー"), working_gray);
    form->addRow(look::section(QStringLiteral("カラーマネジメントの方針（開く原稿・読み込む画像）")));
    form->addRow(QStringLiteral("RGB"), policy_rgb);
    form->addRow(QStringLiteral("CMYK"), policy_cmyk);
    form->addRow(QStringLiteral("グレー"), policy_gray);
    form->addRow(QString(), ask_mismatch);
    form->addRow(QString(), ask_paste);
    form->addRow(QString(), ask_missing);
    form->addRow(look::section(QStringLiteral("変換のオプション")));
    form->addRow(QStringLiteral("変換の意図"), intent);
    form->addRow(QString(), bpc);
    form->addRow(QString(), dither);
    form->addRow(look::section(QStringLiteral("詳細")));
    auto* gamma_row = new QWidget;
    auto* gamma_line = new QHBoxLayout(gamma_row);
    gamma_line->setContentsMargins(0, 0, 0, 0);
    gamma_line->addWidget(gamma_on);
    gamma_line->addWidget(gamma);
    gamma_line->addStretch(1);
    form->addRow(QString(), gamma_row);
    form->addRow(QStringLiteral("モニターのプロファイル"), monitor);
    auto* note = new QLabel(QStringLiteral("このパソコンでの色の扱いです（文字のガンマのほかは原稿に残りません）。変換は操作として行い、取り消せます。"));
    note->setWordWrap(true);
    form->addRow(note);
    look::quiet_labels(form);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    buttons->addButton(reset, QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* body = new QWidget;
    body->setLayout(form);
    look::frame(this, look::header(QStringLiteral("カラー設定"), QStringLiteral("作業用の空間・プロファイルの方針・変換・モニター")), body, nullptr,
                look::footer(buttons));
}

void ColorSettingsDialog::show_settings(const Settings& s) {
    for (QComboBox* box : {working_rgb, working_cmyk, working_gray, monitor}) box->blockSignals(true);
    fill(working_rgb, Space::Rgb, s.working_rgb);
    fill(working_cmyk, Space::Cmyk, s.working_cmyk);
    fill(working_gray, Space::Gray, s.working_gray);
    monitor->clear();
    monitor->addItem(QStringLiteral("自動（システムのモニターのプロファイル、無ければ sRGB）"), QStringLiteral("auto"));
    for (const char* id : {"srgb", "display-p3", "adobe-rgb-compat", "prophoto-compat", "rec2020"}) {
        const core::BuiltinProfile* b = core::builtin_profile(id);
        monitor->addItem(builtin_name(*b), QString::fromLatin1(id));
    }
    if (monitor->findData(s.monitor) < 0 && !s.monitor.isEmpty()) monitor->addItem(QFileInfo(s.monitor).fileName(), s.monitor);
    monitor->addItem(QStringLiteral("ファイル…"), kPick);
    monitor->setCurrentIndex(std::max(0, monitor->findData(s.monitor)));
    for (QComboBox* box : {working_rgb, working_cmyk, working_gray, monitor}) box->blockSignals(false);
    for (const auto& [box, value] : {std::pair{policy_rgb, s.policy_rgb}, {policy_cmyk, s.policy_cmyk}, {policy_gray, s.policy_gray}})
        box->setCurrentIndex(std::max(0, box->findData(value)));
    ask_mismatch->setChecked(s.ask_mismatch);
    ask_paste->setChecked(s.ask_paste);
    ask_missing->setChecked(s.ask_missing);
    intent->setCurrentIndex(std::max(0, intent->findData(s.intent)));
    bpc->setChecked(s.bpc);
    dither->setChecked(s.dither);
}

Settings ColorSettingsDialog::settings() const {
    Settings s;
    s.working_rgb = working_rgb->currentData().toString();
    s.working_cmyk = working_cmyk->currentData().toString();
    s.working_gray = working_gray->currentData().toString();
    s.policy_rgb = policy_rgb->currentData().toString();
    s.policy_cmyk = policy_cmyk->currentData().toString();
    s.policy_gray = policy_gray->currentData().toString();
    s.ask_mismatch = ask_mismatch->isChecked();
    s.ask_paste = ask_paste->isChecked();
    s.ask_missing = ask_missing->isChecked();
    s.intent = intent->currentData().toString();
    s.bpc = bpc->isChecked();
    s.dither = dither->isChecked();
    s.monitor = monitor->currentData().toString();
    return s;
}

std::optional<double> ColorSettingsDialog::text_gamma() const {
    if (!gamma_on->isChecked()) return std::nullopt;
    return gamma->value();
}

}  // namespace genko::app::color_settings
