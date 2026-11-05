// PC5 G-b (render/text/opentype.hpp): which records the OpenType typesetting sets, their letters' attributes, a paragraph
// shaped run by run in SheenBidi's order, and a row's glyphs in the order they are drawn.

#include "render/text/opentype.hpp"

#include <QChar>

#include <algorithm>
#include <cmath>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/text/memo.hpp"

namespace genko::render::text {

using core::Json;

bool sets_opentype(const core::StoryLine& line) {
    if (line.native_type && line.native_type->is_object()) {
        const Json& nt = *line.native_type;
        if (nt.value("shaping", Json()) == Json("opentype")) return true;
        for (const char* key : {"char", "para", "runs", "paragraphs", "char_style", "para_style"}) {
            if (nt.contains(key)) return true;
        }
    }
    if (line.wrap == "vertical" || !line.style.is_object()) return false;
    const auto it = line.style.find("features");
    if (it == line.style.end() || !core::py_truthy(*it)) return false;
    try {
        return !core::iterate(*it).empty();
    } catch (const std::exception&) {
        return false;
    }
}

namespace {

thread_local const Json* g_styles = nullptr;
thread_local std::string g_digest;

}  // namespace

TypeStyles::TypeStyles(const Json* styles) : was_(g_styles), was_digest_(g_digest) {
    g_styles = styles;
    g_digest = styles != nullptr && !styles->is_null() ? detail::digest_of(styles->dump()) : std::string();
}
TypeStyles::~TypeStyles() {
    g_styles = was_;
    g_digest = was_digest_;
}
const Json* TypeStyles::current() { return g_styles; }
const std::string& TypeStyles::digest() { return g_digest; }

Resolved resolve(const core::StoryLine& line, std::u32string_view text, const std::vector<std::size_t>& from, const Json* styles) {
    Resolved out;
    const std::u32string source = u32(line.text);
    const std::vector<std::size_t> bounds = core::ruby::bounds(source);  // (graphemes' first code points, and the end)
    const std::vector<core::type_text::CharRun> runs = core::type_text::resolve_chars(line, styles);
    const std::int64_t paragraphs = core::type_text::paragraph_count(line.text);
    for (std::int64_t p = 0; p < paragraphs; ++p) out.paras.push_back(core::type_text::resolve_para(line, styles, p));
    if (out.paras.empty()) out.paras.push_back(core::type_text::resolve_para(line, styles, 0));
    // (each code point of the record's text: its paragraph, counted by its line breaks)
    std::vector<std::size_t> para_at(source.size() + 1, 0);
    for (std::size_t i = 0, p = 0; i <= source.size(); ++i) {
        para_at[i] = std::min(p, out.paras.size() - 1);
        if (i < source.size() && source[i] == U'\n') ++p;
    }
    std::size_t last = 0;
    out.chars.reserve(text.size());
    out.para_of.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::size_t cp = i < from.size() ? from[i] : std::u32string::npos;
        if (cp == std::u32string::npos || cp > source.size()) cp = last;
        last = cp;
        const auto g = static_cast<std::int64_t>(std::upper_bound(bounds.begin(), bounds.end(), cp) - bounds.begin()) - 1;
        // (the run the grapheme is in: the last that starts at or before it — the runs cover the text in order)
        auto it = std::upper_bound(runs.begin(), runs.end(), g, [](std::int64_t at, const core::type_text::CharRun& r) { return at < r.start; });
        const core::type_text::CharRun& run = it == runs.begin() ? runs.front() : *(it - 1);
        out.chars.push_back(run.attrs);
        out.para_of.push_back(para_at[std::min(cp, source.size())]);
    }
    return out;
}

ShapedParagraph shape_paragraph(std::u32string_view text, const std::vector<Look>& looks, const std::vector<std::size_t>& look_of,
                                std::string_view direction) {
    ShapedParagraph out;
    out.bidi = BidiParagraph(std::u32string(text), direction);
    out.advance.assign(text.size(), 0.0);
    out.starts.assign(text.size(), false);
    out.tracking.assign(text.size(), 0.0);
    out.look_of = look_of;
    if (text.empty()) return out;
    const std::vector<std::string> scripts = scripts_of(text);
    const std::vector<std::uint8_t>& levels = out.bidi.levels();
    std::size_t start = 0;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        if (i < text.size() && look_of[i] == look_of[start] && levels[i] == levels[start] && scripts[i] == scripts[start]) continue;
        ShapedRun run;
        run.start = start;
        run.end = i;
        run.look = look_of[start];
        run.level = levels[start];
        const Look& look = looks[run.look];
        run.scale_x = look.shape.scale_x;
        // (the run shaped in its paragraph: HarfBuzz sees the letters around it — an Arabic word changing colour or size
        // mid-word stays joined, review P3-5; all caps: the paragraph with the run's letters in capitals)
        std::u32string capitals;
        if (look.all_caps) {
            capitals.assign(text);
            for (std::size_t k = start; k < i; ++k) capitals[k] = static_cast<char32_t>(QChar::toUpper(static_cast<char32_t>(capitals[k])));
        }
        ShapeRequest request;
        request.context = look.all_caps ? std::u32string_view(capitals) : text;
        request.context_start = start;
        request.context_length = i - start;
        request.font = look.font;
        request.size_px = static_cast<double>(look.size);
        request.direction = (run.level & 1) != 0 ? "rtl" : "ltr";
        request.language = look.language;
        request.features = look.features;
        request.variations = look.variations;
        request.script = scripts[start];
        run.glyphs = shape(request);
        const bool rtl = (run.level & 1) != 0;
        for (std::size_t k = 1; k < run.glyphs.size() && run.monotone; ++k) {
            run.monotone = rtl ? run.glyphs[k].cluster <= run.glyphs[k - 1].cluster : run.glyphs[k].cluster >= run.glyphs[k - 1].cluster;
        }
        for (ShapedGlyph& g : run.glyphs) {
            g.cluster += static_cast<std::uint32_t>(start);
            const std::size_t c = std::min<std::size_t>(g.cluster, text.size() - 1);
            out.advance[c] += g.x_advance * look.shape.scale_x;
            out.starts[c] = true;
        }
        out.starts[start] = true;
        for (std::size_t k = start; k < i; ++k) {
            out.tracking[k] = look.tracking;
            if (out.starts[k]) out.advance[k] += look.tracking;
        }
        out.runs.push_back(std::move(run));
        start = i;
    }
    return out;
}

namespace {

// The glyphs of a run whose clusters are in [from, to): their first and their end in its glyphs — found by halving where
// the clusters are monotone, else the run's all (each told by its cluster).
std::pair<std::size_t, std::size_t> glyphs_between(const ShapedRun& run, std::size_t from, std::size_t to) {
    const std::vector<ShapedGlyph>& g = run.glyphs;
    if (!run.monotone) return {0, g.size()};
    if ((run.level & 1) == 0) {
        const auto b = std::partition_point(g.begin(), g.end(), [&](const ShapedGlyph& x) { return x.cluster < from; });
        const auto e = std::partition_point(b, g.end(), [&](const ShapedGlyph& x) { return x.cluster < to; });
        return {static_cast<std::size_t>(b - g.begin()), static_cast<std::size_t>(e - g.begin())};
    }
    const auto b = std::partition_point(g.begin(), g.end(), [&](const ShapedGlyph& x) { return x.cluster >= to; });
    const auto e = std::partition_point(b, g.end(), [&](const ShapedGlyph& x) { return x.cluster >= from; });
    return {static_cast<std::size_t>(b - g.begin()), static_cast<std::size_t>(e - g.begin())};
}

}  // namespace

RowGlyphs row_glyphs(const ShapedParagraph& paragraph, std::size_t start, std::size_t end, double extra) {
    RowGlyphs out;
    end = std::min(end, paragraph.advance.size());
    if (start >= end) return out;
    out.span.assign(end - start, {0.0, 0.0});
    std::vector<bool> seen(end - start, false);
    double x = 0.0;
    for (const BidiParagraph::Run& visual : paragraph.bidi.visual_runs(start, end)) {
        // (the runs over the visual run: the paragraph's runs are in its order, end to end — found by halving, review P2-5)
        std::vector<const ShapedRun*> runs;
        const auto first = std::partition_point(paragraph.runs.begin(), paragraph.runs.end(), [&](const ShapedRun& r) { return r.end <= visual.start; });
        for (auto r = first; r != paragraph.runs.end() && r->start < visual.end; ++r) {
            if (r->end > visual.start) runs.push_back(&*r);
        }
        if ((visual.level & 1) != 0) std::reverse(runs.begin(), runs.end());
        for (const ShapedRun* r : runs) {
            std::optional<std::size_t> current;
            const auto [from, to] = glyphs_between(*r, visual.start, visual.end);
            for (std::size_t k = from; k < to; ++k) {
                const ShapedGlyph& g = r->glyphs[k];
                if (g.cluster < visual.start || g.cluster >= visual.end) continue;
                if (current && *current != g.cluster) {  // (a cluster done: its tracking, and a justified row's share)
                    x += paragraph.tracking[*current] + extra;
                    out.span[*current - start].second = x;
                }
                if (!current || *current != g.cluster) {
                    current = g.cluster;
                    if (!seen[g.cluster - start]) {
                        out.span[g.cluster - start].first = x;
                        seen[g.cluster - start] = true;
                    }
                }
                RowGlyph glyph;
                glyph.look = r->look;
                glyph.cluster = g.cluster;
                glyph.place = GlyphPlace{g.glyph, x + g.x_offset * r->scale_x, -g.y_offset};
                out.glyphs.push_back(glyph);
                x += g.x_advance * r->scale_x;
            }
            if (current) {
                x += paragraph.tracking[*current] + extra;
                out.span[*current - start].second = x;
            }
        }
    }
    out.width = std::max(0.0, x - extra);
    // (a letter inside a cluster: its cluster's span)
    std::size_t head = start;
    for (std::size_t i = start; i < end; ++i) {
        if (paragraph.starts[i]) head = i;
        else out.span[i - start] = out.span[head - start];
    }
    return out;
}

}  // namespace genko::render::text
