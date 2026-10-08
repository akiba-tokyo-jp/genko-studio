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
#include <map>
#include <mutex>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_MODULE_H
#include FT_SYSTEM_H
#include FT_BITMAP_H

#include "render/page.hpp"

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/imaging.hpp"
#include "render/not_yet_ported.hpp"

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
// --- Pillow's _imagingft.c without raqm --------------------------------------------------------------------------
// The functions below are font_getlength, bounding_box_and_anchors, font_getsize and font_render of Pillow 12.3.0
// with its text_layout_fallback (the BASIC layout): the same FreeType calls with the same load flags, the pen kept
// in the same 26.6 ints, the start fraction, the stroke and the offsets in single precision as Pillow's C floats.

// PIXEL(x): 26.6 to whole pixels, half up ((x + 32) & -64) >> 6.
constexpr std::int64_t ft_pixel(std::int64_t x) { return (x + 32) >> 6; }

// text_layout_fallback (no mono mask, no colour): one glyph per code point, its advance in 26.6; a kerning pair adds
// its adjustment, rounded to whole pixels (PIXEL(delta.x), added to the 26.6 advance as Pillow adds it), to the
// glyph before it.
struct Laid {
    FT_UInt index = 0;
    int advance = 0;
};
std::vector<Laid> basic_layout(FT_Face face, std::u32string_view text, std::stop_token stop) {
    std::vector<Laid> out;
    out.reserve(text.size());
    const bool kerning = FT_HAS_KERNING(face);
    FT_UInt last = 0;
    std::int64_t pen = 0;
    for (const char32_t ch : text) {
        if (stop.stop_requested()) throw Cancelled();
        const FT_UInt index = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        check_text(FT_Load_Glyph(face, index, FT_LOAD_DEFAULT));
        if (kerning && last != 0 && index != 0) {
            FT_Vector delta{};
            if (FT_Get_Kerning(face, last, index, FT_KERNING_DEFAULT, &delta) == 0) {
                out.back().advance += static_cast<int>(ft_pixel(delta.x));
            }
        }
        const FT_Pos advance = face->glyph->metrics.horiAdvance;
        pen += advance;
        if (std::abs(pen) > 64LL * 1048576) throw core::Error("value", "text advance exceeded");
        out.push_back({index, static_cast<int>(advance)});
        last = index;
    }
    return out;
}

struct TextBounds {
    std::int64_t width = 0;
    std::int64_t height = 0;
    int x_offset = 0;
    int y_offset = 0;
};

// bounding_box_and_anchors (horizontal): the glyphs' control boxes and the pen line from 0 to the advance, and the
// offset of the box's corner from the anchor point.
TextBounds bounding_box_and_anchors(FT_Face face, std::string_view anchor, const std::vector<Laid>& glyphs, int load_flags,
                                    std::stop_token stop) {
    std::int64_t position = 0;
    std::int64_t x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    const auto glyph_delete = [](FT_Glyph p) { FT_Done_Glyph(p); };
    for (const Laid& g : glyphs) {
        if (stop.stop_requested()) throw Cancelled();
        const std::int64_t px = ft_pixel(position);
        const std::int64_t py = ft_pixel(0);
        position += g.advance;
        const std::int64_t advanced = ft_pixel(position);
        if (advanced > x_max) x_max = advanced;
        check_text(FT_Load_Glyph(face, g.index, load_flags));
        FT_Glyph raw = nullptr;
        check_text(FT_Get_Glyph(face->glyph, &raw));
        std::unique_ptr<std::remove_pointer_t<FT_Glyph>, decltype(glyph_delete)> glyph(raw, glyph_delete);
        FT_BBox bbox{};
        FT_Glyph_Get_CBox(glyph.get(), FT_GLYPH_BBOX_PIXELS, &bbox);
        x_max = std::max<std::int64_t>(x_max, bbox.xMax + px);
        x_min = std::min<std::int64_t>(x_min, bbox.xMin + px);
        y_max = std::max<std::int64_t>(y_max, bbox.yMax + py);
        y_min = std::min<std::int64_t>(y_min, bbox.yMin + py);
    }
    const std::string_view a = anchor.empty() ? std::string_view("la") : anchor;
    const auto bad = [&]() { return core::PyValueError("bad anchor specified: " + std::string(a)); };
    if (a.size() != 2) throw bad();
    std::int64_t x_anchor = 0;
    std::int64_t y_anchor = 0;
    if (!glyphs.empty()) {
        switch (a[0]) {
            case 'l': x_anchor = 0; break;
            case 'm': x_anchor = ft_pixel(position / 2); break;  // (C's long division: towards zero)
            case 'r': x_anchor = ft_pixel(position); break;
            default: throw bad();
        }
        const FT_Size_Metrics& m = face->size->metrics;
        switch (a[1]) {
            case 'a': y_anchor = ft_pixel(m.ascender); break;
            case 't': y_anchor = y_max; break;
            case 'm': y_anchor = ft_pixel((m.ascender + m.descender) / 2); break;
            case 's': y_anchor = 0; break;
            case 'b': y_anchor = y_min; break;
            case 'd': y_anchor = ft_pixel(m.descender); break;
            default: throw bad();
        }
    }
    return {x_max - x_min, y_max - y_min, static_cast<int>(-x_anchor + x_min), static_cast<int>(-(-y_anchor + y_max))};
}

// font_render with ImageDraw's "L" mode: the mask the size of the text's box (widened by the stroke and the start
// fraction), each glyph's coverage laid over it as Pillow blends it. (Pillow allocates the mask before its first
// pass over the glyph bitmaps; the passes are pure, so the bitmaps are made first here: a glyph too large for
// FreeType's working memory is refused before the mask is allocated.)
std::pair<Image, Point> render_laid(FT_Face face, FT_Library library, const std::vector<Laid>& glyphs, float stroke_width,
                                    bool stroke_filled, std::string_view anchor, float x_start, float y_start,
                                    std::stop_token stop) {
    int load_flags = stroke_width != 0.0F ? FT_LOAD_NO_BITMAP : FT_LOAD_DEFAULT;
    const TextBounds bounds = bounding_box_and_anchors(face, anchor, glyphs, load_flags, stop);
    // width += ceil(stroke_width * 2 + x_start): the float sum, its ceiling
    const std::int64_t width = bounds.width + static_cast<std::int64_t>(std::ceil(static_cast<double>(stroke_width * 2.0F + x_start)));
    const std::int64_t height = bounds.height + static_cast<std::int64_t>(std::ceil(static_cast<double>(stroke_width * 2.0F + y_start)));
    if (width > 1048576 || height > 65536 || width < 0 || height < 0) throw core::Error("value", "text bounds exceeded");
    // x_offset = round(x_offset - stroke_width)
    const Point offset{static_cast<int>(std::round(static_cast<double>(static_cast<float>(bounds.x_offset) - stroke_width))),
                       static_cast<int>(std::round(static_cast<double>(static_cast<float>(bounds.y_offset) - stroke_width)))};
    if (glyphs.empty() || width == 0 || height == 0) {
        return {Image::create("L", {static_cast<int>(width), static_cast<int>(height)}, 0), offset};
    }
    // x_min and y_max of the glyph bitmaps (with the stroke's flags but not stroked: "must match font_getsize")
    int x = 0, x_min = 0, y_max = 0;
    for (const Laid& g : glyphs) {
        if (stop.stop_requested()) throw Cancelled();
        const int px = static_cast<int>(ft_pixel(x));
        const int py = static_cast<int>(ft_pixel(0));
        check_text(FT_Load_Glyph(face, g.index, load_flags | FT_LOAD_RENDER));
        const FT_GlyphSlot slot = face->glyph;
        if (slot->bitmap_top + py > y_max) y_max = slot->bitmap_top + py;
        if (slot->bitmap_left + px < x_min) x_min = slot->bitmap_left + px;
        x += g.advance;
    }
    Image mask = Image::create("L", {static_cast<int>(width), static_cast<int>(height)}, 0);
    Imaging im = mask.raw();
    FT_Stroker raw_stroker = nullptr;
    if (stroke_width != 0.0F) {
        check_text(FT_Stroker_New(library, &raw_stroker));
        FT_Stroker_Set(raw_stroker, static_cast<FT_Fixed>(std::round(static_cast<double>(stroke_width * 64.0F))),
                       FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
    }
    std::unique_ptr<std::remove_pointer_t<FT_Stroker>, decltype(&FT_Stroker_Done)> stroker(raw_stroker, FT_Stroker_Done);
    // the pen at the text's origin: (-x_min + stroke_width + x_start) * 64 and (-y_max - stroke_width - y_start) * 64,
    // each a C float, rounded
    x = static_cast<int>(std::round(static_cast<double>((static_cast<float>(-x_min) + stroke_width + x_start) * 64.0F)));
    const int y = static_cast<int>(std::round(static_cast<double>((static_cast<float>(-y_max) + (-stroke_width) - y_start) * 64.0F)));
    if (!stroker) load_flags |= FT_LOAD_RENDER;
    const auto glyph_delete = [](FT_Glyph p) { FT_Done_Glyph(p); };
    FT_Bitmap converted;
    FT_Bitmap_Init(&converted);
    const auto converted_done = [&](FT_Bitmap* b) { FT_Bitmap_Done(library, b); };
    std::unique_ptr<FT_Bitmap, decltype(converted_done)> converted_hold(&converted, converted_done);
    for (const Laid& g : glyphs) {
        if (stop.stop_requested()) throw Cancelled();
        const int px = static_cast<int>(ft_pixel(x));
        const int py = static_cast<int>(ft_pixel(y));
        check_text(FT_Load_Glyph(face, g.index, load_flags));
        const FT_GlyphSlot slot = face->glyph;
        std::unique_ptr<std::remove_pointer_t<FT_Glyph>, decltype(glyph_delete)> glyph(nullptr, glyph_delete);
        FT_Bitmap bitmap{};
        int xx = 0, yy = 0;
        if (stroker) {
            FT_Glyph raw = nullptr;
            check_text(FT_Get_Glyph(slot, &raw));
            FT_Error error = stroke_filled ? FT_Glyph_StrokeBorder(&raw, stroker.get(), 0, 1) : FT_Glyph_Stroke(&raw, stroker.get(), 1);
            if (!error) {
                FT_Vector origin{0, 0};
                error = FT_Glyph_To_Bitmap(&raw, FT_RENDER_MODE_NORMAL, &origin, 1);
            }
            glyph.reset(raw);
            check_text(error);
            const auto bitmap_glyph = reinterpret_cast<FT_BitmapGlyph>(raw);
            bitmap = bitmap_glyph->bitmap;
            xx = px + bitmap_glyph->left;
            yy = -(py + bitmap_glyph->top);
        } else {
            bitmap = slot->bitmap;
            xx = px + slot->bitmap_left;
            yy = -(py + slot->bitmap_top);
        }
        if (bitmap.buffer != nullptr) {
            unsigned int convert_scale = 1;
            switch (bitmap.pixel_mode) {
                case FT_PIXEL_MODE_MONO: convert_scale = 255; break;
                case FT_PIXEL_MODE_GRAY2: convert_scale = 255 / 3; break;
                case FT_PIXEL_MODE_GRAY4: convert_scale = 255 / 15; break;
                default: convert_scale = 1;
            }
            switch (bitmap.pixel_mode) {
                case FT_PIXEL_MODE_MONO:
                case FT_PIXEL_MODE_GRAY2:
                case FT_PIXEL_MODE_GRAY4:
                    check_text(FT_Bitmap_Convert(library, &bitmap, &converted, 1));
                    bitmap = converted;
                    break;
                case FT_PIXEL_MODE_GRAY: break;
                default: throw core::Error("value", "unsupported bitmap pixel mode");
            }
            // the glyph clipped to the mask, blended in as font_render does for "L"
            int x0 = 0;
            int x1 = static_cast<int>(bitmap.width);
            if (xx < 0) x0 = -xx;
            if (xx + x1 > im->xsize) x1 = im->xsize - xx;
            const unsigned char* source = bitmap.buffer;
            for (unsigned int row = 0; row < bitmap.rows; ++row, ++yy) {
                if (yy >= 0 && yy < im->ysize) {
                    unsigned char* line = reinterpret_cast<unsigned char*>(im->image8[yy]);
                    for (int k = x0; k < x1; ++k) {
                        const unsigned int src_alpha = source[k] * convert_scale;
                        unsigned char& target = line[xx + k];
                        if (src_alpha > 0) {
                            if (target > 0) {
                                const unsigned int tmp = target * (255 - src_alpha) + 128;  // MULDIV255
                                const unsigned int v = src_alpha + (((tmp >> 8) + tmp) >> 8);
                                target = static_cast<unsigned char>(v > 255 ? 255 : v);  // CLIP8
                            } else {
                                target = static_cast<unsigned char>(src_alpha);
                            }
                        }
                    }
                }
                source += bitmap.pitch;
            }
        }
        x += g.advance;
    }
    return {std::move(mask), offset};
}

// The text's code points (text_string's checks: at most 64 KiB of valid UTF-8).
std::u32string text_chars(std::string_view text) {
    const auto chars=text_string(text).toUcs4();
    return std::u32string(chars.begin(),chars.end());
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
    return with_text_font(selected,size,[&](FT_Face face,FT_Library){
        std::int64_t advance=0;
        for(const Laid& glyph:basic_layout(face,text_chars(text),stop))advance+=glyph.advance;
        return static_cast<double>(advance)/64;
    });
}

std::pair<Image, std::array<int, 4>> text_mask(std::string_view text, std::string_view font, int size,
                                              PointD fraction, std::stop_token stop, int stroke) {
    if (size < 1 || size > 8192 || text.size() > 65536 || stroke < 0 || stroke > 512 ||
        !std::isfinite(fraction.x) || !std::isfinite(fraction.y) || fraction.x < 0 || fraction.x >= 1 ||
        fraction.y < 0 || fraction.y >= 1) throw core::Error("value", "invalid text geometry");
    if (stop.stop_requested()) throw Cancelled();
    const auto selected=text_font(font,text,stop);
    return with_text_font(selected,size,[&](FT_Face face,FT_Library library) -> std::pair<Image,std::array<int,4>> {
        // ImageDraw.text's getmask2 (anchor "la", the stroke filled), the fraction handed over as Pillow's C floats
        auto [mask, offset] = render_laid(face, library, basic_layout(face, text_chars(text), stop), static_cast<float>(stroke),
                                          true, "la", static_cast<float>(fraction.x), static_cast<float>(fraction.y), stop);
        const std::array<int, 4> box{offset.x, offset.y, offset.x + mask.width(), offset.y + mask.height()};
        return {std::move(mask), box};
    });
}

// --- TrueTypeFont, TrueTypeFonts -----------------------------------------------------------------------------------

TrueTypeFont::TrueTypeFont(FT_FaceRec_* face, FT_LibraryRec_* library, int size, std::stop_token stop)
    : face_(face), library_(library), size_(size), stop_(std::move(stop)) {}

TrueTypeFont::~TrueTypeFont() {
    if (face_ != nullptr) (void)FT_Done_Face(face_);
}

std::pair<int, int> TrueTypeFont::getmetrics() const {
    // font_getattr_ascent / _descent
    const FT_Size_Metrics& m = face_->size->metrics;
    return {static_cast<int>(ft_pixel(m.ascender)), static_cast<int>(-ft_pixel(m.descender))};
}

double TrueTypeFont::getlength(std::u32string_view text) const {
    // font_getlength: the advances summed in 26.6 (a C int), then FreeTypeFont.getlength's / 64
    int length = 0;
    for (const Laid& g : basic_layout(face_, text, stop_)) length += g.advance;
    return static_cast<double>(length) / 64;
}

std::array<int, 4> TrueTypeFont::getbbox(std::u32string_view text, int stroke_width, std::string_view anchor) const {
    // font_getsize (FT_LOAD_DEFAULT whatever the stroke), then FreeTypeFont.getbbox's widening by the stroke
    const TextBounds b = bounding_box_and_anchors(face_, anchor, basic_layout(face_, text, stop_), FT_LOAD_DEFAULT, stop_);
    const int left = b.x_offset - stroke_width;
    const int top = b.y_offset - stroke_width;
    return {left, top, left + static_cast<int>(b.width) + 2 * stroke_width, top + static_cast<int>(b.height) + 2 * stroke_width};
}

std::pair<Image, Point> TrueTypeFont::getmask2(std::u32string_view text, int stroke_width, std::string_view anchor, float start_x,
                                               float start_y, bool stroke_filled) const {
    if (text.size() > 1000000) throw core::PyValueError("too many characters in string");  // (_string_length_check)
    return render_laid(face_, library_, basic_layout(face_, text, stop_), static_cast<float>(stroke_width), stroke_filled, anchor,
                       start_x, start_y, stop_);
}

namespace {

// A bundled font's bytes, read from the resources once per process and then shared (they never change).
std::shared_ptr<const QByteArray> bundled_bytes(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const QByteArray>> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = cache.find(name); it != cache.end()) return it->second;
    init_text_resources();
    QFile source(QStringLiteral(":/genko/text/") + QString::fromStdString(name));
    if (!source.open(QIODevice::ReadOnly)) throw core::Error("file", "text font unavailable");
    auto bytes = std::make_shared<QByteArray>(source.readAll());
    if (bytes->isEmpty()) throw core::Error("file", "text font unavailable");
    cache.emplace(name, bytes);
    return bytes;
}

}  // namespace

struct TrueTypeFonts::State {
    // (members are destroyed in reverse: the fonts, then the library, then the files' bytes and their reservations)
    ImageAllocationBudget budget{512ULL * 1024 * 1024};  // shares, never enlarges, the enclosing image budget
    std::stop_token stop;
    struct File {
        std::shared_ptr<const QByteArray> bytes;
        ImagingMemoryInstance owner{};
        bool reserved = false;
        ~File() {
            if (reserved) genko_imaging_budget_release(&owner);
        }
    };
    std::map<std::string, std::unique_ptr<File>> files;
    TextMemory memory{0, false};
    FT_MemoryRec_ memory_rec{};
    FT_Library library = nullptr;
    std::map<std::pair<std::string, int>, std::unique_ptr<TrueTypeFont>> fonts;

    ~State() {
        fonts.clear();
        if (library != nullptr) (void)FT_Done_Library(library);
    }

    const File& file(const std::string& font) {
        if (const auto it = files.find(font); it != files.end()) return *it->second;
        auto entry = std::make_unique<File>();
        constexpr qint64 cap = 32 * 1024 * 1024;
        if (bundled_text_font(font)) {
            entry->bytes = bundled_bytes(font);
        } else {
            // a font file: Python's truetype() falls back to Pillow's own default font when the file cannot be read
            if (font.size() > 4096) throw core::Error("value", "font name exceeded");
            QFile source(QString::fromStdString(font));
            if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) throw NotYetPorted("default_font");
            if (source.size() <= 0 || source.size() > cap) throw core::Error("value", "text font exceeds byte limit");
            auto bytes = std::make_shared<QByteArray>(source.readAll());
            if (bytes->size() != source.size()) throw core::Error("value", "text font changed during read");
            entry->bytes = std::move(bytes);
        }
        genko_imaging_clear_error();
        if (!genko_imaging_budget_reserve(&entry->owner, static_cast<std::uint64_t>(entry->bytes->size()) + 4096)) {
            detail::throw_imaging_error();
        }
        entry->reserved = true;
        return *files.emplace(font, std::move(entry)).first->second;
    }
};

TrueTypeFonts::TrueTypeFonts(std::stop_token stop) : state_(std::make_unique<State>()) {
    State& s = *state_;
    s.stop = std::move(stop);
    s.memory_rec.user = &s.memory;
    s.memory_rec.alloc = text_allocate;
    s.memory_rec.free = text_free;
    s.memory_rec.realloc = text_reallocate;
    genko_imaging_clear_error();
    check_text(FT_New_Library(&s.memory_rec, &s.library));
    FT_Add_Default_Modules(s.library);
    if (s.memory.second) detail::throw_imaging_error();
    FT_Set_Default_Properties(s.library);
    if (s.memory.second) detail::throw_imaging_error();
}

TrueTypeFonts::~TrueTypeFonts() = default;

std::stop_token TrueTypeFonts::stop() const { return state_->stop; }

const TrueTypeFont& TrueTypeFonts::truetype(const std::string& font, int size) {
    State& s = *state_;
    if (s.stop.stop_requested()) throw Cancelled();
    if (size <= 0) throw core::PyValueError("font size must be greater than 0, not " + std::to_string(size));
    if (size > 0xFFFF) throw core::Error("value", "invalid text geometry");
    const auto key = std::make_pair(font, size);
    if (const auto it = s.fonts.find(key); it != s.fonts.end()) return *it->second;
    const State::File& file = s.file(font);
    FT_Face face = nullptr;
    const FT_Error error = FT_New_Memory_Face(s.library, reinterpret_cast<const FT_Byte*>(file.bytes->constData()),
                                              static_cast<FT_Long>(file.bytes->size()), 0, &face);
    if (error) {
        if (genko_imaging_error_kind() != 0 || s.memory.second) detail::throw_imaging_error();
        if (bundled_text_font(font)) check_text(error);
        throw NotYetPorted("default_font");  // (genko.fonts.truetype: Pillow's load_default())
    }
    auto opened = std::unique_ptr<TrueTypeFont>(new TrueTypeFont(face, s.library, size, s.stop));
    // getfont: FT_Request_Size, nominal, size * 64 both ways, no resolution
    FT_Size_RequestRec request{};
    request.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
    request.width = static_cast<FT_Long>(size) * 64;
    request.height = request.width;
    request.horiResolution = 0;
    request.vertResolution = 0;
    if (const FT_Error sized = FT_Request_Size(face, &request); sized) {
        if (genko_imaging_error_kind() != 0 || s.memory.second) detail::throw_imaging_error();
        if (bundled_text_font(font)) check_text(sized);
        throw NotYetPorted("default_font");
    }
    if (s.memory.second) detail::throw_imaging_error();
    return *s.fonts.emplace(key, std::move(opened)).first->second;
}

// --- Draw::text --------------------------------------------------------------------------------------------------

void Draw::text(PointD xy, std::u32string_view text, const TrueTypeFont& font, const std::optional<Ink>& fill,
                std::string_view anchor, int stroke_width, const std::optional<Ink>& stroke_fill) {
    Target& t = *target_;
    // ImageText._get_fontmode: "1" for these, which draw with a mono mask (not ported)
    const ModeID mode = t.im->mode;
    if (mode == IMAGING_MODE_1 || mode == IMAGING_MODE_P || mode == IMAGING_MODE_I || mode == IMAGING_MODE_F) {
        throw NotYetPorted("text_mono_mask");
    }
    // ImageText._split(xy, anchor, "left")
    struct Line {
        double x;
        double y;
        std::u32string_view text;
    };
    std::vector<std::u32string_view> lines;
    for (std::size_t start = 0;;) {
        const std::size_t end = text.find(U'\n', start);
        lines.push_back(text.substr(start, end == std::u32string_view::npos ? std::u32string_view::npos : end - start));
        if (end == std::u32string_view::npos) break;
        start = end + 1;
    }
    const std::string a = anchor.empty() ? std::string("la") : std::string(anchor);
    if (a.size() != 2) throw core::PyValueError("anchor must be a 2 character string");
    std::vector<Line> parts;
    if (lines.size() == 1) {
        parts.push_back({xy.x, xy.y, lines[0]});
    } else {
        if (a[1] == 't' || a[1] == 'b') throw core::PyValueError("anchor not supported for multiline text");
        // the font's "A" (its bottom from the anchor), the stroke and spacing=4 between lines
        const double line_spacing = font.getbbox(U"A", stroke_width)[3] + stroke_width + 4;
        double top = xy.y;
        std::vector<double> widths;
        double max_width = 0;
        for (const auto line : lines) {
            widths.push_back(font.getlength(line));
            max_width = std::max(max_width, widths.back());
        }
        const auto n = static_cast<double>(lines.size());
        if (a[1] == 'm') {
            top -= (n - 1) * line_spacing / 2.0;
        } else if (a[1] == 'd') {
            top -= (n - 1) * line_spacing;
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            double left = xy.x;
            const double width_difference = max_width - widths[i];
            if (a[0] == 'm') {
                left -= width_difference / 2.0;
            } else if (a[0] == 'r') {
                left -= width_difference;
            }
            parts.push_back({left, top, lines[i]});
            top += line_spacing;
        }
    }
    const INT32 ink = t.ink_of(fill);
    const std::optional<INT32> stroke_ink =
        stroke_width != 0 ? std::optional<INT32>(stroke_fill ? detail::ink_for(*stroke_fill, t.im) : ink) : std::nullopt;
    for (const Line& line : parts) {
        const auto draw_text = [&](INT32 colour, int width) {
            // x = int(line.x), the fraction (math.modf) handed to the font as a C float; draw_bitmap's ImagingFill2
            double whole_x = 0.0;
            double whole_y = 0.0;
            const double fraction_x = std::modf(line.x, &whole_x);
            const double fraction_y = std::modf(line.y, &whole_y);
            auto [mask, offset] = font.getmask2(line.text, width, a, static_cast<float>(fraction_x), static_cast<float>(fraction_y), true);
            const int x = c_int(whole_x) + offset.x;
            const int y = c_int(whole_y) + offset.y;
            detail::check_status(ImagingFill2(t.im, &colour, mask.raw(), x, y, x + mask.width(), y + mask.height()));
        };
        if (stroke_ink) {
            draw_text(*stroke_ink, stroke_width);
            if (ink != *stroke_ink) draw_text(ink, 0);
        } else {
            draw_text(ink, 0);
        }
    }
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

void Draw::rounded_rectangle(const BoxF& box, double radius, const Ink& fill) {
    // ImageDraw.rounded_rectangle with corners=None and no outline
    if (box.x1 < box.x0) throw core::Error("value", "x1 must be greater than or equal to x0");
    if (box.y1 < box.y0) throw core::Error("value", "y1 must be greater than or equal to y0");
    double d = core::py_min(core::py_min(box.x1 - box.x0, box.y1 - box.y0), radius * 2);
    const double x0 = core::py_round_whole(box.x0);
    const double y0 = core::py_round_whole(box.y0);
    const double x1 = core::py_round_whole(box.x1);
    const double y1 = core::py_round_whole(box.y1);
    const bool full_x = d >= x1 - x0 - 1;
    if (full_x) d = x1 - x0;  // (the two left and two right corners are joined)
    const bool full_y = d >= y1 - y0 - 1;
    if (full_y) d = y1 - y0;  // (the two top and two bottom corners are joined)
    if (full_x && full_y) {  // (all corners joined: a circle)
        ellipse(box, fill);
        return;
    }
    if (d == 0) {  // (corners without a curve: a rectangle)
        rectangle(box, fill);
        return;
    }
    const double r = core::py_trunc(core::py_floor(d / 2));  // int(d // 2)
    // the corners (self.draw.draw_pieslice(box, start, end, fill_ink, 1)), then the rectangles between them
    if (full_x) {
        pieslice(BoxF{x0, y0, x0 + d, y0 + d}, 180, 360, fill);
        pieslice(BoxF{x0, y1 - d, x0 + d, y1}, 0, 180, fill);
    } else if (full_y) {
        pieslice(BoxF{x0, y0, x0 + d, y0 + d}, 90, 270, fill);
        pieslice(BoxF{x1 - d, y0, x1, y0 + d}, 270, 90, fill);
    } else {
        pieslice(BoxF{x0, y0, x0 + d, y0 + d}, 180, 270, fill);
        pieslice(BoxF{x1 - d, y0, x1, y0 + d}, 270, 360, fill);
        pieslice(BoxF{x1 - d, y1 - d, x1, y1}, 0, 90, fill);
        pieslice(BoxF{x0, y1 - d, x0 + d, y1}, 90, 180, fill);
    }
    if (full_x) {
        rectangle(BoxF{x0, y0 + r + 1, x1, y1 - r - 1}, fill);
    } else if (x1 - r - 1 >= x0 + r + 1) {
        rectangle(BoxF{x0 + r + 1, y0, x1 - r - 1, y1}, fill);
    }
    if (!full_x && !full_y) {
        rectangle(BoxF{x0, y0 + r + 1, x0 + r, y1 - r - 1}, fill);
        rectangle(BoxF{x1 - r, y0 + r + 1, x1, y1 - r - 1}, fill);
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
