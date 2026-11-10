#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <optional>

#include "core/ink.hpp"
#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QPushButton;

// 編集 → カラー設定… (SPEC COLOR-03, PC1b; PhotoCraft v0.2.0's engine/src/color_cmds.rs ColorSettings, MIT): the
// working spaces, what to do with a book's or a picture's profile that is not the working one, the conversions'
// options and the monitor's profile — this computer's settings (QSettings color/*), never the book's. The ops never
// read them: the window resolves them into the ops it sends (a working space as a profile, an intent, a black point).
// 文字の色をガンマで混ぜる is the book's own (set_text_gamma), shown here for the book in front.

namespace genko::app::color_settings {

struct Settings {
    // a built-in profile's id, an ICC file's path, or (CMYK) "export": the CMYK profile chosen for export, or without
    // one Genko's own conversion (Python's 「CMYK で見る」)
    QString working_rgb = QStringLiteral("srgb");
    QString working_cmyk = QStringLiteral("coated-cmyk");  // (the user's decision: upstream's)
    QString working_gray = QStringLiteral("sgray");
    // preserve | convert | off, for books opened and pictures taken in
    QString policy_rgb = QStringLiteral("preserve");
    QString policy_cmyk = QStringLiteral("preserve");
    QString policy_gray = QStringLiteral("preserve");
    bool ask_mismatch = true;  // ask when a book opened has pages of another profile
    bool ask_paste = true;     // ask when a picture taken in has another profile
    bool ask_missing = false;  // ask when a picture taken in has none (and the working space is not the default)
    QString intent = QStringLiteral("relative");
    bool bpc = true;
    bool dither = false;  // (Genko's own default: the conversions' default, PC1a; upstream's is on)
    QString monitor = QStringLiteral("auto");

    bool operator==(const Settings&) const = default;
};

Settings load();
void save(const Settings& settings);

enum class Space { Rgb, Gray, Cmyk };

// A working space resolved: its profile's bytes (none: sRGB for RGB; for CMYK "export" without a profile chosen for
// export — Genko's own conversion), whether it is the built-in default of its space (srgb, sgray, coated-cmyk), the id
// an op names it by (a built-in's, else empty: give the bytes), and its description.
struct Working {
    QString spec;
    core::Bytes icc;
    QString builtin;  // (the built-in's id)
    bool is_default = false;
    bool usable = true;  // (false: a file that cannot be read, or "export" without a profile: no working profile)
    QString name;
};
Working working(Space space, const Settings& settings);
// The CMYK profile chosen last for export (color/icc), while its file is there.
std::optional<std::filesystem::path> export_icc();
// The working space's profile in an op: {"profile": id} or {"icc": base64}.
void put_profile(core::Json& op, const Working& working);
// The ref ("sha256:…") of a profile's bytes, "" for none (sRGB).
std::string ref_of(const core::Bytes& icc);

class ColorSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // `text_gamma`: the book in front's (none: Python's mixing); `book` false without a book that can change.
    ColorSettingsDialog(QWidget* parent, const Settings& settings, std::optional<double> text_gamma, bool book);
    Settings settings() const;
    std::optional<double> text_gamma() const;
    void show_settings(const Settings& settings);

    QComboBox* working_rgb = nullptr;
    QComboBox* working_cmyk = nullptr;
    QComboBox* working_gray = nullptr;
    QComboBox* policy_rgb = nullptr;
    QComboBox* policy_cmyk = nullptr;
    QComboBox* policy_gray = nullptr;
    QCheckBox* ask_mismatch = nullptr;
    QCheckBox* ask_paste = nullptr;
    QCheckBox* ask_missing = nullptr;
    QComboBox* intent = nullptr;
    QCheckBox* bpc = nullptr;
    QCheckBox* dither = nullptr;
    QCheckBox* gamma_on = nullptr;
    QDoubleSpinBox* gamma = nullptr;
    QComboBox* monitor = nullptr;
    QPushButton* reset = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

}  // namespace genko::app::color_settings
