#ifndef HF_WAVEFORM_H
#define HF_WAVEFORM_H

#include <stdint.h>

// FPGA_TRACE_SIZE in proxmark3/common_fpga/fpga.h.
#define HF_WAVEFORM_COUNT 3072U
#define HF_WAVEFORM_OFFSET_BYTES 4U
#define HF_WAVEFORM_BUFFER_SIZE (HF_WAVEFORM_COUNT + HF_WAVEFORM_OFFSET_BYTES)

int hf_waveform_capture(uint8_t samples[HF_WAVEFORM_COUNT]);

#endif
