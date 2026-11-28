#ifndef LF_CAPTURE_H
#define LF_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

enum { LfCaptureRestoreFailed = 1 };

int lf_capture_samples(uint32_t count);

#endif
