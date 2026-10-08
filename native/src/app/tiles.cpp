#include "app/tiles.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <future>
#include <mutex>
#include <unordered_set>

#include "core/anim.hpp"
#include "core/pyconv.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/not_yet_ported.hpp"
#include "render/anim.hpp"
#include "render/colour.hpp"
#include "render/page.hpp"

namespace genko::app {

namespace {

bool same_num(const core::Num& a, const core::Num& b) { return a == b; }

bool same_rect(const core::Rect& a, const core::Rect& b) {
    return same_num(a.x, b.x) && same_num(a.y, b.y) && same_num(a.width, b.width) && same_num(a.height, b.height);
}

bool same_points(const std::optional<std::vector<core::Point>>& a, const std::optional<std::vector<core::Point>>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->size() != b->size()) return false;
    for (std::size_t i = 0; i < a->size(); ++i) {
        if (!same_num((*a)[i].x, (*b)[i].x) || !same_num((*a)[i].y, (*b)[i].y)) return false;
    }
    return true;
}

bool same_frames(const std::vector<core::Frame>& a, const std::vector<core::Frame>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const core::Frame& x = a[i];
        const core::Frame& y = b[i];
        if (x.id != y.id || !same_rect(x.rect, y.rect) || x.split_axis != y.split_axis || x.clip != y.clip || x.bleed != y.bleed ||
            x.border_mm != y.border_mm || !same_points(x.poly, y.poly) || x.split != y.split || x.custom != y.custom ||
            x.curves != y.curves || x.line != y.line || x.corner_mm != y.corner_mm || !same_frames(x.children, y.children)) {
            return false;
        }
    }
    return true;
}

bool same_spec(const core::PageSpec& a, const core::PageSpec& b) {
    return same_num(a.width_mm, b.width_mm) && same_num(a.height_mm, b.height_mm) && same_num(a.dpi, b.dpi) &&
           same_num(a.bleed_mm, b.bleed_mm) && same_num(a.inner_margin_mm, b.inner_margin_mm) && a.expression == b.expression &&
           a.preset == b.preset && a.trim_w_mm == b.trim_w_mm && a.trim_h_mm == b.trim_h_mm && a.margins_mm == b.margins_mm;
}

// Everything of a layer that is drawn, except its lines.
bool same_layer_look(const core::Layer& a, const core::Layer& b) {
    if (a.patches.size() != b.patches.size()) return false;
    for (std::size_t i = 0; i < a.patches.size(); ++i) {
        if (a.patches[i].png != b.patches[i].png || a.patches[i].attrs != b.patches[i].attrs) return false;
    }
    const bool masks = a.mask.has_value() == b.mask.has_value() && (!a.mask || (a.mask->png == b.mask->png && a.mask->enabled == b.mask->enabled));
    return masks && a.id == b.id && a.role == b.role && a.kind == b.kind && a.visible == b.visible && a.exportable == b.exportable &&
           a.raster_png == b.raster_png && a.color_raster == b.color_raster && a.fill_rgb == b.fill_rgb && a.opacity == b.opacity && a.blend == b.blend && a.clip == b.clip &&
           a.lock_alpha == b.lock_alpha && a.panel_clip == b.panel_clip && a.panel_each == b.panel_each && a.tone == b.tone &&
           a.color == b.color && a.fill == b.fill && a.adjust == b.adjust && a.effect == b.effect && a.color_prints == b.color_prints &&
           a.screen == b.screen && a.asset == b.asset && a.region.has_value() == b.region.has_value() && a.angle == b.angle;
}

// The page drawn faintly over `page` (render_page's onion: in name and proof, the first page of that index), or none.
const core::Page* onion_page(const core::Document& doc, const core::Page& page, const std::string& mode) {
    if ((mode != "name" && mode != "proof") || !page.onion_from || !page.onion_from->truthy()) return nullptr;
    for (const auto& p : doc.pages) {
        if (p->index == *page.onion_from) return p.get();
    }
    return nullptr;
}

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888)
        .convertToFormat(QImage::Format_RGB32);
}

}  // namespace

struct PageRenderer::Result {
    std::string page_id;
    int dpi = 0;
    int index = 0;
    QRect region;  // page px
    std::uint64_t generation = 0;
    bool rough = false;
    bool cancelled = false;
    QString error;
    QImage image;
    std::vector<std::string> omitted;
};

PageRenderer::PageRenderer(QObject* parent) : QObject(parent), self_(std::make_shared<PageRenderer*>(this)) {
    pool_.setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 2));
}

PageRenderer::~PageRenderer() {
    self_.reset();  // (results still coming find no one)
    for (auto& [dpi, level] : levels_) {
        for (Tile& tile : level.tiles) {
            if (tile.stop) tile.stop->request_stop();
        }
    }
    pool_.waitForDone();
}

const core::Page* PageRenderer::page() const { return shown_page_; }

void PageRenderer::clear() {
    for (auto& [dpi, level] : levels_) {
        for (Tile& tile : level.tiles) {
            if (tile.stop) tile.stop->request_stop();
        }
    }
    levels_.clear();
    doc_.reset();
    page_ptr_.reset();
    shown_page_ = nullptr;
    page_id_.clear();
    ++generation_;
    omitted_.clear();
    emit omittedChanged(omitted_);
    emit updated(QRectF());
}

void PageRenderer::set_mode(const std::string& mode) {
    if (mode == mode_) return;
    mode_ = mode;
    ++generation_;
    for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
    dispatch();
}

// The onion skin of one frame of one page (the page by its identity: a changed page is another), made by the first tile
// that needs it while the others wait for it.
struct PageRenderer::OnionStore {
    using Value = std::shared_ptr<const render::Image>;
    std::mutex mutex;
    std::shared_ptr<const core::Page> page;
    std::int64_t frame = 0;
    int dpi = 0;
    std::shared_future<Value> value;

    Value get(const std::shared_ptr<const core::Page>& of, std::int64_t at, int resolution, const DocPtr& doc) {
        std::promise<Value> made;
        std::shared_future<Value> wait;
        bool mine = false;
        {
            std::lock_guard lock(mutex);
            if (page != of || frame != at || dpi != resolution || !value.valid()) {
                page = of;
                frame = at;
                dpi = resolution;
                value = made.get_future().share();
                mine = true;
            }
            wait = value;
        }
        if (mine) {
            try {
                auto ghost = render::anim::onion(*of, at, resolution, 1, 1, 0.35, doc.get());
                made.set_value(ghost ? std::make_shared<const render::Image>(std::move(*ghost)) : nullptr);
            } catch (...) {
                made.set_value(nullptr);  // (a faint picture that cannot be made: the frame alone)
            }
        }
        return wait.get();
    }
};

void PageRenderer::set_anim(std::int64_t frame, bool onion) {
    if (frame == frame_ && onion == onion_) return;
    frame_ = frame;
    onion_ = onion;
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;  // (nothing on screen changes)
    ++generation_;
    for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
    dispatch();
}

void PageRenderer::set_cmyk_proof(std::optional<std::optional<std::filesystem::path>> proof) {
    if (proof == proof_) return;
    proof_ = std::move(proof);
    ++generation_;
    for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
    dispatch();
}

void PageRenderer::show(DocPtr doc, std::size_t index) {
    if (!doc || index >= doc->pages.size() || doc->is_deferred(index)) {
        clear();  // (a page whose strokes and pictures are not read yet: shown when they are)
        return;
    }
    const std::shared_ptr<core::Page>& ptr = doc->pages[index];
    const core::Page& page = *ptr;
    const bool same_page = doc_ && page.id == page_id_ && shown_page_ != nullptr;
    // (the page drawn faintly over it, edited, gone or come: all of it changed)
    const bool same_onion = !same_page || onion_page(*doc_, *shown_page_, mode_) == onion_page(*doc, page, mode_);
    if (!same_page) {
        for (auto& [dpi, level] : levels_) {
            for (Tile& tile : level.tiles) {
                if (tile.stop) tile.stop->request_stop();
            }
        }
        levels_.clear();
        page_id_ = page.id;
        ++generation_;
        first_shown_ = false;
        rough_first_ = true;
        omitted_.clear();
        emit omittedChanged(omitted_);
    } else if (shown_page_ != &page || doc_->brush_custom != doc->brush_custom || doc_->story.size() != doc->story.size() || !same_onion) {
        // the same page, changed: only the tiles its new or removed lines cover are drawn again (anything else
        // changed: all of them)
        const core::Page& old = *shown_page_;
        bool whole = !(same_spec(old.spec, page.spec) && same_frames(old.frames, page.frames) && old.binding == page.binding &&
                       old.fills == page.fills && old.extra == page.extra && old.effects == page.effects && old.ruler == page.ruler &&
                       old.rulers == page.rulers && old.prims == page.prims && old.numero == page.numero &&
                       old.onion_from == page.onion_from && same_onion && old.layers.size() == page.layers.size() &&
                       doc_->brush_custom == doc->brush_custom && doc_->story.size() == doc->story.size());
        std::vector<core::StrokePtr> touched;
        if (!whole) {
            for (std::size_t i = 0; i < page.layers.size() && !whole; ++i) {
                const core::Layer& a = old.layers[i];
                const core::Layer& b = page.layers[i];
                if (!same_layer_look(a, b)) {
                    whole = true;
                    break;
                }
                if (a.strokes == b.strokes) continue;
                if (b.effect && core::py_truthy(*b.effect)) {  // (border effects reach beyond a line's own box)
                    whole = true;
                    break;
                }
                std::unordered_set<const core::Stroke*> was;
                for (const auto& s : a.strokes->items) was.insert(s.get());
                std::unordered_set<const core::Stroke*> now;
                for (const auto& s : b.strokes->items) {
                    now.insert(s.get());
                    if (!was.contains(s.get())) touched.push_back(s);
                }
                for (const auto& s : a.strokes->items) {
                    if (!now.contains(s.get())) touched.push_back(s);
                }
            }
        }
        ++generation_;
        for (auto& [dpi, level] : levels_) {
            if (whole) {
                mark(level, QRect(QPoint(0, 0), level.size), generation_);
                continue;
            }
            const render::Size size{level.size.width(), level.size.height()};
            for (const core::StrokePtr& stroke : touched) {
                const auto box = render::brushes::extent(size, core::stroke_points(*stroke), dpi,
                                                         stroke->width_mm != 0.0 ? stroke->width_mm : 0.35, stroke->kind);
                if (!box) continue;
                mark(level, QRect(box->x0 - 2, box->y0 - 2, box->width() + 4, box->height() + 4), generation_);
            }
        }
    }
    doc_ = std::move(doc);
    index_ = index;
    page_ptr_ = ptr;
    shown_page_ = &page;
    dispatch();
}

QRect PageRenderer::tile_rect(const Level& level, int index) const {
    const int col = index % level.cols;
    const int row = index / level.cols;
    return QRect(col * kTile, row * kTile, kTile, kTile).intersected(QRect(QPoint(0, 0), level.size));
}

PageRenderer::Level& PageRenderer::level(int dpi, bool whole) {
    auto it = levels_.find(dpi);
    if (it != levels_.end()) {
        it->second.whole = it->second.whole || whole;
        return it->second;
    }
    Level made;
    made.dpi = dpi;
    const core::Page& page = *shown_page_;
    made.size = QSize(render::mm_to_px(page.spec.width_mm.value(), dpi), render::mm_to_px(page.spec.height_mm.value(), dpi));
    made.cols = (made.size.width() + kTile - 1) / kTile;
    made.rows = (made.size.height() + kTile - 1) / kTile;
    made.whole = whole;
    made.tiles.resize(static_cast<std::size_t>(made.cols * made.rows));
    // a page seen for the first time with many lines: a rough look first, then the real one
    const bool rough = whole && rough_first_ && render::rough_needed(page, dpi);
    for (std::size_t i = 0; i < made.tiles.size(); ++i) {
        Tile& tile = made.tiles[i];
        tile.changed = generation_;
        tile.pending = QRect(QPoint(0, 0), tile_rect(made, static_cast<int>(i)).size());
        tile.want_rough = rough;
    }
    rough_first_ = false;
    return levels_.emplace(dpi, std::move(made)).first->second;
}

void PageRenderer::mark(Level& level, const QRect& px, std::uint64_t generation) {
    const QRect area = px.intersected(QRect(QPoint(0, 0), level.size));
    if (area.isEmpty()) return;
    for (int row = area.top() / kTile; row <= area.bottom() / kTile && row < level.rows; ++row) {
        for (int col = area.left() / kTile; col <= area.right() / kTile && col < level.cols; ++col) {
            const int index = row * level.cols + col;
            Tile& tile = level.tiles[static_cast<std::size_t>(index)];
            const QRect rect = tile_rect(level, index);
            tile.changed = generation;
            tile.pending = tile.pending.united(area.intersected(rect).translated(-rect.topLeft()));
            if (tile.running && tile.stop) tile.stop->request_stop();  // (what it is drawing is out of date)
        }
    }
}

void PageRenderer::want(int base_dpi, int wanted_dpi, const QRectF& visible_mm) {
    if (!shown_page_) return;
    base_dpi_ = base_dpi;
    wanted_dpi_ = wanted_dpi;
    visible_mm_ = visible_mm;
    Level& base = level(base_dpi, true);
    // the finer tiles of the part in sight, nearest the middle first
    for (auto& [dpi, item] : levels_) item.wanted.clear();
    const auto in_sight = [&](Level& l) {
        const double px = l.dpi / 25.4;
        const QRect seen = QRectF(visible_mm.x() * px, visible_mm.y() * px, visible_mm.width() * px, visible_mm.height() * px)
                               .toAlignedRect()
                               .adjusted(-kTile / 2, -kTile / 2, kTile / 2, kTile / 2)
                               .intersected(QRect(QPoint(0, 0), l.size));
        const QPointF middle = QRectF(seen).center();
        std::vector<std::pair<double, int>> order;
        for (int i = 0; i < static_cast<int>(l.tiles.size()); ++i) {
            const QRect rect = tile_rect(l, i);
            if (!l.whole && !rect.intersects(seen)) continue;
            const QPointF d = QRectF(rect).center() - middle;
            order.emplace_back((rect.intersects(seen) ? 0.0 : 1e12) + d.x() * d.x() + d.y() * d.y(), i);
        }
        std::sort(order.begin(), order.end());
        for (const auto& [distance, i] : order) l.wanted.push_back(i);
    };
    in_sight(base);
    if (wanted_dpi > base_dpi) in_sight(level(wanted_dpi, false));
    // levels no longer needed go once the base is whole again (the old one shows meanwhile)
    const bool base_ready = std::all_of(base.tiles.begin(), base.tiles.end(), [](const Tile& t) { return t.valid > 0; });
    for (auto it = levels_.begin(); it != levels_.end();) {
        const bool keep = it->first == base_dpi || (wanted_dpi > base_dpi && it->first == wanted_dpi) || !base_ready;
        if (keep) {
            ++it;
            continue;
        }
        for (Tile& tile : it->second.tiles) {
            if (tile.stop) tile.stop->request_stop();
        }
        it = levels_.erase(it);
    }
    dispatch();
}

void PageRenderer::dispatch() {
    if (!shown_page_ || !doc_) return;
    if (proof_ && proof_profile_) {  // (the profile as it is now: another one, or its file gone, draws everything again)
        std::optional<std::filesystem::path> now = proof_profile_();
        if (now != *proof_) {
            proof_ = std::move(now);
            ++generation_;
            for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
        }
    }
    const int most = pool_.maxThreadCount();
    // the whole page first (the tiles in sight before the others), then the finer tiles in sight
    std::vector<Level*> order;
    if (auto it = levels_.find(base_dpi_); it != levels_.end()) order.push_back(&it->second);
    if (wanted_dpi_ > base_dpi_) {
        if (auto it = levels_.find(wanted_dpi_); it != levels_.end()) order.push_back(&it->second);
    }
    for (Level* l : order) {
        for (const int index : l->wanted) {
            if (running_ >= most) return;
            Tile& tile = l->tiles[static_cast<std::size_t>(index)];
            if (tile.running || tile.pending.isEmpty()) continue;
            const QRect rect = tile_rect(*l, index);
            const bool rough = tile.want_rough;
            const QRect region = rough ? rect : tile.pending.translated(rect.topLeft());
            tile.running = true;
            tile.stop = std::make_shared<std::stop_source>();
            ++running_;
            std::weak_ptr<PageRenderer*> self = self_;
            DocPtr doc = doc_;
            const std::size_t page_index = index_;
            const std::string page_id = page_id_;
            const std::uint64_t generation = generation_;
            const int dpi = l->dpi;
            const std::string mode = mode_;
            const auto proof = proof_;
            const std::stop_token stop = tile.stop->get_token();
            const std::int64_t frame = frame_;
            const bool onion = onion_;
            if (!onions_) onions_ = std::make_shared<OnionStore>();
            const std::shared_ptr<OnionStore> onions = onions_;
            pool_.start([self, doc, page_index, page_id, generation, dpi, index, region, rough, mode, proof, stop, frame, onion, onions]() {
                Result result;
                result.page_id = page_id;
                result.dpi = dpi;
                result.index = index;
                result.region = region;
                result.generation = generation;
                result.rough = rough;
                try {
                    render::RenderOptions options;
                    options.mode = mode;
                    options.rough = rough;
                    options.skip_unported = true;  // (a preview on screen: what is not drawn yet is reported)
                    options.region = render::RenderRegion{region.x(), region.y(), region.width(), region.height()};
                    options.stop = stop;
                    const std::shared_ptr<const core::Page> raw = doc->pages[page_index];
                    render::RenderResult drawn;
                    if (core::anim::is_animation(*raw)) {  // (the frame shown, with the frames around it faint)
                        options.at_frame = true;
                        drawn = render::render_page(core::anim::at_frame(*raw, frame), dpi, options, doc.get());
                        if (onion) {
                            if (const auto ghost = onions->get(raw, frame, std::min(dpi, 100), doc)) {
                                const render::Size full{render::mm_to_px(raw->spec.width_mm.value(), dpi),
                                                        render::mm_to_px(raw->spec.height_mm.value(), dpi)};
                                const render::Box box{region.x(), region.y(), region.x() + region.width(), region.y() + region.height()};
                                const render::Image part = ghost->size() == full ? ghost->crop(box) : ghost->resize_region(full, box);
                                drawn.image = render::alpha_composite(drawn.image.convert("RGBA"), part).convert("RGB");
                            }
                        }
                    } else {
                        drawn = render::render_page(*raw, dpi, options, doc.get());
                    }
                    if (proof) drawn.image = render::colour::proof(drawn.image, *proof);  // (per pixel: a tile as the page)
                    result.image = to_qimage(drawn.image);
                    result.omitted = std::move(drawn.omitted);
                } catch (const render::Cancelled&) {
                    result.cancelled = true;
                } catch (const std::exception& error) {
                    result.error = QString::fromUtf8(error.what());
                }
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, result = std::move(result)]() {
                        if (const auto alive = self.lock()) (*alive)->deliver(result);
                    },
                    Qt::QueuedConnection);
            });
        }
    }
}

void PageRenderer::report_omitted(const std::vector<std::string>& elements) {
    bool more = false;
    for (const std::string& element : elements) {
        const QString name = QString::fromStdString(element);
        if (!omitted_.contains(name)) {
            omitted_.push_back(name);
            more = true;
        }
    }
    if (more) emit omittedChanged(omitted_);
}

void PageRenderer::deliver(const Result& r) {
    --running_;
    auto it = levels_.find(r.dpi);
    if (it == levels_.end() || r.page_id != page_id_ || r.index >= static_cast<int>(it->second.tiles.size())) {
        dispatch();
        return;
    }
    Level& l = it->second;
    Tile& tile = l.tiles[static_cast<std::size_t>(r.index)];
    tile.running = false;
    tile.stop.reset();
    if (r.cancelled || r.generation < tile.changed) {  // (drawn from an older page: thrown away)
        dispatch();
        return;
    }
    const QRect rect = tile_rect(l, r.index);
    const double px = 25.4 / r.dpi;  // (the part drawn, in mm: only that part of the screen is painted again)
    const auto in_mm = [px](const QRect& part) { return QRectF(part.x() * px, part.y() * px, part.width() * px, part.height() * px); };
    if (!r.error.isEmpty()) {
        // (a broken asset must not take the editor down: the tile stays white and the problem is told)
        emit failed(r.error);
        if (tile.image.isNull()) {
            tile.image = QImage(rect.size(), QImage::Format_RGB32);
            tile.image.fill(Qt::white);
        }
        tile.pending = QRect();
        tile.want_rough = false;
        tile.rough = false;
        tile.valid = r.generation;
        dispatch();
        emit updated(in_mm(rect));
        return;
    }
    if (r.rough && tile.valid >= tile.changed && !tile.rough && tile.pending.isEmpty()) {
        dispatch();  // (the real picture came first)
        return;
    }
    if (tile.image.isNull()) {
        tile.image = QImage(rect.size(), QImage::Format_RGB32);
        tile.image.fill(Qt::white);
    }
    {
        QPainter painter(&tile.image);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(r.region.topLeft() - rect.topLeft(), r.image);
    }
    if (r.rough) {
        tile.rough = true;
        tile.want_rough = false;
    } else {
        tile.rough = false;
        tile.pending = QRect();  // (it was exactly this region: anything newer would have a newer generation)
    }
    tile.valid = r.generation;
    report_omitted(r.omitted);
    if (!first_shown_) {
        first_shown_ = true;
        emit firstShown();
    }
    dispatch();
    emit updated(in_mm(r.region));
    if (settled()) emit settledChanged();
}

bool PageRenderer::current(int dpi, const QRectF& rect_mm) const {
    const auto it = levels_.find(dpi);
    if (it == levels_.end()) return false;
    const Level& l = it->second;
    const double px = dpi / 25.4;
    const QRect area = QRectF(rect_mm.x() * px, rect_mm.y() * px, rect_mm.width() * px, rect_mm.height() * px)
                           .toAlignedRect()
                           .intersected(QRect(QPoint(0, 0), l.size));
    if (area.isEmpty()) return true;
    for (int row = area.top() / kTile; row <= area.bottom() / kTile && row < l.rows; ++row) {
        for (int col = area.left() / kTile; col <= area.right() / kTile && col < l.cols; ++col) {
            const Tile& tile = l.tiles[static_cast<std::size_t>(row * l.cols + col)];
            if (tile.valid == 0 || tile.valid < tile.changed || !tile.pending.isEmpty() || tile.rough) return false;
        }
    }
    return true;
}

bool PageRenderer::settled() const {
    if (!shown_page_) return true;
    for (const auto& [dpi, l] : levels_) {
        if (dpi != base_dpi_ && dpi != wanted_dpi_) continue;
        for (const int index : l.wanted) {
            const Tile& tile = l.tiles[static_cast<std::size_t>(index)];
            if (tile.valid == 0 || tile.valid < tile.changed || !tile.pending.isEmpty() || tile.rough || tile.running) return false;
        }
    }
    return true;
}

int PageRenderer::shown_dpi() const {
    if (wanted_dpi_ > base_dpi_ && current(wanted_dpi_, visible_mm_)) return wanted_dpi_;
    return base_dpi_;
}

void PageRenderer::paint(QPainter& painter, const QRectF& visible_mm) const {
    const QTransform to_screen = painter.transform();
    for (const auto& [dpi, l] : levels_) {
        const bool finer = dpi > base_dpi_ && base_dpi_ > 0;
        const double mm = 25.4 / dpi;
        // page pixels → the screen; exactly one to one at 100 % (no resampling of the page's pixels)
        QTransform t = QTransform::fromScale(mm, mm) * to_screen;
        if (std::abs(t.m11() - 1.0) < 1e-9 && std::abs(t.m22() - 1.0) < 1e-9 && std::abs(t.m12()) < 1e-12 && std::abs(t.m21()) < 1e-12) {
            t = QTransform::fromTranslate(std::round(t.dx()), std::round(t.dy()));
        }
        painter.setTransform(t);
        const double px = dpi / 25.4;
        const QRect seen = QRectF(visible_mm.x() * px, visible_mm.y() * px, visible_mm.width() * px, visible_mm.height() * px)
                               .toAlignedRect()
                               .adjusted(-2, -2, 2, 2);
        for (int i = 0; i < static_cast<int>(l.tiles.size()); ++i) {
            const Tile& tile = l.tiles[static_cast<std::size_t>(i)];
            if (tile.image.isNull()) continue;
            if (finer && (tile.valid < tile.changed || !tile.pending.isEmpty() || tile.rough)) continue;  // (only finished ones)
            const QRect rect = tile_rect(l, i);
            if (!rect.intersects(seen)) continue;
            painter.drawImage(rect.topLeft(), tile.image);
        }
    }
    painter.setTransform(to_screen);
}

QImage PageRenderer::compose(int dpi) const {
    const auto it = levels_.find(dpi);
    if (it == levels_.end()) return {};
    const Level& l = it->second;
    QImage out(l.size, QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    for (int i = 0; i < static_cast<int>(l.tiles.size()); ++i) {
        const Tile& tile = l.tiles[static_cast<std::size_t>(i)];
        if (!tile.image.isNull()) painter.drawImage(tile_rect(l, i).topLeft(), tile.image);
    }
    painter.end();
    return out;
}

}  // namespace genko::app
