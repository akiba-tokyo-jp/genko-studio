/*
 * Genko: the columns [x0, x1) and rows [y0, y1) of ImagingResample(imIn, xsize, ysize, filter, box), the same pixels
 * as cropping the whole result (see resample_region.c). Returns NULL and sets the libImaging error on failure.
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

#ifdef __cplusplus
}
#endif

#endif /* GENKO_PILLOW_RESAMPLE_REGION_H */
