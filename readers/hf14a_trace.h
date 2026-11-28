#ifndef HF14A_TRACE_H
#define HF14A_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HF14A_TRACE_CAPACITY 8192U

typedef struct {
    uint32_t timestamp;
    uint16_t duration;
    bool from_tag;
    size_t length;
    const uint8_t* data;
    size_t next;
} Hf14aTraceFrame;

int hf14a_trace_capture(uint8_t* data, uint32_t* length);
bool hf14a_trace_frame(const uint8_t* data, size_t length, size_t offset, Hf14aTraceFrame* frame);

#endif
