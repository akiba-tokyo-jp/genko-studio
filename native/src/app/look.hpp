#pragma once

#include <QSize>
#include <QString>
#include <QWidget>

#include <optional>

#include "core/model.hpp"

class QDialog;
class QDialogButtonBox;
class QFormLayout;
class QFrame;
class QLabel;
class QLayout;
class QScrollArea;

// The dialogs made of the same parts as the main window (Python's genko/app/dialog_look.py): a heading with one quiet
// line under it, sections of rows on the left, a picture card on the right, and a footer whose one main button is in
// the accent. On a small screen (1024 × 640, or a large display scale) the body scrolls and the card goes under it,
// while the heading and the footer's buttons stay in sight (SPEC UX-02).

namespace genko::app::look {

QWidget* header(const QString& title, const QString& subtitle = {});
QLabel* section(const QString& title);
// Rows of a section: the names quiet and right-aligned, the fields growing.
QFormLayout* form();
void quiet_labels(QFormLayout* rows);
QFrame* card(QWidget* content, const QString& caption = {});
QFrame* card(QLayout* content, const QString& caption = {});
QWidget* footer(QDialogButtonBox* buttons, QWidget* note = nullptr);
// The whole layout: heading, the body (scrolling) with its card beside it (or under it when narrow), the footer.
QScrollArea* frame(QDialog* dialog, QWidget* head, QWidget* body, QWidget* side, QWidget* foot);
QScrollArea* frame(QDialog* dialog, QWidget* head, QLayout* body, QWidget* side, QWidget* foot);
// The dialog at `wanted` size, made smaller to fit the screen it is on (never taller or wider than it; cap: nor can it be
// made so — off for the main window, which may be maximized).
void fit_to_screen(QWidget* window, QSize wanted, bool cap = true);

// The sheet as it will be: the paper, the bleed, the finished size and the basic frame, to scale.
class PaperDiagram : public QWidget {
    Q_OBJECT

public:
    explicit PaperDiagram(QWidget* parent = nullptr);
    void show_spec(const core::PageSpec& spec, bool error = false);
    const std::optional<core::PageSpec>& spec() const { return spec_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::optional<core::PageSpec> spec_;
    bool error_ = false;
};

// What the lines in the paper picture mean.
QWidget* legend();

}  // namespace genko::app::look
