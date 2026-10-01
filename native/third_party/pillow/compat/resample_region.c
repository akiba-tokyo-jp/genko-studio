/*
 * Genko: a part of a resampled image, computed alone.
 *
 * libImaging/Resample.c is included here unchanged (and built only through this file), so that its own static
 * functions are used: each output pixel of genko_ImagingResampleRegion is computed by the same coefficients, in the
 * same order and with the same rounding as ImagingResample computes it for the whole picture. Only the output
 * columns [x0, x1) and rows [y0, y1) are made (a render region: docs/cpp-migration/ARCHITECTURE.md §4a).
 */
#include "../libImaging/Resample.c"

#include "resample_region.h"

/* An image sharing the rows of `im` from column x0, `width` wide (not owning any pixels). */
static Imaging
column_view(Imaging im, int x0, int width) {
    int y;
    Imaging view = ImagingNewPrologue(im->mode, width, im->ysize);
    if (!view) {
        return NULL;
    }
    for (y = 0; y < im->ysize; y++) {
        view->image[y] = im->image[y] + (size_t)x0 * (size_t)im->pixelsize;
    }
    return view;
}

Imaging
genko_ImagingResampleRegion(
    Imaging imIn, int xsize, int ysize, int filter, float box[4], int x0, int y0, int x1, int y1
) {
    struct filter *filterp;
    ResampleFunction ResampleHorizontal;
    ResampleFunction ResampleVertical;
    Imaging imTemp = NULL;
    Imaging imOut = NULL;
    Imaging view = NULL;
    int need_horizontal, need_vertical;
    int ksize_horiz, ksize_vert;
    int *bounds_horiz, *bounds_vert;
    double *kk_horiz, *kk_vert;
    int first, last, i;

    if (x0 < 0 || y0 < 0 || x1 > xsize || y1 > ysize || x1 <= x0 || y1 <= y0) {
        return (Imaging)ImagingError_ValueError("bad region");
    }

    /* as ImagingResample */
    if (imIn->mode == IMAGING_MODE_P || imIn->mode == IMAGING_MODE_1) {
        return (Imaging)ImagingError_ModeError();
    }
    if (imIn->type == IMAGING_TYPE_SPECIAL) {
        if (isModeI16(imIn->mode)) {
            ResampleHorizontal = _ImagingResampleHorizontal_16bpc;
            ResampleVertical = _ImagingResampleVertical_16bpc;
        } else {
            return (Imaging)ImagingError_ModeError();
        }
    } else if (imIn->image8) {
        ResampleHorizontal = _ImagingResampleHorizontal_8bpc;
        ResampleVertical = _ImagingResampleVertical_8bpc;
    } else {
        switch (imIn->type) {
            case IMAGING_TYPE_UINT8:
                ResampleHorizontal = _ImagingResampleHorizontal_8bpc;
                ResampleVertical = _ImagingResampleVertical_8bpc;
                break;
            case IMAGING_TYPE_INT32:
            case IMAGING_TYPE_FLOAT32:
                ResampleHorizontal = _ImagingResampleHorizontal_32bpc;
                ResampleVertical = _ImagingResampleVertical_32bpc;
                break;
            default:
                return (Imaging)ImagingError_ModeError();
        }
    }
    switch (filter) {
        case IMAGING_TRANSFORM_BOX:
            filterp = &BOX;
            break;
        case IMAGING_TRANSFORM_BILINEAR:
            filterp = &BILINEAR;
            break;
        case IMAGING_TRANSFORM_HAMMING:
            filterp = &HAMMING;
            break;
        case IMAGING_TRANSFORM_BICUBIC:
            filterp = &BICUBIC;
            break;
        case IMAGING_TRANSFORM_LANCZOS:
            filterp = &LANCZOS;
            break;
        default:
            return (Imaging)ImagingError_ValueError("unsupported resampling filter");
    }

    /* as ImagingResampleInner, for the rows and columns of the region only */
    need_horizontal = xsize != imIn->xsize || box[0] || box[2] != xsize;
    need_vertical = ysize != imIn->ysize || box[1] || box[3] != ysize;
    if (!need_horizontal && !need_vertical) {
        return ImagingCrop(imIn, x0, y0, x1, y1);
    }

    ksize_vert = precompute_coeffs(imIn->ysize, box[1], box[3], ysize, filterp, &bounds_vert, &kk_vert);
    if (!ksize_vert) {
        return NULL;
    }

    if (need_horizontal) {
        ksize_horiz = precompute_coeffs(imIn->xsize, box[0], box[2], xsize, filterp, &bounds_horiz, &kk_horiz);
        if (!ksize_horiz) {
            free(bounds_vert);
            free(kk_vert);
            return NULL;
        }
        if (need_vertical) {
            /* the source rows the region's rows use */
            first = bounds_vert[y0 * 2];
            last = bounds_vert[(y1 - 1) * 2] + bounds_vert[(y1 - 1) * 2 + 1];
            for (i = y0; i < y1; i++) {
                bounds_vert[i * 2] -= first;
            }
            imTemp = ImagingNewDirty(imIn->mode, x1 - x0, last - first);
            if (imTemp) {
                ResampleHorizontal(
                    imTemp, imIn, first, ksize_horiz, bounds_horiz + x0 * 2, kk_horiz + (size_t)x0 * ksize_horiz
                );
                imOut = ImagingNewDirty(imIn->mode, x1 - x0, y1 - y0);
                if (imOut) {
                    ResampleVertical(
                        imOut, imTemp, 0, ksize_vert, bounds_vert + y0 * 2, kk_vert + (size_t)y0 * ksize_vert
                    );
                }
                ImagingDelete(imTemp);
            }
        } else {
            /* (the rows stay as they are: the horizontal pass on the region's own rows) */
            imOut = ImagingNewDirty(imIn->mode, x1 - x0, y1 - y0);
            if (imOut) {
                ResampleHorizontal(
                    imOut, imIn, y0, ksize_horiz, bounds_horiz + x0 * 2, kk_horiz + (size_t)x0 * ksize_horiz
                );
            }
        }
        free(bounds_horiz);
        free(kk_horiz);
    } else {
        /* vertical only: the source columns are the region's columns */
        view = column_view(imIn, x0, x1 - x0);
        if (view) {
            imOut = ImagingNewDirty(imIn->mode, x1 - x0, y1 - y0);
            if (imOut) {
                ResampleVertical(imOut, view, 0, ksize_vert, bounds_vert + y0 * 2, kk_vert + (size_t)y0 * ksize_vert);
            }
            ImagingDelete(view);
        }
    }
    free(bounds_vert);
    free(kk_vert);
    return imOut;
}
