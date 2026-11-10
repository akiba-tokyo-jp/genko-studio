/*
 * Genko: the columns [x0, x1) and rows [y0, y1) of ImagingResample(imIn, xsize, ysize, filter, box), the same pixels
 * as cropping the whole result (see resample_region.c): a new image, or NULL with the libImaging error set.
 */
#ifndef GENKO_PILLOW_RESAMPLE_REGION_H
#define GENKO_PILLOW_RESAMPLE_REGION_H

#ifdef __cplusplus
extern "C" {
#endif

struct ImagingMemoryInstance;

struct ImagingMemoryInstance *
genko_ImagingResampleRegion(
    struct ImagingMemoryInstance *imIn,
    int xsize,
    int ysize,
    int filter,
    float box[4],
    int x0,
    int y0,
    int x1,
    int y1
);

/* The same pixels written into `into` (an image of the region's size, x1 - x0 by y1 - y0, and imIn's mode — a view of
 * rows of a larger image, row_bands.h, writes them in place): `into` back, or NULL with the libImaging error set (what
 * it holds then is not to be used). */
struct ImagingMemoryInstance *
genko_ImagingResampleRegionInto(
    struct ImagingMemoryInstance *into,
    struct ImagingMemoryInstance *imIn,
    int xsize,
    int ysize,
    int filter,
    float box[4],
    int x0,
    int y0,
    int x1,
    int y1
);

#ifdef __cplusplus
}
#endif

#endif /* GENKO_PILLOW_RESAMPLE_REGION_H */
