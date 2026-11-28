#ifndef BIGBUF_H
#define BIGBUF_H

#include <stdint.h>

int bigbuf_download(uint8_t* data, uint32_t start, uint32_t count);

int bigbuf_download_with_trace_length(
    uint8_t* data, uint32_t start, uint32_t count, uint32_t* trace_length);

#endif
