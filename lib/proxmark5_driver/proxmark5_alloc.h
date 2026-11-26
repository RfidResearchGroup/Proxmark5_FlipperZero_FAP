#ifndef PROXMARK5_ALLOC_H
#define PROXMARK5_ALLOC_H

#include <furi.h>

static inline void* proxmark5_try_alloc(size_t size) {
    // The Flipper heap's block size includes its two-word header.
    const size_t header_size = 2U * sizeof(uint32_t);
    int32_t lock = furi_kernel_lock();
    size_t available = memmgr_heap_get_max_free_block();
    void* buffer = NULL;
    if(size && available > header_size && size <= available - header_size) {
        buffer = malloc(size);
    }
    if(lock == 0) furi_kernel_unlock();
    return buffer;
}

#endif
