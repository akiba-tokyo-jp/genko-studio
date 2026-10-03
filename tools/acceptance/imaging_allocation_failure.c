/* Deterministic pixel-block allocation failure, without a production hook.
 * Include the exact Storage.c under calloc interception; all other files use libc.
 * Run under a timeout: the old cleanup reacquires its own non-recursive arena mutex.
 */
#include <stdio.h>
#include <stdlib.h>
static int fail_pixels;
static int rejected;
static void *checked_calloc(size_t count, size_t bytes) {
    if (fail_pixels && count == 1 && bytes >= 512) { ++rejected; return NULL; }
    return calloc(count, bytes);
}
#define calloc checked_calloc
#include "../../native/third_party/pillow/libImaging/Storage.c"
#undef calloc
int main(void) {
    if (!genko_imaging_budget_begin(1000000)) return 2;
    ImagingMemorySetBlockAllocator(&ImagingDefaultArena, 0);
    fail_pixels = 1;
    Imaging bad = ImagingNew(IMAGING_MODE_L, 512, 512);
    fprintf(stderr, "bad=%p rejected=%d live=%llu error=%s\n", (void*)bad, rejected,
            (unsigned long long)genko_imaging_budget_live(), genko_imaging_error_message());
    if (bad || rejected < 2 || genko_imaging_budget_live() != 0) return 3;
    fail_pixels = 0;
    genko_imaging_clear_error();
    Imaging good = ImagingNew(IMAGING_MODE_L, 8, 8);
    if (!good || genko_imaging_budget_live() != 64) return 4;
    ImagingDelete(good);
    if (genko_imaging_budget_live() != 0) return 5;
    genko_imaging_budget_end();
    puts("allocation failure returned; budget recovered; subsequent allocation succeeded");
    return 0;
}
