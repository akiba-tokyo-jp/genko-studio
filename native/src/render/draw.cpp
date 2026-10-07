// ImageDraw (Pillow 12.3.0 PIL/ImageDraw.py; the draw_* methods of _imaging.c) over libImaging's Draw.c.

#include "render/draw.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <QFile>
#include <QFileInfo>
#include <QResource>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_MODULE_H
#include FT_SYSTEM_H

#include "render/page.hpp"

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/imaging.hpp"

static void init_text_resources(){
    static const bool ready=[](){Q_INIT_RESOURCE(genko_text);return true;}();
    (void)ready;
}

namespace genko::render {

namespace {

constexpr double kRadToDeg = 180.0 / core::kPi;  // mathmodule.c radToDeg
constexpr double kDegToRad = core::kPi / 180.0;  // mathmodule.c degToRad

// _imaging.c's (int)xy[i]: truncation toward zero; what x86 gives for values an int cannot hold.
int c_int(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<int>::min();
    return static_cast<int>(v);
}

// The page-sized image Draw.c draws on for a band of rows: rows [y0, y0 + band->ysize) are the band's, the others
// share one scratch row (written to and never read back).
Imaging make_view(Imaging band, int y0, int page_height, std::vector<unsigned char>& scratch) {
    Imaging view = detail::check(ImagingNewPrologue(band->mode, band->xsize, page_height));
    scratch.assign(static_cast<std::size_t>(band->linesize > 0 ? band->linesize : 1), 0);
    for (int y = 0; y < page_height; ++y) {
        const int row = y - y0;
        view->image[y] = (row >= 0 && row < band->ysize) ? band->image[row] : reinterpret_cast<char*>(scratch.data());
    }
    return view;
}

}  // namespace

struct Draw::Target {
    Imaging im = nullptr;
    Imaging owned_view = nullptr;
    std::vector<unsigned char> scratch;
    // a view: where its own rows are (for masks of the same shape)
    bool is_view = false;
    int window_y0 = 0;
    int window_rows = 0;
    int blend = 0;
    INT32 default_ink = 0;

    ~Target() {
        if (owned_view != nullptr) ImagingDelete(owned_view);
    }

    // _getink(ink): the ink to draw with, or the default one when there is none
    INT32 ink_of(const std::optional<Ink>& colour) const {
        return colour ? detail::ink_for(*colour, im) : default_ink;
    }
};

Draw::Draw(Image& image, std::string_view mode) : target_(std::make_unique<Target>()) {
    if (image.empty()) throw core::Error("value", "no image");
    Target& t = *target_;
    t.im = image.raw();
    const std::string_view own = image.mode();
    if (mode.empty()) mode = own;
    if (mode != own) {
        if (mode == "RGBA" && own == "RGB") {
            t.blend = 1;
        } else {
            throw core::Error("value", "mode mismatch");
        }
    }
    t.default_ink = detail::ink_for(Ink(mode == "I" || mode == "F" ? 1 : -1), t.im);
}

Draw::Draw(std::unique_ptr<Target> target) : target_(std::move(target)) {}

Draw::~Draw() = default;

namespace {
bool bundled_text_font(std::string_view name) {
    return name=="gothic" || name=="mincho" || name=="maru" || name=="hand" || name=="sfx" || name=="sfx_pop";
}
bool kana_like(char32_t c) {
    return (c>=0x3040 && c<=0x30ff) || (c>=0x31f0 && c<=0x31ff) || (c>=0x3000 && c<=0x303f) ||
           (c>=0xff61 && c<=0xff9f) || c==U'〜' || c==U'～' || c==U'…' || c==U'‥';
}
QString text_string(std::string_view text) {
    if(text.size()>65536)throw core::Error("value","text length exceeded");
    const QString value=QString::fromUtf8(text.data(),static_cast<qsizetype>(text.size()));
    if(value.toUtf8().toStdString()!=text)throw core::Error("value","invalid UTF-8 text");
    return value;
}
// FreeType calls C allocators for outlines, embedded bitmaps, raster scratch and stroked bitmaps.
// Attach each allocation to the existing shared image budget before malloc; C callbacks never throw.
struct alignas(std::max_align_t) TextAllocation {
    ImagingMemoryInstance owner{};
    std::size_t size = 0;
};
using TextMemory = std::pair<std::uint64_t,bool>; // bytes, sticky allocation failure
constexpr std::uint64_t kTextWorkLimit = 128ULL*1024*1024;
void* text_allocate(FT_Memory memory,long size) noexcept {
    auto& state=*static_cast<TextMemory*>(memory->user);
    if(state.second || size<=0)return nullptr;
    const auto count=static_cast<std::uint64_t>(size)+sizeof(TextAllocation);
    if(count>kTextWorkLimit-state.first) {
        state.second=true;(void)ImagingError_MemoryError();return nullptr;
    }
    ImagingMemoryInstance reservation{};
    if(!genko_imaging_budget_reserve(&reservation,count)) {state.second=true;return nullptr;}
    void* storage=std::malloc(static_cast<std::size_t>(count));
    if(!storage) {
        genko_imaging_budget_release(&reservation);state.second=true;(void)ImagingError_MemoryError();return nullptr;
    }
    auto* header=new(storage) TextAllocation{};
    header->owner=reservation;header->size=static_cast<std::size_t>(size);
    state.first+=count;
    return header+1;
}
void text_free(FT_Memory memory,void* block) noexcept {
    if(!block)return;
    auto* header=static_cast<TextAllocation*>(block)-1;
    auto& state=*static_cast<TextMemory*>(memory->user);
    state.first-=header->size+sizeof(TextAllocation);
    genko_imaging_budget_release(&header->owner);
    header->~TextAllocation();std::free(header);
}
void* text_reallocate(FT_Memory memory,long current,long size,void* block) noexcept {
    (void)current;
    if(size<=0) {text_free(memory,block);return nullptr;}
    void* next=text_allocate(memory,size);
    if(!next)return nullptr; // A failed realloc keeps the caller's old allocation alive.
    if(block) {
        const auto* header=static_cast<TextAllocation*>(block)-1;
        std::memcpy(next,block,std::min(header->size,static_cast<std::size_t>(size)));
        text_free(memory,block);
    }
    return next;
}
void check_text(FT_Error error) {
    if(!error)return;
    if(genko_imaging_error_kind()!=0)detail::throw_imaging_error();
    if(error==FT_Err_Out_Of_Memory)throw core::Error("memory","FreeType text allocation failed");
    throw core::Error("value","FreeType text operation failed");
}
template<class Fn> auto with_text_font(std::string_view font,int size,Fn&& fn) {
    init_text_resources();
    const auto name=QString::fromUtf8(font.data(),static_cast<qsizetype>(font.size()));
    QFile source(bundled_text_font(font)?QStringLiteral(":/genko/text/")+name:name);
    if(!source.open(QIODevice::ReadOnly|QIODevice::Unbuffered))throw core::Error("file","text font unavailable");
    constexpr qint64 cap=32*1024*1024;
    if(source.size()<=0 || source.size()>cap)throw core::Error("value","text font exceeds byte limit");
    ImageAllocationBudget budget(512ULL*1024*1024); // Shares, never enlarges, the enclosing image budget.
    genko_imaging_clear_error();
    ImagingMemoryInstance source_owner{};
    const auto release=[](ImagingMemoryInstance* p){genko_imaging_budget_release(p);};
    std::unique_ptr<ImagingMemoryInstance,decltype(release)> source_hold(&source_owner,release);
    const qint64 expected=source.size();
    if(expected<=0 || expected>cap)throw core::Error("value","text font exceeds byte limit");
    if(!genko_imaging_budget_reserve(&source_owner,static_cast<std::uint64_t>(expected)+4096))detail::throw_imaging_error();
    QByteArray bytes(static_cast<qsizetype>(expected),Qt::Uninitialized);
    if(source.read(bytes.data(),expected)!=expected || !source.atEnd())throw core::Error("value","text font changed during read");
    TextMemory state{0,false};
    FT_MemoryRec_ memory{};memory.user=&state;memory.alloc=text_allocate;memory.free=text_free;memory.realloc=text_reallocate;
    const auto check=check_text;
    FT_Library raw_library=nullptr;check(FT_New_Library(&memory,&raw_library));
    const auto done_library=[](FT_Library p){(void)FT_Done_Library(p);};
    std::unique_ptr<std::remove_pointer_t<FT_Library>,decltype(done_library)> library(raw_library,done_library);
    FT_Add_Default_Modules(library.get());
    if(state.second)detail::throw_imaging_error();
    FT_Set_Default_Properties(library.get());
    if(state.second)detail::throw_imaging_error();
    FT_Face raw_face=nullptr;check(FT_New_Memory_Face(library.get(),reinterpret_cast<const FT_Byte*>(bytes.constData()),static_cast<FT_Long>(bytes.size()),0,&raw_face));
    const auto done_face=[](FT_Face p){(void)FT_Done_Face(p);};
    std::unique_ptr<std::remove_pointer_t<FT_Face>,decltype(done_face)> face(raw_face,done_face);
    size=std::max(6,size);check(FT_Set_Pixel_Sizes(face.get(),static_cast<FT_UInt>(size),static_cast<FT_UInt>(size)));
    auto result=fn(face.get(),library.get());
    if(state.second)detail::throw_imaging_error();
    return result;
}
std::vector<std::pair<FT_UInt,std::int64_t>> basic_text_glyphs(FT_Face face,std::string_view text,std::stop_token stop) {
    const auto chars=text_string(text).toUcs4();std::vector<std::pair<FT_UInt,std::int64_t>> out;out.reserve(static_cast<std::size_t>(chars.size()));FT_UInt last=0;
    for(const auto ch:chars){
        if(stop.stop_requested())throw Cancelled();
        const FT_UInt index=FT_Get_Char_Index(face,static_cast<FT_ULong>(ch));
        check_text(FT_Load_Glyph(face,index,FT_LOAD_DEFAULT));
        if(FT_HAS_KERNING(face)&&last&&index){FT_Vector delta{};if(!FT_Get_Kerning(face,last,index,FT_KERNING_DEFAULT,&delta))out.back().second+=static_cast<std::int64_t>(std::floor(static_cast<double>(delta.x+32)/64));}
        out.emplace_back(index,face->glyph->metrics.horiAdvance);last=index;
    }
    return out;
}
} // namespace

std::string text_font(std::string_view spec,std::string_view text,std::stop_token stop) {
    if(stop.stop_requested())throw Cancelled();
    const auto chars=text_string(text).toUcs4();
    if(spec.size()>4096)throw core::Error("value","font name exceeded");
    const auto name=QString::fromUtf8(spec.data(),static_cast<qsizetype>(spec.size()));
    if(name.toUtf8().toStdString()!=spec)throw core::Error("value","invalid UTF-8 font name");
    const bool custom=!bundled_text_font(spec) && spec!="antique" && QFileInfo(name).isFile();
    const bool composite=!custom && !bundled_text_font(spec);
    const std::string first=custom||!composite?std::string(spec):(!chars.empty()&&kana_like(chars[0])?"mincho":"gothic");
    if(chars.size()!=1)return first;
    const auto has=[&](const std::string& font){return with_text_font(font,32,[&](FT_Face face,FT_Library){return FT_Get_Char_Index(face,chars[0])!=0 || QChar::isSpace(chars[0]);});};
    if(has(first))return first;
    const std::string other=custom?first:composite?(first=="mincho"?"gothic":"mincho"):first;
    for(const std::string& font:{other,std::string("gothic"),std::string("sfx")})if(has(font))return font;
    return first;
}
double text_length(std::string_view text,std::string_view font,int size,std::stop_token stop) {
    if(size<1 || size>8192)throw core::Error("value","invalid text geometry");
    if(stop.stop_requested())throw Cancelled();
    const auto selected=text_font(font,text,stop);
    return with_text_font(selected,size,[&](FT_Face face,FT_Library){std::int64_t advance=0;for(const auto& glyph:basic_text_glyphs(face,text,stop)){advance+=glyph.second;if(std::abs(advance)>64LL*1048576)throw core::Error("value","text advance exceeded");}return static_cast<double>(advance)/64;});
}

std::pair<Image, std::array<int, 4>> text_mask(std::string_view text, std::string_view font, int size,
                                              PointD fraction, std::stop_token stop, int stroke) {
    if (size < 1 || size > 8192 || text.size() > 65536 || stroke < 0 || stroke > 512 ||
        !std::isfinite(fraction.x) || !std::isfinite(fraction.y) || fraction.x < 0 || fraction.x >= 1 ||
        fraction.y < 0 || fraction.y >= 1) throw core::Error("value", "invalid text geometry");
    if (stop.stop_requested()) throw Cancelled();
    const auto selected=text_font(font,text,stop);
    return with_text_font(selected,size,[&](FT_Face face,FT_Library library) -> std::pair<Image,std::array<int,4>> {
    const auto check=check_text;
    const int flags = stroke ? FT_LOAD_NO_BITMAP : FT_LOAD_DEFAULT;
    const auto pixel = [](std::int64_t p) { return static_cast<int>(std::floor(static_cast<double>(p + 32) / 64)); };
    const auto glyphs=basic_text_glyphs(face,text,stop);
    const std::size_t count = glyphs.size();
    // Pillow _imagingft: pixel-rounded pen bounds include the baseline; LA anchor uses ascender.
    int xmin = 0, xmax = 0, ymin = 0, ymax = 0, bitmap_xmin = 0, bitmap_ymax = 0;
    std::int64_t pen = 0;
    const auto glyph_delete = [](FT_Glyph p) { FT_Done_Glyph(p); };
    for (std::size_t i = 0; i < count; ++i) {
        if (stop.stop_requested()) throw Cancelled();
        if (std::abs(pen) > 64LL * 1048576) throw core::Error("value", "text advance exceeded");
        const int px = pixel(pen + 0), py = pixel(0);
        check(FT_Load_Glyph(face, glyphs[i].first, flags));
        FT_Glyph raw = nullptr; check(FT_Get_Glyph(face->glyph, &raw));
        std::unique_ptr<std::remove_pointer_t<FT_Glyph>, decltype(glyph_delete)> glyph(raw, glyph_delete);
        FT_BBox bounds{}; FT_Glyph_Get_CBox(glyph.get(), FT_GLYPH_BBOX_PIXELS, &bounds);
        xmin = std::min(xmin, px + static_cast<int>(bounds.xMin)); xmax = std::max(xmax, px + static_cast<int>(bounds.xMax));
        ymin = std::min(ymin, py + static_cast<int>(bounds.yMin)); ymax = std::max(ymax, py + static_cast<int>(bounds.yMax));
        check(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL));
        bitmap_xmin = std::min(bitmap_xmin, face->glyph->bitmap_left + px);
        bitmap_ymax = std::max(bitmap_ymax, face->glyph->bitmap_top + py);
        pen += glyphs[i].second; xmax = std::max(xmax, pixel(pen));
    }
    if (std::abs(pen) > 64LL * 1048576) throw core::Error("value", "text advance exceeded");
    const int ascent = count ? pixel(face->size->metrics.ascender) : 0;
    const int width = xmax - xmin + 2 * stroke + static_cast<int>(std::ceil(fraction.x));
    const int height = ymax - ymin + 2 * stroke + static_cast<int>(std::ceil(fraction.y));
    if (width > 1048576 || height > 65536) throw core::Error("value", "text bounds exceeded");
    Image mask = Image::create("L", {width, height}, 0);
    std::array<int, 4> box{xmin - stroke, ascent - ymax - stroke, xmin - stroke + width, ascent - ymax - stroke + height};
    FT_Stroker raw_stroker = nullptr;
    if (stroke) { check(FT_Stroker_New(library, &raw_stroker)); FT_Stroker_Set(raw_stroker, static_cast<FT_Fixed>(stroke * 64), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0); }
    std::unique_ptr<std::remove_pointer_t<FT_Stroker>, decltype(&FT_Stroker_Done)> stroker(raw_stroker, FT_Stroker_Done);
    std::int64_t x = static_cast<std::int64_t>(std::round((-bitmap_xmin + stroke + fraction.x) * 64));
    const std::int64_t y = static_cast<std::int64_t>(std::round((-bitmap_ymax - stroke - fraction.y) * 64));
    for (std::size_t i = 0; i < count; ++i) {
        if (stop.stop_requested()) throw Cancelled();
        check(FT_Load_Glyph(face, glyphs[i].first, flags | (stroke ? 0 : FT_LOAD_RENDER)));
        FT_Glyph raw = nullptr;
        if (stroke) {
            check(FT_Get_Glyph(face->glyph, &raw));
            const auto error = FT_Glyph_StrokeBorder(&raw, stroker.get(), 0, 1);
            if (error) { FT_Done_Glyph(raw); check(error); }
            const auto bitmap_error = FT_Glyph_To_Bitmap(&raw, FT_RENDER_MODE_NORMAL, nullptr, 1);
            if (bitmap_error) { FT_Done_Glyph(raw); check(bitmap_error); }
        }
        std::unique_ptr<std::remove_pointer_t<FT_Glyph>, decltype(glyph_delete)> glyph(raw, glyph_delete);
        const auto bitmap_glyph = reinterpret_cast<FT_BitmapGlyph>(raw);
        const FT_Bitmap& bitmap = stroke ? bitmap_glyph->bitmap : face->glyph->bitmap;
        const int left = pixel(x + 0) + (stroke ? bitmap_glyph->left : face->glyph->bitmap_left);
        const int top = -(pixel(y + 0) + (stroke ? bitmap_glyph->top : face->glyph->bitmap_top));
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) throw core::Error("value", "unsupported text bitmap");
        for (unsigned int by = 0; by < bitmap.rows; ++by) {
            const int yy = top + static_cast<int>(by); if (yy < 0 || yy >= height) continue;
            const auto* row = bitmap.buffer + (bitmap.pitch < 0 ? (bitmap.rows - 1 - by) * static_cast<unsigned int>(-bitmap.pitch) : by * static_cast<unsigned int>(bitmap.pitch));
            auto* dest = mask.raw()->image8[yy];
            for (unsigned int bx = 0; bx < bitmap.width; ++bx) {
                const int xx = left + static_cast<int>(bx); if (xx < 0 || xx >= width) continue;
                const unsigned int value = bitmap.pixel_mode == FT_PIXEL_MODE_GRAY ? row[bx] : ((row[bx / 8] & (0x80u >> (bx % 8))) ? 255u : 0u);
                const unsigned int product = static_cast<unsigned int>(dest[xx]) * (255u - value) + 128u;
                dest[xx] = static_cast<unsigned char>(std::min(255u, value + ((product + (product >> 8)) >> 8)));
            }
        }
        x += glyphs[i].second;
    }
    return {std::move(mask), box};
    });
}

void Draw::draw_lines(std::span<const PointD> xy, int ink, int width) {
    Imaging im = target_->im;
    const int blend = target_->blend;
    const std::size_t n = xy.size();
    if (width == 1) {
        const PointD* last = nullptr;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            detail::check_status(ImagingDrawLine(im, c_int(xy[i].x), c_int(xy[i].y), c_int(xy[i + 1].x),
                                                 c_int(xy[i + 1].y), &ink, blend));
            last = &xy[i + 1];
        }
        if (last != nullptr) (void)ImagingDrawPoint(im, c_int(last->x), c_int(last->y), &ink, blend);
    } else {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            detail::check_status(ImagingDrawWideLine(im, c_int(xy[i].x), c_int(xy[i].y), c_int(xy[i + 1].x),
                                                     c_int(xy[i + 1].y), &ink, width, blend, nullptr));
        }
    }
}

void Draw::line(std::span<const PointD> xy, const std::optional<Ink>& fill, int width, Joint joint) {
    const INT32 ink = target_->ink_of(fill);
    if (width == 0) return;
    draw_lines(xy, ink, width);
    if (joint != Joint::Curve || width <= 4) return;
    for (std::size_t i = 1; i + 1 < xy.size(); ++i) {
        const PointD point = xy[i];
        double angles[2];
        const PointD ends[2][2] = {{xy[i - 1], point}, {point, xy[i + 1]}};
        for (int k = 0; k < 2; ++k) {
            const PointD& start = ends[k][0];
            const PointD& end = ends[k][1];
            angles[k] = core::py_fmod(core::py_atan2(end.x - start.x, start.y - end.y) * kRadToDeg, 360.0);
        }
        if (angles[0] == angles[1]) continue;  // a straight line: no joint is needed

        const double distance = width / 2.0 - 1;
        const auto coord_at_angle = [&](const PointD& coord, double angle) {
            angle -= 90;
            const double dx = distance * core::py_cos(angle * kDegToRad);
            const double dy = distance * core::py_sin(angle * kDegToRad);
            return PointD{coord.x + (dx > 0 ? std::floor(dx) : std::ceil(dx)), coord.y + (dy > 0 ? std::floor(dy) : std::ceil(dy))};
        };
        const bool flipped = (angles[1] > angles[0] && angles[1] - 180 > angles[0]) ||
                             (angles[1] < angles[0] && angles[1] + 180 > angles[0]);
        const BoxF coords{point.x - width / 2.0 + 1, point.y - width / 2.0 + 1, point.x + width / 2.0 - 1,
                          point.y + width / 2.0 - 1};
        double start = 0.0;
        double end = 0.0;
        if (flipped) {
            start = angles[1] + 90;
            end = angles[0] + 90;
        } else {
            start = angles[0] - 90;
            end = angles[1] - 90;
        }
        pieslice(coords, start - 90, end - 90, fill);
        if (width > 8) {
            // cover potential gaps between the line and the joint
            std::vector<PointD> gap;
            if (flipped) {
                gap = {coord_at_angle(point, angles[0] + 90), point, coord_at_angle(point, angles[1] + 90)};
            } else {
                gap = {coord_at_angle(point, angles[0] - 90), point, coord_at_angle(point, angles[1] - 90)};
            }
            line(gap, fill, 3);
        }
    }
}

void Draw::polygon(std::span<const PointD> xy, const std::optional<Ink>& fill, const std::optional<Ink>& outline,
                   int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (xy.size() < 2) throw core::Error("value", "coordinate list must contain at least 2 coordinates");
    std::vector<int> ixy;
    ixy.reserve(xy.size() * 2);
    for (const PointD& p : xy) {
        ixy.push_back(c_int(p.x));
        ixy.push_back(c_int(p.y));
    }
    const int count = static_cast<int>(xy.size());
    if (fill_ink) {
        detail::check_status(ImagingDrawPolygon(t.im, count, ixy.data(), &*fill_ink, 1, 0, t.blend, nullptr));
    }
    if (ink && ink != fill_ink && width != 0) {
        if (width == 1) {
            detail::check_status(ImagingDrawPolygon(t.im, count, ixy.data(), &*ink, 0, 1, t.blend, nullptr));
        } else {
            // to avoid expanding the polygon outwards, the fill is the mask of the outline (a "1" image of the
            // picture's size, drawn with the ink of 1)
            const INT32 mask_ink = detail::ink_for(Ink(1), t.im);
            Image band;
            Imaging mask = nullptr;
            Imaging mask_view = nullptr;
            std::vector<unsigned char> scratch;
            if (t.is_view) {
                band = Image::create("1", Size{t.im->xsize, t.window_rows});
                mask_view = make_view(band.raw(), t.window_y0, t.im->ysize, scratch);
                mask = mask_view;
            } else {
                band = Image::create("1", Size{t.im->xsize, t.im->ysize});
                mask = band.raw();
            }
            const int drawn = ImagingDrawPolygon(mask, count, ixy.data(), &mask_ink, 1, 0, 0, nullptr);
            const int outlined = drawn < 0 ? drawn
                                           : ImagingDrawPolygon(t.im, count, ixy.data(), &*ink, 0, width * 2 - 1,
                                                                t.blend, mask);
            if (mask_view != nullptr) ImagingDelete(mask_view);
            detail::check_status(outlined);
        }
    }
}

namespace {

// The two-corner shapes of _imaging.c: the corners in order, then truncated.
struct Corners {
    int x0, y0, x1, y1;
};

Corners corners_of(const BoxF& box) {
    if (box.x1 < box.x0) throw core::Error("value", "x1 must be greater than or equal to x0");
    if (box.y1 < box.y0) throw core::Error("value", "y1 must be greater than or equal to y0");
    return Corners{c_int(box.x0), c_int(box.y0), c_int(box.x1), c_int(box.y1)};
}

}  // namespace

void Draw::ellipse(const BoxF& box, const std::optional<Ink>& fill, const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawEllipse(t.im, c.x0, c.y0, c.x1, c.y1, &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawEllipse(t.im, c.x0, c.y0, c.x1, c.y1, &*ink, 0, width, t.blend));
    }
}

void Draw::rectangle(const BoxF& box, const std::optional<Ink>& fill, const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawRectangle(t.im, c.x0, c.y0, c.x1, c.y1, &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawRectangle(t.im, c.x0, c.y0, c.x1, c.y1, &*ink, 0, width, t.blend));
    }
}

void Draw::point(std::span<const PointD> xy, const std::optional<Ink>& fill) {
    Target& t = *target_;
    const INT32 ink = t.ink_of(fill);
    for (const PointD& p : xy) detail::check_status(ImagingDrawPoint(t.im, c_int(p.x), c_int(p.y), &ink, t.blend));
}

void Draw::arc(const BoxF& box, double start, double end, const std::optional<Ink>& fill, int width) {
    Target& t = *target_;
    const INT32 ink = t.ink_of(fill);
    if (width == 0) return;
    const Corners c = corners_of(box);
    detail::check_status(ImagingDrawArc(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                        static_cast<float>(end), &ink, width, t.blend));
}

void Draw::pieslice(const BoxF& box, double start, double end, const std::optional<Ink>& fill,
                    const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawPieslice(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                                 static_cast<float>(end), &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawPieslice(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                                 static_cast<float>(end), &*ink, 0, width, t.blend));
    }
}

void Draw::chord(const BoxF& box, double start, double end, const std::optional<Ink>& fill,
                 const std::optional<Ink>& outline, int width) {
    Target& t = *target_;
    const bool both_none = !fill && !outline;
    const std::optional<INT32> ink = both_none ? std::optional<INT32>(t.default_ink)
                                               : (outline ? std::optional<INT32>(detail::ink_for(*outline, t.im)) : std::nullopt);
    const std::optional<INT32> fill_ink = fill ? std::optional<INT32>(detail::ink_for(*fill, t.im)) : std::nullopt;
    if (fill_ink) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawChord(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                              static_cast<float>(end), &*fill_ink, 1, 0, t.blend));
    }
    if (ink && ink != fill_ink && width != 0) {
        const Corners c = corners_of(box);
        detail::check_status(ImagingDrawChord(t.im, c.x0, c.y0, c.x1, c.y1, static_cast<float>(start),
                                              static_cast<float>(end), &*ink, 0, width, t.blend));
    }
}

// --- PageCanvas -----------------------------------------------------------------------------------------------------

PageCanvas::PageCanvas(Image& part, const Box& area, Size page, std::string_view mode)
    : part_(part), area_(area), page_(page) {
    if (part.empty()) throw core::Error("value", "no image");
    if (part.size() != Size{area.width(), area.height()}) throw core::Error("value", "the part is not the area's size");
    if (area == Box{0, 0, page.width, page.height}) {
        draw_ = std::make_unique<Draw>(part, mode);
        return;
    }
    // the rows of the area, as wide as the page
    Imaging band = nullptr;
    if (area.x0 == 0 && area.x1 == page.width) {
        band = part.raw();
    } else {
        band_ = Image::create_blank(part.mode(), Size{page.width, area.height()});
        band_.paste(part, Point{area.x0, 0});
        band = band_.raw();
    }
    auto target = std::make_unique<Draw::Target>();
    target->owned_view = make_view(band, area.y0, page.height, target->scratch);
    target->im = target->owned_view;
    target->is_view = true;
    target->window_y0 = area.y0;
    target->window_rows = area.height();
    const std::string_view own = part.mode();
    if (mode.empty()) mode = own;
    if (mode != own) {
        if (mode == "RGBA" && own == "RGB") {
            target->blend = 1;
        } else {
            throw core::Error("value", "mode mismatch");
        }
    }
    target->default_ink = detail::ink_for(Ink(mode == "I" || mode == "F" ? 1 : -1), target->im);
    draw_.reset(new Draw(std::move(target)));
}

PageCanvas::~PageCanvas() = default;

void PageCanvas::commit() {
    if (committed_) return;
    committed_ = true;
    draw_.reset();
    if (!band_.empty()) {
        const Image back = band_.crop(Box{area_.x0, 0, area_.x1, area_.height()});
        part_.paste(back, Point{0, 0});
    }
}

}  // namespace genko::render
