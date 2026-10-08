#include "render/color_canvas.hpp"
#include "core/strokes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/imaging.hpp"

namespace genko::render {
namespace {
struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};
using Pixel = std::array<double, 5>; // straight linear RGB, alpha, preceding layer alpha
using RGB = std::array<double,3>;
// PhotoCraft color/blend.rs scalar formulas, evaluated without an RGBA8 intermediate.
// The existing RGB8/Python compositor is deliberately unchanged (its float32 and Soft Light semantics differ).
double luminosity(const RGB& c) { return .3*c[0]+.59*c[1]+.11*c[2]; }
RGB set_luminosity(RGB c,double target) {
    const double delta=target-luminosity(c);for(auto& v:c)v+=delta;
    const double l=luminosity(c),lo=*std::min_element(c.begin(),c.end()),hi=*std::max_element(c.begin(),c.end());
    if(lo<0)for(auto& v:c)v=std::abs(l-lo)<1e-12?l:l+(v-l)*l/(l-lo);
    if(hi>1)for(auto& v:c)v=std::abs(hi-l)<1e-12?l:l+(v-l)*(1-l)/(hi-l);
    return c;
}
double saturation(const RGB& c) { return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end()); }
RGB set_saturation(RGB c,double target) {
    const double lo=*std::min_element(c.begin(),c.end()),span=saturation(c);
    for(auto& v:c)v=span>0?(v-lo)*target/span:0;
    return c;
}
double color_burn(double b,double s) { return b>=1?1:s<=0?0:1-std::min(1.0,(1-b)/s); }
double color_dodge(double b,double s) { return b<=0?0:s>=1?1:std::min(1.0,b/(1-s)); }
RGB blend_rgb(std::string_view mode,const RGB& b,const RGB& s) {
    if(mode=="hue")return set_luminosity(set_saturation(s,saturation(b)),luminosity(b));
    if(mode=="saturation")return set_luminosity(set_saturation(b,saturation(s)),luminosity(b));
    if(mode=="color")return set_luminosity(s,luminosity(b));
    if(mode=="luminosity")return set_luminosity(b,luminosity(s));
    RGB out{};
    for(unsigned c=0;c<3;++c){const double cb=b[c],cs=s[c];
        if(mode=="multiply")out[c]=cb*cs;
        else if(mode=="screen")out[c]=cb+cs-cb*cs;
        else if(mode=="overlay")out[c]=cb<=.5?2*cb*cs:1-2*(1-cb)*(1-cs);
        else if(mode=="add")out[c]=std::min(1.0,cb+cs);
        else if(mode=="darken")out[c]=std::min(cb,cs);
        else if(mode=="lighten")out[c]=std::max(cb,cs);
        else if(mode=="color_burn")out[c]=color_burn(cb,cs);
        else if(mode=="color_dodge")out[c]=color_dodge(cb,cs);
        else if(mode=="linear_burn")out[c]=std::max(0.0,cb+cs-1);
        else if(mode=="soft_light")out[c]=cs<=.5?2*cb*cs+cb*cb*(1-2*cs):2*cb*(1-cs)+std::sqrt(std::max(0.0,cb))*(2*cs-1);
        else if(mode=="hard_light")out[c]=cs<=.5?2*cb*cs:cb+(2*cs-1)-cb*(2*cs-1);
        else if(mode=="difference")out[c]=std::abs(cb-cs);
        else if(mode=="exclusion")out[c]=cb+cs-2*cb*cs;
        else if(mode=="subtract")out[c]=std::max(0.0,cb-cs);
        else if(mode=="divide")out[c]=cs<=0?(cb<=0?0:1):std::min(1.0,cb/cs);
        else out[c]=cs; // normal (unknown modes are refused at the public entry)
    }
    return out;
}
std::array<double, 4> linear(std::array<double, 4> p) {
    for (unsigned c = 0; c < 3; ++c) p[c] = core::srgb_to_linear(p[c]);
    return p;
}
int byte(double x) { return static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0)*255)); }
std::array<double, 4> sampled(const core::ColorRasterView& src, double x, double y) {
    x = std::clamp(x, 0.0, double(src.width()-1));
    y = std::clamp(y, 0.0, double(src.height()-1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0+1, src.width()-1), y1 = std::min(y0+1, src.height()-1);
    const double fx = x-x0, fy = y-y0;
    const std::array<double, 4> weights{(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    const std::array<std::size_t, 4> indices{std::size_t(y0)*src.width()+x0, std::size_t(y0)*src.width()+x1,
        std::size_t(y1)*src.width()+x0, std::size_t(y1)*src.width()+x1};
    std::array<double, 4> out{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto p = linear(src.pixel(indices[i]));
        out[3] += p[3]*weights[i];
        for (unsigned c = 0; c < 3; ++c) out[c] += p[c]*p[3]*weights[i];
    }
    if (out[3] > 0) for (unsigned c = 0; c < 3; ++c) out[c] /= out[3];
    return out;
}
}
struct ColorCanvas::Impl {
    // The budget must outlive both the reservation and its payload.
    ImageAllocationBudget budget{512ULL*1024*1024};
    Work work;
    std::vector<Pixel> pixels;
    Size size;
    bool previous = false;
    explicit Impl(Size s) : size(s) {
        if (s.width <= 0 || s.height <= 0) throw core::Error("value", "invalid color canvas dimensions");
        const auto n = std::uint64_t(s.width)*std::uint64_t(s.height);
        if (n > (512ULL*1024*1024)/sizeof(Pixel)) throw core::Error("image_too_large", "color canvas budget exceeded");
        if (!genko_imaging_budget_reserve(&work.reservation, n*sizeof(Pixel))) detail::throw_imaging_error();
        pixels.resize(static_cast<std::size_t>(n));
    }
    void over(std::size_t i, const std::array<double, 4>& src, double opacity, bool clip, std::string_view mode) {
        auto& dst = pixels[i];
        const double a = src[3]*std::clamp(opacity, 0.0, 1.0)*(clip && previous ? dst[4] : 1);
        const bool normal=mode.empty() || mode=="normal";
        // Never evaluate zero-weight colour terms: HDR multiplication can overflow even when invisible.
        if(a==0){dst[4]=src[3];return;}
        if((normal && a==1) || dst[3]==0){
            for(unsigned c=0;c<3;++c){dst[c]=src[c];}dst[3]=a;dst[4]=src[3];return;
        }
        const double alpha=a+dst[3]*(1-a);
        const RGB mixed=normal?RGB{}:blend_rgb(mode,{dst[0],dst[1],dst[2]},{src[0],src[1],src[2]});
        RGB next{};
        if(alpha>0)for(unsigned c=0;c<3;++c){
            if(normal)next[c]=(src[c]*a+dst[c]*dst[3]*(1-a))/alpha;
            else next[c]=(dst[c]*dst[3]*(1-a)+src[c]*a*(1-dst[3])+mixed[c]*a*dst[3])/alpha;
            if(!std::isfinite(next[c]))throw core::Error("value","high-precision composite exceeds finite working range");
        }
        for(unsigned c=0;c<3;++c)dst[c]=next[c];
        dst[3]=alpha;dst[4]=src[3];
    }
};
ColorCanvas::ColorCanvas(const Image& initial) : impl_(std::make_unique<Impl>(initial.size())) {
    const auto rgba = initial.convert("RGBA");
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        for (int x = 0; x < rgba.width(); ++x) {
            const auto p = linear({row[x*4]/255.0, row[x*4+1]/255.0, row[x*4+2]/255.0, row[x*4+3]/255.0});
            impl_->pixels[std::size_t(y)*rgba.width()+x] = {p[0], p[1], p[2], p[3], 0};
        }
    }
}
ColorCanvas::~ColorCanvas() = default;
bool ColorCanvas::supports_blend(std::string_view mode) {
    static constexpr std::array<std::string_view,20> modes={"normal","multiply","screen","add","overlay","darken","lighten",
        "color_burn","color_dodge","linear_burn","soft_light","hard_light","difference","exclusion","subtract","divide",
        "hue","saturation","color","luminosity"};
    return mode.empty() || std::find(modes.begin(),modes.end(),mode)!=modes.end();
}
void ColorCanvas::blend(const Image& input, double opacity, bool clip, std::string_view mode) {
    if(!supports_blend(mode))throw core::Error("not_yet_ported","high-precision blend:"+std::string(mode));
    if (input.size() != impl_->size) throw core::Error("value", "color canvas images do not match");
    const auto rgba = input.convert("RGBA");
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        for (int x = 0; x < rgba.width(); ++x)
            impl_->over(std::size_t(y)*rgba.width()+x,
                linear({row[x*4]/255.0, row[x*4+1]/255.0, row[x*4+2]/255.0, row[x*4+3]/255.0}), opacity, clip, mode);
    }
    impl_->previous = true;
}
void ColorCanvas::blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip, std::string_view mode,
                        const Image* mask) {
    if(!supports_blend(mode))throw core::Error("not_yet_ported","high-precision blend:"+std::string(mode));
    if (full.width <= 0 || full.height <= 0 || area.width() != impl_->size.width || area.height() != impl_->size.height)
        throw core::Error("value", "color canvas region does not match");
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "layer mask does not match");
    for (int y = 0; y < area.height(); ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < area.width(); ++x) {
            auto p = sampled(source, (area.x0+x+.5)*source.width()/full.width-.5,
                (area.y0+y+.5)*source.height()/full.height-.5);
            if (shown) p[3] *= shown[x]/255.0;
            impl_->over(std::size_t(y)*area.width()+x, p, opacity, clip, mode);
        }
    }
    impl_->previous = true;
}
void ColorCanvas::blend(const ColorCanvas& source, double opacity, bool clip, std::string_view mode) {
    if (!supports_blend(mode) || source.impl_->size != impl_->size) throw core::Error("value","color canvas source does not match");
    for (std::size_t i=0;i<impl_->pixels.size();++i) {
        const auto& p=source.impl_->pixels[i];
        impl_->over(i,{p[0],p[1],p[2],p[3]},opacity,clip,mode);
    }
    impl_->previous=true;
}
void ColorCanvas::blend_stroke(const Image& mask, const core::Json& color, double opacity) {
    core::validate_stroke_color(color);
    if (mask.mode() != "L" || mask.size() != impl_->size) throw core::Error("value","stroke coverage does not match");
    auto p=linear({color["values"][0].get<double>(),color["values"][1].get<double>(),color["values"][2].get<double>(),1});
    for (int y=0;y<mask.height();++y) {
        const auto* row=reinterpret_cast<const unsigned char*>(mask.raw()->image[y]);
        for (int x=0;x<mask.width();++x) { p[3]=row[x]/255.;impl_->over(std::size_t(y)*mask.width()+x,p,opacity,false,"normal"); }
    }
    impl_->previous=true;
}
void ColorCanvas::expose(const core::Exposure& e, double opacity, bool clip, const Image* mask) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "exposure mask does not match");
    for (int y = 0; y < impl_->size.height; ++y) for (int x = 0; x < impl_->size.width; ++x) {
        auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
        double amount = std::clamp(opacity, 0.0, 1.0)*(clip && impl_->previous ? p[4] : 1);
        if (mask) amount *= reinterpret_cast<const unsigned char*>(mask->raw()->image[y])[x]/255.0;
        for (unsigned c = 0; c < 3; ++c) p[c] = std::lerp(p[c], e.apply(p[c]), amount);
    }
}
void ColorCanvas::adjust(const core::PreciseAdjustment& adjustment, double opacity, bool clip, const Image* mask) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "adjustment mask does not match");
    for (int y = 0; y < impl_->size.height; ++y) for (int x = 0; x < impl_->size.width; ++x) {
        auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
        double amount = std::clamp(opacity, 0.0, 1.0)*(clip && impl_->previous ? p[4] : 1);
        if (mask) amount *= reinterpret_cast<const unsigned char*>(mask->raw()->image[y])[x]/255.0;
        if (amount == 0) continue;
        const std::array<double, 3> was{core::linear_to_srgb(p[0]), core::linear_to_srgb(p[1]), core::linear_to_srgb(p[2])};
        const auto changed = adjustment(was);
        for (unsigned c = 0; c < 3; ++c) {
            p[c] = core::srgb_to_linear(std::lerp(was[c], changed[c], amount));
            if (!std::isfinite(p[c])) throw core::Error("value", "high-precision adjustment exceeds finite working range");
        }
    }
}
Image ColorCanvas::image() const {
    auto out = Image::create("RGBA", impl_->size, Ink{0, 0, 0, 0});
    for (int y = 0; y < impl_->size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) {
            const auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
            for (unsigned c = 0; c < 3; ++c) {
                const auto encoded=core::linear_to_srgb(p[c]);
                if(!std::isfinite(encoded))throw core::Error("value","high-precision output exceeds finite working range");
                row[x*4+c]=static_cast<unsigned char>(byte(encoded));
            }
            row[x*4+3] = static_cast<unsigned char>(byte(p[3]));
        }
    }
    return out;
}
Size ColorCanvas::size() const { return impl_->size; }
std::array<double, 4> ColorCanvas::linear_pixel(std::size_t i) const {
    if (i >= impl_->pixels.size()) throw core::Error("value", "color canvas pixel out of range");
    const auto& p = impl_->pixels[i];
    return {p[0], p[1], p[2], p[3]};
}
bool ColorCanvas::is_opaque() const {
    // (a raster stretched over the page is resampled: an opaque one's weights sum to 1 within rounding)
    return std::all_of(impl_->pixels.begin(), impl_->pixels.end(), [](const Pixel& p) { return p[3] >= 1 - 1e-12; });
}
bool ColorCanvas::keeps_preceding_alpha() const {
    return std::all_of(impl_->pixels.begin(), impl_->pixels.end(), [](const Pixel& p) { return p[3] == p[4]; });
}
std::string ColorCanvas::color_raster(std::string_view precision) const {
    return core::encode_color_pixels(static_cast<std::uint32_t>(impl_->size.width),
                                    static_cast<std::uint32_t>(impl_->size.height), precision,
        [this](std::size_t i) {
            const auto& p = impl_->pixels[i];
            return std::array<double, 4>{core::linear_to_srgb(p[0]), core::linear_to_srgb(p[1]),
                                         core::linear_to_srgb(p[2]), p[3]};
        });
}
std::optional<core::PreciseAdjustment> correction_of(const core::Layer& layer) {
    core::Json spec = layer.adjust && layer.adjust->is_object() ? *layer.adjust : core::Json::object();
    std::string kind;
    if (const auto it = spec.find("kind"); it != spec.end()) {
        if (core::py_truthy(*it)) kind = it->is_string() ? it->get<std::string>() : core::py_str(*it);
        spec.erase(it);
    }
    if (kind.empty()) return std::nullopt;
    if (kind == "exposure" || std::find(core::kAdjustments.begin(), core::kAdjustments.end(), kind) == core::kAdjustments.end())
        throw core::Error("not_yet_ported", "high-precision correction: " + kind);
    try {
        return core::PreciseAdjustment(kind, spec);
    } catch (const core::PyValueError&) {
        return std::nullopt;  // (a ValueError: the layer does nothing)
    }
}
Image color_raster_preview(const core::ColorRasterView& src, Size full, Box area) {
    const auto transparent = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    ColorCanvas out(transparent);
    out.blend(src, full, area, 1, false);
    return out.image();
}
} // namespace genko::render
