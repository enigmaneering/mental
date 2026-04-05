/*
 * Mental - Reference (Process-Local Data with GPU Pinning)
 *
 * A reference holds data in process-local memory and can optionally
 * be pinned to a GPU device for compute operations.
 *
 * GPU pinning (optional):
 *   mental_reference_pin(ref, device) attaches a backend buffer.
 *   mental_reference_write/read transfer between host and GPU.
 *   Pinned references participate in dispatch and viewport operations.
 */

#include "mental.h"
#include "mental_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include "mental_pthread.h"
#endif

/* ── Disclosure helpers ────────────────────────────────────────── */

static int credential_matches(mental_reference ref,
                               const void *credential, size_t credential_len) {
    if (!credential || credential_len == 0) return 0;
    if (!ref->credential || ref->credential_len == 0) return 0;

    /* Constant-time comparison: always examine the full length of
     * whichever credential is shorter, plus a length mismatch flag.
     * This prevents leaking credential length via timing. */
    volatile uint8_t result = 0;
    size_t len = credential_len < ref->credential_len
               ? credential_len : ref->credential_len;
    const uint8_t *a = (const uint8_t *)ref->credential;
    const uint8_t *b = (const uint8_t *)credential;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    /* Length mismatch counts as a failure */
    result |= (credential_len != ref->credential_len);
    return result == 0;
}

static void refresh_credential(mental_reference ref) {
    if (!ref->credential_fn) return;

    /* First call to determine the size needed */
    size_t out_len = 0;
    uint8_t tmp[256];
    ref->credential_fn(ref->credential_ctx, tmp, sizeof(tmp), &out_len);

    if (out_len > sizeof(tmp)) out_len = sizeof(tmp);

    free(ref->credential);
    ref->credential = malloc(out_len);
    if (ref->credential) {
        memcpy(ref->credential, tmp, out_len);
        ref->credential_len = out_len;
    } else {
        ref->credential_len = 0;
    }
}

/* ── Create ────────────────────────────────────────────────────── */

mental_reference mental_reference_create(size_t size,
                                          mental_relationship mode,
                                          const void *credential,
                                          size_t credential_len,
                                          mental_disclosure *out_disclosure) {
    if (size == 0) return NULL;

    mental_reference ref = calloc(1, sizeof(struct mental_reference_t));
    if (!ref) return NULL;

    ref->data = calloc(1, size);
    if (!ref->data) {
        free(ref);
        return NULL;
    }

    ref->size = size;
    ref->mode = mode;
    ref->valid = 1;
    pthread_mutex_init(&ref->lock, NULL);

    if (credential && credential_len > 0) {
        ref->credential = malloc(credential_len);
        if (ref->credential) {
            memcpy(ref->credential, credential, credential_len);
            ref->credential_len = credential_len;
        }
    }

    if (out_disclosure) {
        mental_disclosure dh = malloc(sizeof(struct mental_disclosure_t));
        if (dh) {
            dh->ref = ref;
            *out_disclosure = dh;
        } else {
            *out_disclosure = NULL;
        }
    }

    return ref;
}

/* ── Accessors (disclosure-aware) ──────────────────────────────── */

void* mental_reference_data(mental_reference ref,
                             const void *credential, size_t credential_len) {
    if (!ref || !ref->data || !ref->valid) return NULL;

    pthread_mutex_lock(&ref->lock);
    refresh_credential(ref);

    int granted = 0;
    switch (ref->mode) {
    case MENTAL_RELATIONALLY_OPEN:
        granted = 1;
        break;
    case MENTAL_RELATIONALLY_INCLUSIVE:
        granted = 1;
        break;
    case MENTAL_RELATIONALLY_EXCLUSIVE:
        granted = credential_matches(ref, credential, credential_len);
        break;
    }

    pthread_mutex_unlock(&ref->lock);
    return granted ? ref->data : NULL;
}

size_t mental_reference_size(mental_reference ref) {
    return (ref && ref->valid) ? ref->size : 0;
}

int mental_reference_writable(mental_reference ref,
                               const void *credential,
                               size_t credential_len) {
    if (!ref || !ref->data || !ref->valid) return 0;

    pthread_mutex_lock(&ref->lock);
    refresh_credential(ref);

    int writable = 0;
    switch (ref->mode) {
    case MENTAL_RELATIONALLY_OPEN:
        writable = 1;
        break;
    case MENTAL_RELATIONALLY_INCLUSIVE:
        writable = credential_matches(ref, credential, credential_len);
        break;
    case MENTAL_RELATIONALLY_EXCLUSIVE:
        writable = credential_matches(ref, credential, credential_len);
        break;
    }

    pthread_mutex_unlock(&ref->lock);
    return writable;
}

/* ── GPU pinning ───────────────────────────────────────────────── */

int mental_reference_pin(mental_reference ref, mental_device device) {
    if (!ref || !ref->valid || !device) return -1;

    pthread_mutex_lock(&ref->lock);

    if (ref->device == device && ref->backend_buffer) {
        pthread_mutex_unlock(&ref->lock);
        return 0;
    }

    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_destroy(ref->backend_buffer);
        ref->backend_buffer = NULL;
    }

    ref->device = device;
    ref->backend_buffer = device->backend->buffer_alloc(
        device->backend_device, ref->size);

    if (!ref->backend_buffer) {
        ref->device = NULL;
        pthread_mutex_unlock(&ref->lock);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED,
                         "Backend buffer allocation failed during pin");
        return -1;
    }

    device->backend->buffer_write(ref->backend_buffer, ref->data, ref->size);

    pthread_mutex_unlock(&ref->lock);
    return 0;
}

int mental_reference_is_pinned(mental_reference ref) {
    if (!ref || !ref->valid) return 0;
    return ref->backend_buffer != NULL;
}

mental_device mental_reference_device(mental_reference ref) {
    if (!ref || !ref->valid) return NULL;
    return ref->device;
}

/* ── Write / Read ──────────────────────────────────────────────── */

size_t mental_reference_write(mental_reference ref,
                               const void *data, size_t bytes) {
    if (!ref || !ref->valid || !data || bytes == 0) return 0;

    pthread_mutex_lock(&ref->lock);

    size_t write_size = bytes < ref->size ? bytes : ref->size;
    memcpy(ref->data, data, write_size);

    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_write(
            ref->backend_buffer, data, write_size);
    }

    pthread_mutex_unlock(&ref->lock);
    return write_size;
}

size_t mental_reference_read(mental_reference ref,
                              void *data, size_t bytes) {
    if (!ref || !ref->valid || !data || bytes == 0) return 0;

    pthread_mutex_lock(&ref->lock);

    size_t read_size = bytes < ref->size ? bytes : ref->size;

    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_read(
            ref->backend_buffer, data, read_size);
    } else {
        memcpy(data, ref->data, read_size);
    }

    pthread_mutex_unlock(&ref->lock);
    return read_size;
}

/* ── Clone ─────────────────────────────────────────────────────── */

mental_reference mental_reference_clone(mental_reference ref,
                                         mental_device device,
                                         const void *credential,
                                         size_t credential_len) {
    if (!ref || !ref->data || !ref->valid)
        return NULL;

    /* Check disclosure (this acquires and releases the lock) */
    void *src = mental_reference_data(ref, credential, credential_len);
    if (!src) return NULL;

    mental_reference clone = mental_reference_create(ref->size,
                                                       MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    if (!clone) return NULL;

    /* Hold the source lock during the copy to prevent concurrent writes */
    pthread_mutex_lock(&ref->lock);
    memcpy(clone->data, ref->data, ref->size);
    pthread_mutex_unlock(&ref->lock);

    if (device) {
        if (mental_reference_pin(clone, device) != 0) {
            mental_reference_close(clone);
            return NULL;
        }
    }

    return clone;
}

/* ── Disclosure API ────────────────────────────────────────────── */

mental_relationship mental_reference_get_disclosure(mental_reference ref) {
    if (!ref || !ref->valid) return MENTAL_RELATIONALLY_OPEN;
    pthread_mutex_lock(&ref->lock);
    mental_relationship mode = ref->mode;
    pthread_mutex_unlock(&ref->lock);
    return mode;
}

/* Internal setters (used by disclosure handle) */
void mental_reference_set_disclosure(mental_reference ref, mental_relationship mode) {
    if (!ref || !ref->valid) return;
    pthread_mutex_lock(&ref->lock);
    ref->mode = mode;
    pthread_mutex_unlock(&ref->lock);
}

void mental_reference_set_credential(mental_reference ref,
                                      const void *credential, size_t len) {
    if (!ref || !ref->valid) return;
    pthread_mutex_lock(&ref->lock);
    free(ref->credential);
    ref->credential = NULL;
    ref->credential_len = 0;
    if (credential && len > 0) {
        ref->credential = malloc(len);
        if (ref->credential) {
            memcpy(ref->credential, credential, len);
            ref->credential_len = len;
        }
    }
    ref->credential_fn  = NULL;
    ref->credential_ctx = NULL;
    pthread_mutex_unlock(&ref->lock);
}

void mental_reference_set_credential_provider(mental_reference ref,
                                               mental_credential_fn fn,
                                               void *ctx) {
    if (!ref || !ref->valid) return;
    pthread_mutex_lock(&ref->lock);
    ref->credential_fn  = fn;
    ref->credential_ctx = ctx;
    if (fn) refresh_credential(ref);
    pthread_mutex_unlock(&ref->lock);
}

/* ── Disclosure Handle ─────────────────────────────────────────── */

void mental_disclosure_set_mode(mental_disclosure dh, mental_relationship mode) {
    if (!dh || !dh->ref) return;
    mental_reference_set_disclosure(dh->ref, mode);
}

void mental_disclosure_set_credential(mental_disclosure dh,
                                        const void *credential, size_t len) {
    if (!dh || !dh->ref) return;
    mental_reference_set_credential(dh->ref, credential, len);
}

void mental_disclosure_set_credential_provider(mental_disclosure dh,
                                                 mental_credential_fn fn, void *ctx) {
    if (!dh || !dh->ref) return;
    mental_reference_set_credential_provider(dh->ref, fn, ctx);
}

void mental_disclosure_close(mental_disclosure dh) {
    if (!dh) return;
    dh->ref = NULL;
    free(dh);
}

/* ── Close ─────────────────────────────────────────────────────── */

void mental_reference_close(mental_reference ref) {
    if (!ref) return;

    pthread_mutex_lock(&ref->lock);

    if (ref->backend_buffer && ref->device) {
        ref->device->backend->buffer_destroy(ref->backend_buffer);
        ref->backend_buffer = NULL;
        ref->device = NULL;
    }

    ref->valid = 0;

    if (ref->credential) {
        free(ref->credential);
        ref->credential = NULL;
        ref->credential_len = 0;
    }

    if (ref->data) {
        free(ref->data);
        ref->data = NULL;
    }

    pthread_mutex_unlock(&ref->lock);
    pthread_mutex_destroy(&ref->lock);
    free(ref);
}
