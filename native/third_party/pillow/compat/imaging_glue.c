/*
 * Genko: the functions libImaging expects from _imaging.c and from Python, without Python.
 *
 * - ImagingError_*: _imaging.c sets a Python exception and returns NULL. Here the message is kept per thread
 *   (genko_imaging_error_message) and NULL is returned in the same way.
 * - ImagingSectionEnter/Leave: _imaging.c releases the GIL there. There is no GIL: nothing to do.
 * - PyErr_SetString / PyErr_Clear / PyCapsule_GetPointer: the few Python calls left in the files Genko builds.
 * - PyMutex_Lock / PyMutex_Unlock: a small spin lock for libImaging's shared memory arena.
 */
#include "Python.h"

#include "imaging_glue.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#endif

#include "../libImaging/Imaging.h"

#if defined(_MSC_VER)
#define GENKO_THREAD_LOCAL __declspec(thread)
#else
#define GENKO_THREAD_LOCAL __thread
#endif

enum { kNoError = 0, kMemoryError = 1, kValueError = 2 };

static GENKO_THREAD_LOCAL int error_kind = kNoError;
static GENKO_THREAD_LOCAL char error_message[256];

static void
set_error(int kind, const char *message) {
    size_t n = 0;
    error_kind = kind;
    if (message) {
        n = strlen(message);
        if (n >= sizeof(error_message)) {
            n = sizeof(error_message) - 1;
        }
        memcpy(error_message, message, n);
    }
    error_message[n] = '\0';
}

int
genko_imaging_error_kind(void) {
    return error_kind;
}

const char *
genko_imaging_error_message(void) {
    return error_kind == kNoError ? "" : error_message;
}

void
genko_imaging_clear_error(void) {
    set_error(kNoError, "");
}

/* --- _imaging.c --------------------------------------------------------------------------------------------- */

void *
ImagingError_MemoryError(void) {
    set_error(kMemoryError, "out of memory");
    return NULL;
}

void *
ImagingError_Mismatch(void) {
    set_error(kValueError, "images do not match");
    return NULL;
}

void *
ImagingError_ModeError(void) {
    set_error(kValueError, "image has wrong mode");
    return NULL;
}

void *
ImagingError_ValueError(const char *message) {
    set_error(kValueError, message ? message : "unrecognized argument value");
    return NULL;
}

void
ImagingSectionEnter(ImagingSectionCookie *cookie) {
    (void)cookie;
}

void
ImagingSectionLeave(ImagingSectionCookie *cookie) {
    (void)cookie;
}

/* --- Python ----------------------------------------------------------------------------------------------- */

PyObject *PyExc_ValueError = NULL;

void
PyErr_SetString(PyObject *type, const char *message) {
    (void)type;
    set_error(kValueError, message);
}

void
PyErr_Clear(void) {
    genko_imaging_clear_error();
}

void *
PyCapsule_GetPointer(PyObject *capsule, const char *name) {
    (void)capsule;
    (void)name;
    set_error(kValueError, "Arrow arrays are not available");
    return NULL;
}

void
PyMutex_Lock(PyMutex *mutex) {
#if defined(_WIN32)
    while (InterlockedExchange((volatile LONG *)&mutex->locked, 1) != 0) {
        SwitchToThread();
    }
#else
    while (__atomic_exchange_n(&mutex->locked, 1, __ATOMIC_ACQUIRE) != 0) {
        sched_yield();
    }
#endif
}

void
PyMutex_Unlock(PyMutex *mutex) {
#if defined(_WIN32)
    InterlockedExchange((volatile LONG *)&mutex->locked, 0);
#else
    __atomic_store_n(&mutex->locked, 0, __ATOMIC_RELEASE);
#endif
}
