/*
 * Genko: views of part of an image and the row-by-row steps of libImaging's box blur and unsharp mask, so that
 * render::Image can run them on bands of rows at once (several threads, each writing rows of its own). Every row is
 * computed by the same code as in the whole passes (ImagingHorizontalBoxBlur itself, ImagingTranspose itself, and the
 * lines of BoxBlur.c's _gaussian_blur_radius and UnsharpMask.c's sharpening copied unchanged), so the pixels are the
 * same as libImaging's whole passes, whatever the number of bands (native/third_party/pillow/GENKO_CHANGES.md).
 */
#include "Imaging.h"
#include "row_bands.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

Imaging
genko_imaging_view(Imaging base, int x0, int y0, int x1, int y1) {
    Imaging view;
    int y;

    if (!base || base->palette || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || x1 > base->xsize || y1 > base->ysize) {
        return (Imaging)ImagingError_ValueError("bad view");
    }
    view = (Imaging)calloc(1, sizeof(struct ImagingMemoryInstance));
    if (!view) {
        return (Imaging)ImagingError_MemoryError();
    }
    view->mode = base->mode;
    view->type = base->type;
    view->depth = base->depth;
    view->bands = base->bands;
    view->xsize = x1 - x0;
    view->ysize = y1 - y0;
    view->pixelsize = base->pixelsize;
    view->linesize = view->xsize * view->pixelsize;
    view->image = (char **)calloc(view->ysize > 0 ? view->ysize : 1, sizeof(char *));
    if (!view->image) {
        free(view);
        return (Imaging)ImagingError_MemoryError();
    }
    for (y = 0; y < view->ysize; y++) {
        view->image[y] = base->image[y0 + y] + (size_t)x0 * (size_t)base->pixelsize;
    }
    if (base->image8) {
        view->image8 = (UINT8 **)view->image;
    }
    if (base->image32) {
        view->image32 = (INT32 **)view->image;
    }
    return view;
}

void
genko_imaging_view_free(Imaging view) {
    if (view) {
        free(view->image);
        free(view);
    }
}

/* BoxBlur.c's _gaussian_blur_radius, unchanged. */
float
genko_gaussian_blur_radius(float radius, int passes) {
    float sigma2, L, l, a;

    sigma2 = radius * radius / passes;
    // from https://www.mia.uni-saarland.de/Publications/gwosdek-ssvm11.pdf
    // [7] Box length.
    L = sqrt(12.0 * sigma2 + 1.0);
    // [11] Integer part of box radius.
    l = floor((L - 1.0) / 2.0);
    // [14], [Fig. 2] Fractional part of box radius.
    a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));

    return l + a;
}

typedef UINT8 pixel[4];

/* UnsharpMask.c's clip8, unchanged. */
static inline UINT8
clip8(int in) {
    if (in >= 255) {
        return 255;
    }
    if (in <= 0) {
        return 0;
    }
    return (UINT8)in;
}

/* UnsharpMask.c's ImagingUnsharpMask after its gaussian blur, over the rows [y0, y1) instead of all: the loop
   unchanged. */
void
genko_unsharp_rows(Imaging imOut, Imaging imIn, int percent, int threshold, int y0, int y1) {
    int x, y, diff;

    pixel *lineIn = NULL;
    pixel *lineOut = NULL;
    UINT8 *lineIn8 = NULL;
    UINT8 *lineOut8 = NULL;

    for (y = y0; y < y1; y++) {
        if (imIn->image8) {
            lineIn8 = imIn->image8[y];
            lineOut8 = imOut->image8[y];
            for (x = 0; x < imIn->xsize; x++) {
                /* compare in/out pixels, apply sharpening */
                diff = lineIn8[x] - lineOut8[x];
                if (abs(diff) > threshold) {
                    /* add the diff*percent to the original pixel */
                    lineOut8[x] = clip8(lineIn8[x] + diff * percent / 100);
                } else {
                    /* new pixel is the same as imIn */
                    lineOut8[x] = lineIn8[x];
                }
            }
        } else {
            lineIn = (pixel *)imIn->image32[y];
            lineOut = (pixel *)imOut->image32[y];
            for (x = 0; x < imIn->xsize; x++) {
                /* compare in/out pixels, apply sharpening */
                diff = lineIn[x][0] - lineOut[x][0];
                lineOut[x][0] = abs(diff) > threshold
                                    ? clip8(lineIn[x][0] + diff * percent / 100)
                                    : lineIn[x][0];

                diff = lineIn[x][1] - lineOut[x][1];
                lineOut[x][1] = abs(diff) > threshold
                                    ? clip8(lineIn[x][1] + diff * percent / 100)
                                    : lineIn[x][1];

                diff = lineIn[x][2] - lineOut[x][2];
                lineOut[x][2] = abs(diff) > threshold
                                    ? clip8(lineIn[x][2] + diff * percent / 100)
                                    : lineIn[x][2];

                diff = lineIn[x][3] - lineOut[x][3];
                lineOut[x][3] = abs(diff) > threshold
                                    ? clip8(lineIn[x][3] + diff * percent / 100)
                                    : lineIn[x][3];
            }
        }
    }
}
