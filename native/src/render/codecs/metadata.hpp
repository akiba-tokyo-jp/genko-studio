#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"

// What a file says besides its pixels that opening it as a book keeps (PC4 F-b, SPEC.md FILE-01; formats/open_file): its
// EXIF block and XMP packet as they are, and whether it holds more than one frame or page. Read from the containers
// themselves — PNG's eXIf and iTXt "XML:com.adobe.xmp" chunks (and acTL), JPEG's APP1 "Exif" and XMP segments, TIFF's
// XMP tag (700) and its IFD chain, WebP's EXIF, XMP and ANMF chunks, GIF's image descriptors —, never by decoding the
// pixels. Bytes that are cut short or broken give what was read before them (never an error: the pixels' reader says
// whether the file opens). The codecs' decode (PC4 F-a) fills its picture's EXIF and XMP from these, and its writers keep
// them where the format holds them.

namespace genko::render::codecs {

struct FileMetadata {
    std::optional<std::string> exif;  // TIFF structure (II*\0 / MM\0*), without JPEG's "Exif\0\0"
    std::optional<std::string> xmp;   // the packet, UTF-8
    int frames = 1;                   // the frames of an animation, the pages of a TIFF (1: one)
    bool palette = false;             // its pixels are numbers of a colour table (a PNG or TIFF of a palette, a GIF, a BMP of 8 bits or less)
    bool one_bit = false;             // its pixels are one bit of grey (black or white)
};

FileMetadata read_metadata(Format format, std::string_view bytes);

// An EXIF block's orientation (0x0112 in its first IFD): 1..8, 1 when it says none or cannot be read.
int exif_orientation(std::string_view exif);
// The EXIF block with its orientation set to 1 (upright: the pixels were turned as it said); as it was when it has none.
std::string exif_upright(std::string exif);
// PC4 F-c (SPEC.md FILE-01「書き出し」, the review's P2-3): the EXIF block a copy of the page carries — its IFD1 (the
// camera's thumbnail: the picture before it was edited or cut) and any IFD after it gone, their entries, values and the
// thumbnail's bytes cleared and cut off the end when nothing else lies past them; the sizes it says (IFD0's ImageWidth and
// ImageLength, the Exif IFD's PixelXDimension and PixelYDimension) the written picture's. Everything else — the camera,
// its MakerNote, GPS — as it was. As it was when it is not a TIFF structure.
std::string exif_for_copy(std::string exif, std::uint32_t width, std::uint32_t height);
// The colour space its Exif IFD says (0xA001): 1 sRGB, 0xFFFF uncalibrated, 0 when it says none.
int exif_colour_space(std::string_view exif);
// An XMP packet with its tiff:Orientation (element or attribute) set to 1.
std::string xmp_upright(std::string xmp);

// The picture turned as an EXIF orientation says (2..8: mirrored, turned 180°, flipped, transposed, turned 90° right,
// transversed, turned 90° left — as Pillow's ImageOps.exif_transpose turns it); its profile and metadata kept, its
// resolution's two axes swapped for 5..8. 1 and anything else: as it is.
CodecImage oriented(const CodecImage& picture, int orientation);

}  // namespace genko::render::codecs
