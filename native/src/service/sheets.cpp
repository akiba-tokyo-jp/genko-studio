#include "service/sheets.hpp"

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/png.hpp"
#include "service/preview.hpp"
#include "service/service_util.hpp"
#include "storage/asset_store.hpp"

namespace genko::service {

render::Image face_crop(const render::Image& image, const Json& box01) {
    const double w = image.width();
    const double h = image.height();
    double x = 0;
    double y = 0;
    double bw = 0;
    double bh = 0;
    if (core::py_truthy(box01)) {
        const std::vector<double> v = core::unpack_floats(box01, 4);
        x = v[0];
        y = v[1];
        bw = v[2];
        bh = v[3];
    } else {
        const double side = core::py_min(0.4 * w, 0.9 * h);
        x = 0.5 - side / w / 2;
        y = 0.03;
        bw = side / w;
        bh = side / h;
    }
    const auto r = [](double v) { return static_cast<int>(core::py_round_int(v)); };
    return image.crop(render::Box{r(x * w), r(y * h), r((x + bw) * w), r((y + bh) * h)});
}

render::Image open_picture(const std::string& data) {
    try {
        return render::open_image(data, render::kPillowOpenLimits);
    } catch (const core::Error& e) {
        if (e.code() == "image_too_large") throw core::PyUncaught("PIL.Image.DecompressionBombError", e.what());
        if (e.code() == "unidentified_image") throw core::PyUncaught("PIL.UnidentifiedImageError", e.what());
        throw core::PyUncaught("OSError", e.what());
    }
}

Json sheet_approval_op(const std::filesystem::path& project, const core::Document& doc, const Json& character_id,
                       const Json& candidate_id, const Json& face_box01) {
    const Json all = util::or_dict(core::py_get(doc.studio, "character_candidates"));
    core::require_hashable(character_id);
    Json cand;
    const auto it = character_id.is_string() ? all.find(character_id.get<std::string>()) : all.end();
    if (it != all.end()) {
        for (const Json& c : core::iterate(*it)) {
            if (core::py_equals(core::py_get(c, "id"), candidate_id)) {
                cand = c;
                break;
            }
        }
    }
    Json op = Json::object({{"op", "approve"}, {"gate", "sheet"}, {"character_id", character_id}, {"candidate_id", candidate_id}});
    storage::AssetStore store(project);
    std::optional<std::string> data;
    if (core::py_truthy(cand)) data = store.get_bytes(core::py_str(util::item(cand, "asset")), ".png");
    if (data) {
        const render::Image image = open_picture(*data);
        const Json box = core::py_truthy(face_box01) ? face_box01 : core::py_get(cand, "face_box01");
        op["face_asset"] = store.put_bytes(png(face_crop(image, box)), ".png");
    }
    return op;
}

}  // namespace genko::service
