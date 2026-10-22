// TIFF and WebP pictures read as the Python baseline's Pillow reads them (M6: Image.open and load through libtiff and
// libwebp): each case is made by the reference Python's Pillow (the modes its TIFF writer writes — 1, L, LA, P, RGB, RGBA,
// CMYK, I;16, I, F with compressions, I;16B (big-endian), LAB, PA — with no compression, LZW, Deflate, PackBits,
// Group 4 and JPEG; strips and tiles; the EXIF orientation; big-endian 32-bit integers and floats and RGBA without an
// ExtraSamples tag written by hand; WebP lossless, lossy, with alpha, and an animation's first frame), and this build's render::open_image gives the same mode, size and bytes — and the
// same RGBA picture after convert, as the app takes it — without tolerance. A TIFF whose samples lie in separate
// planes is the one kind this build does not read yet: it says so (NotYetPorted "image_format") rather than drawing
// something else, and knows it from the file's directory alone (unported_image_format).

#include <QtTest>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "core/pyconv.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/tiff_webp.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace render = genko::render;

namespace {

// The cases, made and read by Pillow: name, the file's bytes, and what Pillow opened (mode, size, its bytes, RGBA).
const char* kCases = R"PY(
import base64, io, json, struct, sys
from PIL import Image, ImageDraw, TiffImagePlugin

def picture(mode, size=(37, 23)):
    im = Image.new("RGB", size, (250, 240, 230))
    d = ImageDraw.Draw(im)
    d.ellipse((3, 2, size[0] - 4, size[1] - 3), fill=(30, 120, 200), outline=(10, 10, 10))
    d.line((0, 0, size[0], size[1]), fill=(200, 30, 40), width=3)
    if mode in ("RGBA", "LA", "PA"):
        rgba = im.convert("RGBA")
        alpha = Image.linear_gradient("L").resize(size)
        rgba.putalpha(alpha)
        return rgba.convert(mode) if mode != "PA" else rgba.convert("RGBA").quantize(16).convert("PA")
    if mode == "P":
        return im.quantize(13)
    if mode == "1":
        return im.convert("L").point(lambda v: 255 if v > 128 else 0).convert("1")
    if mode == "I;16":
        return im.convert("L").convert("I").point(lambda v: v * 257).convert("I;16")
    if mode == "I;16B":
        return Image.frombytes("I;16B", size, picture("I;16", size).tobytes("raw", "I;16B"))
    if mode == "I":
        return im.convert("L").convert("I").point(lambda v: v * 70000 - 9000000)
    if mode == "F":
        return im.convert("L").convert("F").point(lambda v: v / 7.0 - 3.25)
    if mode == "LAB":
        return Image.merge("LAB", (im.convert("L"), Image.new("L", size, 120), Image.linear_gradient("L").resize(size)))
    return im.convert(mode)

cases = []
def add(name, data):
    im = Image.open(io.BytesIO(data))
    im.load()
    cases.append({"name": name, "bytes": base64.b64encode(data).decode(), "mode": im.mode, "size": list(im.size),
                  "raw": base64.b64encode(im.tobytes()).decode(),
                  "rgba": base64.b64encode(im.convert("RGBA").tobytes()).decode(),
                  "palette": base64.b64encode(bytes(im.getpalette() or [])).decode()})

def tiff(name, im, **kw):
    out = io.BytesIO()
    im.save(out, "TIFF", **kw)
    add(name, out.getvalue())

for mode in ("1", "L", "LA", "P", "RGB", "RGBA", "CMYK", "I;16"):
    for compression in (None, "tiff_lzw", "tiff_adobe_deflate", "packbits"):
        tiff(f"tiff {mode} {compression}", picture(mode), compression=compression)
for mode in ("I", "F"):
    for compression in (None, "tiff_lzw"):
        tiff(f"tiff {mode} {compression}", picture(mode), compression=compression)
for mode in ("I;16B", "LAB", "PA"):  # (Pillow cannot read back its own uncompressed YCbCr)
    tiff(f"tiff {mode} None", picture(mode))
tiff("tiff 1 group4", picture("1"), compression="group4")
tiff("tiff RGB jpeg", picture("RGB"), compression="jpeg", quality=90)
tiff("tiff L rows per strip", picture("L", (40, 33)), compression="tiff_lzw", tiffinfo={278: 4})
for orientation in range(2, 9):
    tiff(f"tiff orientation {orientation}", picture("RGB"), tiffinfo={274: orientation})

# a tiled TIFF (16 × 16 tiles over 20 × 18, uncompressed, grey), written by hand
w, h, tw, th = 20, 18, 16, 16
pixels = bytes((x * 11 + y * 7) % 256 for y in range(h) for x in range(w))
tiles = []
for ty in range(0, h, th):
    for tx in range(0, w, tw):
        tiles.append(bytes(pixels[(ty + r) * w + tx + c] if ty + r < h and tx + c < w else 0 for r in range(th) for c in range(tw)))
n = 10
ifd = 8
extra = ifd + 2 + n * 12 + 4
offsets_at = extra
counts_at = offsets_at + 4 * len(tiles)
data_at = counts_at + 4 * len(tiles)
def entry(tag, typ, count, value): return struct.pack("<HHI", tag, typ, count) + value
short = lambda v: struct.pack("<HH", v, 0)
long_ = lambda v: struct.pack("<I", v)
entries = [entry(256, 3, 1, short(w)), entry(257, 3, 1, short(h)), entry(258, 3, 1, short(8)), entry(259, 3, 1, short(1)),
           entry(262, 3, 1, short(1)), entry(277, 3, 1, short(1)), entry(322, 3, 1, short(tw)), entry(323, 3, 1, short(th)),
           entry(324, 4, len(tiles), long_(offsets_at)), entry(325, 4, len(tiles), long_(counts_at))]
blob = b"II*\x00" + struct.pack("<I", ifd) + struct.pack("<H", n) + b"".join(entries) + struct.pack("<I", 0)
blob += b"".join(struct.pack("<I", data_at + i * tw * th) for i in range(len(tiles)))
blob += b"".join(struct.pack("<I", tw * th) for _ in tiles)
assert len(blob) == data_at
add("tiff tiled", blob + b"".join(tiles))

# uncompressed, written by hand: big-endian 32-bit samples (Pillow reads them in the file's order) and RGBA without an
# ExtraSamples tag (Pillow takes the fourth sample as alpha; libtiff would call it unspecified)
def raw_tiff(name, order, w, h, photo, bits, fmt, extra, data):
    e = "<" if order == "II" else ">"
    tags = [(256, 4, [w]), (257, 4, [h]), (258, 3, bits), (259, 3, [1]), (262, 3, [photo]), (273, 4, [0]), (277, 3, [len(bits)]),
            (278, 4, [h]), (279, 4, [len(data)]), (339, 3, [fmt] * len(bits))]
    if extra is not None:
        tags.append((338, 3, extra))
    tags.sort()
    pack = lambda typ, vals: b"".join(struct.pack(e + ("H" if typ == 3 else "I"), v) for v in vals)
    at = 8 + 2 + 12 * len(tags) + 4
    blobs, where = b"", {}
    for tag, typ, vals in tags:
        if len(pack(typ, vals)) > 4:
            where[tag] = at + len(blobs)
            blobs += pack(typ, vals)
    data_at = at + len(blobs)
    entries = b""
    for tag, typ, vals in tags:
        vals = [data_at] if tag == 273 else vals
        payload = pack(typ, vals)
        entries += struct.pack(e + "HHI", tag, typ, len(vals)) + (struct.pack(e + "I", where[tag]) if tag in where else payload.ljust(4, b"\0"))
    head = (b"II*\x00" if order == "II" else b"MM\x00*") + struct.pack(e + "I", 8)
    add(name, head + struct.pack(e + "H", len(tags)) + entries + struct.pack(e + "I", 0) + blobs + data)

raw_tiff("tiff I;32BS big", "MM", 5, 3, 1, [32], 2, None, b"".join(struct.pack(">i", (i * 7919 - 50000) * 997) for i in range(15)))
raw_tiff("tiff F;32BF big", "MM", 5, 3, 1, [32], 3, None, b"".join(struct.pack(">f", i * 1.25 - 7.5) for i in range(15)))
raw_tiff("tiff RGBA no ExtraSamples", "II", 5, 3, 2, [8, 8, 8, 8], 1, None, bytes((i * 37) % 256 for i in range(60)))

def webp(name, im, **kw):
    out = io.BytesIO()
    im.save(out, "WEBP", **kw)
    add(name, out.getvalue())

webp("webp lossless RGBA", picture("RGBA"), lossless=True)
webp("webp lossless RGB", picture("RGB"), lossless=True)
webp("webp lossy RGB", picture("RGB", (64, 48)), quality=80)
webp("webp lossy RGBA", picture("RGBA", (64, 48)), quality=70)
frames = [picture("RGB"), picture("RGB").transpose(Image.Transpose.FLIP_LEFT_RIGHT)]
out = io.BytesIO()
frames[0].save(out, "WEBP", save_all=True, append_images=frames[1:], duration=100, lossless=True)
add("webp animation", out.getvalue())
json.dump(cases, sys.stdout)
)PY";

// A TIFF of 2 × 2 whose red, green and blue lie in three planes (PlanarConfiguration 2), uncompressed: Pillow opens it.
const char* kPlanar =
    "SUkqAAgAAAAKAAABAwABAAAAAgAAAAEBAwABAAAAAgAAAAIBAwADAAAAhgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAADAAAAjAAAABUBAwABAAAAAwAAABYBAwAB"
    "AAAAAgAAABcBBAADAAAAmAAAABwBAwABAAAAAgAAAAAAAAAIAAgACACkAAAAqAAAAKwAAAAEAAAABAAAAAQAAAD/AAD/AP8AAAAA/wA=";

}  // namespace

class TestContractTiffWebp : public QObject {
    Q_OBJECT

private slots:
    void readAsPillowReadsThem() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QTemporaryDir scratch;
        const genko::test::Run made = genko::test::run(genko::test::python_ref(), {QStringLiteral("-I"), QStringLiteral("-c"), QString::fromUtf8(kCases)},
                                                       genko::test::python_env(scratch.path()));
        QVERIFY2(made.finished && made.exit_code == 0, qPrintable(made.described()));
        const Json cases = Json::parse(made.out.toStdString());
        QVERIFY(cases.size() >= 40);
        int tiffs = 0;
        int webps = 0;
        for (const Json& c : cases) {
            const std::string name = c["name"].get<std::string>();
            const std::string bytes = genko::core::a2b_base64(c["bytes"].get<std::string>());
            QVERIFY2(!render::unported_image_format(bytes), name.c_str());
            const render::Size size = render::picture_size(bytes);
            QVERIFY2(size.width == c["size"][0].get<int>() && size.height == c["size"][1].get<int>(), name.c_str());
            render::Image image;
            try {
                image = render::open_image(bytes, render::kPillowOpenLimits);
            } catch (const std::exception& e) {
                QFAIL((name + ": " + e.what()).c_str());
            }
            QVERIFY2(std::string(image.mode()) == c["mode"].get<std::string>(), (name + ": " + std::string(image.mode())).c_str());
            QVERIFY2(image.width() == size.width && image.height() == size.height, name.c_str());
            QVERIFY2(image.tobytes() == genko::core::a2b_base64(c["raw"].get<std::string>()), (name + ": the bytes").c_str());
            QVERIFY2(image.convert("RGBA").tobytes() == genko::core::a2b_base64(c["rgba"].get<std::string>()), (name + ": RGBA").c_str());
            if (std::string(image.mode()) == "P" || std::string(image.mode()) == "PA") {
                std::string palette;
                for (const auto& [r, g, b] : image.palette()) palette += {static_cast<char>(r), static_cast<char>(g), static_cast<char>(b)};
                const std::string theirs = genko::core::a2b_base64(c["palette"].get<std::string>());
                QVERIFY2(!palette.empty() || theirs.empty(), (name + ": no palette").c_str());
                QVERIFY2(theirs.starts_with(palette) || palette.starts_with(theirs), (name + ": the palette").c_str());
            }
            (name.starts_with("tiff") ? tiffs : webps) += 1;
        }
        QCOMPARE(tiffs, 8 * 4 + 2 * 2 + 3 + 1 + 1 + 1 + 7 + 1 + 3);
        QCOMPARE(webps, 5);
    }

    void aTiffInPlanesIsReadAndAJpeg2000IsSaidNotRead() {
        // the samples in three planes (PlanarConfiguration 2): read as Pillow reads them (M7), the pixels Pillow 12.3.0's
        const std::string planar = genko::core::a2b_base64(kPlanar);
        QVERIFY(render::tiff_signature(planar) && !render::unported_image_format(planar));
        const render::Image read = render::open_image(planar, render::kPillowOpenLimits);
        QCOMPARE(std::string(read.mode()), std::string("RGB"));
        QCOMPARE(render::picture_size(planar).width, 2);
        QCOMPARE(read.tobytes(), std::string("\xff\0\0\0\xff\0\0\0\xff\xff\0\0", 12));
        // a format Pillow opens that this build does not read (JPEG 2000): said so by name, not "no picture" — a PPM is read
        // since PC4 F-a (the codecs: red, green, blue, white)
        const std::string jp2 = genko::core::a2b_base64("AAAADGpQICANCocKAAAAFGZ0eXBqcDIgAAAAAGpwMiA=");
        QVERIFY(render::unported_image_format(jp2));
        bool said = false;
        try {
            (void)render::open_image(jp2, render::kPillowOpenLimits);
        } catch (const render::NotYetPorted& e) {
            said = e.element() == "image_format";
        }
        QVERIFY(said);
        const std::string ppm = genko::core::a2b_base64("UDYKMiAyCjI1NQr/AAAA/wAAAP////8=");
        QVERIFY(!render::unported_image_format(ppm));
        const render::Image ppm_read = render::open_image(ppm, render::kPillowOpenLimits);
        QCOMPARE(std::string(ppm_read.mode()), std::string("RGB"));
        QCOMPARE(ppm_read.tobytes(), std::string("\xff\0\0\0\xff\0\0\0\xff\xff\xff\xff", 12));
        QCOMPARE(render::picture_size(ppm).width, 2);
        // bytes that only begin as a TIFF or a WebP are no picture (as Pillow cannot identify them), not "not yet read"
        for (const std::string& broken : {std::string("II*\0", 4) + std::string(60, '\x01'), std::string("RIFF\x10\0\0\0WEBPVP8 ", 16) + std::string(8, '\0'),
                                          std::string("RIFF\x10\0\0\0WEBPABCD", 16)}) {
            QVERIFY(!render::unported_image_format(broken));
            bool unidentified = false;
            try {
                (void)render::open_image(broken, render::kPillowOpenLimits);
            } catch (const genko::core::Error& e) {
                unidentified = e.code() == "unidentified_image" || e.code() == "format";
            }
            QVERIFY(unidentified);
        }
    }
};

QTEST_GUILESS_MAIN(TestContractTiffWebp)
#include "test_contract_tiff_webp.moc"
