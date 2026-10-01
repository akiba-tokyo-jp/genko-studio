/*
 * Genko: what _imaging.c (Pillow's Python binding) provides to libImaging, without Python.
 *
 * libImaging reports a failure by calling ImagingError_* and returning NULL. Python keeps the message as the
 * pending exception; here it is kept per thread, for the caller to read right after the failing call.
 */
#ifndef GENKO_PILLOW_IMAGING_GLUE_H
#define GENKO_PILLOW_IMAGING_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The kind of the last error on this thread: 0 none, 1 memory, 2 value (ValueError and the mode errors). */
int
genko_imaging_error_kind(void);
/* Its message ("" when there is none). Valid until the next libImaging call on this thread. */
const char *
genko_imaging_error_message(void);
void
genko_imaging_clear_error(void);

#ifdef __cplusplus
}
#endif

#endif /* GENKO_PILLOW_IMAGING_GLUE_H */
