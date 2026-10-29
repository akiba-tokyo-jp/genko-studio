#include "app/tiles.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <atomic>
#include <chrono>
#include <mutex>
#include <tuple>
#include <set>
#include <unordered_set>

#include "app/display_color.hpp"
#include "core/anim.hpp"
#include "core/covers.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/not_yet_ported.hpp"
#include "render/anim.hpp"
#include "render/color_canvas.hpp"
#include "render/colour.hpp"
#include "render/hdr_preview.hpp"
#include "render/ink_canvas.hpp"
#include "render/inspect.hpp"
#include "render/page.hpp"
#include "render/proof.hpp"

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

// channels@1: a page's channels as they are drawn (a spot printed; an alpha channel laid over).
bool same_channels(const std::vector<core::Channel>& a, const std::vector<core::Channel>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const core::Channel& x = a[i];
        const core::Channel& y = b[i];
        if (x.id != y.id || x.kind != y.kind || x.raster != y.raster || x.color != y.color || x.opacity != y.opacity ||
            x.indicates != y.indicates || x.solidity != y.solidity) {
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
           a.raster_png == b.raster_png && a.color_raster == b.color_raster && a.ink_raster == b.ink_raster && a.fill_rgb == b.fill_rgb && a.opacity == b.opacity && a.blend == b.blend && a.clip == b.clip &&
           a.lock_alpha == b.lock_alpha && a.panel_clip == b.panel_clip && a.panel_each == b.panel_each && a.tone == b.tone &&
           a.color == b.color && a.fill == b.fill && a.adjust == b.adjust && a.effect == b.effect && a.color_prints == b.color_prints &&
           a.screen == b.screen && a.asset == b.asset && a.region.has_value() == b.region.has_value() && a.angle == b.angle &&
           a.smart == b.smart &&  // (smart-objects@1: its matrix and settings)
           a.style == b.style &&  // (layer-styles@1, PC4 E6: its style)
           a.shapes == b.shapes && a.vector_mask == b.vector_mask &&  // (pen-shapes@1: its shape parts, a list made anew when changed; vector@1)
           a.group == b.group && a.parent_id == b.parent_id;  // (groups@1, PC5 I-b: drawn as a group, or in one)
}

// smart-objects@1 (PC3 A1): the contents the page's smart objects show — and those inside them — are the same in both
// books (contents are values: changed, they are other objects — edited in コンテンツを編集's tab, replaced, updated).
bool same_contents(const core::Document& a, const core::Document& b, const core::Page& page) {
    if (a.smart_contents.empty() && b.smart_contents.empty()) return true;
    std::set<std::string> seen;
    std::vector<std::string> todo;
    for (const std::string& id : core::smart::shown_on(page)) todo.push_back(id);
    while (!todo.empty()) {
        const std::string id = std::move(todo.back());
        todo.pop_back();
        if (!seen.insert(id).second) continue;
        const auto x = a.smart_contents.find(id);
        const auto y = b.smart_contents.find(id);
        const core::SmartContents* was = x == a.smart_contents.end() ? nullptr : x->second.get();
        const core::SmartContents* now = y == b.smart_contents.end() ? nullptr : y->second.get();
        if (was != now) return false;
        if (now != nullptr && now->page) {
            for (const std::string& inner : core::smart::shown_on(*now->page)) todo.push_back(inner);
        }
    }
    return true;
}

// The page's lines of dialogue as they are drawn: the same words, places, balloons, tails and styles.
bool same_line(const core::StoryLine& a, const core::StoryLine& b) {
    const bool tails = a.tail.has_value() == b.tail.has_value() && (!a.tail || (same_num(a.tail->x, b.tail->x) && same_num(a.tail->y, b.tail->y)));
    return tails && a.id == b.id && a.text == b.text && a.speaker == b.speaker && a.frame_id == b.frame_id && a.ruby == b.ruby &&
           same_num(a.x_mm, b.x_mm) && same_num(a.y_mm, b.y_mm) && same_num(a.w_mm, b.w_mm) && same_num(a.h_mm, b.h_mm) &&
           a.balloon == b.balloon && a.wrap == b.wrap && a.ruby_runs == b.ruby_runs && a.ruby_spans == b.ruby_spans && same_points(a.path, b.path) &&
           a.emphasis_runs == b.emphasis_runs && a.style_runs == b.style_runs && a.style == b.style && a.tails == b.tails;
}

bool same_story(const core::Document& a, const core::Document& b, const core::Num& index) {
    const auto x = a.story_for_page(index);
    const auto y = b.story_for_page(index);
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i] != y[i] && !same_line(*x[i], *y[i])) return false;
    }
    return true;
}

// What the book itself gives each page's drawing: the font its lines are drawn in when they name none (set_meta
// font_path), the nombre's settings, the side the book starts on (where the nombre goes) and the gamma its letters are
// mixed in (set_text_gamma).
bool same_book_look(const core::Document& a, const core::Document& b) {
    return a.font_path == b.font_path && a.nombre == b.nombre && a.start_side == b.start_side && a.text_gamma == b.text_gamma &&
           a.global_light == b.global_light;  // (layer-styles@1, PC4 E6: 包括光源)
}

// How many covers come before the page (by number): the nombre counts the pages from the first that is not one
// (render/page.cpp nombre_text).
std::size_t covers_before(const core::Document& doc, const core::Page& page) {
    std::size_t n = 0;
    for (const auto& p : doc.pages) {
        if (p->index < page.index && core::is_cover(*p)) ++n;
    }
    return n;
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
    QImage shown;  // (null: as image)
    QImage mask;   // (null: no gamut warning)
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
    // (two kept: the whole page's tiles and the finer ones in sight ask at their own resolutions, turn about)
    struct Slot {
        std::shared_ptr<const core::Page> page;
        std::int64_t frame = 0;
        int dpi = 0;
        std::shared_future<Value> value;
        std::uint64_t used = 0;
    };
    std::mutex mutex;
    std::array<Slot, 2> kept;  // (not "slots": a Qt word)
    std::uint64_t clock = 0;

    Value get(const std::shared_ptr<const core::Page>& of, std::int64_t at, int resolution, const DocPtr& doc) {
        std::promise<Value> made;
        std::shared_future<Value> wait;
        bool mine = false;
        {
            std::lock_guard lock(mutex);
            Slot* slot = nullptr;
            for (Slot& s : kept) {
                if (s.value.valid() && s.page == of && s.frame == at && s.dpi == resolution) slot = &s;
            }
            if (slot == nullptr) {
                slot = &*std::min_element(kept.begin(), kept.end(), [](const Slot& a, const Slot& b) { return a.used < b.used; });
                slot->page = of;
                slot->frame = at;
                slot->dpi = resolution;
                slot->value = made.get_future().share();
                mine = true;
            }
            slot->used = ++clock;
            wait = slot->value;
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

// (PC5 H-a, review P2-3) A page drawn whole for any part of it (render::draws_whole_page: a correction that moves shapes):
// drawn whole once a level — the first tile that needs it draws it, the others wait for it — and every tile cut from it.
// Kept while the tiles are being drawn; let go when they are all drawn.
struct PageRenderer::WholeStore {
    using Value = std::shared_ptr<const render::RenderResult>;
    struct Slot {
        const core::Page* page = nullptr;
        const core::Document* doc = nullptr;
        int dpi = 0;
        std::uint64_t generation = 0;
        bool rough = false;
        std::shared_future<Value> value;
        std::uint64_t used = 0;
    };
    std::mutex mutex;
    std::array<Slot, 2> kept;
    std::uint64_t clock = 0;
    std::atomic<std::size_t> drawn{0};

    Value get(const core::Page* page, const core::Document* doc, int dpi, std::uint64_t generation, bool rough,
              const std::function<render::RenderResult()>& make) {
        std::promise<Value> made;
        std::shared_future<Value> wait;
        bool mine = false;
        {
            std::lock_guard lock(mutex);
            Slot* slot = nullptr;
            for (Slot& s : kept) {
                if (!s.value.valid() || s.page != page || s.doc != doc || s.dpi != dpi || s.generation != generation || s.rough != rough) continue;
                // (one cancelled — its first tile was stopped —: drawn again)
                const bool given_up = s.value.wait_for(std::chrono::seconds(0)) == std::future_status::ready && s.value.get() == nullptr;
                if (!given_up) slot = &s;
            }
            if (slot == nullptr) {
                slot = &*std::min_element(kept.begin(), kept.end(), [](const Slot& a, const Slot& b) { return a.used < b.used; });
                *slot = Slot{page, doc, dpi, generation, rough, made.get_future().share(), 0};
                mine = true;
            }
            slot->used = ++clock;
            wait = slot->value;
        }
        if (mine) {
            try {
                Value value = std::make_shared<const render::RenderResult>(make());
                ++drawn;
                made.set_value(std::move(value));
            } catch (const render::Cancelled&) {
                made.set_value(nullptr);
            } catch (...) {
                made.set_exception(std::current_exception());
            }
        }
        return wait.get();
    }
    void release() {
        std::lock_guard lock(mutex);
        for (Slot& s : kept) s = Slot{};
    }
};

namespace {

// A part of a page drawn whole, as render_page draws a part of it: its picture, inks, values and marks cut there.
render::RenderResult cut_part(const render::RenderResult& whole, const QRect& region) {
    const render::Box box{region.x(), region.y(), region.x() + region.width(), region.y() + region.height()};
    render::RenderResult out;
    out.image = whole.image.crop(box);
    out.omitted = whole.omitted;
    if (whole.inks) out.inks = render::crop_inks(*whole.inks, box);
    if (whole.precise && whole.precise->size() >= 16) {  // (GKCR: its rows of the part, in its own precision)
        const std::string& from = *whole.precise;
        const std::size_t sample = static_cast<unsigned char>(from[5]);
        std::uint32_t full = 0;
        for (int b = 0; b < 4; ++b) full |= static_cast<std::uint32_t>(static_cast<unsigned char>(from[8 + b])) << (8 * b);
        std::string part = from.substr(0, 8);
        for (const std::uint32_t v : {static_cast<std::uint32_t>(box.width()), static_cast<std::uint32_t>(box.height())})
            for (int b = 0; b < 4; ++b) part += static_cast<char>(v >> (8 * b) & 0xff);
        const std::size_t row = std::size_t(box.width()) * 4 * sample;
        for (int y = box.y0; y < box.y1; ++y) part.append(from, 16 + (std::size_t(y) * full + std::size_t(box.x0)) * 4 * sample, row);
        out.precise = std::move(part);
    }
    if (whole.drawn_over) {
        std::vector<unsigned char> part(std::size_t(box.width()) * std::size_t(box.height()));
        const auto full = static_cast<std::size_t>(whole.image.width());
        for (int y = box.y0; y < box.y1; ++y)
            std::copy_n(whole.drawn_over->data() + std::size_t(y) * full + std::size_t(box.x0), box.width(),
                        part.data() + std::size_t(y - box.y0) * std::size_t(box.width()));
        out.drawn_over = std::move(part);
    }
    return out;
}

}  // namespace

std::size_t PageRenderer::wholes_drawn() const { return wholes_ ? wholes_->drawn.load() : 0; }

void PageRenderer::set_fine_budget(std::size_t bytes) {
    fine_budget_ = bytes;
    trim_fine();
}

std::size_t PageRenderer::fine_bytes() const {
    std::size_t bytes = 0;
    for (const auto& [dpi, l] : levels_) {
        if (dpi == base_dpi_) continue;
        for (const Tile& tile : l.tiles) {
            bytes += static_cast<std::size_t>(tile.image.sizeInBytes()) + static_cast<std::size_t>(tile.shown.sizeInBytes()) +
                     static_cast<std::size_t>(tile.mask.sizeInBytes());
        }
    }
    return bytes;
}

void PageRenderer::trim_fine() {
    // (review P2-3: the finer levels keep the tiles in sight and those seen last, up to their budget; the whole page's
    // level keeps all of its)
    std::size_t bytes = fine_bytes();
    if (bytes <= fine_budget_) return;
    std::vector<std::tuple<std::uint64_t, int, int>> out_of_sight;  // (seen, level, index)
    for (auto& [dpi, l] : levels_) {
        if (dpi == base_dpi_) continue;
        for (int i = 0; i < static_cast<int>(l.tiles.size()); ++i) {
            const Tile& tile = l.tiles[static_cast<std::size_t>(i)];
            if (tile.image.isNull() || tile.running || tile.seen >= want_clock_) continue;
            out_of_sight.emplace_back(tile.seen, dpi, i);
        }
    }
    std::sort(out_of_sight.begin(), out_of_sight.end());
    for (const auto& [seen, dpi, i] : out_of_sight) {
        if (bytes <= fine_budget_) break;
        if (texture_budget_) forget_on_card(dpi, i);
        Level& l = levels_.at(dpi);
        Tile& tile = l.tiles[static_cast<std::size_t>(i)];
        bytes -= std::min(bytes, static_cast<std::size_t>(tile.image.sizeInBytes()) + static_cast<std::size_t>(tile.shown.sizeInBytes()) +
                                     static_cast<std::size_t>(tile.mask.sizeInBytes()));
        tile.image = QImage();
        tile.shown = QImage();
        tile.mask = QImage();
        tile.out = 0;
        tile.valid = 0;
        tile.rough = false;
        tile.changed = generation_;
        tile.pending = QRect(QPoint(0, 0), tile_rect(l, i).size());  // (drawn again when it is wanted)
        ++fine_released_;
    }
}

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

void PageRenderer::set_hdr(const render::HdrPreview& preview) {
    const render::HdrPreview next = preview.clamped();
    if (next == hdr_) return;
    hdr_ = next;
    const core::Page* p = page();
    if (p != nullptr && render::hdr_page(*p)) redraw_all();  // (another page shows as it is: nothing on screen changes)
}

int PageRenderer::pixel_dpi() const {
    const core::Page* p = page();
    if (p == nullptr) return 0;
    const double dpi = p->spec.dpi.value();
    return std::isfinite(dpi) && dpi >= 1 ? static_cast<int>(std::lround(dpi)) : 0;
}

void PageRenderer::set_pixel_art(bool on) {
    if (on == pixel_art_) return;
    pixel_art_ = on;
    emit updated(QRectF());  // (the same tiles, laid otherwise)
}

void PageRenderer::set_texture_budget(std::optional<std::size_t> bytes) {
    texture_budget_ = bytes;
    on_card_.clear();
    texture_bytes_ = 0;
    textures_released_ = 0;
}

void PageRenderer::forget_on_card(int dpi, int index) {
    const auto it = on_card_.find({dpi, index});
    if (it == on_card_.end()) return;
    texture_bytes_ -= std::min(texture_bytes_, it->second.bytes);
    on_card_.erase(it);
    // (its picture copied: the card lets the texture of the one it had go — Qt's texture cache drops a picture's texture
    // when its data goes; a picture painted over in place would leave the old one there)
    const auto level = levels_.find(dpi);
    if (level == levels_.end() || index < 0 || index >= static_cast<int>(level->second.tiles.size())) return;
    Tile& tile = level->second.tiles[static_cast<std::size_t>(index)];
    if (!tile.image.isNull()) tile.image = tile.image.copy();
    if (!tile.shown.isNull()) tile.shown = tile.shown.copy();
}

void PageRenderer::trim_textures() {
    if (!texture_budget_) return;
    // (the ones whose tile is gone — another page, a level let go —: their pictures went, and their textures with them)
    for (auto it = on_card_.begin(); it != on_card_.end();) {
        const auto level = levels_.find(it->first.first);
        const bool gone = level == levels_.end() || it->first.second >= static_cast<int>(level->second.tiles.size()) ||
                          level->second.tiles[static_cast<std::size_t>(it->first.second)].image.isNull();
        if (gone) {
            texture_bytes_ -= std::min(texture_bytes_, it->second.bytes);
            it = on_card_.erase(it);
        } else {
            ++it;
        }
    }
    // past the budget: the ones drawn longest ago first, never one of the frame just drawn
    while (texture_bytes_ > *texture_budget_) {
        auto oldest = on_card_.end();
        for (auto it = on_card_.begin(); it != on_card_.end(); ++it) {
            if (it->second.frame < card_frame_ && (oldest == on_card_.end() || it->second.frame < oldest->second.frame)) oldest = it;
        }
        if (oldest == on_card_.end()) break;
        const auto [dpi, index] = oldest->first;
        forget_on_card(dpi, index);
        ++textures_released_;
    }
    ++card_frame_;
}

void PageRenderer::redraw_all() {
    ++generation_;
    for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
    dispatch();
}

void PageRenderer::set_proof_setup(const render::proof::Setup& setup) {
    if (setup == setup_) return;
    const bool shown = proof_on_ || gamut_;
    setup_ = setup;
    if (shown) redraw_all();
}

void PageRenderer::set_proof(bool on) {
    if (on == proof_on_) return;
    proof_on_ = on;
    redraw_all();
}

void PageRenderer::set_gamut(std::optional<GamutLook> look) {
    if (look == gamut_) return;
    gamut_ = std::move(look);
    redraw_all();
    emit gamutCounted();
}

void PageRenderer::set_monitor(std::shared_ptr<const display_color::Monitor> monitor) {
    if (monitor == monitor_) return;
    monitor_ = std::move(monitor);
    redraw_all();
}

std::pair<std::size_t, std::size_t> PageRenderer::gamut_count() const {
    if (!gamut_) return {0, 0};
    const auto it = levels_.find(base_dpi_);
    if (it == levels_.end()) return {0, 0};
    std::size_t out = 0, all = 0;
    for (const Tile& tile : it->second.tiles) {
        if (tile.mask.isNull() || tile.valid == 0) continue;
        out += tile.out;
        all += static_cast<std::size_t>(tile.mask.width()) * static_cast<std::size_t>(tile.mask.height());
    }
    return {out, all};
}

void PageRenderer::set_channel_view(ChannelView view) {
    if (view == channel_view_) return;
    channel_view_ = std::move(view);
    ++generation_;
    for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
    dispatch();
}

void PageRenderer::set_screen_dots(bool on) {
    if (on == screen_dots_) return;
    screen_dots_ = on;
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
    // (the page drawn faintly over it, edited, gone or come, or its lines changed: all of it changed)
    const core::Page* onion = onion_page(*doc, page, mode_);
    const bool same_onion = !same_page || (onion_page(*doc_, *shown_page_, mode_) == onion &&
                                           (onion == nullptr || same_story(*doc_, *doc, onion->index)));
    const bool same_book = !same_page || same_book_look(*doc_, *doc);
    // (the same page on other paper — 原稿用紙の設定, or its undo — is drawn anew at its new size, as another page is)
    const bool resized = same_page && !(same_num(shown_page_->spec.width_mm, page.spec.width_mm) &&
                                        same_num(shown_page_->spec.height_mm, page.spec.height_mm));
    if (!same_page || resized) {
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
    } else if (shown_page_ != &page || doc_->brush_custom != doc->brush_custom || doc_->story.size() != doc->story.size() || !same_onion ||
               !same_book || !same_story(*doc_, *doc, page.index) ||
               covers_before(*doc_, *shown_page_) != covers_before(*doc, page) || !same_contents(*doc_, *doc, page)) {
        // the same page, changed: only the tiles its new or removed lines cover are drawn again (anything else
        // changed: all of them — its number too, and the covers before it, which the nombre and the binding edge
        // follow)
        const core::Page& old = *shown_page_;
        bool whole = !(old.index == page.index && covers_before(*doc_, old) == covers_before(*doc, page) &&
                       same_spec(old.spec, page.spec) && same_frames(old.frames, page.frames) && old.binding == page.binding &&
                       old.color == page.color &&  // (color-mode@1: another mode, profile or table of inks: all of it)
                       same_channels(old.channels, page.channels) &&  // (channels@1)
                       old.fills == page.fills && old.extra == page.extra && old.effects == page.effects && old.ruler == page.ruler &&
                       old.rulers == page.rulers && old.prims == page.prims && old.numero == page.numero &&
                       old.onion_from == page.onion_from && same_onion && same_book && old.layers.size() == page.layers.size() &&
                       doc_->brush_custom == doc->brush_custom && doc_->story.size() == doc->story.size() &&
                       same_story(*doc_, *doc, page.index) &&  // (a line edited, moved or restyled: the whole page, as Python draws it)
                       same_contents(*doc_, *doc, page));      // (smart-objects@1: a smart object's contents changed)
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
                // (border effects and a layer style reach beyond a line's own box — a style's gradient follows the layer's box)
                if ((b.effect && core::py_truthy(*b.effect)) || b.style) {
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
            // (a nombre takes a white halo by what lies under its letters: a line drawn or taken away under any of it
            // draws all of it again, in every tile it spans)
            std::vector<render::Box> nombres;
            try {
                if (!touched.empty()) nombres = render::nombre_areas(page, dpi, doc.get());
            } catch (const std::exception&) {
                mark(level, QRect(QPoint(0, 0), level.size), generation_);  // (where it lies is not known: all of it)
                continue;
            }
            const render::Size size{level.size.width(), level.size.height()};
            for (const core::StrokePtr& stroke : touched) {
                const auto box = render::brushes::extent(size, core::stroke_points(*stroke), dpi,
                                                         stroke->width_mm != 0.0 ? stroke->width_mm : 0.35, stroke->kind);
                if (!box) continue;
                const QRect drawn(box->x0 - 2, box->y0 - 2, box->width() + 4, box->height() + 4);
                mark(level, drawn, generation_);
                for (const render::Box& nombre : nombres) {
                    const QRect under(nombre.x0, nombre.y0, nombre.width(), nombre.height());
                    if (drawn.intersects(under)) mark(level, under, generation_);
                }
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
    ++want_clock_;  // (PC5 H-a: the tiles wanted now, seen now)
    for (auto& [dpi, l] : levels_) {
        for (const int i : l.wanted) l.tiles[static_cast<std::size_t>(i)].seen = want_clock_;
    }
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
    trim_fine();
    dispatch();
}

void PageRenderer::dispatch() {
    if (!shown_page_ || !doc_) return;
    if ((proof_on_ || gamut_) && !setup_.icc && proof_profile_) {
        // (the export profile as it is now: another one, or its file gone, draws everything again)
        std::optional<std::filesystem::path> now = proof_profile_();
        if (now != setup_.export_icc) {
            setup_.export_icc = std::move(now);
            ++generation_;
            for (auto& [dpi, level] : levels_) mark(level, QRect(QPoint(0, 0), level.size), generation_);
        }
    }
    QThreadPool& runner = shared_pool_ != nullptr ? *shared_pool_ : pool_;  // (PC5 H-c: a pane's, the panes' pool)
    const int most = shared_pool_ != nullptr ? shared_count_ : pool_.maxThreadCount();
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
            const std::optional<render::proof::Setup> proof = proof_on_ ? std::optional<render::proof::Setup>(setup_) : std::nullopt;
            const std::optional<std::pair<GamutLook, render::proof::Setup>> gamut =
                gamut_ ? std::optional<std::pair<GamutLook, render::proof::Setup>>(std::pair{*gamut_, setup_}) : std::nullopt;
            // (モニターの RGB: the page's numbers go to the monitor as they are)
            const std::shared_ptr<const display_color::Monitor> monitor =
                proof && proof->kind == render::proof::Kind::Monitor ? nullptr : monitor_;
            const bool dots = screen_dots_;
            const ChannelView view = channel_view_;
            const std::stop_token stop = tile.stop->get_token();
            const std::int64_t frame = frame_;
            const bool onion = onion_;
            const render::HdrPreview hdr = hdr_;
            if (!onions_) onions_ = std::make_shared<OnionStore>();
            const std::shared_ptr<OnionStore> onions = onions_;
            if (!wholes_) wholes_ = std::make_shared<WholeStore>();
            const std::shared_ptr<WholeStore> wholes = wholes_;
            runner.start([self, doc, page_index, page_id, generation, dpi, index, region, rough, mode, proof, gamut, monitor, dots, view, stop, frame,
                         onion, onions, hdr, wholes]() {
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
                    options.screen_dots = dots;
                    options.skip_unported = true;  // (a preview on screen: what is not drawn yet is reported)
                    options.region = render::RenderRegion{region.x(), region.y(), region.width(), region.height()};
                    options.stop = stop;
                    options.planes = view.planes;  // (channels@1: the channel panel's view)
                    options.overlays = view.overlays;
                    options.spots = view.spots;
                    const std::shared_ptr<const core::Page> raw = doc->pages.at(page_index);  // (a page gone meanwhile: caught below)
                    // (a CMYK page's plates are its own inks: drawn with them)
                    const bool cmyk_page = raw->color && raw->color->space == core::InkSpace::Cmyk;
                    const bool plates = proof && (proof->kind == render::proof::Kind::Plate || proof->kind == render::proof::Kind::CmyPlate);
                    // (the page's own colours — its profile, and its values where render_page gives them: an RGB page of a
                    // profile's, a grey or CMYK page's inks — checked by the gamut warning and shown by the kinds that show
                    // the page's numbers, as upstream reads its composite in its own profile)
                    render::proof::Source source;
                    if (raw->rgb && raw->rgb->icc) source.icc = raw->rgb->icc;
                    if (raw->color && raw->color->space == core::InkSpace::Gray) source = render::proof::Source{raw->color->icc, true, false, nullptr};
                    if (cmyk_page) source = render::proof::Source{raw->color->icc, false, true, nullptr};
                    const bool numbers = proof && (proof->kind == render::proof::Kind::Srgb || proof->kind == render::proof::Kind::Monitor ||
                                                   proof->kind == render::proof::Kind::LegacyMac);
                    const bool values = source.icc && (gamut || numbers) && !core::anim::is_animation(*raw);
                    options.inks = (cmyk_page && plates) || (values && raw->color);
                    options.precise = values && !raw->color;
                    // (PC5 H-a: a 32-bit page shown through 32 bit プレビューオプション — its values or inks, before they are cut)
                    const bool hdr_on = !hdr.identity() && render::hdr_page(*raw) && !core::anim::is_animation(*raw);
                    if (hdr_on) {
                        (raw->color ? options.inks : options.precise) = true;
                        options.drawn_over = true;  // (what is drawn over the page stays as drawn: review P3-4)
                    }
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
                    } else if (render::draws_whole_page(*raw)) {  // (drawn whole once a level, each tile cut from it: P2-3)
                        render::RenderOptions whole = options;
                        whole.region.reset();
                        const auto page = wholes->get(raw.get(), doc.get(), dpi, generation, rough,
                                                      [&] { return render::render_page(*raw, dpi, whole, doc.get()); });
                        if (!page) throw render::Cancelled();
                        drawn = cut_part(*page, region);
                    } else {
                        drawn = render::render_page(*raw, dpi, options, doc.get());
                    }
                    if (values) {
                        if (drawn.precise && !raw->color) source.values = render::proof::values_of(*drawn.precise);
                        else if (drawn.inks && raw->color) source.values = render::proof::values_of(*drawn.inks);
                    }
                    // (per pixel: a tile as the page. The gamut checked on the page's colours, before the proof: its values,
                    // or the colours drawn)
                    std::vector<unsigned char> out_of_gamut;
                    std::size_t outside = 0;
                    if (gamut) {
                        const auto check = render::proof::Gamut::of(gamut->second, source);
                        if (check->of_source() && source.values) {
                            out_of_gamut = check->mask(*source.values, drawn.image, gamut->first.threshold, &outside);
                        } else {
                            out_of_gamut = render::proof::Gamut::of(gamut->second)->mask(drawn.image, gamut->first.threshold, &outside);
                        }
                    }
                    if (hdr_on) {
                        if (raw->color && drawn.inks) drawn.image = render::hdr_shown(drawn.image, *drawn.inks, *raw->color, hdr, drawn.drawn_over ? &*drawn.drawn_over : nullptr);
                        else if (!raw->color && drawn.precise) drawn.image = render::hdr_shown(drawn.image, *drawn.precise, render::RgbSpace::of(*raw), hdr,
                                                                                  drawn.drawn_over ? &*drawn.drawn_over : nullptr);
                    }
                    if (proof) {
                        if (cmyk_page && plates && drawn.inks) {
                            drawn.image = render::proof::page_plates(*drawn.inks, raw->color->icc, *proof);
                        } else if (!(cmyk_page && proof->kind == render::proof::Kind::Profile)) {  // (a CMYK page shows its inks already)
                            drawn.image = render::proof::apply(drawn.image, *proof, source);
                        }
                    }
                    result.image = to_qimage(drawn.image);
                    if (gamut) {
                        result.mask = QImage(result.image.size(), QImage::Format_Grayscale8);
                        for (int y = 0; y < result.mask.height(); ++y)
                            std::copy_n(out_of_gamut.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(result.mask.width()),
                                        result.mask.width(), result.mask.scanLine(y));
                    }
                    if ((gamut && outside > 0) || monitor) {
                        result.shown = result.image.copy();
                        if (gamut && outside > 0) {  // (the mark over the colours out of gamut, at its opacity)
                            const QColor c = gamut->first.color;
                            const double a = std::clamp(gamut->first.opacity, 0.0, 1.0);
                            const auto mix = [a](int under, int over) { return static_cast<int>(std::lround(under + (over - under) * a)); };
                            for (int y = 0; y < result.shown.height(); ++y) {
                                auto* row = reinterpret_cast<QRgb*>(result.shown.scanLine(y));
                                const uchar* m = result.mask.constScanLine(y);
                                for (int x = 0; x < result.shown.width(); ++x) {
                                    if (m[x] == 0) continue;
                                    const QRgb p = row[x];
                                    row[x] = qRgb(mix(qRed(p), c.red()), mix(qGreen(p), c.green()), mix(qBlue(p), c.blue()));
                                }
                            }
                        }
                        if (monitor) monitor->apply(result.shown);
                    }
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
    if (texture_budget_) forget_on_card(r.dpi, r.index);  // (painted over: its texture on the card is out of date)
    const QPoint at = r.region.topLeft() - rect.topLeft();
    {
        QPainter painter(&tile.image);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(at, r.image);
    }
    // (as painted: the mark and the monitor's colours, where there are any; the gamut warning's mask and count)
    if (!r.shown.isNull()) {
        if (tile.shown.isNull()) tile.shown = tile.image.copy();
        QPainter painter(&tile.shown);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(at, r.shown);
    } else if (!tile.shown.isNull()) {
        QPainter painter(&tile.shown);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(at, r.image);
    }
    if (!r.mask.isNull()) {
        if (tile.mask.isNull() || tile.mask.size() != rect.size()) {
            tile.mask = QImage(rect.size(), QImage::Format_Grayscale8);
            tile.mask.fill(0);
        }
        for (int y = 0; y < r.mask.height(); ++y) std::copy_n(r.mask.constScanLine(y), r.mask.width(), tile.mask.scanLine(at.y() + y) + at.x());
        tile.out = 0;
        for (int y = 0; y < tile.mask.height(); ++y) {
            const uchar* m = tile.mask.constScanLine(y);
            tile.out += static_cast<std::size_t>(std::count(m, m + tile.mask.width(), uchar(255)));
        }
    } else {
        tile.mask = QImage();
        tile.out = 0;
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
    if (gamut_ && l.dpi == base_dpi_) emit gamutCounted();
    trim_fine();
    if (settled() && wholes_) wholes_->release();  // (every tile drawn: the whole pages they were cut from let go)
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
    const bool smooth = painter.testRenderHint(QPainter::SmoothPixmapTransform);
    const int pixel = pixel_dpi();
    for (const auto& [dpi, l] : levels_) {
        const bool finer = dpi > base_dpi_ && base_dpi_ > 0;
        const double mm = 25.4 / dpi;
        // page pixels → the screen; exactly one to one at 100 % (no resampling of the page's pixels)
        QTransform t = QTransform::fromScale(mm, mm) * to_screen;
        // (PC5 H-a: mirrored too — a mirrored view at 100 % lays the pixels on the screen's as they are, unresampled)
        if (std::abs(std::abs(t.m11()) - 1.0) < 1e-9 && std::abs(std::abs(t.m22()) - 1.0) < 1e-9 && std::abs(t.m12()) < 1e-12 && std::abs(t.m21()) < 1e-12) {
            t = QTransform(t.m11() < 0 ? -1 : 1, 0, 0, t.m22() < 0 ? -1 : 1, std::round(t.dx()), std::round(t.dy()));
        }
        painter.setTransform(t);
        // (PC5 H-a: the page's own pixels enlarged — 画素の段 — are laid as squares, nearest; ピクセルアートのプレビュー: every
        // level so)
        const bool enlarged = std::abs(t.determinant()) > 1.0 + 1e-9;
        painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth && !pixel_art_ && !(enlarged && pixel > 0 && dpi >= pixel));
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
            const QImage& picture = tile.shown.isNull() ? tile.image : tile.shown;
            painter.drawImage(rect.topLeft(), picture);
            if (texture_budget_) {  // (on the card now: counted, with the frame it was drawn in)
                auto [it, made] = on_card_.try_emplace({dpi, i});
                if (made) {
                    it->second.bytes = static_cast<std::size_t>(picture.sizeInBytes());
                    texture_bytes_ += it->second.bytes;
                }
                it->second.frame = card_frame_;
            }
        }
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    painter.setTransform(to_screen);
}

std::optional<QColor> PageRenderer::pixel_at(double x_mm, double y_mm) const {
    const core::Page* p = page();
    if (p == nullptr || base_dpi_ == 0) return std::nullopt;
    // (PC5 H-a review P3-3: what the view only shows — the proof, 32 bit プレビュー — is not the page's colour: the page as
    // it is drawn there, as the status bar's values read it)
    if (proof_on_ || (!hdr_.identity() && render::hdr_page(*p))) {
        try {
            const core::Json at = render::pixel_at(*p, doc_.get(), x_mm, y_mm, std::nullopt, base_dpi_);
            const core::Json& srgb = at.at("srgb");
            const auto c = [&srgb](int i) { return std::clamp(static_cast<int>(std::lround(srgb.at(i).get<double>())), 0, 255); };
            return QColor(c(0), c(1), c(2));
        } catch (const std::exception&) {  // (off the page)
            return std::nullopt;
        }
    }
    const auto it = levels_.find(base_dpi_);
    if (it == levels_.end()) return std::nullopt;
    const Level& l = it->second;
    // (as Python's canvas reads its picture of the page: int(x / width * pixels))
    const int px = static_cast<int>(x_mm / p->spec.width_mm.value() * l.size.width());
    const int py = static_cast<int>(y_mm / p->spec.height_mm.value() * l.size.height());
    if (!(0 <= px && px < l.size.width() && 0 <= py && py < l.size.height())) return std::nullopt;
    const Tile& tile = l.tiles[static_cast<std::size_t>((py / kTile) * l.cols + px / kTile)];
    if (tile.image.isNull() || tile.valid == 0) return std::nullopt;
    return tile.image.pixelColor(px % kTile, py % kTile);
}

QImage PageRenderer::compose_shown(int dpi) const {
    const auto it = levels_.find(dpi);
    if (it == levels_.end()) return {};
    const Level& l = it->second;
    QImage out(l.size, QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    for (int i = 0; i < static_cast<int>(l.tiles.size()); ++i) {
        const Tile& tile = l.tiles[static_cast<std::size_t>(i)];
        if (!tile.image.isNull()) painter.drawImage(tile_rect(l, i).topLeft(), tile.shown.isNull() ? tile.image : tile.shown);
    }
    painter.end();
    return out;
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
