// 書体 (fonts@1, 書体の追加 F2): the palette of lettering faces (app/font_palette.hpp).

#include "app/font_palette.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <map>

#include "app/lettering.hpp"
#include "app/theme.hpp"
#include "core/font_catalog.hpp"
#include "core/model.hpp"
#include "render/font_files.hpp"
#include "render/image.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"

namespace genko::app {

namespace {

constexpr int kKey = Qt::UserRole + 1;       // a face's key (a heading: empty)
constexpr int kCategory = Qt::UserRole + 2;  // a heading's category id; a face's
constexpr int kMissing = Qt::UserRole + 3;   // the face's file is not on this computer
constexpr int kLabel = Qt::UserRole + 4;     // the row's words without 「ファイルなし」
constexpr int kDrawn = Qt::UserRole + 5;     // its sample has been made
constexpr int kDpi = 96;
constexpr int kSampleW = 132, kSampleH = 30;  // (the sample's room: five letters of 4.6 mm at 96 dpi and a little air)
const char* const kWords = "あア漢字！";

// Whether a face can be drawn in now, by one stat (an added face: a file of the right size is there; the others: always).
bool there(const QString& key) {
    const std::string k = key.toStdString();
    return !core::fonts::added(k) || render::font_files::file_there(k);
}

// A face's row: its name on a line of its own (the dock is narrow: beside the sample a name was cut to its first
// letters), under it what there is to know of it — かなだけ…, ファイルなし —, then its sample. A heading is drawn as the
// list draws any row.
class FaceRow : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int kPad = 8, kTop = 5, kGap = 2, kBottom = 6;

    static QFont small(const QFont& font) {
        QFont out = font;
        out.setBold(false);
        if (out.pointSizeF() > 0) out.setPointSizeF(out.pointSizeF() * 0.86);
        return out;
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.data(kKey).toString().isEmpty()) return QStyledItemDelegate::sizeHint(option, index);
        const QStringList lines = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
        const QFont font = index.data(Qt::FontRole).isValid() ? qvariant_cast<QFont>(index.data(Qt::FontRole)) : option.font;
        const int more = static_cast<int>(lines.size()) - 1;
        return QSize(kSampleW + 2 * kPad,
                     kTop + QFontMetrics(font).height() + more * QFontMetrics(small(font)).height() + kGap + kSampleH + kBottom);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.data(kKey).toString().isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        // (the row's words as they are kept: the style's own copy has its line breaks turned into separators)
        const QStringList lines = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
        const QIcon icon = opt.icon;
        // (the row's ground as the look draws it — hover, the chosen row, the marked face's colour —, with no words or
        // picture of the style's own placing)
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
        QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        painter->save();
        const bool gone = index.data(kMissing).toBool();
        const QPalette& colours = opt.widget != nullptr ? opt.widget->palette() : opt.palette;
        const QColor ink = gone ? QColor(theme::tokens().faint) : colours.color(QPalette::Active, QPalette::Text);
        const QRect room = option.rect.adjusted(kPad, kTop, -kPad, -kBottom);
        int y = room.top();
        painter->setFont(opt.font);
        painter->setPen(ink);
        const QFontMetrics name(opt.font);
        painter->drawText(QRect(room.left(), y, room.width(), name.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          name.elidedText(lines.value(0), Qt::ElideRight, room.width()));
        y += name.height();
        const QFont lesser = small(opt.font);
        const QFontMetrics note(lesser);
        painter->setFont(lesser);
        painter->setPen(gone ? ink : QColor(theme::tokens().muted));
        for (qsizetype i = 1; i < lines.size(); ++i) {
            painter->drawText(QRect(room.left(), y, room.width(), note.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              note.elidedText(lines[i], Qt::ElideRight, room.width()));
            y += note.height();
        }
        y += kGap;
        if (!icon.isNull()) icon.paint(painter, QRect(room.left(), y, kSampleW, kSampleH), Qt::AlignLeft | Qt::AlignVCenter);
        painter->restore();
    }
};

}  // namespace

QSize face_sample_size() { return QSize(kSampleW, kSampleH); }

QImage face_sample(const QString& key, double scale) {
    // (kept by the face and the scale: set once a process)
    static std::map<std::pair<QString, int>, QImage> kept;
    // (… for the files as they are: a file found to be another's since, or — in tests — another folder, and the samples
    // are set again)
    static std::pair<std::string, std::uint64_t> kept_of;
    if (const std::pair<std::string, std::uint64_t> now{render::font_files::dir(), render::font_files::generation()}; now != kept_of) {
        kept.clear();
        kept_of = now;
    }
    const int dpi = std::max(kDpi, static_cast<int>(std::lround(kDpi * scale)));
    if (const auto found = kept.find({key, dpi}); found != kept.end()) return found->second;
    const double factor = static_cast<double>(dpi) / kDpi;
    const std::string face = key.toStdString();
    QImage picture;
    // (an added face that is not installed — read and checked here, this once — has no sample: its row says so)
    if (!core::fonts::added(face) || render::font_files::present(face)) {
        core::StoryLine line;
        line.id = "face_" + face;
        line.text = kWords;
        line.balloon = "none";
        line.wrap = "horizontal";
        line.w_mm = core::Num(60.0);
        line.h_mm = core::Num(10.0);
        line.style = core::Json{{"font", face}, {"size_mm", 4.6}};
        try {
            const render::text::Layout layout = render::text::text_layout(line, dpi);
            const render::Image rgba = layout.image.mode() == "RGBA" ? layout.image : layout.image.convert("RGBA");
            const std::string bytes = rgba.tobytes();
            const QImage letters =
                QImage(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888).copy();
            // (in the sample's room: from its left, about its middle; a face wider than the room is made to fit)
            const QSize room(static_cast<int>(std::lround(kSampleW * factor)), static_cast<int>(std::lround(kSampleH * factor)));
            picture = QImage(room, QImage::Format_ARGB32_Premultiplied);
            picture.fill(Qt::transparent);
            QPainter painter(&picture);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            QSize size = letters.size();
            if (size.width() > room.width() || size.height() > room.height()) size.scale(room, Qt::KeepAspectRatio);
            painter.drawImage(QRect(QPoint(0, (room.height() - size.height()) / 2), size), letters);
            painter.end();
            picture.setDevicePixelRatio(factor);
        } catch (const std::exception& error) {  // (a sample that cannot be set: none — the row's name says what it is)
            qWarning("FontPalette: %s: %s", face.c_str(), error.what());
            picture = QImage();
        }
    }
    kept[{key, dpi}] = picture;
    return picture;
}

FontPalette::FontPalette(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("font_palette"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* note = new QLabel(QStringLiteral("書体をクリックすると、選んだ台詞がその書体になります。"
                                           "台詞を選んでいないときや、文字ツールを使っているときは、次に置く台詞の書体になります。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    theme::hint(note);
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("font_search"));
    search_->setPlaceholderText(QStringLiteral("書体をさがす（名前・使い道）"));
    search_->setAccessibleName(QStringLiteral("書体をさがす"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    none_ = new QLabel(QStringLiteral("当てはまる書体がありません"));
    none_->setObjectName(QStringLiteral("font_none"));
    none_->setVisible(false);
    layout->addWidget(none_);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("font_faces"));
    list_->setAccessibleName(QStringLiteral("書体"));
    list_->setIconSize(face_sample_size());
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::DefaultContextMenu);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setWordWrap(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setItemDelegate(new FaceRow(list_));
    // (until its sample is made each face shows an empty one of the same size: nothing jumps when the samples come)
    QPixmap blank(face_sample_size());
    blank.fill(Qt::transparent);
    const QIcon waiting(blank);
    QString category;
    for (const lettering::FaceRow& face : lettering::catalog_faces()) {
        if (face.category != category) {
            category = face.category;
            QString name = category;
            for (const auto& [id, label] : lettering::face_categories()) {
                if (id == category) name = label;
            }
            auto* head = new QListWidgetItem(name);
            head->setFlags(Qt::NoItemFlags);  // (a heading: not chosen, not picked)
            head->setData(kCategory, category);
            QFont bold = head->font();
            bold.setBold(true);
            head->setFont(bold);
            head->setSizeHint(QSize(0, 26));
            list_->addItem(head);
        }
        const QString words = face.mark.isEmpty() ? face.label : face.label + QLatin1Char('\n') + face.mark;
        auto* item = new QListWidgetItem(waiting, words);
        item->setData(kKey, face.key);
        item->setData(kCategory, face.category);
        item->setData(kLabel, words);
        item->setData(kMissing, false);
        item->setData(kDrawn, false);
        list_->addItem(item);
        order_.push_back(item);
    }
    layout->addWidget(list_, 1);
    setFocusProxy(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !item->data(kKey).toString().isEmpty()) emit picked(item->data(kKey).toString(), QString());
    });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& words) { filter(words); });
    list_->installEventFilter(this);
    timer_ = new QTimer(this);
    timer_->setInterval(0);
    connect(timer_, &QTimer::timeout, this, [this] { draw_some(1); });  // (one a turn: a face's file is read for its first sample)
}

QListWidgetItem* FontPalette::item(const QString& key) const {
    if (key.isEmpty()) return nullptr;
    for (QListWidgetItem* it : order_) {
        if (it->data(kKey).toString() == key) return it;
    }
    return nullptr;
}

QListWidgetItem* FontPalette::heading(const QString& category) const {
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        if (it->data(kKey).toString().isEmpty() && it->data(kCategory).toString() == category) return it;
    }
    return nullptr;
}

QStringList FontPalette::shown() const {
    QStringList out;
    for (QListWidgetItem* it : order_) {
        if (!it->isHidden()) out << it->data(kKey).toString();
    }
    return out;
}

bool FontPalette::missing(const QString& key) const {
    const QListWidgetItem* it = item(key);
    return it != nullptr && it->data(kMissing).toBool();
}

void FontPalette::refresh_files() {
    for (QListWidgetItem* it : order_) {
        const QString key = it->data(kKey).toString();
        const bool gone = !there(key);
        if (gone == it->data(kMissing).toBool()) continue;
        it->setData(kMissing, gone);
        it->setText(gone ? it->data(kLabel).toString() + QStringLiteral("\nファイルなし") : it->data(kLabel).toString());
        if (it->data(kDrawn).toBool()) show_sample(it);
    }
    for (QListWidgetItem* it : order_) {
        const QString key = it->data(kKey).toString();
        const lettering::FaceRow* face = nullptr;
        for (const lettering::FaceRow& row : lettering::catalog_faces()) {
            if (row.key == key) face = &row;
        }
        if (face == nullptr) continue;
        const bool gone = it->data(kMissing).toBool();
        it->setForeground(gone ? QBrush(QColor(theme::tokens().faint)) : QBrush());
        it->setToolTip(gone ? QStringLiteral("%1\n%2\nこの書体のファイルが、このパソコンにありません。アプリを入れ直すと使えます").arg(face->label, face->use)
                            : QStringLiteral("%1\n%2\nクリックで、選んだ台詞がこの書体になります（選んでいなければ、次に置く台詞の書体に）").arg(face->label, face->use));
    }
}

void FontPalette::show_sample(QListWidgetItem* it) {
    const QString key = it->data(kKey).toString();
    QImage sample = face_sample(key, devicePixelRatioF());
    it->setData(kDrawn, true);
    if (sample.isNull()) {  // (not installed — found out for certain when its file was read: said, as a file that is not there)
        if (!it->data(kMissing).toBool() && core::fonts::added(key.toStdString())) {
            it->setData(kMissing, true);
            it->setText(it->data(kLabel).toString() + QStringLiteral("\nファイルなし"));
            it->setForeground(QBrush(QColor(theme::tokens().faint)));
        }
        return;
    }
    // the letters in the list's own text colour (light on a dark look), fainter for a face that cannot be picked
    QPainter painter(&sample);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(sample.rect(), it->data(kMissing).toBool() ? QColor(theme::tokens().faint) : palette().color(QPalette::Active, QPalette::Text));
    painter.end();
    QIcon icon;
    const QPixmap pixmap = QPixmap::fromImage(sample);
    icon.addPixmap(pixmap, QIcon::Normal);
    icon.addPixmap(pixmap, QIcon::Selected);  // (not turned white by the selection: the row's own highlight says it)
    it->setIcon(icon);
}

void FontPalette::draw_some(int most) {
    for (int n = 0; n < most && next_ < static_cast<int>(order_.size()); ++n, ++next_) show_sample(order_[static_cast<std::size_t>(next_)]);
    if (next_ >= static_cast<int>(order_.size())) timer_->stop();
}

void FontPalette::draw_samples() { draw_some(static_cast<int>(order_.size())); }

void FontPalette::filter(const QString& words) {
    const QStringList parts = words.toLower().split(QRegularExpression(QStringLiteral("[\\s　]+")), Qt::SkipEmptyParts);
    std::map<QString, bool> any;
    int found = 0;
    for (QListWidgetItem* it : order_) {
        const QString key = it->data(kKey).toString();
        bool matches = true;
        for (const lettering::FaceRow& row : lettering::catalog_faces()) {
            if (row.key != key) continue;
            for (const QString& part : parts) matches = matches && row.search.contains(part);
        }
        it->setHidden(!matches);
        if (matches) {
            any[it->data(kCategory).toString()] = true;
            ++found;
        }
    }
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        if (it->data(kKey).toString().isEmpty()) it->setHidden(!any[it->data(kCategory).toString()]);
    }
    none_->setVisible(found == 0);
}

std::unique_ptr<QMenu> FontPalette::item_menu(const QString& key) {
    if (item(key) == nullptr) return nullptr;
    auto menu = std::make_unique<QMenu>(this);
    const bool gone = missing(key);
    const std::pair<QString, const char*> entries[] = {{QStringLiteral("選んだ台詞をこの書体にする"), "line"},
                                                       {QStringLiteral("次に置く台詞をこの書体にする"), "next"},
                                                       {QStringLiteral("選んだルビをこの書体にする"), "ruby"},
                                                       {QStringLiteral("この本のいつもの書体にする"), "book"}};
    for (const auto& [words, how] : entries) {
        QAction* act = menu->addAction(words, this, [this, key, how = QString::fromLatin1(how)] { emit picked(key, how); });
        act->setEnabled(!gone);
    }
    return menu;
}

void FontPalette::mark(const QString& key) {
    QListWidgetItem* wanted = item(key);
    const bool fresh = wanted != nullptr && marked() != key;
    const QSignalBlocker quiet(list_);
    for (QListWidgetItem* it : order_) {
        QFont font = it->font();
        font.setBold(it == wanted);
        it->setFont(font);
        it->setBackground(it == wanted ? QBrush(QColor(theme::tokens().accent_soft)) : QBrush());
    }
    list_->clearSelection();
    if (wanted != nullptr) wanted->setSelected(true);
    if (fresh) {
        list_->setCurrentItem(wanted, QItemSelectionModel::NoUpdate);
        list_->scrollToItem(wanted);
    }
    list_->setProperty("marked", wanted != nullptr ? key : QString());
}

QString FontPalette::marked() const { return list_->property("marked").toString(); }

bool FontPalette::event(QEvent* event) {
    // (moved to a screen of another scale: the samples made again at it, one at a time as when first shown)
    if (event->type() == QEvent::DevicePixelRatioChange && list_ != nullptr && next_ > 0) {
        next_ = 0;
        if (isVisible() && !timer_->isActive()) timer_->start();
    }
    return QWidget::event(event);
}

void FontPalette::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // (the look changed — light, dark, the brightness: the samples in the new text colour, the marked row's colour)
    if ((event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) && list_ != nullptr) {
        for (int i = 0; i < next_ && i < static_cast<int>(order_.size()); ++i) show_sample(order_[static_cast<std::size_t>(i)]);
        for (QListWidgetItem* it : order_) {
            if (it->data(kMissing).toBool()) it->setForeground(QBrush(QColor(theme::tokens().faint)));
        }
        if (!marked().isEmpty()) mark(marked());
    }
}

void FontPalette::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh_files();  // (forty stats: which files are there now)
    if (next_ < static_cast<int>(order_.size()) && !timer_->isActive()) timer_->start();
}

bool FontPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
            const QListWidgetItem* current = list_->currentItem();
            if (current != nullptr && !current->data(kKey).toString().isEmpty()) emit picked(current->data(kKey).toString(), QString());
            return true;
        }
    }
    if (watched == list_ && event->type() == QEvent::ContextMenu) {
        auto* asked = static_cast<QContextMenuEvent*>(event);
        QListWidgetItem* at = asked->reason() == QContextMenuEvent::Mouse ? list_->itemAt(list_->viewport()->mapFrom(list_, asked->pos())) : list_->currentItem();
        if (at == nullptr || at->data(kKey).toString().isEmpty()) return true;
        if (const std::unique_ptr<QMenu> menu = item_menu(at->data(kKey).toString())) menu->exec(asked->globalPos());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace genko::app
