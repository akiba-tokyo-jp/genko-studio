// PC5 G-0 (type@1, render/text/shaping.hpp): the OpenType typesetting's way in, through HarfBuzz.

#include "render/text/shaping.hpp"

#include <hb.h>
#include <hb-ot.h>

#include <QByteArray>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include "core/error.hpp"
#include "render/draw.hpp"

namespace genko::render::text {

namespace {

// A font's HarfBuzz face, kept with the bytes it reads (they never change).
struct Face {
    std::shared_ptr<const QByteArray> bytes;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    ~Face() {
        if (face != nullptr) hb_face_destroy(face);
        if (blob != nullptr) hb_blob_destroy(blob);
    }
};

std::shared_ptr<const Face> face_of(const std::string& font, unsigned index) {
    static std::mutex mutex;
    static std::map<std::pair<std::string, unsigned>, std::shared_ptr<const Face>> cache;
    const auto key = std::make_pair(font, index);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
    }
    auto made = std::make_shared<Face>();
    made->bytes = font_bytes(font);  // (read outside the lock)
    const std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() >= 16) cache.clear();  // (a few faces kept: those in use stay held by their callers)
    made->blob = hb_blob_create(made->bytes->constData(), static_cast<unsigned>(made->bytes->size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    made->face = hb_face_create(made->blob, index);
    cache.emplace(key, made);
    return made;
}

hb_tag_t tag_of(const std::string& tag) {
    if (tag.size() != 4) throw core::Error("value", "an OpenType tag is 4 characters: " + tag);
    return HB_TAG(tag[0], tag[1], tag[2], tag[3]);
}

}  // namespace

std::vector<ShapedGlyph> shape(const ShapeRequest& request) {
    if (!(request.size_px > 0)) throw core::Error("value", "the size of a run to set must be above 0");
    if (!(request.size_px <= 100000)) throw core::Error("value", "the size of a run to set must be at most 100000 px");
    const std::shared_ptr<const Face> face = face_of(request.font, request.face_index);
    hb_font_t* font = hb_font_create(face->face);
    const int scale = static_cast<int>(request.size_px * 64.0 + 0.5);  // (26.6 fixed point, as FreeType's)
    hb_font_set_scale(font, scale, scale);
    std::vector<hb_variation_t> axes;
    for (const auto& [tag, value] : request.variations) axes.push_back(hb_variation_t{tag_of(tag), value});
    if (!axes.empty()) hb_font_set_variations(font, axes.data(), static_cast<unsigned>(axes.size()));
    hb_buffer_t* buffer = hb_buffer_create();
    const bool in_context = !request.context.empty();
    const std::size_t item_start = in_context ? std::min(request.context_start, request.context.size()) : 0;
    if (in_context) {
        const std::size_t length = std::min(request.context_length, request.context.size() - item_start);
        hb_buffer_add_utf32(buffer, reinterpret_cast<const std::uint32_t*>(request.context.data()), static_cast<int>(request.context.size()),
                            static_cast<unsigned>(item_start), static_cast<int>(length));
    } else {
        hb_buffer_add_utf8(buffer, request.text.data(), static_cast<int>(request.text.size()), 0, -1);
    }
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    if (request.direction == "ltr") hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    else if (request.direction == "rtl") hb_buffer_set_direction(buffer, HB_DIRECTION_RTL);
    else if (request.direction == "ttb") hb_buffer_set_direction(buffer, HB_DIRECTION_TTB);
    if (!request.language.empty()) hb_buffer_set_language(buffer, hb_language_from_string(request.language.c_str(), -1));
    if (request.script.size() == 4 && request.script != "Zyyy" && request.script != "Zinh" && request.script != "Zzzz") {
        hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(HB_TAG(request.script[0], request.script[1], request.script[2], request.script[3])));
    }
    hb_buffer_guess_segment_properties(buffer);
    std::vector<hb_feature_t> features;
    for (const auto& [tag, value] : request.features) features.push_back(hb_feature_t{tag_of(tag), value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
    hb_shape(font, buffer, features.data(), static_cast<unsigned>(features.size()));
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
    // (clusters are byte offsets of the UTF-8 text: told as code points; of a context, its code points: from the run's first)
    std::vector<std::uint32_t> code_point_at(in_context ? 0 : request.text.size() + 1, 0);
    std::uint32_t n = 0;
    for (std::size_t i = 0; !in_context && i < request.text.size(); ++i) {
        const bool lead = (static_cast<unsigned char>(request.text[i]) & 0xC0) != 0x80;
        code_point_at[i] = lead ? n++ : (n > 0 ? n - 1 : 0);
    }
    if (!in_context) code_point_at[request.text.size()] = n;
    std::vector<ShapedGlyph> out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        ShapedGlyph g;
        g.glyph = info[i].codepoint;
        g.cluster = in_context ? static_cast<std::uint32_t>(info[i].cluster - std::min<std::size_t>(info[i].cluster, item_start))
                               : code_point_at[std::min<std::size_t>(info[i].cluster, request.text.size())];
        g.x_advance = pos[i].x_advance / 64.0;
        g.y_advance = pos[i].y_advance / 64.0;
        g.x_offset = pos[i].x_offset / 64.0;
        g.y_offset = pos[i].y_offset / 64.0;
        out.push_back(g);
    }
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return out;
}

std::string harfbuzz_version() { return hb_version_string(); }

namespace {

// A name of the font (its English one where it has one), "" for none.
std::string name_of(hb_face_t* face, hb_ot_name_id_t id) {
    for (const hb_language_t language : {hb_language_from_string("en", -1), HB_LANGUAGE_INVALID}) {
        const unsigned length = hb_ot_name_get_utf8(face, id, language, nullptr, nullptr);
        if (length == 0 || length > 4096) continue;
        std::string out(length + 1, '\0');
        unsigned size = length + 1;
        hb_ot_name_get_utf8(face, id, language, &size, out.data());
        out.resize(size);
        if (!out.empty()) return out;
    }
    return {};
}

std::string tag_name(hb_tag_t tag) {
    char text[5] = {};
    hb_tag_to_string(tag, text);
    return std::string(text, 4);
}

}  // namespace

FontInfo font_info(const std::string& font, unsigned face_index) {
    const std::shared_ptr<const Face> held = face_of(font, face_index);
    hb_face_t* face = held->face;
    FontInfo out;
    out.family = name_of(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
    if (out.family.empty()) out.family = name_of(face, HB_OT_NAME_ID_FONT_FAMILY);
    out.style = name_of(face, HB_OT_NAME_ID_TYPOGRAPHIC_SUBFAMILY);
    if (out.style.empty()) out.style = name_of(face, HB_OT_NAME_ID_FONT_SUBFAMILY);
    out.postscript = name_of(face, HB_OT_NAME_ID_POSTSCRIPT_NAME);
    out.faces = std::max(1U, hb_face_count(held->blob));
    const unsigned axes = hb_ot_var_get_axis_count(face);
    std::vector<hb_ot_var_axis_info_t> infos(axes);
    unsigned got = axes;
    if (axes > 0) hb_ot_var_get_axis_infos(face, 0, &got, infos.data());
    for (unsigned i = 0; i < got; ++i) {
        FontAxis axis;
        axis.tag = tag_name(infos[i].tag);
        axis.name = name_of(face, infos[i].name_id);
        axis.min = infos[i].min_value;
        axis.value = infos[i].default_value;
        axis.max = infos[i].max_value;
        out.axes.push_back(std::move(axis));
    }
    for (const hb_tag_t table : {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS}) {
        const unsigned count = hb_ot_layout_table_get_feature_tags(face, table, 0, nullptr, nullptr);
        std::vector<hb_tag_t> tags(count);
        unsigned n = count;
        if (count > 0) hb_ot_layout_table_get_feature_tags(face, table, 0, &n, tags.data());
        for (unsigned i = 0; i < n; ++i) out.features.push_back(tag_name(tags[i]));
    }
    std::sort(out.features.begin(), out.features.end());
    out.features.erase(std::unique(out.features.begin(), out.features.end()), out.features.end());
    return out;
}

}  // namespace genko::render::text
