// PC4 F-e: Camera Raw's window for a camera RAW file (app/camera_raw_open.hpp). The words are Adobe Camera Raw's (日本語版);
// the panel is 節 D's (app/camera_raw_window), the development render/raw's (LibRaw).

#include "app/camera_raw_open.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <stop_token>
#include <vector>

#include "app/ask.hpp"
#include "app/camera_raw_window.hpp"
#include "app/config.hpp"
#include "app/look.hpp"
#include "core/pyops.hpp"
#include "core/raw_develop.hpp"
#include "render/page.hpp"
#include "render/raw.hpp"
#include "service/wording.hpp"

namespace genko::app::camera_raw_open {

using core::Json;

// --- 環境設定 → Camera Raw, and what 完了 remembers ---------------------------------------------------------------------

namespace {

bool truthy(const QVariant& v) {
    const QString text = v.toString().toLower();
    return text == QLatin1String("true") || text == QLatin1String("1");
}

template <std::size_t N>
std::string one_of(const QString& value, const std::array<std::string_view, N>& set, std::string_view fallback) {
    const std::string text = value.toStdString();
    return std::find(set.begin(), set.end(), text) != set.end() ? text : std::string(fallback);
}

constexpr int kRememberedMost = 100;

}  // namespace

Workflow workflow() {
    const auto store = settings();
    Workflow w;
    w.space = one_of(store->value(QStringLiteral("raw/space"), QStringLiteral("adobe-rgb-compat")).toString(), core::raw::kSpaces, "adobe-rgb-compat");
    w.depth = store->value(QStringLiteral("raw/depth"), 16).toInt() == 8 ? 8 : 16;
    bool ok = false;
    const double dpi = store->value(QStringLiteral("raw/resolution"), 300).toDouble(&ok);
    w.dpi = ok && std::isfinite(dpi) && dpi >= 1 && dpi <= 100000 ? dpi : 300;
    w.sharpen = one_of(store->value(QStringLiteral("raw/sharpen"), QStringLiteral("none")).toString(), core::raw::kSharpens, "none");
    w.smart = truthy(store->value(QStringLiteral("raw/smart_object"), QStringLiteral("false")));
    w.auto_tone = truthy(store->value(QStringLiteral("raw/auto_tone"), QStringLiteral("false")));
    return w;
}

void set_workflow(const Workflow& w) {
    const auto store = settings();
    store->setValue(QStringLiteral("raw/space"), QString::fromStdString(w.space));
    store->setValue(QStringLiteral("raw/depth"), w.depth);
    store->setValue(QStringLiteral("raw/resolution"), w.dpi);
    store->setValue(QStringLiteral("raw/sharpen"), QString::fromStdString(w.sharpen));
    store->setValue(QStringLiteral("raw/smart_object"), w.smart ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("raw/auto_tone"), w.auto_tone ? QStringLiteral("true") : QStringLiteral("false"));
}

std::optional<Json> remembered(const std::string& ref) {
    const auto store = settings();
    const QString text = store->value(QStringLiteral("raw/remembered/%1").arg(QString::fromStdString(ref).remove(QStringLiteral("sha256:")))).toString();
    if (text.isEmpty()) return std::nullopt;
    try {
        Json read = Json::parse(text.toStdString());
        if (read.is_object()) return read;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

void remember(const std::string& ref, const Json& settings_of) {
    const auto store = settings();
    const QString key = QString::fromStdString(ref).remove(QStringLiteral("sha256:"));
    QStringList order = store->value(QStringLiteral("raw/remembered_order")).toStringList();
    order.removeAll(key);
    order.prepend(key);
    while (order.size() > kRememberedMost) store->remove(QStringLiteral("raw/remembered/%1").arg(order.takeLast()));
    store->setValue(QStringLiteral("raw/remembered_order"), order);
    store->setValue(QStringLiteral("raw/remembered/%1").arg(key), QString::fromStdString(settings_of.dump()));
}

// --- the window -----------------------------------------------------------------------------------------------------------

namespace {

constexpr int kPreviewSide = 900;

QComboBox* choices(const std::vector<std::pair<QString, QString>>& items, const QString& name) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    for (const auto& [label, value] : items) box->addItem(label, value);
    return box;
}

void choose(QComboBox* box, const std::string& value) {
    const int at = box->findData(QString::fromStdString(value));
    if (at >= 0) box->setCurrentIndex(at);
}

std::string chosen_of(const QComboBox* box) { return box->currentData().toString().toStdString(); }

// (a preview: its picture, or why there is none)
struct Made {
    std::shared_ptr<render::raw::Preview> preview;
    QString error;
};

class RawDialog : public QDialog {
public:
    RawDialog(QWidget* parent, std::shared_ptr<const std::string> bytes, const QString& name, const Json& start, double dpi, bool editing)
        : QDialog(parent), bytes_(std::move(bytes)), editing_(editing) {
        setObjectName(QStringLiteral("camera_raw_open"));
        setWindowTitle(QStringLiteral("Camera Raw — %1").arg(name));
        resize(1200, 800);
        setProperty("raw_previews", 0);
        setProperty("raw_previewing", false);
        const Workflow defaults = workflow();
        const core::raw::Develop develop = core::raw::develop_of(start.is_object() ? std::optional<Json>(start) : std::nullopt);
        const bool given = start.is_object() && start.contains("develop");

        auto* layout = new QVBoxLayout(this);
        panel_ = new camera_raw_window::CameraRawPanel(this);
        layout->addWidget(panel_, 1);

        // (the RAW stage, at the top of 基本補正; the panel's relative white balance and exposure hidden: the RAW's own)
        auto* raw = new QWidget;
        raw->setObjectName(QStringLiteral("raw_stage"));
        auto* rows = new QFormLayout(raw);
        rows->setContentsMargins(0, 0, 0, 8);
        white_ = choices({{QStringLiteral("撮影時の設定"), QStringLiteral("as_shot")},
                          {QStringLiteral("自動"), QStringLiteral("auto")},
                          {QStringLiteral("カスタム"), QStringLiteral("custom")}},
                         QStringLiteral("raw_white_balance"));
        temperature_ = new QSpinBox;
        temperature_->setObjectName(QStringLiteral("raw_temperature"));
        temperature_->setRange(static_cast<int>(core::raw::kTemperatureMin), static_cast<int>(core::raw::kTemperatureMax));
        temperature_->setSingleStep(50);
        temperature_->setSuffix(QStringLiteral(" K"));
        tint_ = new QSpinBox;
        tint_->setObjectName(QStringLiteral("raw_tint"));
        tint_->setRange(-static_cast<int>(core::raw::kTintMax), static_cast<int>(core::raw::kTintMax));
        exposure_ = new QDoubleSpinBox;
        exposure_->setObjectName(QStringLiteral("raw_exposure"));
        exposure_->setRange(-core::raw::kExposureMax, core::raw::kExposureMax);
        exposure_->setDecimals(2);
        exposure_->setSingleStep(0.05);
        auto_ = new QPushButton(QStringLiteral("自動"));
        auto_->setObjectName(QStringLiteral("raw_auto"));
        auto_->setToolTip(QStringLiteral("中間の明るさが中間の灰色になるように、露光量を決めます"));
        auto* exposure_row = new QWidget;
        auto* exposure_line = new QHBoxLayout(exposure_row);
        exposure_line->setContentsMargins(0, 0, 0, 0);
        exposure_line->addWidget(exposure_, 1);
        exposure_line->addWidget(auto_);
        demosaic_ = choices({{QStringLiteral("AHD"), QStringLiteral("ahd")},
                             {QStringLiteral("AAHD"), QStringLiteral("aahd")},
                             {QStringLiteral("DHT"), QStringLiteral("dht")},
                             {QStringLiteral("DCB"), QStringLiteral("dcb")},
                             {QStringLiteral("PPG"), QStringLiteral("ppg")},
                             {QStringLiteral("VNG"), QStringLiteral("vng")},
                             {QStringLiteral("バイリニア"), QStringLiteral("linear")}},
                            QStringLiteral("raw_demosaic"));
        highlights_ = choices({{QStringLiteral("クリップ"), QStringLiteral("clip")},
                               {QStringLiteral("クリップしない"), QStringLiteral("unclip")},
                               {QStringLiteral("ブレンド"), QStringLiteral("blend")},
                               {QStringLiteral("再構築"), QStringLiteral("rebuild")}},
                              QStringLiteral("raw_highlights"));
        status_ = new QLabel;
        status_->setObjectName(QStringLiteral("raw_status"));
        status_->setWordWrap(true);
        rows->addRow(QStringLiteral("ホワイトバランス"), white_);
        rows->addRow(QStringLiteral("色温度"), temperature_);
        rows->addRow(QStringLiteral("色かぶり補正"), tint_);
        rows->addRow(QStringLiteral("露光量"), exposure_row);
        rows->addRow(QStringLiteral("デモザイク"), demosaic_);
        rows->addRow(QStringLiteral("ハイライト"), highlights_);
        rows->addRow(status_);
        panel_->add_to_basic(raw);
        panel_->set_hidden({"temperature", "tint", "exposure"});

        // (the ワークフロー)
        auto* flow = new QWidget;
        flow->setObjectName(QStringLiteral("raw_workflow"));
        auto* line = new QHBoxLayout(flow);
        line->setContentsMargins(0, 0, 0, 0);
        space_ = choices({{QStringLiteral("Adobe RGB (1998)"), QStringLiteral("adobe-rgb-compat")},
                          {QStringLiteral("sRGB IEC61966-2.1"), QStringLiteral("srgb")},
                          {QStringLiteral("ProPhoto RGB"), QStringLiteral("prophoto-compat")},
                          {QStringLiteral("Display P3"), QStringLiteral("display-p3")}},
                         QStringLiteral("raw_space"));
        depth_ = choices({{QStringLiteral("8 bit/チャンネル"), QStringLiteral("8")}, {QStringLiteral("16 bit/チャンネル"), QStringLiteral("16")}},
                         QStringLiteral("raw_depth"));
        resolution_ = new QDoubleSpinBox;
        resolution_->setObjectName(QStringLiteral("raw_resolution"));
        resolution_->setRange(1, 100000);
        resolution_->setDecimals(0);
        resolution_->setSuffix(QStringLiteral(" ppi"));
        sharpen_ = choices({{QStringLiteral("なし"), QStringLiteral("none")},
                            {QStringLiteral("スクリーン"), QStringLiteral("screen")},
                            {QStringLiteral("光沢紙"), QStringLiteral("glossy")},
                            {QStringLiteral("マット紙"), QStringLiteral("matte")}},
                           QStringLiteral("raw_sharpen"));
        smart_ = new QCheckBox(QStringLiteral("スマートオブジェクトとして開く"));
        smart_->setObjectName(QStringLiteral("raw_smart"));
        line->addWidget(new QLabel(QStringLiteral("色空間")));
        line->addWidget(space_);
        line->addWidget(new QLabel(QStringLiteral("ビット数")));
        line->addWidget(depth_);
        if (!editing_) {
            line->addWidget(new QLabel(QStringLiteral("解像度")));
            line->addWidget(resolution_);
        }
        line->addWidget(new QLabel(QStringLiteral("出力のシャープ")));
        line->addWidget(sharpen_);
        if (!editing_) {
            line->addWidget(smart_);
        } else {  // (a smart object's window shows neither, yet they are its own: never left behind as windows of their own)
            for (QWidget* unused : {static_cast<QWidget*>(resolution_), static_cast<QWidget*>(smart_)}) {
                unused->setParent(flow);
                unused->hide();
            }
        }
        line->addStretch(1);
        layout->addWidget(flow);

        // (the buttons: 画像を開く — オブジェクトを開く with Shift or スマートオブジェクトとして開く —, キャンセル, 完了; a smart object's OK)
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        cancel_ = new QPushButton(QStringLiteral("キャンセル"));
        cancel_->setObjectName(QStringLiteral("raw_cancel"));
        open_ = new QPushButton(editing_ ? QStringLiteral("OK") : QStringLiteral("画像を開く"));
        open_->setObjectName(editing_ ? QStringLiteral("raw_ok") : QStringLiteral("raw_open"));
        open_->setDefault(true);
        buttons->addWidget(cancel_);
        if (!editing_) {
            done_ = new QPushButton(QStringLiteral("完了"));
            done_->setObjectName(QStringLiteral("raw_done"));
            done_->setToolTip(QStringLiteral("設定をこのファイルに覚えて、開かずに閉じます"));
            buttons->addWidget(done_);
        }
        buttons->addWidget(open_);
        layout->addLayout(buttons);

        // (where it starts: the settings given, else the ワークフロー's)
        choose(white_, develop.white_balance);
        temperature_->setValue(static_cast<int>(std::lround(develop.temperature_k)));
        tint_->setValue(static_cast<int>(std::lround(develop.tint)));
        exposure_->setValue(develop.exposure_ev);
        choose(demosaic_, develop.demosaic);
        choose(highlights_, develop.highlights);
        choose(space_, given ? develop.space : defaults.space);
        choose(depth_, std::to_string(given ? develop.depth : defaults.depth));
        choose(sharpen_, given ? develop.sharpen : defaults.sharpen);
        resolution_->setValue(dpi > 0 ? dpi : defaults.dpi);
        smart_->setChecked(defaults.smart);
        orientation_ = develop.orientation;
        auto_tone_ = defaults.auto_tone && !given;
        if (const Json* adjust = start.is_object() ? core::get(start, "adjust") : nullptr; adjust != nullptr && adjust->is_object()) {
            panel_->set_settings(*adjust);
        }
        show_open_words();

        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(150);
        connect(timer_, &QTimer::timeout, this, [this] { preview_now(); });
        const auto later = [this] { timer_->start(); };
        connect(white_, &QComboBox::currentIndexChanged, this, later);
        connect(temperature_, &QSpinBox::valueChanged, this, [this] {
            if (quiet_) return;
            choose_custom();
            timer_->start();
        });
        connect(tint_, &QSpinBox::valueChanged, this, [this] {
            if (quiet_) return;
            choose_custom();
            timer_->start();
        });
        connect(exposure_, &QDoubleSpinBox::valueChanged, this, later);
        connect(demosaic_, &QComboBox::currentIndexChanged, this, later);
        connect(highlights_, &QComboBox::currentIndexChanged, this, later);
        connect(auto_, &QPushButton::clicked, this, [this] { apply_auto(); });
        connect(smart_, &QCheckBox::toggled, this, [this] { show_open_words(); });
        connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
        connect(open_, &QPushButton::clicked, this, [this] {
            const bool object = !editing_ && ((QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) || smart_->isChecked());
            finish(editing_ ? Ending::Open : object ? Ending::OpenObject : Ending::Open);
        });
        if (done_ != nullptr) connect(done_, &QPushButton::clicked, this, [this] { finish(Ending::Done); });
        preview_now();
    }

    ~RawDialog() override { stop_.request_stop(); }
    RawDialog(const RawDialog&) = delete;
    RawDialog& operator=(const RawDialog&) = delete;

    Chosen chosen() const {
        Chosen out;
        out.ending = ending_;
        out.raw = Json{{"develop", develop_json(true)}, {"adjust", panel_->settings()}};
        out.dpi = resolution_->value();
        return out;
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Shift) {
            shift_ = true;
            show_open_words();
        }
        QDialog::keyPressEvent(event);
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Shift) {
            shift_ = false;
            show_open_words();
        }
        QDialog::keyReleaseEvent(event);
    }

private:
    // The development the controls say (`all`: with the ワークフロー's output; without, the RAW stage the preview develops).
    Json develop_json(bool all) const {
        Json d = Json::object();
        d["white_balance"] = chosen_of(white_);
        if (chosen_of(white_) == "custom") {
            d["temperature_k"] = temperature_->value();
            d["tint"] = tint_->value();
        }
        d["exposure_ev"] = std::round(exposure_->value() * 100) / 100;
        d["demosaic"] = chosen_of(demosaic_);
        d["highlights"] = chosen_of(highlights_);
        d["orientation"] = orientation_;
        if (all) {
            d["space"] = chosen_of(space_);
            d["depth"] = depth_->currentData().toInt();
            d["sharpen"] = chosen_of(sharpen_);
        }
        return core::raw::to_json(core::raw::develop_from(&d));
    }

    void show_open_words() {
        if (editing_) return;
        open_->setText(shift_ || smart_->isChecked() ? QStringLiteral("オブジェクトを開く") : QStringLiteral("画像を開く"));
    }

    void choose_custom() {
        if (chosen_of(white_) == "custom") return;
        const QSignalBlocker hold(white_);
        choose(white_, "custom");
    }

    // The RAW stage developed on a thread at the window's size (the last asked for shown).
    void preview_now() {
        stop_.request_stop();
        stop_ = std::stop_source();
        const std::stop_token stop = stop_.get_token();
        const core::raw::Develop develop = [&] {
            const Json d = develop_json(false);
            return core::raw::develop_from(&d);
        }();
        const int asked = ++asked_;
        setProperty("raw_previewing", true);
        auto* watcher = new QFutureWatcher<Made>(this);
        connect(watcher, &QFutureWatcher<Made>::finished, this, [this, watcher, asked] {
            Made made = watcher->result();
            watcher->deleteLater();
            if (asked != asked_) return;  // (a newer one asked)
            setProperty("raw_previewing", false);
            if (!made.preview) {
                status_->setText(made.error);
                return;
            }
            shown_preview(std::move(made.preview));
        });
        const std::shared_ptr<const std::string> bytes = bytes_;
        watcher->setFuture(QtConcurrent::run([bytes, develop, stop]() -> Made {
            try {
                render::raw::Options options;
                options.stopped = [stop] { return stop.stop_requested(); };
                return Made{std::make_shared<render::raw::Preview>(render::raw::preview(*bytes, develop, kPreviewSide, options)), QString()};
            } catch (const render::Cancelled&) {
                return Made{nullptr, QString()};
            } catch (const std::exception& error) {
                return Made{nullptr, wording::error(QString::fromUtf8(error.what()))};
            }
        }));
    }

    void shown_preview(std::shared_ptr<render::raw::Preview> preview) {
        last_ = preview;
        // (撮影時の設定・自動: the white they balance to shown as its temperature and tint)
        if (chosen_of(white_) != "custom") {
            const auto [t, tint] = render::raw::temperature_of(preview->info, preview->multipliers);
            quiet_ = true;
            temperature_->setValue(static_cast<int>(std::lround(std::clamp(t, core::raw::kTemperatureMin, core::raw::kTemperatureMax))));
            tint_->setValue(static_cast<int>(std::lround(std::clamp(tint, -core::raw::kTintMax, core::raw::kTintMax))));
            quiet_ = false;
        }
        QString said;
        for (const std::string& note : preview->notes) said += (said.isEmpty() ? QString() : QStringLiteral("\n")) + QString::fromStdString(note);
        status_->setText(said);
        const core::raw::Develop as_is;
        const auto [full_w, full_h] = render::raw::developed_size(preview->info, as_is);
        auto picture = std::make_shared<camera_raw_window::Picture>();
        picture->width = preview->width;
        picture->height = preview->height;
        picture->rgba = preview->rgba;
        picture->floats = true;
        picture->s = std::max(1e-3, static_cast<double>(std::max(preview->width, preview->height)) / static_cast<double>(std::max<std::int64_t>({full_w, full_h, 1})));
        picture->box_mm = {0, 0, static_cast<double>(preview->width), static_cast<double>(preview->height)};
        picture->to_screen = render::raw::working_to_screen;
        panel_->set_picture(picture);
        setProperty("raw_previews", property("raw_previews").toInt() + 1);
        if (auto_tone_) {  // (環境設定's 自動補正: once, when the window opens)
            auto_tone_ = false;
            apply_auto();
        }
    }

    void apply_auto() {
        if (!last_) return;
        exposure_->setValue(std::clamp(exposure_->value() + render::raw::auto_exposure(*last_), -core::raw::kExposureMax, core::raw::kExposureMax));
    }

    void finish(Ending ending) {
        try {
            (void)panel_->settings();  // (what the table refuses: said, the window kept)
        } catch (const std::exception& error) {
            ask::warning(this, windowTitle(), wording::error(QString::fromUtf8(error.what())));
            return;
        }
        ending_ = ending;
        accept();
    }

    std::shared_ptr<const std::string> bytes_;
    bool editing_ = false;
    camera_raw_window::CameraRawPanel* panel_ = nullptr;
    QComboBox* white_ = nullptr;
    QSpinBox* temperature_ = nullptr;
    QSpinBox* tint_ = nullptr;
    QDoubleSpinBox* exposure_ = nullptr;
    QPushButton* auto_ = nullptr;
    QComboBox* demosaic_ = nullptr;
    QComboBox* highlights_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* space_ = nullptr;
    QComboBox* depth_ = nullptr;
    QDoubleSpinBox* resolution_ = nullptr;
    QComboBox* sharpen_ = nullptr;
    QCheckBox* smart_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* done_ = nullptr;
    QTimer* timer_ = nullptr;
    std::stop_source stop_;
    int asked_ = 0;
    bool quiet_ = false;
    bool shift_ = false;
    bool auto_tone_ = false;
    std::string orientation_ = "file";
    std::shared_ptr<render::raw::Preview> last_;
    Ending ending_ = Ending::Cancel;
};

}  // namespace

Chosen run(QWidget* parent, std::shared_ptr<const std::string> bytes, const QString& name, const Json& start, double dpi, bool editing) {
    RawDialog dialog(parent, std::move(bytes), name, start, dpi, editing);
    if (ask::exec(&dialog) != QDialog::Accepted) return Chosen{};
    return dialog.chosen();
}

}  // namespace genko::app::camera_raw_open
