/* Run the current vendor split, then arm only its C++ ownership handoff. */
#define ImagingSplit genko_test_real_ImagingSplit
#include "../../third_party/pillow/libImaging/Bands.c"
#undef ImagingSplit
extern void genko_test_after_split(void);
int ImagingSplit(Imaging im, Imaging bands[4]) {
    const int result = genko_test_real_ImagingSplit(im, bands);
    if (result) genko_test_after_split();
    return result;
}
