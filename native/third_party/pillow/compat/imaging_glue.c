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

enum { kNoError = 0, kMemoryError = 1, kValueError = 2, kBudgetError = 3 };

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

typedef struct {
    uint64_t limit, live, peak;
    size_t refs;
    PyMutex mutex;
} GenkoBudget;
static GENKO_THREAD_LOCAL GenkoBudget *active_budget;
static GENKO_THREAD_LOCAL unsigned budget_depth;

int genko_imaging_budget_begin(uint64_t limit) {
    if (active_budget) {
        ++budget_depth; /* Recursive area calls share the same budget. */
        return 1;
    }
    active_budget = (GenkoBudget *)calloc(1, sizeof(GenkoBudget));
    if (!active_budget) { ImagingError_MemoryError(); return 0; }
    active_budget->limit = limit;
    active_budget->refs = 1;
    budget_depth = 1;
    return 1;
}

static void budget_unref(GenkoBudget *budget) {
    int last;
    PyMutex_Lock(&budget->mutex);
    last = --budget->refs == 0;
    PyMutex_Unlock(&budget->mutex);
    if (last) free(budget);
}

void genko_imaging_budget_end(void) {
    if (active_budget && --budget_depth == 0) {
        GenkoBudget *budget = active_budget;
        active_budget = NULL;
        budget_unref(budget);
    }
}

uint64_t genko_imaging_budget_live(void) {
    uint64_t value = 0;
    if (active_budget) {
        PyMutex_Lock(&active_budget->mutex);
        value = active_budget->live;
        PyMutex_Unlock(&active_budget->mutex);
    }
    return value;
}
uint64_t genko_imaging_budget_peak(void) {
    uint64_t value = 0;
    if (active_budget) {
        PyMutex_Lock(&active_budget->mutex);
        value = active_budget->peak;
        PyMutex_Unlock(&active_budget->mutex);
    }
    return value;
}

int genko_imaging_budget_reserve(Imaging im, uint64_t bytes) {
    GenkoBudget *budget = active_budget;
    if (!budget) return 1;
    PyMutex_Lock(&budget->mutex);
    if (bytes > budget->limit - budget->live) {
        PyMutex_Unlock(&budget->mutex);
        set_error(kBudgetError, "the area holds too many masks at once");
        return 0;
    }
    budget->live += bytes;
    if (budget->live > budget->peak) budget->peak = budget->live;
    ++budget->refs;
    im->genko_budget = budget;
    im->genko_budget_bytes = bytes;
    PyMutex_Unlock(&budget->mutex);
    return 1;
}

void genko_imaging_budget_release(Imaging im) {
    GenkoBudget *budget = (GenkoBudget *)im->genko_budget;
    if (!budget) return;
    PyMutex_Lock(&budget->mutex);
    budget->live -= im->genko_budget_bytes;
    PyMutex_Unlock(&budget->mutex);
    im->genko_budget = NULL;
    budget_unref(budget);
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
