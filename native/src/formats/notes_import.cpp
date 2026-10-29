// PC5 J-c (formats/notes_import.hpp): ファイル → 読み込み → 注釈… — a file's notes onto a page.

#include "formats/notes_import.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "render/pcraft_annotations.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/pcraft_read.hpp"
#include "render/psd.hpp"
#include "render/psd_annotations.hpp"

namespace genko::formats {

using core::Json;

std::vector<core::Annotation> read_annotations(std::string_view bytes) {
    std::vector<core::Annotation> out;
    if (bytes.substr(0, 4) == "8BPS") {
        const render::psd::File file = [&] {
            try {
                return render::psd::File::read(std::string(bytes), render::psd::default_limits(), true);
            } catch (const core::Error& error) {
                throw core::Error("format", std::string("the PSD cannot be read (") + error.what() + ")");
            }
        }();
        const double mm = 25.4 / (file.dpi() > 0 ? file.dpi() : 72.0);
        for (const render::psd::File::TaggedBlock& block : file.global_blocks()) {
            if (block.key != render::psd_annotations::kKey) continue;
            const auto notes = render::psd_annotations::parse(block.data);
            if (!notes) throw core::Error("format", "the PSD's notes cannot be read");
            out = render::psd_annotations::notes_of(*notes, {mm, 0, 0, mm, 0, 0}, {});
            break;
        }
    } else if (render::pcraft::is_pcraft(bytes)) {
        Json manifest;
        try {
            manifest = render::pcraft::read_manifest(bytes);
        } catch (const render::pcraft::Unreadable& error) {
            throw core::Error("format", std::string("the .pcraft is broken (") + error.what() + ")");
        }
        const Json* d = render::pcraft::optional_field(manifest, "document");
        if (d == nullptr || !d->is_object()) throw core::Error("format", "the .pcraft is broken (no document)");
        double dpi = 72;
        if (const Json* r = render::pcraft::optional_field(*d, "resolution_dpi"); r != nullptr && r->is_number() && std::isfinite(r->get<double>()))
            dpi = std::clamp<double>(static_cast<double>(std::llround(r->get<double>() > 0 ? r->get<double>() : 72.0)), 72, 100000);
        if (const Json* notes = render::pcraft::optional_field(*d, "notes")) out = render::pcraft_annotations::notes_of(*notes, 25.4 / dpi);
    } else {
        throw core::Error("format", "notes are read from a PSD, a PSB or a .pcraft");
    }
    if (out.empty()) throw core::Error("value", "the file has no notes");
    return out;
}

Json annotation_ops(const std::vector<core::Annotation>& notes, const core::Page& page) {
    if (page.annotations.size() + notes.size() > core::kAnnotationsMax) throw core::Error("value", "a page holds at most 1000 notes");
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    Json ops = Json::array();
    for (const core::Annotation& a : notes) {
        // (the mark inside the paper — its icon some 4 × 5 mm —, its box along)
        const double x = std::clamp(a.at_mm[0], 0.0, std::max(0.0, w - 4.0)), y = std::clamp(a.at_mm[1], 0.0, std::max(0.0, h - 5.0));
        Json op{{"op", "add_annotation"}, {"page", page.index.json()}, {"at_mm", Json::array({core::py_round(x, 4), core::py_round(y, 4)})},
                {"text", a.text}, {"author", a.author}, {"color", Json::array({a.color[0], a.color[1], a.color[2]})}, {"open", a.open},
                {"modified", a.modified}};
        if (a.popup_mm) {
            const auto& b = *a.popup_mm;
            op["popup_mm"] = Json::array({core::py_round(b[0] + x - a.at_mm[0], 4), core::py_round(b[1] + y - a.at_mm[1], 4), b[2], b[3]});
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

}  // namespace genko::formats
