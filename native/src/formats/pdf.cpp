#include "formats/pdf.hpp"

#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "formats/output.hpp"

namespace genko::formats {

namespace {

// f"{v:.3f}"
std::string f3(double v) {
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 3);
    return std::string(buf, result.ptr);
}

// zlib.compress(data, 6), the data given in parts (the same stream as one call makes).
class Deflater {
public:
    Deflater() {
        if (deflateInit(&z_, 6) != Z_OK) throw core::Error("memory", "zlib cannot compress");
    }
    ~Deflater() { deflateEnd(&z_); }
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;

    void add(std::string_view data, bool last) {
        z_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        z_.avail_in = static_cast<uInt>(data.size());
        char buf[1 << 16];
        for (;;) {
            z_.next_out = reinterpret_cast<Bytef*>(buf);
            z_.avail_out = sizeof buf;
            const int result = deflate(&z_, last ? Z_FINISH : Z_NO_FLUSH);
            if (result == Z_STREAM_ERROR) throw core::Error("memory", "zlib cannot compress");
            out_.append(buf, sizeof buf - z_.avail_out);
            if (last ? result == Z_STREAM_END : (z_.avail_in == 0 && z_.avail_out != 0)) break;
        }
    }
    std::string take() { return std::move(out_); }

private:
    z_stream z_{};
    std::string out_;
};

std::string stream(const std::string& head, std::string_view data) {
    std::string out = "<< " + head + " /Length " + std::to_string(data.size()) + " >>\nstream\n";
    out += data;
    out += "\nendstream";
    return out;
}

// The picture's bytes (tobytes()) deflated, a band of rows at a time.
std::string deflated_pixels(const render::Image& picture) {
    Deflater deflater;
    const int rows = picture.height();
    const int band = std::max(1, (1 << 22) / std::max(1, picture.width() * std::max(1, picture.bands())));
    for (int top = 0; top < rows; top += band) {
        const int bottom = std::min(rows, top + band);
        const std::string raw = (top == 0 && bottom == rows) ? picture.tobytes()
                                                             : picture.crop(render::Box{0, top, picture.width(), bottom}).tobytes();
        deflater.add(raw, bottom == rows);
    }
    if (rows == 0) deflater.add({}, true);
    return deflater.take();
}

}  // namespace

PdfBoxes pdf_boxes(const core::Page& page, std::string_view area) {
    using Rect4 = std::array<double, 4>;
    const Rect4 paper{0.0, 0.0, page.spec.width_mm.value(), page.spec.height_mm.value()};
    const core::Rect b = page.bleed_rect_mm();
    const core::Rect t = page.trim_rect_mm();
    const Rect4 bleed{b.x.value(), b.y.value(), b.width.value(), b.height.value()};
    const Rect4 trim{t.x.value(), t.y.value(), t.width.value(), t.height.value()};
    const Rect4& m = area == "bleed" ? bleed : area == "trim" ? trim : paper;
    const double mx = m[0], my = m[1], mw = m[2], mh = m[3];
    const double pt = 72 / 25.4;
    const auto box = [&](const Rect4& r) {
        const double x = r[0], y = r[1], w = r[2], h = r[3];
        const double x0 = std::max(x, mx) - mx;
        const double x1 = std::min(x + w, mx + mw) - mx;
        const double top = std::max(y, my) - my;
        const double bottom = std::min(y + h, my + mh) - my;
        return std::array<double, 4>{core::py_round(x0 * pt, 3), core::py_round((mh - bottom) * pt, 3), core::py_round(x1 * pt, 3),
                                     core::py_round((mh - top) * pt, 3)};
    };
    return PdfBoxes{box(m), box(bleed), box(trim)};
}

std::filesystem::path write_pdf(const std::filesystem::path& path, const std::vector<PdfPage>& pages,
                                const std::function<render::Image(std::size_t)>& picture) {
    // The object numbers first (catalog 1, pages 2, then for each page its profile when new, image, content and page),
    // so the catalog and the page tree are written before the pictures are made.
    struct Numbers {
        int profile = 0;
        bool new_profile = false;
        int image = 0;
        int content = 0;
        int page = 0;
    };
    std::vector<Numbers> numbers(pages.size());
    std::map<std::string_view, int> profile_ids;
    int count = 2;
    std::string kids;
    for (std::size_t n = 0; n < pages.size(); ++n) {
        const PdfPage& page = pages[n];
        if (page.mode != "1" && page.mode != "L" && page.mode != "RGB" && page.mode != "CMYK") {
            throw core::Error("key", "'" + page.mode + "'");
        }
        if (!page.profile.empty() && (page.mode == "RGB" || page.mode == "CMYK")) {
            const auto found = profile_ids.find(page.profile);
            if (found == profile_ids.end()) {
                profile_ids[page.profile] = ++count;
                numbers[n].new_profile = true;
            }
            numbers[n].profile = profile_ids[page.profile];
        }
        numbers[n].image = ++count;
        numbers[n].content = ++count;
        numbers[n].page = ++count;
        kids += (kids.empty() ? "" : " ") + std::to_string(numbers[n].page) + " 0 R";
    }

    detail::PartFile file(path);
    std::vector<std::uint64_t> offsets;
    const auto object = [&](std::string_view body) {
        offsets.push_back(file.size());
        file.write(std::to_string(offsets.size()) + " 0 obj\n");
        file.write(body);
        file.write("\nendobj\n");
    };
    file.write(std::string_view("%PDF-1.6\n%\xe2\xe3\xcf\xd3\n", 15));
    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>");
    for (std::size_t n = 0; n < pages.size(); ++n) {
        const PdfPage& page = pages[n];
        const render::Image made = picture(n);
        if (made.mode() != page.mode) throw core::Error("value", "the picture is not in the page's colour");
        std::string space;
        int bits = 8;
        int comps = 1;
        if (page.mode == "1") {
            space = "/DeviceGray";
            bits = 1;
        } else if (page.mode == "L") {
            space = "/DeviceGray";
        } else if (page.mode == "RGB") {
            space = "/DeviceRGB";
            comps = 3;
        } else {
            space = "/DeviceCMYK";
            comps = 4;
        }
        if (numbers[n].profile != 0) {
            if (numbers[n].new_profile) {
                Deflater deflater;
                deflater.add(page.profile, true);
                object(stream("/N " + std::to_string(comps) + " /Filter /FlateDecode", deflater.take()));
            }
            space = "[/ICCBased " + std::to_string(numbers[n].profile) + " 0 R]";
        }
        object(stream("/Type /XObject /Subtype /Image /Width " + std::to_string(made.width()) + " /Height " + std::to_string(made.height()) +
                          " /ColorSpace " + space + " /BitsPerComponent " + std::to_string(bits) + " /Filter /FlateDecode",
                      deflated_pixels(made)));
        const auto& media = page.boxes.media;
        object(stream("", "q " + f3(media[2] - media[0]) + " 0 0 " + f3(media[3] - media[1]) + " 0 0 cm /Im0 Do Q"));
        std::string boxes;
        for (const auto& [key, value] : {std::pair{"MediaBox", &page.boxes.media}, std::pair{"BleedBox", &page.boxes.bleed},
                                         std::pair{"TrimBox", &page.boxes.trim}}) {
            boxes += (boxes.empty() ? "/" : " /") + std::string(key) + " [" + f3((*value)[0]) + " " + f3((*value)[1]) + " " + f3((*value)[2]) +
                     " " + f3((*value)[3]) + "]";
        }
        object("<< /Type /Page /Parent 2 0 R " + boxes + " /Resources << /XObject << /Im0 " + std::to_string(numbers[n].image) +
               " 0 R >> >> /Contents " + std::to_string(numbers[n].content) + " 0 R >>");
    }
    const std::uint64_t xref = file.size();
    std::string tail = "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::uint64_t offset : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010llu 00000 n \n", static_cast<unsigned long long>(offset));
        tail += line;
    }
    tail += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    file.write(tail);
    file.commit();
    return path;
}

}  // namespace genko::formats
