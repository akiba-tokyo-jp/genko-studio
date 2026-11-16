// ポーズ集… (人形のポーズ集 G4, SPEC FIGURE-02): app/pose_gallery.hpp.

#include "app/pose_gallery.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>

#include "app/look.hpp"
#include "core/ids.hpp"
#include "core/pose_library.hpp"
#include "render/figure_thumbs.hpp"

namespace genko::app {

using core::Json;
namespace library = core::pose_library;
namespace thumbs = render::figure_thumbs;

namespace {

// The pictures drawn, kept while the app runs (by pose, view, body and size).
thumbs::Cache& cache() {
    static thumbs::Cache c(4096);
    return c;
}

QImage to_image(const std::string& grey, int w, int h) {
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int g = static_cast<unsigned char>(grey[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]);
            line[x] = g == 255 ? qRgb(255, 255, 255) : qRgb(g, g, std::min(255, g + 18));
        }
    }
    return image;
}

}  // namespace

PoseGallery::PoseGallery(QWidget* parent, TargetOf target, Apply apply)
    : QDialog(parent), target_(std::move(target)), apply_(std::move(apply)), pool_(std::make_unique<QThreadPool>()), self_(std::make_shared<PoseGallery*>(this)) {
    setWindowTitle(QStringLiteral("ポーズ集"));
    setObjectName(QStringLiteral("pose_gallery"));
    pool_->setMaxThreadCount(2);
    categories = new QListWidget;
    categories->setObjectName(QStringLiteral("pose_categories"));
    categories->setMaximumWidth(260);
    std::size_t all = 0;
    for (const auto& c : library::categories()) all += c.count;
    auto* every = new QListWidgetItem(QStringLiteral("すべて（%1）").arg(all));
    every->setData(Qt::UserRole, QString());
    categories->addItem(every);
    for (const auto& c : library::categories()) {
        auto* item = new QListWidgetItem(QStringLiteral("%1（%2）").arg(QString::fromStdString(c.ja)).arg(c.count));
        item->setData(Qt::UserRole, QString::fromStdString(c.id));
        item->setToolTip(QString::fromStdString(c.en));
        categories->addItem(item);
    }
    search = new QLineEdit;
    search->setObjectName(QStringLiteral("pose_search"));
    search->setPlaceholderText(QStringLiteral("さがす（日本語・英語。空白で区切ると全部に合うもの）"));
    search->setClearButtonEnabled(true);
    view = new QComboBox;
    view->setObjectName(QStringLiteral("pose_view"));
    view->addItem(QStringLiteral("正面"), thumbs::kFront);
    view->addItem(QStringLiteral("斜め"), thumbs::kThreeQuarter);
    view->addItem(QStringLiteral("横"), thumbs::kSide);
    view->setCurrentIndex(1);
    mirror = new QCheckBox(QStringLiteral("左右反転"));
    mirror->setObjectName(QStringLiteral("pose_mirror"));
    mirror->setToolTip(QStringLiteral("ポーズの左右を入れ替えて付けます（一覧の絵も反転）"));
    keep = new QCheckBox(QStringLiteral("足もとを保つ"));
    keep->setObjectName(QStringLiteral("pose_keep"));
    keep->setChecked(true);
    keep->setToolTip(QStringLiteral("ポーズを付けても、人形のいちばん低い所（足もと）の高さを今のままにします。外すと腰の位置を保ちます"));
    random_button = new QPushButton(QStringLiteral("ランダム"));
    random_button->setObjectName(QStringLiteral("pose_random"));
    random_button->setToolTip(QStringLiteral("いまの分類と言葉に合うポーズから 24 をランダムに出します"));
    all_button = new QPushButton(QStringLiteral("一覧に戻る"));
    all_button->setEnabled(false);
    apply_button = new QPushButton(QStringLiteral("このポーズにする"));
    apply_button->setObjectName(QStringLiteral("pose_apply"));
    apply_button->setDefault(false);
    apply_button->setAutoDefault(false);
    state = new QLabel;
    state->setWordWrap(true);
    grid_ = new QListWidget;
    grid_->setObjectName(QStringLiteral("pose_grid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kWidth, kHeight));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setGridSize(QSize(kWidth + 16, kHeight + 44));
    grid_->setSpacing(4);

    auto* options = new QHBoxLayout;
    options->addWidget(new QLabel(QStringLiteral("向き")));
    options->addWidget(view);
    options->addWidget(mirror);
    options->addWidget(keep);
    options->addWidget(random_button);
    options->addWidget(all_button);
    options->addStretch(1);
    auto* right = new QVBoxLayout;
    right->addWidget(search);
    right->addLayout(options);
    right->addWidget(grid_, 1);
    auto* foot = new QHBoxLayout;
    foot->addWidget(state, 1);
    foot->addWidget(apply_button);
    right->addLayout(foot);
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(categories);
    layout->addLayout(right, 1);

    connect(categories, &QListWidget::currentRowChanged, this, [this](int) { list_poses(); });
    connect(search, &QLineEdit::textChanged, this, [this](const QString&) { list_poses(); });
    connect(view, &QComboBox::currentIndexChanged, this, [this](int) { draw_pictures(); });
    connect(mirror, &QCheckBox::toggled, this, [this](bool) { draw_pictures(); });
    connect(random_button, &QPushButton::clicked, this, [this] { pick_random(); });
    connect(all_button, &QPushButton::clicked, this, [this] { list_poses(); });
    connect(apply_button, &QPushButton::clicked, this, [this] { apply_chosen(); });
    connect(grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { apply_chosen(); });
    connect(grid_, &QListWidget::currentRowChanged, this, [this](int row) { apply_button->setEnabled(row >= 0); });
    follow_target();
    categories->setCurrentRow(0);
    look::fit_to_screen(this, QSize(1100, 720));
}

PoseGallery::~PoseGallery() {
    self_.reset();
    pool_->clear();
    pool_->waitForDone();
}

std::vector<std::string> PoseGallery::shown() const { return ids_; }

void PoseGallery::follow_target() {
    const std::optional<Target> t = target_ ? target_() : std::nullopt;
    const Json body = t && t->body.is_object() ? t->body : Json::object();
    if (body != body_) {
        body_ = body;
        draw_pictures();
    }
}

void PoseGallery::list_poses() {
    library::Filter filter;
    if (QListWidgetItem* item = categories->currentItem()) filter.category = item->data(Qt::UserRole).toString().toStdString();
    filter.query = search->text().toStdString();
    std::vector<std::string> ids;
    for (const Json* p : library::list(filter)) ids.push_back(p->at("id").get<std::string>());
    all_button->setEnabled(false);
    show_ids(ids, QStringLiteral("%1 ポーズ").arg(ids.size()));
}

std::string PoseGallery::pick_random(const std::string& seed, std::size_t n) {
    library::Filter filter;
    if (QListWidgetItem* item = categories->currentItem()) filter.category = item->data(Qt::UserRole).toString().toStdString();
    filter.query = search->text().toStdString();
    std::uint64_t value = 0;
    if (const auto given = library::seed_of(seed)) {
        value = *given;
    } else {
        value = QRandomGenerator::global()->generate64();
    }
    std::vector<std::string> ids;
    for (const Json* p : library::sample(library::list(filter), n, value)) ids.push_back(p->at("id").get<std::string>());
    const std::string used = library::seed_text(value);
    all_button->setEnabled(true);
    show_ids(ids, QStringLiteral("ランダムに %1 ポーズ（%2）").arg(ids.size()).arg(QString::fromStdString(used)));
    return used;
}

void PoseGallery::show_ids(const std::vector<std::string>& ids, const QString& words) {
    ids_ = ids;
    grid_->clear();
    for (const std::string& id : ids_) {
        const Json* pose = library::find(id);
        const QString name = pose != nullptr ? QString::fromStdString(pose->value("ja", id)) : QString::fromStdString(id);
        auto* item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, QString::fromStdString(id));
        const bool pair = pose != nullptr && library::is_pair(*pose);
        item->setToolTip(QStringLiteral("%1\n%2%3").arg(QString::fromStdString(id), pose != nullptr ? QString::fromStdString(pose->value("en", std::string())) : QString(),
                                                        pair ? QStringLiteral("\n二人のポーズ（選んだ人形と、その相手か新しい人形に付けます）") : QString()));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        grid_->addItem(item);
    }
    state->setText(words + QStringLiteral("。ダブルクリックか Enter で、選んだ人形に付けます"));
    apply_button->setEnabled(false);
    draw_pictures();
}

void PoseGallery::draw_pictures() {
    ++generation_;
    pool_->clear();  // (the pictures asked before, not yet started, are not drawn)
    pictures_.assign(ids_.size(), std::string());
    QPixmap blank(kWidth, kHeight);
    blank.fill(QColor(QStringLiteral("#f4f4f4")));
    for (int i = 0; i < grid_->count(); ++i) grid_->item(i)->setIcon(QIcon(blank));
    pending_ = static_cast<int>(ids_.size());
    const double turn = view->currentData().toDouble();
    const bool flipped = mirror->isChecked();
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        std::weak_ptr<PoseGallery*> self = self_;
        const std::string id = ids_[i] + (flipped ? std::string(library::kMirror) : std::string());
        const Json body = body_;
        const int generation = generation_;
        const int row = static_cast<int>(i);
        pool_->start([self, id, body, turn, generation, row]() {
            std::string grey;
            try {
                grey = cache().get(id, turn, body, kWidth, kHeight, 2);
            } catch (const std::exception&) {
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, grey, generation, row]() {
                    const auto alive = self.lock();
                    if (!alive) return;
                    PoseGallery* gallery = *alive;
                    if (generation != gallery->generation_) return;  // (pictures asked before this)
                    --gallery->pending_;
                    if (grey.empty() || row >= gallery->grid_->count()) return;
                    gallery->pictures_[static_cast<std::size_t>(row)] = grey;
                    gallery->grid_->item(row)->setIcon(QIcon(QPixmap::fromImage(to_image(grey, kWidth, kHeight))));
                },
                Qt::QueuedConnection);
        });
    }
}

bool PoseGallery::wait_pictures(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (pending_ > 0) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

std::string PoseGallery::picture_of(int row) const {
    return row >= 0 && static_cast<std::size_t>(row) < pictures_.size() ? pictures_[static_cast<std::size_t>(row)] : std::string();
}

bool PoseGallery::apply_chosen() {
    QListWidgetItem* item = grid_->currentItem();
    if (item == nullptr) return false;
    const std::optional<Target> t = target_ ? target_() : std::nullopt;
    if (!t) {
        state->setText(QStringLiteral("ポーズを付けるデッサン人形がありません。先に人形を置いて選びます"));
        return false;
    }
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    Json op = Json::object({{"op", "pose_figure"},
                            {"page", t->page},
                            {"id", t->id},
                            {"pose_id", id},
                            {"mirror", mirror->isChecked()},
                            {"keep", keep->isChecked() ? "ground" : "pelvis"}});
    const Json* pose = library::find(id);
    const bool pair = pose != nullptr && library::is_pair(*pose);
    if (pair) op["partner_id"] = t->partner.empty() ? core::new_id() : t->partner;  // (its partner, or a new figure beside it)
    const bool done = apply_ && apply_(op);
    if (done) {
        state->setText(QStringLiteral("「%1」%2を%3付けました（元に戻す で取り消せます）")
                           .arg(item->text(), mirror->isChecked() ? QStringLiteral("（左右反転）") : QString(),
                                pair ? (t->partner.empty() ? QStringLiteral("相手の人形を足して二人に") : QStringLiteral("二人に")) : QString()));
        follow_target();
    }
    return done;
}

void PoseGallery::keyPressEvent(QKeyEvent* event) {
    // (Enter puts the pose chosen on the figure from the grid or the button; in the search it does nothing more)
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        QWidget* at = focusWidget();
        if (at == grid_ || at == apply_button) apply_chosen();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

}  // namespace genko::app
