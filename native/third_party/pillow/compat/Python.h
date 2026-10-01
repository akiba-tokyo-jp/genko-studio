/*
 * Genko: the few parts of Python.h that Pillow's libImaging uses, so that it builds without Python.
 *
 * libImaging includes "Python.h" through ImPlatform.h. This header stands in for it (it is found first on the
 * include path of the pillow_imaging target and its users). It gives the types the declarations in Imaging.h
 * need, the error functions Storage.c and ColorLUT.c call (implemented in imaging_glue.c) and a real mutex for the
 * shared memory arena: Genko renders on several threads, so Py_GIL_DISABLED is defined and PyMutex is a lock
 * (Pillow's free-threaded build does the same). Nothing here changes how a pixel is computed.
 */
#ifndef GENKO_PILLOW_COMPAT_PYTHON_H
#define GENKO_PILLOW_COMPAT_PYTHON_H

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What CPython's pyconfig.h tells ImPlatform.h. */
#ifndef HAVE_PROTOTYPES
#define HAVE_PROTOTYPES 1
#endif
#ifndef STDC_HEADERS
#define STDC_HEADERS 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef ptrdiff_t Py_ssize_t;

/* An object that is never made or looked into (only the Arrow and codec parts, which Genko does not build, use it). */
typedef struct genko_py_object PyObject;

#ifndef PY_LONG_LONG
#define PY_LONG_LONG long long
#endif

#define Py_INCREF(op) ((void)(op))
#define Py_DECREF(op) ((void)(op))
#define Py_XDECREF(op) ((void)(op))

extern PyObject *PyExc_ValueError;
void
PyErr_SetString(PyObject *type, const char *message);
void
PyErr_Clear(void);
void *
PyCapsule_GetPointer(PyObject *capsule, const char *name);

/* The shared memory arena and the image reference counts are guarded by this lock. */
#ifndef Py_GIL_DISABLED
#define Py_GIL_DISABLED 1
#endif
typedef struct {
    volatile long locked;
} PyMutex;
void
PyMutex_Lock(PyMutex *mutex);
void
PyMutex_Unlock(PyMutex *mutex);

#ifdef __cplusplus
}
#endif

#endif /* GENKO_PILLOW_COMPAT_PYTHON_H */
