/* Only Geometry.c's scale-table allocation is intercepted, not pixel storage.
 * All code under test is the current vendored implementation. */
#include <stdlib.h>
static int fail_table;
static int failures;
void genko_test_fail_scale_table(int fail) { fail_table = fail; failures = 0; }
int genko_test_scale_failures(void) { return failures; }
static void *checked_calloc(size_t count, size_t size) {
    if (fail_table && size == sizeof(int)) { ++failures; return NULL; }
    return calloc(count, size);
}
#define calloc checked_calloc
#include "../../third_party/pillow/libImaging/Geometry.c"
#undef calloc
