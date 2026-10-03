#include "app/look.hpp"

#include <QBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>

#include <algorithm>

#include "app/theme.hpp"

namespace genko::app::look {

namespace {

// The body and its card side by side, or the card under the body when the dialog is narrow.
class Middle : public QWidget {
public:
    Middle(QWidget* body, QWidget* side) : side_(side) {
        layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(18);
        layout_->addWidget(body, 3);
        if (side != nullptr) layout_->addWidget(side, 2);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (side_ == nullptr) return;
        const auto direction = event->size().width() < 600 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
        if (layout_->direction() != direction) layout_->setDirection(direction);
    }

private:
    QBoxLayout* layout_ = nullptr;
    QWidget* side_ = nullptr;
};

}  // namespace

QWidget* header(const QString& title, const QString& subtitle) {
    auto* box = new QWidget;
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(2);
    auto* heading = new QLabel(title);
    heading->setWordWrap(true);
    theme::role(heading, "title");
    layout->addWidget(heading);
    if (!subtitle.isEmpty()) {
        auto* note = new QLabel(subtitle);
        note->setWordWrap(true);
        theme::role(note, "hint");
        layout->addWidget(note);
    }
    return box;
}

QLabel* section(const QString& title) {
    auto* label = new QLabel(title);
    theme::role(label, "section");
    return label;
}

QFormLayout* form() {
    auto* rows = new QFormLayout;
    rows->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rows->setRowWrapPolicy(QFormLayout::WrapLongRows);  // (a narrow dialog puts a long field under its name)
    rows->setHorizontalSpacing(10);
    rows->setVerticalSpacing(6);
    return rows;
}

void quiet_labels(QFormLayout* rows) {
    for (int i = 0; i < rows->rowCount(); ++i) {
        QLayoutItem* item = rows->itemAt(i, QFormLayout::LabelRole);
        if (item != nullptr) {
            if (auto* label = qobject_cast<QLabel*>(item->widget())) theme::role(label, "label");
        }
    }
}

QFrame* card(QWidget* content, const QString& caption) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("dialogCard"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    if (!caption.isEmpty()) layout->addWidget(section(caption));
    layout->addWidget(content, 1);
    return frame;
}

QFrame* card(QLayout* content, const QString& caption) {
    auto* holder = new QWidget;
    holder->setLayout(content);
    return card(holder, caption);
}

QWidget* footer(QDialogButtonBox* buttons, QWidget* note) {
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("dialogFooter"));
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 10, 0, 0);
    if (note != nullptr) {
        layout->addWidget(note, 1);
    } else {
        layout->addStretch(1);
    }
    for (const auto role : {QDialogButtonBox::Ok, QDialogButtonBox::Save, QDialogButtonBox::Apply}) {
        if (QPushButton* button = buttons->button(role)) {
            theme::primary(button);
            button->setDefault(true);
            break;
        }
    }
    layout->addWidget(buttons);
    return box;
}

QScrollArea* frame(QDialog* dialog, QWidget* head, QWidget* body, QWidget* side, QWidget* foot) {
    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(20, 18, 20, 16);
    outer->setSpacing(12);
    outer->addWidget(head);
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("dialogBody"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(new Middle(body, side));
    outer->addWidget(scroll, 1);
    outer->addWidget(foot);
    theme::name_buttons(dialog);
    return scroll;
}

QScrollArea* frame(QDialog* dialog, QWidget* head, QLayout* body, QWidget* side, QWidget* foot) {
    auto* holder = new QWidget;
    holder->setLayout(body);
    return frame(dialog, head, holder, side, foot);
}

void fit_to_screen(QWidget* window, QSize wanted, bool cap) {
    QScreen* screen = window->screen() != nullptr ? window->screen() : QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        window->resize(wanted);
        return;
    }
    const QRect room = screen->availableGeometry();
    // (room for the window's frame and title bar, which the window manager draws around it)
    const QSize most(std::max(200, room.width() - 16), std::max(160, room.height() - 40));
    if (cap) window->setMaximumSize(most);
    window->resize(std::min(wanted.width(), most.width()), std::min(wanted.height(), most.height()));
    const QRect at = window->frameGeometry();
    if (!room.contains(at)) window->move(room.topLeft() + QPoint(std::max(0, (room.width() - at.width()) / 2), std::max(0, (room.height() - at.height()) / 2)));
}

PaperDiagram::PaperDiagram(QWidget* parent) : QWidget(parent) { setMinimumSize(QSize(170, 220)); }

void PaperDiagram::show_spec(const core::PageSpec& spec, bool error) {
    spec_ = spec;
    error_ = error;
    update();
}

void PaperDiagram::paintEvent(QPaintEvent*) {
    if (!spec_) return;
    const theme::Tokens t = theme::tokens();
    const core::PageSpec& spec = *spec_;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double pad = 10;
    const double width_mm = spec.width_mm.value();
    const double height_mm = spec.height_mm.value();
    if (width_mm <= 0 || height_mm <= 0) return;
    const double scale = std::min((width() - 2 * pad) / width_mm, (height() - 2 * pad) / height_mm);
    const double ox = (width() - width_mm * scale) / 2;
    const double oy = (height() - height_mm * scale) / 2;
    const auto box = [&](double x, double y, double w, double h) { return QRectF(ox + x * scale, oy + y * scale, w * scale, h * scale); };
    painter.fillRect(box(0, 0, width_mm, height_mm), QColor(QStringLiteral("#f4f4f2")));
    try {
        const auto [tx, ty] = spec.trim_origin();
        const auto [tw_num, th_num] = spec.trim_size();
        const double tw = tw_num.value();
        const double th = th_num.value();
        const double b = spec.bleed_mm.value();
        const auto m = spec.margins();
        painter.fillRect(box(tx - b, ty - b, tw + 2 * b, th + 2 * b), QColor(QStringLiteral("#e3e6ea")));
        painter.fillRect(box(tx, ty, tw, th), QColor(QStringLiteral("#ffffff")));
        painter.setPen(QPen(QColor(200, 60, 120, 200), 1));
        painter.drawRect(box(tx, ty, tw, th));
        painter.setPen(QPen(QColor(40, 126, 214, 200), 1, Qt::DashLine));
        painter.drawRect(box(tx + m.outer, ty + m.top, tw - m.outer - m.inner, th - m.top - m.bottom));
    } catch (const std::exception&) {
        return;
    }
    painter.setPen(QPen(QColor(error_ ? t.danger : t.border), 1));
    painter.drawRect(box(0, 0, width_mm, height_mm));
}

QWidget* legend() {
    auto* box = new QWidget;
    auto* rows = new QVBoxLayout(box);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    const struct {
        const char* colour;
        const char* mark;
        const char* words;
    } items[] = {{"#c83c78", "━", "仕上がり（ここで切られる）"}, {"#287ed6", "┅", "基本枠（コマを収める目安）"},
                 {"#e3e6ea", "■", "裁ち落とし（切られる帯）"}};
    for (const auto& item : items) {
        auto* label = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>　%3")
                                     .arg(QString::fromUtf8(item.colour), QString::fromUtf8(item.mark), QString::fromUtf8(item.words)));
        label->setWordWrap(true);
        theme::role(label, "hint");
        rows->addWidget(label);
    }
    return box;
}

}  // namespace genko::app::look
