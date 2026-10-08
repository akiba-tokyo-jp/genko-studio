#pragma once

#include <QColor>
#include <QString>

#include <functional>
#include <memory>
#include <utility>
#include <optional>

class QDialog;
class QWidget;

// The small questions the app asks (a number, a colour, yes or no, a folder, a place to save) and the dialogs it runs,
// in one place, so tests can answer them (set_responder) without a person. Without a responder the real dialogs open.

namespace genko::app::ask {

struct Responder {
    std::function<std::optional<double>(const QString& title, const QString& label, double value, double lo, double hi, int decimals)>
        get_double;
    std::function<std::optional<int>(const QString& title, const QString& label, int value, int lo, int hi)> get_int;
    std::function<std::optional<QString>(const QString& title, const QString& label, const QString& text)> get_text;
    std::function<std::optional<QColor>(const QColor& now, const QString& title)> colour;
    std::function<bool(const QString& title, const QString& text)> question;  // yes
    std::function<void(const QString& title, const QString& text)> warning;
    std::function<QString(const QString& caption, const QString& start)> existing_dir;
    std::function<QString(const QString& caption, const QString& suggested)> save_path;
    // a place to save and the filter chosen with it
    std::function<std::pair<QString, QString>(const QString& caption, const QString& suggested, const QString& filter)> save_path_filtered;
    std::function<QString(const QString& caption, const QString& filter)> open_path;
    std::function<int(QDialog* dialog)> exec;  // QDialog::exec's result for a dialog the app runs
};

// The responder for tests (null: the real dialogs).
void set_responder(std::shared_ptr<Responder> responder);
std::shared_ptr<Responder> responder();

std::optional<double> get_double(QWidget* parent, const QString& title, const QString& label, double value, double lo,
                                 double hi, int decimals);
std::optional<int> get_int(QWidget* parent, const QString& title, const QString& label, int value, int lo, int hi);
std::optional<QString> get_text(QWidget* parent, const QString& title, const QString& label, const QString& text = {});
std::optional<QColor> colour(QWidget* parent, const QColor& now, const QString& title);
bool question(QWidget* parent, const QString& title, const QString& text);
void warning(QWidget* parent, const QString& title, const QString& text);
QString existing_dir(QWidget* parent, const QString& caption, const QString& start = {});
QString save_path(QWidget* parent, const QString& caption, const QString& suggested, const QString& filter = {});
// save_path, and which of the filters ("A (*.a);;B (*.b)") was chosen with it (`chosen`).
QString save_path_filtered(QWidget* parent, const QString& caption, const QString& suggested, const QString& filter, QString* chosen);
// A file to read (nothing: none chosen); `filter` as QFileDialog takes it ("画像 (*.png *.jpg)").
QString open_path(QWidget* parent, const QString& caption, const QString& filter = {});
int exec(QDialog* dialog);

}  // namespace genko::app::ask
