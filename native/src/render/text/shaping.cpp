// PC5 G-0 (type@1, render/text/shaping.hpp): the OpenType typesetting's way in, through HarfBuzz.

#include "render/text/shaping.hpp"

#include <hb.h>

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
    hb_buffer_add_utf8(buffer, request.text.data(), static_cast<int>(request.text.size()), 0, -1);
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    if (request.direction == "ltr") hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    else if (request.direction == "rtl") hb_buffer_set_direction(buffer, HB_DIRECTION_RTL);
    else if (request.direction == "ttb") hb_buffer_set_direction(buffer, HB_DIRECTION_TTB);
    if (!request.language.empty()) hb_buffer_set_language(buffer, hb_language_from_string(request.language.c_str(), -1));
    hb_buffer_guess_segment_properties(buffer);
    std::vector<hb_feature_t> features;
    for (const auto& [tag, value] : request.features) features.push_back(hb_feature_t{tag_of(tag), value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
    hb_shape(font, buffer, features.data(), static_cast<unsigned>(features.size()));
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
    // (clusters are byte offsets of the UTF-8 text: told as code points)
    std::vector<std::uint32_t> code_point_at(request.text.size() + 1, 0);
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < request.text.size(); ++i) {
        const bool lead = (static_cast<unsigned char>(request.text[i]) & 0xC0) != 0x80;
        code_point_at[i] = lead ? n++ : (n > 0 ? n - 1 : 0);
    }
    code_point_at[request.text.size()] = n;
    std::vector<ShapedGlyph> out;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        ShapedGlyph g;
        g.glyph = info[i].codepoint;
        g.cluster = code_point_at[std::min<std::size_t>(info[i].cluster, request.text.size())];
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

}  // namespace genko::render::text
