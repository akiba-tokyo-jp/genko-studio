#include "app/material_panel.hpp"
#include "app/config.hpp"
#include "app/material_preview_cache.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QResource>
#include <QSet>
#include <QVBoxLayout>

#include "core/json.hpp"
#include "core/paths.hpp"
#include "render/materials.hpp"
#include "render/selection.hpp"
#include "render/tones.hpp"

// Q_INIT_RESOURCE declares the resource symbol in the current namespace; keep this function global.
static void initialize_material_resources() { Q_INIT_RESOURCE(genko_materials); }

namespace genko::app {

namespace {
// The original Python library remains read-only. No absolute/nested/reparse image path is accepted.
QJsonArray user_entries(const QString& root) {
    const QFileInfo directory(root), file(root + "/library.json");
    if (!directory.isDir() || directory.isSymLink() || directory.canonicalFilePath()!=directory.absoluteFilePath() ||
        file.isSymLink() || !file.isFile() || file.size()> (1<<20)) return {};
    QFile input(file.absoluteFilePath());if(!input.open(QIODevice::ReadOnly))return {};
    const auto document=QJsonDocument::fromJson(input.read((1<<20)+1));
    if(!document.isArray())return {};
    QJsonArray result;QSet<QString> ids;
    const QSet<QString> kinds={"tone","effect","image","lines","lettering","brush","prim"};
    for(const auto& value:document.array()) {
        if(!value.isObject())continue;
        auto entry=value.toObject();const auto id=entry.value("id").toString();
        // (old catalog kinds read as tones, as materials.normalize reads them)
        if(entry.value("kind").toString()=="dot" || entry.value("kind").toString()=="noise")
            entry=QJsonDocument::fromJson(QByteArray::fromStdString(core::dump(render::materials::normalize(core::parse_python_json(
                QJsonDocument(entry).toJson(QJsonDocument::Compact).toStdString())), core::DumpOptions{}))).object();
        if(!id.startsWith("u-") || id.size()>128 || ids.contains(id) || !kinds.contains(entry.value("kind").toString()))continue;
        if(entry.value("name").toString().size()>512 || entry.value("folder").toString().size()>512)continue;
        entry["user"]=true;entry.remove("preview");ids.insert(id);result.append(entry);
    }
    return result;
}
using UserPreview=std::pair<QImage,bool>;
UserPreview user_preview(const QString& library,const QString& filename,const QString& cacheRoot) {
    const QFileInfo directory(library),file(QDir(library).filePath(filename));
    if(filename.isEmpty() || filename=="." || filename==".." || filename.contains('/') || filename.contains('\\') ||
       directory.isSymLink() || directory.canonicalFilePath()!=directory.absoluteFilePath() || file.isSymLink() || !file.isFile() ||
       file.canonicalPath()!=directory.canonicalFilePath() || file.size()<=0 || file.size()>(64<<20))return {};
    QFile source(file.absoluteFilePath());if(!source.open(QIODevice::ReadOnly))return {};
    const QByteArray bytes=source.read((64<<20)+1);if(bytes.size()>(64<<20))return {};
    QBuffer probe;probe.setData(bytes);probe.open(QIODevice::ReadOnly);QImageReader reader(&probe,"PNG");const auto dimensions=reader.size();
    if(!dimensions.isValid() || qint64(dimensions.width())*dimensions.height()>4194304)return {};
    MaterialPreviewSpec spec;spec.render_settings="user-image/png/preserve-aspect";
    const QByteArray digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
    MaterialPreviewCache cache(cacheRoot);bool generated=false;
    QImage image;
    try { image=cache.obtain(digest,spec,[&]{
        generated=true;reader.setScaledSize(dimensions.scaled(spec.pixels,Qt::KeepAspectRatio));
        const QImage decoded=reader.read();if(decoded.isNull())return QImage();
        QImage preview(spec.pixels,QImage::Format_ARGB32);preview.fill(QColor::fromRgba(spec.background));
        QPainter painter(&preview);painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize size=decoded.size().scaled(spec.pixels,Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((spec.pixels.width()-size.width())/2,(spec.pixels.height()-size.height())/2),size),decoded);painter.end();
        return preview;
    }); } catch (...) { return {}; } // A failed decode/allocation is a failed preview, never a GUI exception.
    return {image,generated};
}
// materials.thumbnail for the person's drawn parts (their lines, 1 px, on white) and tones (a swatch at 110 dpi);
// nothing for the other kinds yet.
UserPreview user_drawing(const QByteArray& entry,const QString& cacheRoot) {
    core::Json item;
    try { item=core::parse_python_json(entry.toStdString()); } catch (...) { return {}; }
    const std::string kind=item.value("kind",std::string());
    if(kind!="lines" && kind!="tone")return {};
    MaterialPreviewSpec spec;spec.render_settings=QByteArray("user-")+QByteArray::fromStdString(kind)+"/v1";
    const QByteArray digest=QCryptographicHash::hash(entry,QCryptographicHash::Sha256);
    MaterialPreviewCache cache(cacheRoot);bool generated=false;QImage image;
    try { image=cache.obtain(digest,spec,[&]{
        generated=true;const int size=spec.pixels.width();
        if(kind=="tone") {
            const core::Json tone=item.contains("tone") && item["tone"].is_object() ? item["tone"] : core::Json::object();
            const render::Image swatch=render::tones::swatch(tone,render::Size{size,size},110).convert("RGB");
            const std::string raw=swatch.tobytes();
            return QImage(reinterpret_cast<const uchar*>(raw.data()),swatch.width(),swatch.height(),swatch.width()*3,QImage::Format_RGB888).copy();
        }
        QImage preview(spec.pixels,QImage::Format_RGB32);preview.fill(Qt::white);
        const auto items=render::selection::items_from_json(item.value("items",core::Json::object()));
        double x0=0,y0=0,x1=0,y1=0;bool any=false;
        for(const auto& stroke:items.strokes)for(const auto& p:stroke->points){
            if(!any){x0=x1=p.x;y0=y1=p.y;any=true;}
            x0=std::min(x0,p.x);y0=std::min(y0,p.y);x1=std::max(x1,p.x);y1=std::max(y1,p.y);
        }
        if(!any)return preview;
        const double k=(size-8)/std::max(1e-6,std::max(x1-x0,y1-y0));
        QPainter painter(&preview);painter.setPen(QPen(QColor(20,20,20),1));
        for(const auto& stroke:items.strokes){
            QPolygonF line;for(const auto& p:stroke->points)line<<QPointF(4+(p.x-x0)*k,4+(p.y-y0)*k);
            painter.drawPolyline(line);
        }
        return preview;
    }); } catch (...) { return {}; }
    return {image,generated};
}
// Python's material panel KIND_WORD (the tooltip) and materials.KIND_WORDS (what a search finds a kind by).
const QHash<QString,QString>& kind_word() {
    static const QHash<QString,QString> words={{"tone",QStringLiteral("トーン")},{"effect",QStringLiteral("効果線")},{"image",QStringLiteral("画像")},
        {"lines",QStringLiteral("パーツ")},{"lettering",QStringLiteral("描き文字")},{"brush",QStringLiteral("ブラシ")},{"prim",QStringLiteral("3D")}};
    return words;
}
}

MaterialBrowser::MaterialBrowser(QWidget* parent, std::function<void(const QString&, const QString&)> activate) : QWidget(parent) {
    initialize_material_resources();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    label_ = new QLabel;
    label_->setWordWrap(true);
    label_->hide();
    setProperty("userPreviewGenerations",0);
    layout->addWidget(label_);
    folder_ = new QComboBox;
    folder_->setObjectName(QStringLiteral("materialFolder"));
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("materialSearch"));
    search_->setPlaceholderText(QStringLiteral("素材を探す（名前・タグ・種類）"));
    search_->setClearButtonEnabled(true);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("materialList"));
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(56, 56));
    list_->setGridSize(QSize(84, 100));
    list_->setResizeMode(QListView::Adjust);
    list_->setWordWrap(true);
    list_->setMinimumSize(100, 100);
    folder_row_ = new QHBoxLayout;
    folder_row_->addWidget(folder_, 1);
    layout->addLayout(folder_row_);
    layout->addWidget(search_);
    layout->addWidget(list_, 1);

    // Only one visible user preview is in flight. Widgets and original libraries never enter the worker.
    timer_=new QTimer(this);timer_->setSingleShot(true);
    auto* watcher=new QFutureWatcher<UserPreview>(this);
    QObject::connect(timer_,&QTimer::timeout,this,[this,watcher]{
        if(!list_->isVisible() || watcher->isRunning())return;
        list_->doItemsLayout();
        for(int row=0;row<list_->count();++row){auto* item=list_->item(row);
            if(!item->data(Qt::UserRole+1).toBool() || item->data(Qt::UserRole+3).toBool() ||
               !list_->visualItemRect(item).intersects(list_->viewport()->rect()))continue;
            const QString filename=item->data(Qt::UserRole+2).toString();
            const QByteArray drawing=item->data(Qt::UserRole+5).toByteArray();
            if(filename.isEmpty() && drawing.isEmpty()){item->setData(Qt::UserRole+3,true);continue;}
            watcher->setProperty("entryId",item->data(Qt::UserRole));
            watcher->setProperty("entryFile",filename);
            const QString library=library_,cacheRoot=cache_root_;
            if(!filename.isEmpty())watcher->setFuture(QtConcurrent::run([library,filename,cacheRoot]{return user_preview(library,filename,cacheRoot);}));
            else watcher->setFuture(QtConcurrent::run([drawing,cacheRoot]{return user_drawing(drawing,cacheRoot);}));
            break;
        }
    });
    QObject::connect(watcher,&QFutureWatcher<UserPreview>::finished,this,[this,watcher]{
        UserPreview result;
        try { result=watcher->result(); } catch (...) { /* Future failures follow the same original-preserving UI path. */ }
        if(result.second)setProperty("userPreviewGenerations",property("userPreviewGenerations").toInt()+1);
        for(int row=0;row<list_->count();++row){auto* item=list_->item(row);
            if(item->data(Qt::UserRole)!=watcher->property("entryId") || item->data(Qt::UserRole+2).toString()!=watcher->property("entryFile").toString())continue;
            item->setData(Qt::UserRole+3,true);
            if(!result.first.isNull())item->setIcon(QIcon(QPixmap::fromImage(result.first)));
            else if(!item->data(Qt::UserRole+2).toString().isEmpty())item->setToolTip(item->toolTip()+QStringLiteral("（画像を読み込めません。原本は変更していません）"));
        }
        timer_->start(0);
    });
    QObject::connect(list_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int){timer_->start(0);});
    QObject::connect(list_->verticalScrollBar(),&QScrollBar::rangeChanged,this,[this](int,int){timer_->start(0);});
    if(activate)QObject::connect(list_,&QListWidget::itemDoubleClicked,this,[activate](QListWidgetItem* item){if(item)activate(item->data(Qt::UserRole).toString(),item->data(Qt::UserRole+4).toString());});
    QObject::connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { fill(); });
    QObject::connect(folder_, &QComboBox::currentIndexChanged, this, [this](int) { fill(); });
    reload();
}

void MaterialBrowser::reload() {
    // (UTF-8 paths: on Windows, path::string() is the ANSI code page, which a Japanese user folder may not fit)
    library_=QString::fromStdString(core::path_to_utf8(config_dir()/"materials"));
    cache_root_=QString::fromStdString(core::path_to_utf8(config_dir()/"cache"/"material-previews"));
    entries_.clear();
    QFile input(QStringLiteral(":/genko/materials/catalog.json"));
    if (!input.open(QIODevice::ReadOnly)) {
        label_->setText(QStringLiteral("組込み素材の一覧を読めませんでした。原稿は変更していません。"));
        label_->show();
    } else {
        const QJsonDocument catalog = QJsonDocument::fromJson(input.readAll());
        if (!catalog.isArray()) {
            label_->setText(QStringLiteral("組込み素材の一覧が壊れています。原稿は変更していません。"));
            label_->show();
        } else {
            for (const QJsonValue& value : catalog.array()) entries_.append(value.toObject());
        }
    }
    for(const auto& value:user_entries(library_))entries_.append(value.toObject());
    fill_folders();
}

void MaterialBrowser::fill_folders() {
    const QString keep=folder_->currentData().toString();
    folder_->blockSignals(true);
    folder_->clear();
    folder_->addItem(QStringLiteral("すべて"), QString());
    QStringList names;
    for (const QVariant& value : entries_) {
        const QString name=value.toJsonObject().value(QStringLiteral("folder")).toString();
        const QString shown=name.isEmpty()?QStringLiteral("その他"):name;
        if(!names.contains(shown))names<<shown;
    }
    try {  // (and the empty folders made with ＋フォルダ; the catalog is not read again)
        for(const std::string& name:render::materials::empty_folders(config_dir()))if(!names.contains(QString::fromStdString(name)))names<<QString::fromStdString(name);
    } catch (const std::exception&) {}
    for(const QString& name:names)folder_->addItem(name,name);
    folder_->setCurrentIndex(std::max(0,folder_->findData(keep)));
    folder_->blockSignals(false);
    fill();
}

void MaterialBrowser::fill() {
    const QString selected = current_id();
    list_->clear();
    const QString query = search_->text().trimmed();
    const QString where = folder_->currentData().toString();
    // (materials.search: every word in the name, the folder, the tags or the kind's words; a search looks in every folder)
    const QStringList words = query.toLower().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QVariant& value : entries_) {
        const QJsonObject entry = value.toJsonObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const QString category = entry.value(QStringLiteral("folder")).toString();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        if (!where.isEmpty() && query.isEmpty() && (category.isEmpty() ? QStringLiteral("その他") : category) != where) continue;
        QStringList tags;
        for (const QJsonValue& tag : entry.value(QStringLiteral("tags")).toArray()) tags << (tag.isString() ? tag.toString() : QString::number(tag.toDouble()));
        const QString hay = (QStringList{name, category} + tags + QStringList{QString::fromStdString(render::materials::kind_words(kind.toStdString()))})
                                .join(QLatin1Char(' ')).toLower();
        bool matched = true;
        for (const QString& word : words) if (!hay.contains(word)) { matched = false; break; }
        if (!matched) continue;
        const QString id = entry.value(QStringLiteral("id")).toString();
        // QIcon reads the prebuilt PNG on demand. No renderer/Python is invoked on startup or scrolling.
        auto* item = new QListWidgetItem(QIcon(entry.value(QStringLiteral("preview")).toString()), name.isEmpty() ? id : name, list_);
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole+4,kind);
        const bool user=entry.value("user").toBool();
        item->setData(Qt::UserRole+1,user);
        if(user&&kind=="image")item->setData(Qt::UserRole+2,entry.value("file").toString());
        if(user&&(kind=="lines"||kind=="tone"))item->setData(Qt::UserRole+5,QJsonDocument(entry).toJson(QJsonDocument::Compact));
        item->setData(Qt::UserRole+3,!user);
        item->setToolTip(kind_word().value(kind) + QStringLiteral(" ・ ") + category + (user ? QStringLiteral(" ・ マイ素材") : QString()) +
                         (tags.isEmpty() ? QString() : QStringLiteral("\nタグ: ") + tags.join(QStringLiteral("、"))));
        if (id == selected) list_->setCurrentItem(item);
    }
    timer_->start(0);
}

QString MaterialBrowser::current_id() const { return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString(); }

void MaterialBrowser::select(const QString& material_id) {
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(Qt::UserRole).toString() == material_id) {
            list_->setCurrentRow(row);
            return;
        }
    }
}

QString MaterialBrowser::folder() const { return folder_->currentData().toString(); }

void MaterialBrowser::choose_folder(const QString& name) { folder_->setCurrentIndex(std::max(0, folder_->findData(name))); }

void MaterialBrowser::add_beside_folder(QWidget* widget) { folder_row_->addWidget(widget); }

QWidget* make_builtin_material_panel(QWidget* parent, std::function<void(const QString&, const QString&)> activate) {
    return new MaterialBrowser(parent, std::move(activate));
}

}  // namespace genko::app
