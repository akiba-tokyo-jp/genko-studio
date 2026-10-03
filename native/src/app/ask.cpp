#include "app/ask.hpp"

#include <QColorDialog>
#include <QDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>

namespace genko::app::ask {

namespace {

std::shared_ptr<Responder>& current() {
    static std::shared_ptr<Responder> value;
    return value;
}

}  // namespace

void set_responder(std::shared_ptr<Responder> responder) { current() = std::move(responder); }

std::shared_ptr<Responder> responder() { return current(); }

std::optional<double> get_double(QWidget* parent, const QString& title, const QString& label, double value, double lo,
                                 double hi, int decimals) {
    if (const auto r = current(); r && r->get_double) return r->get_double(title, label, value, lo, hi, decimals);
    bool ok = false;
    const double out = QInputDialog::getDouble(parent, title, label, value, lo, hi, decimals, &ok);
    return ok ? std::optional<double>(out) : std::nullopt;
}

std::optional<int> get_int(QWidget* parent, const QString& title, const QString& label, int value, int lo, int hi) {
    if (const auto r = current(); r && r->get_int) return r->get_int(title, label, value, lo, hi);
    bool ok = false;
    const int out = QInputDialog::getInt(parent, title, label, value, lo, hi, 1, &ok);
    return ok ? std::optional<int>(out) : std::nullopt;
}

std::optional<QString> get_text(QWidget* parent, const QString& title, const QString& label, const QString& text) {
    if (const auto r = current(); r && r->get_text) return r->get_text(title, label, text);
    bool ok = false;
    const QString out = QInputDialog::getText(parent, title, label, QLineEdit::Normal, text, &ok);
    return ok ? std::optional<QString>(out) : std::nullopt;
}

std::optional<QColor> colour(QWidget* parent, const QColor& now, const QString& title) {
    if (const auto r = current(); r && r->colour) return r->colour(now, title);
    const QColor chosen = QColorDialog::getColor(now, parent, title);
    return chosen.isValid() ? std::optional<QColor>(chosen) : std::nullopt;
}

bool question(QWidget* parent, const QString& title, const QString& text) {
    if (const auto r = current(); r && r->question) return r->question(title, text);
    return QMessageBox::question(parent, title, text) == QMessageBox::Yes;
}

void warning(QWidget* parent, const QString& title, const QString& text) {
    if (const auto r = current(); r && r->warning) {
        r->warning(title, text);
        return;
    }
    QMessageBox::warning(parent, title, text);
}

QString existing_dir(QWidget* parent, const QString& caption, const QString& start) {
    if (const auto r = current(); r && r->existing_dir) return r->existing_dir(caption, start);
    return QFileDialog::getExistingDirectory(parent, caption, start);
}

QString save_path(QWidget* parent, const QString& caption, const QString& suggested) {
    if (const auto r = current(); r && r->save_path) return r->save_path(caption, suggested);
    return QFileDialog::getSaveFileName(parent, caption, suggested);
}

int exec(QDialog* dialog) {
    if (const auto r = current(); r && r->exec) return r->exec(dialog);
    return dialog->exec();
}

}  // namespace genko::app::ask
