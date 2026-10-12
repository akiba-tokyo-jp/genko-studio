/*
 * Genko: what render::Image needs to run libImaging's row-by-row passes on bands of rows at once (several threads, each
 * writing rows of its own): the same pixels as the whole passes (see row_bands.c and GENKO_CHANGES.md).
 */
#ifndef GENKO_PILLOW_ROW_BANDS_H
#define GENKO_PILLOW_ROW_BANDS_H

#ifdef __cplusplus
extern "C" {
#endif

struct ImagingMemoryInstance;

/* A view of the columns [x0, x1) and rows [y0, y1) of `base`: the same pixels (no copy), its own row table only. For
 * modes without a palette. NULL (and the libImaging error) when it cannot be made. Free it with genko_imaging_view_free
 * (never ImagingDelete); `base` must outlive it. */
struct ImagingMemoryInstance *
genko_imaging_view(struct ImagingMemoryInstance *base, int x0, int y0, int x1, int y1);
void
genko_imaging_view_free(struct ImagingMemoryInstance *view);

/* BoxBlur.c: one horizontal box blur pass over every row of imIn into imOut (imIn may be imOut). Defined there without
 * `static`, not declared in Imaging.h. */
struct ImagingMemoryInstance *
ImagingHorizontalBoxBlur(
    struct ImagingMemoryInstance *imOut, struct ImagingMemoryInstance *imIn, float floatRadius
);

/* BoxBlur.c's static _gaussian_blur_radius(radius, passes): the box radius ImagingGaussianBlur gives ImagingBoxBlur. The
 * same lines. */
float
genko_gaussian_blur_radius(float radius, int passes);

/* UnsharpMask.c's second step over the rows [y0, y1): imOut holds the blurred imIn and is made sharp in place. The same
 * lines. */
void
genko_unsharp_rows(
    struct ImagingMemoryInstance *imOut,
    struct ImagingMemoryInstance *imIn,
    int percent,
    int threshold,
    int y0,
    int y1
);

#ifdef __cplusplus
}
#endif

#endif /* GENKO_PILLOW_ROW_BANDS_H */
