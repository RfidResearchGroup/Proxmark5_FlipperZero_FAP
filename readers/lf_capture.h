#ifndef LF_CAPTURE_H
#define LF_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "pm3_cmd.h"

enum { LfCaptureRestoreFailed = 1 };

void* lf_capture_alloc(size_t size);
int lf_capture_samples(uint32_t count);
int lf_sampling_get_config(sample_config* config);
int lf_sampling_set_config(const sample_config* config);

#endif
