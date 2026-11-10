// The submission pack (Python's genko/pack.py): 1-bit TIFFs and PNGs at the print resolution, list.csv and README.txt.

#include "core/pyconv.hpp"
#include "formats/export.hpp"
#include "formats/output.hpp"

namespace genko::formats {

std::vector<fs::path> export_pack(const core::Document& episode, const fs::path& dest, std::string_view preset, std::optional<std::int64_t> dpi_given) {
    detail::make_dirs(dest);
    const std::int64_t dpi = dpi_given && *dpi_given != 0 ? *dpi_given : episode.spec.dpi.truthy() ? core::py_int(episode.spec.dpi) : 600;
    // (one export: the TIFFs, the PNGs and the two texts moved into place together)
    detail::Output output;
    std::vector<fs::path> written = detail::print_into(output, episode, dest, "tiff", dpi, 180, true);
    const std::vector<fs::path> pngs = detail::print_into(output, episode, dest, "png", dpi, 180, true);
    written.insert(written.end(), pngs.begin(), pngs.end());
    std::string lines = "page,numero,dpi,expression,spread_with,width_mm,height_mm,bleed_mm,preset";
    for (const auto& page : episode.pages) {
        lines += "\n" + page->index.repr() + "," + (page->numero ? page->index.repr() : std::string()) + "," + std::to_string(dpi) + "," +
                 page->spec.expression + "," + (page->spread_with && page->spread_with->truthy() ? page->spread_with->repr() : std::string()) +
                 "," + page->spec.width_mm.repr() + "," + page->spec.height_mm.repr() + "," + page->spec.bleed_mm.repr() + "," + std::string(preset);
    }
    const fs::path csv = detail::join(dest, "list.csv");
    output.put(csv, detail::native_text(lines + "\n"));
    const fs::path readme = detail::join(dest, "README.txt");
    output.put(readme, detail::native_text("preset=" + std::string(preset) + "\nbleed_mm=" + episode.spec.bleed_mm.repr() + "\ninner_margin_mm=" +
                                           episode.spec.inner_margin_mm.repr() + "\nDo not print a publisher logo.\n"));
    output.commit();
    written.push_back(csv);
    written.push_back(readme);
    return written;
}

}  // namespace genko::formats
